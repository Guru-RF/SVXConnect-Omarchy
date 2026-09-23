/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * Global PTT via org.freedesktop.portal.GlobalShortcuts.
 *
 * Works on Wayland and on X11, needs no elevated permission and no group
 * membership, and — the part that matters — emits Deactivated as well as
 * Activated, so it can drive a real hold-to-talk rather than a toggle.
 *
 * TWO KINDS OF PORTAL
 * -------------------
 * xdg-desktop-portal-kde assigns the key itself (from preferred_trigger, or
 * from the user in System Settings) and reports it back in
 * trigger_description. xdg-desktop-portal-hyprland, which is what Omarchy runs,
 * only registers the shortcut: it reports no trigger at all, and nothing fires
 * until a Hyprland bind names it with the `global` dispatcher. See
 * ptt/hyprlandbinding.h. So an empty trigger is an error on KDE and the normal
 * state of a fresh install on Hyprland, and the code below distinguishes them.
 *
 * Three ordering rules, each of which fails opaquely when broken:
 *
 *   1. Registry.Register must be the FIRST portal call on its connection, so
 *      this uses its own QDBusConnection — Qt already talks to the Settings
 *      portal during QGuiApplication construction.
 *   2. The Request::Response subscription must exist BEFORE the call that
 *      generates it, or the reply races the subscription.
 *   3. BindShortcuts may be called once per session. Rebinding means closing
 *      the session and making a new one.
 */
#ifndef SVXCONNECT_OMARCHY_PORTALBACKEND_H
#define SVXCONNECT_OMARCHY_PORTALBACKEND_H

#include "ptt/pttbackend.h"

#include <QDBusObjectPath>
#include <QVariantMap>
#include <QList>
#include <QPair>
#include <QDir>
#include <QMetaType>

#include <functional>

/* The portal's a(sa{sv}) — an array of (shortcut id, properties) structs.
 *
 * QtDBus cannot marshal this on its own: it has no idea that the pair is a
 * D-Bus STRUCT rather than two separate arguments, so BindShortcuts fails at
 * marshalling time with "Unregistered type". The stream operators below supply
 * that, and qDBusRegisterMetaType() must be called once before the first call
 * that uses them. */
using Shortcut     = QPair<QString, QVariantMap>;
using ShortcutList = QList<Shortcut>;

Q_DECLARE_METATYPE(Shortcut)
Q_DECLARE_METATYPE(ShortcutList)

class QDBusArgument;
QDBusArgument &operator<<(QDBusArgument &arg, const Shortcut &s);
const QDBusArgument &operator>>(const QDBusArgument &arg, Shortcut &s);

class QDBusServiceWatcher;
class QDBusPendingCall;
class QDBusMessage;

class PortalBackend : public PttBackend {
    Q_OBJECT

public:
    explicit PortalBackend(QObject *parent = nullptr);
    ~PortalBackend() override;

    QString id() const override          { return QStringLiteral("portal"); }
    QString displayName() const override { return tr("Desktop portal"); }

    PttAvailability probe() const override;
    bool start(const PttBinding &binding) override;
    void stop() override;

    /* On KDE, what the desktop bound. On Hyprland, the key a bind in the
     * user's config names — which can change whenever they edit it, so it is
     * read fresh rather than remembered. */
    QString activeTrigger() const override;

    /* A live session with our shortcut registered. On Hyprland that is enough
     * to be "active" even with no key bound yet: binding a key is a config
     * edit, not a portal call, and needs no new session. */
    bool isActive() const override;

    /* Is an appropriately-named .desktop installed? Registry.Register needs
     * one and fails opaquely without it. */
    static bool hasDesktopFile();

private slots:
    void onCreateSessionResponse(uint code, const QVariantMap &results);
    void onListResponse(uint code, const QVariantMap &results);
    void onBindResponse(uint code, const QVariantMap &results);
    void onActivated(const QDBusObjectPath &session, const QString &shortcutId,
                     qulonglong timestamp, const QVariantMap &options);
    void onDeactivated(const QDBusObjectPath &session, const QString &shortcutId,
                       qulonglong timestamp, const QVariantMap &options);
    void onShortcutsChanged(const QDBusObjectPath &session, const ShortcutList &shortcuts);

    /* The two ways a session dies without a Deactivated: the desktop closes it
     * (org.freedesktop.portal.Session::Closed), or xdg-desktop-portal itself
     * exits or restarts and takes every session with it. Either is lost(). */
    void onSessionClosed(const QVariantMap &details);
    void onPortalVanished();
    void onPortalAppeared();

private:
    /* Subscribe to Activated / Deactivated / ShortcutsChanged. MUST run on
     * every start, on BOTH paths — adopting an existing registration and
     * creating a new one — or the shortcut fires into nothing. */
    void subscribeSignals();

    void createSession();
    void listShortcuts();
    void bindShortcut();

    /* Run `then` with the reply once it arrives, on this thread. */
    void whenAnswered(const QDBusPendingCall &call,
                      std::function<void(const QDBusMessage &)> then);

    /* Our entry in a portal a(sa{sv}) reply. `found` says whether it was
     * listed at all; the returned trigger may still be empty. */
    static QString triggerFrom(const QVariantMap &results, bool *found);

    /* Reject a binding that lost its modifiers (KDE: CAPS+Return comes back as
     * a bare "Return", which would transmit on every Enter keypress). */
    bool acceptTrigger(const QString &bound);

    /* The session is gone and will deliver nothing more: release a held key,
     * forget the session, and say so. */
    void sessionLost(const QString &why);

    bool    m_conn = false;
    bool    m_registered = false;
    bool    m_subscribed = false;
    bool    m_hyprland = false;
    bool    m_down = false;        /* Activated seen, Deactivated not yet */
    quint64 m_generation = 0;      /* bumped by stop(); stale replies compare */
    QString m_session;
    QString m_requested;
    QString m_active;

    QDBusServiceWatcher *m_watcher = nullptr;
};

#endif
