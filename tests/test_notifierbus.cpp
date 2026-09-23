/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * The talker notification must not block the GUI thread.
 *
 * The GUI thread is also the thread that runs the core, so a blocking D-Bus
 * call there stops audio in both directions and the reflector heartbeats for
 * as long as the other end takes. 0.1.13 called Notify synchronously — up to
 * QtDBus's 25 s default — on every new talker while the window was in the
 * tray. This starts a private bus, puts a notification daemon on it that takes
 * three seconds to answer, announces a talker, and times the call.
 *
 * Skips (and passes) when dbus-daemon is not installed.
 */
#include <atomic>
#include <cstdio>
#include <cstring>

#include <QCoreApplication>
#include <QDBusConnection>
#include <QElapsedTimer>
#include <QProcess>
#include <QThread>
#include <QVariantMap>

#include "ui/notifier.h"

namespace {

int g_fail;
int g_run;

void check(bool ok, const char *what)
{
    ++g_run;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

std::atomic<int> g_notifies{0};

} // namespace

/* A notification daemon on a hung shell. */
class SlowNotifications : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")

public slots:
    uint Notify(const QString &, uint, const QString &, const QString &, const QString &,
                const QStringList &, const QVariantMap &, int)
    {
        ++g_notifies;
        QThread::sleep(3);
        return 42;
    }
};

/* ---- the stub core: one talker, then another ---------------------------- */

struct svx_app {
    svx_config cfg{};
    tg_manager tgm{};
};

extern "C" {
tg_manager       *app_tgm(svx_app *a)          { return &a->tgm; }
const svx_config *app_config(const svx_app *a) { return &a->cfg; }
}

int main(int argc, char **argv)
{
    QProcess daemon;
    daemon.start(QStringLiteral("dbus-daemon"),
                 {QStringLiteral("--session"), QStringLiteral("--nofork"),
                  QStringLiteral("--print-address=1")});
    if (!daemon.waitForStarted(3000) || !daemon.waitForReadyRead(3000)) {
        std::printf("skip  dbus-daemon is not available\n");
        return 0;
    }
    const QByteArray address = daemon.readLine().trimmed();
    qputenv("DBUS_SESSION_BUS_ADDRESS", address);

    QCoreApplication app(argc, argv);

    /* The daemon lives on its own thread and its own connection, so its
     * three-second sleep blocks only itself. */
    QThread serviceThread;
    serviceThread.start();
    SlowNotifications service;
    service.moveToThread(&serviceThread);
    bool registered = false;
    QMetaObject::invokeMethod(&service, [&]() {
        QDBusConnection c = QDBusConnection::connectToBus(QString::fromLatin1(address),
                                                          QStringLiteral("fake-daemon"));
        registered = c.registerService(QStringLiteral("org.freedesktop.Notifications"))
                  && c.registerObject(QStringLiteral("/org/freedesktop/Notifications"), &service,
                                      QDBusConnection::ExportAllSlots);
    }, Qt::BlockingQueuedConnection);
    check(registered, "fake notification daemon on the private bus");

    svx_app core;
    std::strcpy(core.cfg.callsign, "ON6URE");

    Notifier n(&core);
    n.setEnabled(true);
    n.tickModel(true);                          /* learns: nobody talking */

    std::strcpy(core.tgm.active[0].call, "ON4ABC");
    std::strcpy(core.tgm.active[0].full, "ON4ABC-M");
    core.tgm.active[0].tg = 8;
    core.tgm.n_active = 1;

    QElapsedTimer t;
    t.start();
    n.tickModel(true);                          /* ON4ABC starts: announce */
    const qint64 took = t.elapsed();
    std::printf("      tickModel() with a new talker took %lld ms\n", static_cast<long long>(took));
    check(took < 100, "announcing a talker does not wait for the notification daemon");

    /* And the call really went out: the daemon sees it. */
    QElapsedTimer w;
    w.start();
    while (g_notifies.load() == 0 && w.elapsed() < 2000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    check(g_notifies.load() == 1, "the notification was delivered");

    /* Let the reply land before tearing down, so nothing is left in flight. */
    w.restart();
    while (w.elapsed() < 3500)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    serviceThread.quit();
    serviceThread.wait();
    daemon.kill();
    daemon.waitForFinished(2000);

    std::printf("\n%d/%d passed\n", g_run - g_fail, g_run);
    return g_fail ? 1 : 0;
}

#include "test_notifierbus.moc"
