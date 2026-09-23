/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 */
#include "ptt/portalbackend.h"
#include "ptt/hyprlandbinding.h"
#include "core/svxcore.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusMetaType>
#include <QDBusArgument>
#include <QDBusObjectPath>
#include <QDBusServiceWatcher>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QFile>
#include <QStandardPaths>
#include <QUuid>

/* The (s, a{sv}) struct the portal's a(sa{sv}) is made of. */
QDBusArgument &operator<<(QDBusArgument &arg, const Shortcut &s)
{
    arg.beginStructure();
    arg << s.first << s.second;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, Shortcut &s)
{
    arg.beginStructure();
    arg >> s.first >> s.second;
    arg.endStructure();
    return arg;
}

namespace {

const char *kService  = "org.freedesktop.portal.Desktop";
const char *kPath     = "/org/freedesktop/portal/desktop";
const char *kIface    = "org.freedesktop.portal.GlobalShortcuts";
const char *kRequest  = "org.freedesktop.portal.Request";
const char *kRegistry = "org.freedesktop.host.portal.Registry";
const char *kSession  = "org.freedesktop.portal.Session";

/* Must equal the basename of an installed .desktop file, or Register fails
 * with "Could not register app ID: App info not found".
 *
 * Plain "SVXConnect" rather than a reverse-DNS id: it is what the user sees
 * wherever the desktop lists the shortcut, and on Hyprland it is half of the
 * name the bind has to spell out — "SVXConnect:ptt" — so it had better be
 * short and obvious. */
const char *kAppId = "SVXConnect";

const char *kShortcutId = "ptt";

/* How long any portal call may take before it is given up on. Only bounds
 * how long a reply is waited FOR: nothing blocks while it is. */
constexpr int kCallTimeoutMs = 10000;
constexpr int kProbeTimeoutMs = 500;

/* A DEDICATED connection, not QDBusConnection::sessionBus(): Registry.Register
 * must be the first portal call made on its connection, and Qt talks to
 * org.freedesktop.portal.Settings during QGuiApplication construction. */
const char *kConnName = "svxconnect-ptt-portal";

/* The object path the portal will send Request::Response to, which must be
 * subscribed to BEFORE the call is issued or the reply races the subscription.
 * Built from our unique bus name with the leading ':' stripped and '.' -> '_'. */
QString requestPath(const QDBusConnection &conn, const QString &token)
{
    QString sender = conn.baseService();
    if (sender.startsWith(QLatin1Char(':')))
        sender.remove(0, 1);
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    return QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);
}

QString freshToken(const char *prefix)
{
    return QLatin1String(prefix)
         + QUuid::createUuid().toString(QUuid::Id128).left(8);
}

} // namespace

PortalBackend::PortalBackend(QObject *parent)
    : PttBackend(parent), m_hyprland(HyprlandBinding::isHyprland())
{
    /* Once per process; qDBusRegisterMetaType is idempotent. */
    static bool registered = false;
    if (!registered) {
        registered = true;
        qDBusRegisterMetaType<Shortcut>();
        qDBusRegisterMetaType<ShortcutList>();
    }
}

PortalBackend::~PortalBackend()
{
    stop();
    if (m_conn)
        QDBusConnection::disconnectFromBus(QLatin1String(kConnName));
}

PttAvailability PortalBackend::probe() const
{
    PttAvailability a;

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        a.state  = PttAvailability::Unavailable;
        a.reason = tr("No session bus.");
        return a;
    }

    /* The one synchronous portal call left: probe() is a question the settings
     * page asks and must answer on the spot. A plain message rather than a
     * QDBusInterface (which would introspect first, a second blocking round
     * trip), with a short timeout rather than QtDBus's 25 s, so a hung portal
     * costs the opening of Preferences half a second, not the audio. */
    QDBusMessage get = QDBusMessage::createMethodCall(
        QLatin1String(kService), QLatin1String(kPath),
        QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
    get << QLatin1String(kIface) << QStringLiteral("version");
    const QDBusReply<QVariant> v = bus.call(get, QDBus::Block, kProbeTimeoutMs);
    if (!v.isValid()) {
        a.state  = PttAvailability::Unavailable;
        a.reason = tr("No global-shortcuts portal is running.");
        a.instructions = m_hyprland
            ? tr("Omarchy provides it through xdg-desktop-portal-hyprland. Check that the "
                 "package is installed and the portal is running, or use the control FIFO "
                 "binding below.")
            : tr("Bind a key in your compositor to the control FIFO instead — see below.");
        return a;
    }

    /* Register needs an installed .desktop whose basename is our app id. */
    if (!hasDesktopFile()) {
        a.state  = PttAvailability::NeedsSetup;
        a.reason = tr("SVXConnect's desktop entry is missing, or the program its Exec= "
                      "line names cannot be found, so the portal cannot identify it.");
        a.instructions = tr("Install the package. From a build tree, install "
                            "data/%1.desktop into ~/.local/share/applications/ with Exec= "
                            "set to the full path of the binary.").arg(QLatin1String(kAppId));
        a.hasRelease = true;
        return a;
    }

    a.state      = PttAvailability::Available;
    a.hasRelease = true;
    a.reason     = m_hyprland
        ? tr("Registered with Hyprland as %1.").arg(QLatin1String(HyprlandBinding::kShortcutId))
        : tr("Ready.");
    return a;
}

