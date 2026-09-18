/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 */
#include "net/portalinfo.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonParseError>
#include <QSettings>
#include <QTimer>

#include "core/svxcore.h"   /* log_info */

namespace {

constexpr char kTgKey[]      = "portal/talkgroups";
constexpr char kCallKey[]    = "portal/callsigns";
constexpr char kHostKey[]    = "portal/host";
constexpr char kFetchedKey[] = "portal/fetched";
constexpr char kAutoKey[]    = "portal/autoUpdate";

constexpr qint64 kMaxAgeSec = 24 * 60 * 60;

QByteArray userAgent()
{
    return QByteArrayLiteral("SVXConnect-Omarchy/") + SVXCONNECT_VERSION;
}

} // namespace

PortalInfo::PortalInfo(QObject *parent) : QObject(parent)
{
    m_net = new QNetworkAccessManager(this);

    /* A tray application stays up for days, so "once a day" cannot only mean
     * "at start". An hourly look at the age rather than one 24-hour timer: a
     * long timer does not survive a suspended laptop in any useful sense. */
    auto *daily = new QTimer(this);
    daily->setInterval(60 * 60 * 1000);
    connect(daily, &QTimer::timeout, this, [this]() {
        if (!m_network || !autoUpdateSetting() || !m_base.isValid())
            return;
        if (QDateTime::currentSecsSinceEpoch() - lastFetched() > kMaxAgeSec)
            refresh();
    });
    daily->start();
}

bool PortalInfo::autoUpdateSetting()
{
    return QSettings().value(QLatin1String(kAutoKey), true).toBool();
}

void PortalInfo::setAutoUpdateSetting(bool on)
{
    QSettings().setValue(QLatin1String(kAutoKey), on);
}

qint64 PortalInfo::lastFetched() const
{
    return QSettings().value(QLatin1String(kFetchedKey)).toLongLong();
}

void PortalInfo::setNetworkEnabled(bool on)
{
    m_network = on;
}

bool PortalInfo::setManualJson(const QByteArray &talkgroups, const QByteArray &callsigns,
                               QString *error)
{
    /* Both documents are checked before either is stored. */
    auto validate = [error](const QByteArray &text, const QString &what) {
        if (text.trimmed().isEmpty())
            return true;                               /* empty clears it */
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(text, &pe);
        if (pe.error != QJsonParseError::NoError) {
            if (error)
                *error = tr("%1: %2 (at character %3)").arg(what, pe.errorString()).arg(pe.offset);
            return false;
        }
        if (!doc.isObject()) {
            if (error)
                *error = tr("%1: it has to be a JSON object, like {\"8\": \"70cm Repeaters\"}").arg(what);
            return false;
        }
        return true;
    };
    if (!validate(talkgroups, tr("Talkgroup info")) || !validate(callsigns, tr("Callsign info")))
        return false;

    m_rawTalkgroups = talkgroups.trimmed();
    m_rawCallsigns  = callsigns.trimmed();
    m_talkgroups    = parseTalkgroups(m_rawTalkgroups);
    m_callsigns     = parseCallsigns(m_rawCallsigns);

    /* Stored against the current reflector, like a fetched document: a list of
     * Belgian talkgroup names is wrong, not stale, on another network. */
    QSettings s;
    s.setValue(QLatin1String(kHostKey), m_base.host());
    s.setValue(QLatin1String(kTgKey),   m_rawTalkgroups);
    s.setValue(QLatin1String(kCallKey), m_rawCallsigns);

    emit changed();
    return true;
}

QUrl PortalInfo::portalBaseUrl(const QString &host)
{
    QString bare = host.trimmed().section(QLatin1Char(','), 0, 0)
                                 .section(QLatin1Char(':'), 0, 0).trimmed();
    if (bare.isEmpty())
        return QUrl();

    /* The portal is a sibling of the reflector, not a child: reflector.x and x
     * both resolve to portal.x. */
    if (bare.startsWith(QLatin1String("reflector."), Qt::CaseInsensitive))
        bare = bare.mid(int(strlen("reflector.")));
    if (bare.isEmpty())
        return QUrl();

    const QUrl url(QStringLiteral("https://portal.%1/").arg(bare));
    return url.isValid() ? url : QUrl();
}

QHash<quint32, QString> PortalInfo::parseTalkgroups(const QByteArray &json)
{
    QHash<quint32, QString> out;
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    for (auto it = o.constBegin(); it != o.constEnd(); ++it) {
        bool ok = false;
        const quint32 id = it.key().toUInt(&ok);
        /* Anything that is not "<number>": "<text>" is skipped rather than
         * failing the file — a portal that adds a field must not blank the
         * names that were working. */
        if (ok && it.value().isString())
            out.insert(id, it.value().toString());
    }
    return out;
}

