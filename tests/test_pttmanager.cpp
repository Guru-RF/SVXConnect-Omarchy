/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * PttManager against a stub core and a scripted backend.
 *
 * The stub's app_ptt() behaves like ctl_ptt() in app.c: ON starts unless
 * refused, OFF stops, TOGGLE does whichever the core's OWN tx_active calls
 * for. That last point is the whole of the 0.1.13 Toggle-mode bug: the
 * manager kept a private latch, the core ended overs without telling it, and
 * from then on every global key press did the opposite of what was meant —
 * a swallowed press after "the connection dropped", or a transmitter left on
 * after an over started with Space.
 *
 * The PortalBackend checks at the end drive its session-loss paths directly
 * (a portal that closes the session or disappears never sends Deactivated),
 * with the session bus pointed nowhere so nothing touches the real desktop.
 */
#include <cstdio>
#include <vector>

#include <QCoreApplication>
#include <QDBusObjectPath>
#include <QMetaObject>
#include <QVariantMap>

#include "ptt/pttmanager.h"
#include "ptt/portalbackend.h"

namespace {

int g_fail;
int g_run;

void check(bool ok, const char *what)
{
    ++g_run;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

} // namespace

/* ---- the stub core ----------------------------------------------------- */

struct svx_app {
    int  tx     = 0;
    bool refuse = false;              /* "PTT refused: X is talking" */
    std::vector<ctl_tristate> sent;
};

extern "C" {

void app_ptt(svx_app *a, ctl_tristate v)
{
    a->sent.push_back(v);
    const bool start = v == CTL_ON || (v == CTL_TOGGLE && !a->tx);
    if (start) {
        if (!a->refuse)
            a->tx = 1;
    } else {
        a->tx = 0;
    }
}

int  app_tx_active(const svx_app *a) { return a->tx; }
void app_toggle_connect(svx_app *)   {}
void app_reconnect(svx_app *)        {}

} // extern "C"

/* ---- a backend the test presses by hand -------------------------------- */

class FakeBackend : public PttBackend {
public:
    QString id() const override          { return QStringLiteral("portal"); }
    QString displayName() const override { return QStringLiteral("fake"); }
    PttAvailability probe() const override { return {}; }

    bool start(const PttBinding &) override { ++starts; running = true; return true; }
    void stop() override                    { ++stops; running = false; }
    bool isActive() const override          { return running; }

    void press()   { emit pressed(); }
    void release() { emit released(); }
    void lose()    { emit lost(QStringLiteral("gone")); }