bool PortalBackend::hasDesktopFile()
{
    const QString name = QStringLiteral("%1.desktop").arg(QLatin1String(kAppId));
    return !QStandardPaths::locate(QStandardPaths::ApplicationsLocation, name).isEmpty();
}

QString PortalBackend::activeTrigger() const
{
    return m_hyprland ? HyprlandBinding::boundKeys() : m_active;
}

bool PortalBackend::isActive() const
{
    if (m_session.isEmpty())
        return false;
    return m_hyprland || !m_active.isEmpty();
}

bool PortalBackend::start(const PttBinding &binding)
{
    if (!binding.isValid())
        return false;

    stop();

    m_requested = binding.trigger;

    QDBusConnection bus =
        QDBusConnection::connectToBus(QDBusConnection::SessionBus, QLatin1String(kConnName));
    if (!bus.isConnected()) {
        log_err("ptt: could not open a dedicated session bus connection");
        return false;
    }
    m_conn = true;

    /* Watch the portal itself. A portal that exits or restarts while the key
     * is held never sends the Deactivated, and nothing else would notice. */
    if (!m_watcher) {
        m_watcher = new QDBusServiceWatcher(QLatin1String(kService), bus,
                                            QDBusServiceWatcher::WatchForOwnerChange, this);
        connect(m_watcher, &QDBusServiceWatcher::serviceUnregistered,
                this, &PortalBackend::onPortalVanished);
        connect(m_watcher, &QDBusServiceWatcher::serviceRegistered,
                this, &PortalBackend::onPortalAppeared);
    }

    /* Registry.Register FIRST, at most once per connection, and never under
     * Flatpak, where the portal already knows the app id.
     *
     * Every portal call here is ASYNCHRONOUS. They used to block — and a
     * QDBusInterface blocks twice, introspecting in its constructor before the
     * call itself — for up to QtDBus's 25 s default, on the GUI thread that
     * also runs the core. A portal that is slow to start at login, or hung,
     * froze audio and heartbeats for that long. "First" is kept by chaining:
     * CreateSession is only sent once Register has answered. */
    const quint64 gen = m_generation;
    if (!m_registered && !QFile::exists(QStringLiteral("/.flatpak-info"))) {
        m_registered = true;
        QDBusMessage reg = QDBusMessage::createMethodCall(
            QLatin1String(kService), QLatin1String(kPath), QLatin1String(kRegistry),
            QStringLiteral("Register"));
        reg << QLatin1String(kAppId) << QVariantMap{};
        whenAnswered(bus.asyncCall(reg, kCallTimeoutMs), [this, gen](const QDBusMessage &r) {
            if (r.type() == QDBusMessage::ErrorMessage)
                log_warn("ptt: Registry.Register failed: %s", qPrintable(r.errorMessage()));
            if (gen == m_generation)
                createSession();
        });
        return true;
    }

    createSession();
    return true;
}

void PortalBackend::whenAnswered(const QDBusPendingCall &call,
                                 std::function<void(const QDBusMessage &)> then)
{
    auto *w = new QDBusPendingCallWatcher(call, this);
    connect(w, &QDBusPendingCallWatcher::finished, this,
            [then = std::move(then)](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                then(w->reply());
            });
}

void PortalBackend::createSession()
{
    QDBusConnection bus = QDBusConnection(QLatin1String(kConnName));

    /* Subscribe BEFORE calling, or the reply can arrive first. */
    const QString sessionToken = freshToken("svxs");
    const QString sessionReq   = requestPath(bus, sessionToken);

    bus.connect(QString(), sessionReq, QLatin1String(kRequest),
                QStringLiteral("Response"),
                this, SLOT(onCreateSessionResponse(uint, QVariantMap)));

    QVariantMap opts;
    opts.insert(QStringLiteral("handle_token"), sessionToken);
    opts.insert(QStringLiteral("session_handle_token"), freshToken("svxsess"));

    QDBusMessage call = QDBusMessage::createMethodCall(
        QLatin1String(kService), QLatin1String(kPath), QLatin1String(kIface),
        QStringLiteral("CreateSession"));
    call << opts;

    whenAnswered(bus.asyncCall(call, kCallTimeoutMs), [](const QDBusMessage &r) {
        if (r.type() == QDBusMessage::ErrorMessage)
            log_err("ptt: CreateSession failed: %s", qPrintable(r.errorMessage()));
    });
}

