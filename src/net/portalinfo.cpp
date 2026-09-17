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
#include <QSettings>

#include "core/svxcore.h"   /* log_info */

namespace {

constexpr char kTgKey[]      = "portal/talkgroups";
constexpr char kCallKey[]    = "portal/callsigns";
constexpr char kHostKey[]    = "portal/host";
constexpr char kFetchedKey[] = "portal/fetched";

constexpr qint64 kMaxAgeSec = 24 * 60 * 60;

QByteArray userAgent()
{
    return QByteArrayLiteral("SVXConnect-Omarchy/") + SVXCONNECT_VERSION;
}

} // namespace

PortalInfo::PortalInfo(QObject *parent) : QObject(parent)
{
    m_net = new QNetworkAccessManager(this);
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

    if (!sameHost || fetched <= 0 || age > kMaxAgeSec)
        refresh();
}

void PortalInfo::load()
{
    m_talkgroups.clear();
    m_callsigns.clear();

    QSettings s;
    if (s.value(QLatin1String(kHostKey)).toString() != m_base.host())
        return;

    m_talkgroups = parseTalkgroups(s.value(QLatin1String(kTgKey)).toByteArray());
    m_callsigns  = parseCallsigns(s.value(QLatin1String(kCallKey)).toByteArray());
}

void PortalInfo::refresh()
{
    if (!m_base.isValid())
        return;
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

    QNetworkReply *reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, file]() {
        reply->deleteLater();

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status < 200 || status > 299) {
            /* A plain reflector has no portal, and a portal may simply not
             * serve one of the two files. Neither is worth a banner: the
             * window works without any of this. */
            log_info("portal: %s unavailable (%s)", qPrintable(file),
                     status ? qPrintable(QStringLiteral("HTTP %1").arg(status))
                            : qPrintable(reply->errorString()));
            return;
        }

        const QByteArray body = reply->readAll();
        QSettings s;
        s.setValue(QLatin1String(kHostKey), m_base.host());
        s.setValue(QLatin1String(kFetchedKey), QDateTime::currentSecsSinceEpoch());

        if (file.startsWith(QLatin1String("talkgroups"))) {
            const QHash<quint32, QString> parsed = parseTalkgroups(body);
            if (parsed.isEmpty())
                return;
            m_talkgroups = parsed;
            s.setValue(QLatin1String(kTgKey), body);
            log_info("portal: %d talkgroup names", int(parsed.size()));
        } else {
            const QHash<QString, QString> parsed = parseCallsigns(body);
            if (parsed.isEmpty())
                return;
            m_callsigns = parsed;
            s.setValue(QLatin1String(kCallKey), body);
            log_info("portal: %d station descriptions", int(parsed.size()));
        }
        emit changed();
    });
}