QHash<QString, QString> PortalInfo::parseCallsigns(const QByteArray &json)
{
    QHash<QString, QString> out;
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    for (auto it = o.constBegin(); it != o.constEnd(); ++it)
        if (it.value().isString())
            out.insert(it.key().toUpper(), it.value().toString());
    return out;
}

QString PortalInfo::callsignInfo(const QString &callsign) const
{
    return m_callsigns.value(callsign.toUpper());
}

void PortalInfo::setReflector(const QString &host)
{
    const QUrl base = portalBaseUrl(host);
    if (base == m_base && m_host == host)
        return;

    m_host = host;
    m_base = base;

    load();
    emit changed();

    if (!m_base.isValid())
        return;

    /* The cache belongs to one reflector. Pointed at another, it is not stale,
     * it is wrong. */
    QSettings s;
    const bool sameHost = s.value(QLatin1String(kHostKey)).toString() == m_base.host();
    const qint64 fetched = s.value(QLatin1String(kFetchedKey)).toLongLong();
    const qint64 age = QDateTime::currentSecsSinceEpoch() - fetched;

    if (!m_network || !autoUpdateSetting())
        return;   /* the operator's own document stays the operator's */

    if (!sameHost || fetched <= 0 || age > kMaxAgeSec)
        refresh();
}

void PortalInfo::load()
{
    m_talkgroups.clear();
    m_callsigns.clear();
    m_rawTalkgroups.clear();
    m_rawCallsigns.clear();

    QSettings s;
    if (s.value(QLatin1String(kHostKey)).toString() != m_base.host())
        return;

    m_rawTalkgroups = s.value(QLatin1String(kTgKey)).toByteArray();
    m_rawCallsigns  = s.value(QLatin1String(kCallKey)).toByteArray();
    m_talkgroups    = parseTalkgroups(m_rawTalkgroups);
    m_callsigns     = parseCallsigns(m_rawCallsigns);
}

void PortalInfo::refresh()
{
    if (!m_base.isValid())
        return;
    m_lastError.clear();
    fetch(QStringLiteral("talkgroups.json"));
    fetch(QStringLiteral("callsigns.json"));
}

void PortalInfo::fetch(const QString &file)
{
    QNetworkRequest req(m_base.resolved(QUrl(file)));
    req.setRawHeader(QByteArrayLiteral("User-Agent"), userAgent());
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                     QVariant::fromValue(QNetworkRequest::AlwaysNetwork));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QVariant::fromValue(QNetworkRequest::NoLessSafeRedirectPolicy));
    req.setTransferTimeout(15000);

    ++m_pending;
    emit statusChanged();

    const QString askedHost = m_base.host();

    QNetworkReply *reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, file, askedHost]() {
        reply->deleteLater();
        --m_pending;

        /* The reflector changed while this was in flight: the answer belongs
         * to the old one, and storing it would file Belgian talkgroup names
         * under another network. */
        if (askedHost != m_base.host()) {
            emit statusChanged();
            return;
        }

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status < 200 || status > 299) {
            /* A plain reflector has no portal, and a portal may simply not
             * serve one of the two files. Neither is worth a banner: the
             * window works without any of this. */
            m_lastError = status ? tr("HTTP %1 for %2").arg(status).arg(file)
                                 : tr("%1: %2").arg(file, reply->errorString());
            log_info("portal: %s unavailable (%s)", qPrintable(file), qPrintable(m_lastError));
            emit statusChanged();
            return;
        }

        const QByteArray body = reply->readAll();
        QSettings s;
        s.setValue(QLatin1String(kHostKey), m_base.host());
        s.setValue(QLatin1String(kFetchedKey), QDateTime::currentSecsSinceEpoch());

        if (file.startsWith(QLatin1String("talkgroups"))) {
            const QHash<quint32, QString> parsed = parseTalkgroups(body);
            if (parsed.isEmpty()) {
                m_lastError = tr("%1 held no talkgroup names").arg(file);
                emit statusChanged();
                return;
            }
            m_talkgroups    = parsed;
            m_rawTalkgroups = body;
            s.setValue(QLatin1String(kTgKey), body);
            log_info("portal: %d talkgroup names", int(parsed.size()));
        } else {
            const QHash<QString, QString> parsed = parseCallsigns(body);
            if (parsed.isEmpty()) {
                emit statusChanged();   /* an empty callsign list is not an error */
                return;
            }
            m_callsigns    = parsed;
            m_rawCallsigns = body;
            s.setValue(QLatin1String(kCallKey), body);
            log_info("portal: %d station descriptions", int(parsed.size()));
        }
        emit changed();
        emit statusChanged();
    });
}