void PortalBackend::onCreateSessionResponse(uint code, const QVariantMap &results)
{
    if (code != 0) {
        log_warn("ptt: the portal refused the shortcuts session (code %u)", code);
        emit lost(tr("The desktop refused the global-shortcut session."));
        return;
    }

    m_session = results.value(QStringLiteral("session_handle")).toString();
    if (m_session.isEmpty()) {
        /* Some portal versions return the handle as an object path variant. */
        const QDBusObjectPath p =
            qvariant_cast<QDBusObjectPath>(results.value(QStringLiteral("session_handle")));
        m_session = p.path();
    }
    if (m_session.isEmpty()) {
        emit lost(tr("The desktop returned no shortcuts session."));
        return;
    }

    log_info("ptt: portal session %s", qPrintable(m_session));

    /* Per session path, so a Closed for an old session cannot be mistaken for
     * this one. Removed again in stop(). */
    QDBusConnection(QLatin1String(kConnName)).connect(
        QLatin1String(kService), m_session, QLatin1String(kSession), QStringLiteral("Closed"),
        this, SLOT(onSessionClosed(QVariantMap)));

    subscribeSignals();

    /* ASK BEFORE BINDING. BindShortcuts is a request to (re)configure, and
     * KDE's portal answers it by opening its shortcut editor every time;
     * ListShortcuts is the read-only counterpart. */
    listShortcuts();
}

QString PortalBackend::triggerFrom(const QVariantMap &results, bool *found)
{
    if (found) *found = false;
    const QDBusArgument arg = results.value(QStringLiteral("shortcuts")).value<QDBusArgument>();
    ShortcutList list;
    arg >> list;
    for (const auto &s : std::as_const(list)) {
        if (s.first == QLatin1String(kShortcutId)) {
            if (found) *found = true;
            return s.second.value(QStringLiteral("trigger_description")).toString();
        }
    }
    return QString();
}

void PortalBackend::subscribeSignals()
{
    if (m_subscribed)
        return;

    QDBusConnection bus = QDBusConnection(QLatin1String(kConnName));

    /* Activated / Deactivated are what make this a push-to-talk backend rather
     * than a plain shortcut backend: the release is the safety-critical edge. */
    const bool okAct = bus.connect(QString(), QString(), QLatin1String(kIface),
                QStringLiteral("Activated"),
                this, SLOT(onActivated(QDBusObjectPath, QString, qulonglong, QVariantMap)));
    const bool okDeact = bus.connect(QString(), QString(), QLatin1String(kIface),
                QStringLiteral("Deactivated"),
                this, SLOT(onDeactivated(QDBusObjectPath, QString, qulonglong, QVariantMap)));
    bus.connect(QString(), QString(), QLatin1String(kIface),
                QStringLiteral("ShortcutsChanged"),
                this, SLOT(onShortcutsChanged(QDBusObjectPath, ShortcutList)));

    if (!okAct || !okDeact) {
        log_err("ptt: could not subscribe to the portal's key signals "
                "(Activated=%d Deactivated=%d) — the shortcut will do nothing",
                okAct, okDeact);
        return;
    }

    m_subscribed = true;
    log_dbg("ptt: subscribed to portal key signals");
}

void PortalBackend::listShortcuts()
{
    QDBusConnection bus = QDBusConnection(QLatin1String(kConnName));

    const QString token = freshToken("svxl");
    bus.connect(QString(), requestPath(bus, token), QLatin1String(kRequest),
                QStringLiteral("Response"),
                this, SLOT(onListResponse(uint, QVariantMap)));

    QVariantMap opts;
    opts.insert(QStringLiteral("handle_token"), token);

    QDBusMessage call = QDBusMessage::createMethodCall(
        QLatin1String(kService), QLatin1String(kPath), QLatin1String(kIface),
        QStringLiteral("ListShortcuts"));
    call << QVariant::fromValue(QDBusObjectPath(m_session)) << opts;

    const quint64 gen = m_generation;
    whenAnswered(bus.asyncCall(call, kCallTimeoutMs), [this, gen](const QDBusMessage &r) {
        if (r.type() != QDBusMessage::ErrorMessage || gen != m_generation)
            return;
        log_warn("ptt: ListShortcuts failed (%s) — falling back to binding",
                 qPrintable(r.errorMessage()));
        bindShortcut();
    });
}

