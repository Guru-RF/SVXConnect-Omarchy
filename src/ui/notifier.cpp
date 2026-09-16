/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 */
#include "ui/notifier.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QSettings>
#include <QStringList>
#include <QVariantMap>

namespace {

constexpr char kSetting[] = "notify/talkers";

/* The freedesktop notification service. Omarchy's shell implements it, and
 * advertises "actions", which is what makes the card clickable. */
const char *kService = "org.freedesktop.Notifications";
const char *kPath    = "/org/freedesktop/Notifications";
const char *kIface   = "org.freedesktop.Notifications";

/* The callsign as configured, stripped of an SSID, for comparison with a
 * talker's already-stripped `call`. */
QString ownCall(const svx_config *cfg)
{
    QString mine = QString::fromUtf8(cfg->callsign).trimmed().toUpper();
    const int dash = mine.indexOf(QLatin1Char('-'));
    return dash > 0 ? mine.left(dash) : mine;
}

} // namespace

Notifier::Notifier(svx_app *app, QObject *parent) : QObject(parent), m_app(app)
{
    m_enabled = enabledSetting();
}

bool Notifier::enabledSetting()
{
    return QSettings().value(QLatin1String(kSetting), true).toBool();
}

void Notifier::setEnabledSetting(bool on)
{
    QSettings().setValue(QLatin1String(kSetting), on);
}

QStringList Notifier::freshTalkers(const tg_manager *tgm, const QString &own,
                                   QSet<QString> *seen, quint32 *tgOut)
{
    QSet<QString> current;
    QStringList   fresh;      /* display names of talkers that just started */

    for (int i = 0; i < tgm->n_active; ++i) {
        const tgm_talker &t = tgm->active[i];
        const QString call = QString::fromUtf8(t.call).toUpper();
        if (call.isEmpty())
            continue;
        current.insert(call);

        if (seen->contains(call))
            continue;                       /* already talking a tick ago */
        if (!own.isEmpty() && call == own)
            continue;                       /* our own audio, echoed back */

        fresh << QString::fromUtf8(t.full);
        if (tgOut)
            *tgOut = t.tg;
    }

    *seen = current;
    return fresh;
}

void Notifier::tickModel(bool announce)
{
    if (!m_app)
        return;

    quint32 tg = 0;
    const QStringList fresh =
        freshTalkers(app_tgm(m_app), ownCall(app_config(m_app)), &m_active, &tg);

    /* The first pass only learns who is already talking: whoever was on the
     * air when SVXConnect started did not "just start". */
    if (!m_primed) {
        m_primed = true;
        return;
    }

    if (!announce || !m_enabled || fresh.isEmpty())
        return;

    announceTalkers(fresh, tg, fresh.size() - 1);
}

void Notifier::announceTalkers(const QStringList &names, quint32 tg, int extra)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return;

    /* Subscribed lazily, so an application that never notifies never touches
     * these signals. Both are matched on our own notification id. */
    if (!m_subscribed) {
        m_subscribed =
            bus.connect(QLatin1String(kService), QLatin1String(kPath), QLatin1String(kIface),
                        QStringLiteral("ActionInvoked"), this, SLOT(onActionInvoked(uint, QString)))
            && bus.connect(QLatin1String(kService), QLatin1String(kPath), QLatin1String(kIface),
                        QStringLiteral("NotificationClosed"), this, SLOT(onNotificationClosed(uint, uint)));
    }

    const QString summary = names.first();
    QString body = tr("talking on TG %1").arg(tg);
    if (extra > 0)
        body += tr(" · %n more station(s)", nullptr, extra);

    QVariantMap hints;
    hints.insert(QStringLiteral("urgency"), QVariant::fromValue<uchar>(1));
    /* Lets the shell find our icon and group the card with the application. */
    hints.insert(QStringLiteral("desktop-entry"), QStringLiteral("SVXConnect"));
    hints.insert(QStringLiteral("category"), QStringLiteral("im.received"));

    QDBusInterface notifications(QLatin1String(kService), QLatin1String(kPath),
                                 QLatin1String(kIface), bus);
    const QDBusReply<uint> reply = notifications.call(
        QStringLiteral("Notify"),
        QStringLiteral("SVXConnect"),
        m_lastId,                                   /* replace our previous card */
        QStringLiteral("SVXConnect"),               /* icon, by desktop file name */
        summary,
        body,
        QStringList{QStringLiteral("default"), tr("Open")},
        hints,
        8000);

    if (reply.isValid())
        m_lastId = reply.value();
    else
        log_warn("notify: %s", qPrintable(reply.error().message()));
}

void Notifier::onActionInvoked(uint id, const QString &actionKey)
{
    Q_UNUSED(actionKey);
    if (id == m_lastId)
        emit showWindowRequested();
}

void Notifier::onNotificationClosed(uint id, uint reason)
{
    Q_UNUSED(reason);
    /* Forget the id once the card is gone, so the next talker opens a new one
     * rather than trying to replace a card that no longer exists. */
    if (id == m_lastId)
        m_lastId = 0;
}