    int  starts  = 0;
    int  stops   = 0;
    bool running = false;
};

namespace {

/* The portal's replies, delivered the way QtDBus would deliver them. */
void openSession(PortalBackend *portal, const QString &session)
{
    QVariantMap results;
    results.insert(QStringLiteral("session_handle"), session);
    QMetaObject::invokeMethod(portal, "onCreateSessionResponse",
                              Q_ARG(uint, 0u), Q_ARG(QVariantMap, results));
}

void keyDown(PortalBackend *portal, const QString &session)
{
    QMetaObject::invokeMethod(portal, "onActivated",
                              Q_ARG(QDBusObjectPath, QDBusObjectPath(session)),
                              Q_ARG(QString, QStringLiteral("ptt")),
                              Q_ARG(qulonglong, 0ull),
                              Q_ARG(QVariantMap, QVariantMap()));
}

struct Rig {
    svx_app      core;
    FakeBackend *key = new FakeBackend;
    PttManager   mgr{&core, QVector<PttBackend *>{key}};
};

} // namespace

int main(int argc, char **argv)
{
    /* PortalBackend opens nothing until start(), but make sure. */
    qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/svx-test-bus");
    QCoreApplication app(argc, argv);

    /* ---------------------------------------------------------- Toggle */
    {
        Rig r;
        r.mgr.setMode(PttManager::Mode::Toggle);

        r.key->press();
        check(r.core.tx == 1, "toggle: a press keys");

        /* The core ends the over by itself — "the connection dropped". */
        r.core.tx = 0;
        check(!r.mgr.isKeyed(), "toggle: isKeyed() follows the core, not a latch");

        r.core.sent.clear();
        r.key->press();
        check(r.core.tx == 1, "toggle: the next press keys again instead of being swallowed");
        check(!r.core.sent.empty() && r.core.sent.back() != CTL_OFF,
              "toggle: ...and what was sent was not OFF");
    }
    {
        Rig r;
        r.mgr.setMode(PttManager::Mode::Toggle);

        /* Keyed with Space (or the mouse, the tray, the FIFO): not via us. */
        app_ptt(&r.core, CTL_TOGGLE);
        check(r.core.tx == 1, "toggle: keyed from elsewhere");

        r.key->press();
        check(r.core.tx == 0, "toggle: the global key then STOPS the transmitter");
    }
    {
        Rig r;
        r.mgr.setMode(PttManager::Mode::Toggle);

        r.core.refuse = true;           /* somebody else is talking */
        r.key->press();
        check(r.core.tx == 0, "toggle: a refused press leaves the transmitter off");

        r.core.refuse = false;
        r.key->press();
        check(r.core.tx == 1, "toggle: the press after a refusal keys (no latch to undo)");
    }

    /* ------------------------------------------------------------ Hold */
    {
        Rig r;
        r.key->press();
        check(r.core.tx == 1, "hold: press keys");
        r.key->release();
        check(r.core.tx == 0, "hold: release un-keys");
    }
    {
        Rig r;
        r.key->press();
        r.mgr.applyKeyboardBinding(PttBinding{});
        check(r.core.tx == 0, "hold: removing the binding while held un-keys");
        check(r.key->stops == 1, "hold: ...and stops the backend");
    }
    {
        Rig r;
        r.key->press();
        r.mgr.applyKeyboardBinding(PttBinding{QStringLiteral("LOGO+Return")});
        check(r.key->starts == 1, "hold: an inactive backend is (re)started");
        check(r.core.tx == 0, "hold: restarting the binding while held un-keys first");
    }
    {
        Rig r;
        r.key->running = true;          /* a live session, left alone */
        r.key->press();
        r.mgr.applyKeyboardBinding(PttBinding{QStringLiteral("LOGO+Return")});
        check(r.key->starts == 0 && r.core.tx == 1,
              "hold: an unchanged, live binding is not touched and does not un-key");
    }
    {
        Rig r;
        r.key->press();
        r.mgr.setMode(PttManager::Mode::Toggle);
        check(r.core.tx == 0, "hold -> toggle while held un-keys");
        r.key->release();               /* ignored in toggle; must not re-key */
        check(r.core.tx == 0, "...and the late release changes nothing");

        r.key->press();
        check(r.core.tx == 1, "toggle after the switch works normally");
        r.mgr.setMode(PttManager::Mode::Toggle);
        check(r.core.tx == 1, "setting the same mode again does not un-key");
    }
    {
        Rig r;
        r.key->press();
        r.key->lose();
        check(r.core.tx == 0, "hold: a lost backend un-keys");
        r.key->release();
        r.key->press();
        check(r.core.tx == 1, "hold: the counter was reset, the next press keys");
    }
    {
        /* The core stops by itself mid-hold (transmit timeout); the release
         * must not leave the counter wrong for the next press. */
        Rig r;
        r.key->press();
        r.core.tx = 0;
        r.key->release();
        r.key->press();
        check(r.core.tx == 1, "hold: after a core-side stop, the next press keys");
    }

    /* ---------------------------------------------- PortalBackend loss */
    {
        svx_app core;
        auto *portal = new PortalBackend;
        PttManager mgr(&core, QVector<PttBackend *>{portal});
        bool lost = false;
        QObject::connect(&mgr, &PttManager::backendLost, [&lost]() { lost = true; });

        const QString session = QStringLiteral("/org/freedesktop/portal/desktop/session/1_1/t");
        openSession(portal, session);

        keyDown(portal, session);
        check(core.tx == 1, "portal: Activated keys");

        QMetaObject::invokeMethod(portal, "onSessionClosed", Q_ARG(QVariantMap, QVariantMap()));
        check(core.tx == 0, "portal: Session::Closed while held un-keys");
        check(lost, "portal: ...and reports the backend lost");
    }
    {
        svx_app core;
        auto *portal = new PortalBackend;
        PttManager mgr(&core, QVector<PttBackend *>{portal});

        const QString session = QStringLiteral("/org/freedesktop/portal/desktop/session/1_1/u");
        openSession(portal, session);
        keyDown(portal, session);
        QMetaObject::invokeMethod(portal, "onPortalVanished");
        check(core.tx == 0, "portal: the portal vanishing while held un-keys");
    }
    {
        svx_app core;
        auto *portal = new PortalBackend;
        PttManager mgr(&core, QVector<PttBackend *>{portal});

        const QString session = QStringLiteral("/org/freedesktop/portal/desktop/session/1_1/v");
        openSession(portal, session);
        keyDown(portal, session);
        portal->stop();
        check(core.tx == 0, "portal: stop() while held releases the key");
    }

    std::printf("\n%d/%d passed\n", g_run - g_fail, g_run);
    return g_fail ? 1 : 0;
}