void PortalBackend::onListResponse(uint code, const QVariantMap &results)
{
    bool listed = false;
    const QString existing = (code == 0) ? triggerFrom(results, &listed) : QString();

    if (!existing.isEmpty()) {
        /* Already registered and assigned (KDE). Adopt it silently. */
        m_active = existing;
        log_info("ptt: portal shortcut already bound to %s", qPrintable(m_active));
        emit triggerChanged(m_active);
        return;
    }

    /* Hyprland keeps a registration for the life of the portal process but
     * never reports a trigger. Registering again is harmless there, and it is
     * what makes a restarted portal know about us, so always bind. */
    log_info(m_hyprland ? "ptt: registering %s with Hyprland"
                        : "ptt: no portal shortcut registered yet, requesting one (%s)",
             m_hyprland ? HyprlandBinding::kShortcutId : kShortcutId);
    Q_UNUSED(listed);
    bindShortcut();
}

void PortalBackend::bindShortcut()
{
    QDBusConnection bus = QDBusConnection(QLatin1String(kConnName));

    const QString bindToken = freshToken("svxb");
    bus.connect(QString(), requestPath(bus, bindToken), QLatin1String(kRequest),
                QStringLiteral("Response"),
                this, SLOT(onBindResponse(uint, QVariantMap)));

    QVariantMap desc;
    desc.insert(QStringLiteral("description"), tr("Push to talk"));
    desc.insert(QStringLiteral("preferred_trigger"), m_requested);

    ShortcutList shortcuts;
    shortcuts.append(qMakePair(QString::fromLatin1(kShortcutId), desc));

    QVariantMap opts;
    opts.insert(QStringLiteral("handle_token"), bindToken);

    QDBusMessage call = QDBusMessage::createMethodCall(
        QLatin1String(kService), QLatin1String(kPath), QLatin1String(kIface),
        QStringLiteral("BindShortcuts"));
    call << QVariant::fromValue(QDBusObjectPath(m_session))
         << QVariant::fromValue(shortcuts)
         << QString()          /* parent_window */
         << opts;

    whenAnswered(bus.asyncCall(call, kCallTimeoutMs), [](const QDBusMessage &r) {
        if (r.type() == QDBusMessage::ErrorMessage)
            log_err("ptt: BindShortcuts failed: %s", qPrintable(r.errorMessage()));
    });
}

void PortalBackend::onBindResponse(uint code, const QVariantMap &results)
{
    if (code != 0) {
        emit lost(tr("The desktop refused the push-to-talk shortcut."));
        return;
    }

    if (m_hyprland) {
        /* The key is whatever bindings.lua says, not anything in this reply. */
        const QString keys = HyprlandBinding::boundKeys();
        if (keys.isEmpty())
            log_info("ptt: registered %s; no key is bound to it in Hyprland yet",
                     HyprlandBinding::kShortcutId);
        else
            log_info("ptt: registered %s, bound to %s",
                     HyprlandBinding::kShortcutId, qPrintable(keys));
        emit triggerChanged(keys);
        return;
    }

    const QString bound = triggerFrom(results, nullptr);
    if (!acceptTrigger(bound))
        return;

    m_active = bound;
    log_info("ptt: portal bound %s", qPrintable(m_active));
    emit triggerChanged(m_active);
}

bool PortalBackend::acceptTrigger(const QString &bound)
{
    /* THE SAFETY CHECK, for portals that assign the key themselves.
     *
     * preferred_trigger is a hint and the portal may bind something else. The
     * dangerous case is losing the modifiers: CAPS+Return on Plasma 6.3.6
     * yields a bare "Return", and a bare Return as global push-to-talk keys the
     * transmitter every time the operator presses Enter anywhere. On Hyprland
     * the equivalent check is HyprlandBinding::isSafeKeys(), applied before the
     * bind is ever written. */
    if (bound.isEmpty()) {
        log_err("ptt: the desktop registered the shortcut but assigned no key "
                "(requested '%s')", qPrintable(m_requested));
        emit lost(tr("Your desktop registered SVXConnect's push-to-talk shortcut but "
                     "did not assign a key to it. Open your desktop's keyboard shortcut "
                     "settings, find SVXConnect, and assign one there."));
        return false;
    }

    const bool wantedModifier = m_requested.contains(QLatin1Char('+'));
    const bool gotModifier    = bound.contains(QLatin1Char('+'));

    if (wantedModifier && !gotModifier) {
        log_err("ptt: refusing '%s' — asked for '%s' but the desktop dropped the "
                "modifiers, which would key the transmitter on a bare key press",
                qPrintable(bound), qPrintable(m_requested));
        emit lost(tr("Your desktop reduced %1 to a single key (%2). SVXConnect will "
                     "not bind that: it would transmit every time you press %2 in "
                     "any application. Choose a different key combination.")
                      .arg(m_requested, bound));
        return false;
    }

    return true;
}

void PortalBackend::onActivated(const QDBusObjectPath &session, const QString &shortcutId,
                                qulonglong timestamp, const QVariantMap &)
{
    Q_UNUSED(timestamp);   /* its epoch is explicitly undefined by the spec */
    if (session.path() != m_session || shortcutId != QLatin1String(kShortcutId))
        return;

    /* Logged, so a key that arrives and is then refused by the core (no
     * talkgroup, no link, someone else talking) can be told apart from a key
     * that never arrived — a radio problem versus a binding problem. */
    log_info("ptt: key down");
    m_down = true;
    emit pressed();
}

void PortalBackend::onDeactivated(const QDBusObjectPath &session, const QString &shortcutId,
                                  qulonglong timestamp, const QVariantMap &)
{
    Q_UNUSED(timestamp);
    if (session.path() != m_session || shortcutId != QLatin1String(kShortcutId))
        return;

    log_info("ptt: key up");
    m_down = false;
    emit released();
}

void PortalBackend::onShortcutsChanged(const QDBusObjectPath &session, const ShortcutList &list)
{
    if (session.path() != m_session || m_hyprland)
        return;
    for (const auto &s : list) {
        if (s.first != QLatin1String(kShortcutId))
            continue;
        const QString bound = s.second.value(QStringLiteral("trigger_description")).toString();
        if (bound.isEmpty() || bound == m_active)
            continue;
        m_active = bound;
        log_info("ptt: the desktop reassigned push-to-talk to %s", qPrintable(m_active));
        emit triggerChanged(m_active);
    }
}

void PortalBackend::sessionLost(const QString &why)
{
    if (m_session.isEmpty())
        return;

    QDBusConnection(QLatin1String(kConnName)).disconnect(
        QLatin1String(kService), m_session, QLatin1String(kSession), QStringLiteral("Closed"),
        this, SLOT(onSessionClosed(QVariantMap)));
    m_session.clear();
    m_active.clear();

    if (m_down) {
        m_down = false;
        emit released();
    }
    emit lost(why);
}

void PortalBackend::onSessionClosed(const QVariantMap &)
{
    log_warn("ptt: the desktop closed the portal session %s", qPrintable(m_session));
    sessionLost(tr("The desktop closed the global-shortcut session."));
}

void PortalBackend::onPortalVanished()
{
    if (m_session.isEmpty())
        return;
    log_warn("ptt: the desktop portal went away");
    /* A new portal process knows nothing of us: register again next time. */
    m_registered = false;
    sessionLost(tr("The desktop portal stopped. Push-to-talk comes back by itself "
                   "when the portal does."));
}

void PortalBackend::onPortalAppeared()
{
    /* Only after a loss: a session we still hold means this is the first
     * registration of a portal we were already talking to. */
    if (!m_session.isEmpty() || m_requested.isEmpty())
        return;
    log_info("ptt: the desktop portal is back; registering push-to-talk again");
    start(PttBinding{m_requested});
}

void PortalBackend::stop()
{
    /* Answers to anything still in flight belong to the session being
     * stopped; the generation check makes them fall on the floor. */
    ++m_generation;

    /* A held key's release will never arrive from a session we are closing.
     * Release it here, so whoever owns the press never has to guess. */
    if (m_down) {
        m_down = false;
        emit released();
    }

    if (m_session.isEmpty())
        return;

    QDBusConnection bus = QDBusConnection(QLatin1String(kConnName));
    bus.disconnect(QLatin1String(kService), m_session, QLatin1String(kSession),
                   QStringLiteral("Closed"), this, SLOT(onSessionClosed(QVariantMap)));
    if (bus.isConnected()) {
        QDBusMessage close = QDBusMessage::createMethodCall(
            QLatin1String(kService), m_session,
            QStringLiteral("org.freedesktop.portal.Session"), QStringLiteral("Close"));
        bus.call(close, QDBus::NoBlock);
    }

    m_session.clear();
    m_active.clear();

    /* m_subscribed is deliberately NOT cleared. The D-Bus match rules belong to
     * the connection, not the session, and connecting the same rule twice
     * returns false. onActivated/onDeactivated match on the CURRENT session
     * handle, so events for a closed session are ignored. */
}
