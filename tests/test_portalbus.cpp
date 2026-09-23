/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * PortalBackend against a slow portal on a private bus.
 *
 * Every portal call used to be synchronous — Registry.Register,
 * CreateSession, ListShortcuts, BindShortcuts, and a QDBusInterface
 * introspection in front of each — on the GUI thread that also runs the core.
 * A portal slow to come up at login, or hung, held audio and the reflector
 * heartbeats for up to QtDBus's 25 s default per call.
 *
 * The fake portal here takes three seconds over every method. start() must
 * return at once; CreateSession must still only be sent once Register has
 * been ANSWERED (the ordering rule in portalbackend.h, now kept by chaining
 * rather than by blocking); and probe(), the one call that must answer on the
 * spot, must give up in well under a second on a portal that is busy.
 *
 * Skips (and passes) when dbus-daemon is not installed.
 */
#include <cstdio>
#include <mutex>

#include <QCoreApplication>
#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QTimer>
#include <QDBusObjectPath>
#include <QElapsedTimer>
#include <QProcess>
#include <QStringList>
#include <QThread>
#include <QVariantMap>

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

std::mutex  g_mx;
QStringList g_calls;

void record(const char *what)
{
    std::lock_guard<std::mutex> lk(g_mx);
    g_calls << QLatin1String(what);
}

QStringList calls()
{
    std::lock_guard<std::mutex> lk(g_mx);
    return g_calls;
}

void spinUntil(const std::function<bool()> &done, int ms)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

} // namespace

/* ---- the fake portal: one object, two interfaces ------------------------ */

class FakeRegistry : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.host.portal.Registry")

public:
    explicit FakeRegistry(QObject *parent) : QDBusAbstractAdaptor(parent) {}

public slots:
    /* Answers after three seconds WITHOUT blocking this thread, so a
     * CreateSession sent before Register was answered would arrive — and be
     * recorded — first. */
    void Register(const QString &, const QVariantMap &, const QDBusMessage &msg)
    {
        msg.setDelayedReply(true);
        const QDBusMessage reply = msg.createReply();
        QTimer::singleShot(3000, this, [reply]() {
            record("Register answered");
            QDBusConnection(QStringLiteral("fake-portal")).send(reply);
        });
    }
};

class FakeShortcuts : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.GlobalShortcuts")
    Q_PROPERTY(uint version READ version)

public:
    explicit FakeShortcuts(QObject *parent) : QDBusAbstractAdaptor(parent) {}
    uint version() const { return 1; }

public slots:
    QDBusObjectPath CreateSession(const QVariantMap &)
    {
        record("CreateSession");
        QThread::sleep(3);
        return QDBusObjectPath(QStringLiteral("/org/freedesktop/portal/desktop/request/x/y"));
    }
};

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
    qunsetenv("HYPRLAND_INSTANCE_SIGNATURE");   /* the KDE-style path; no hyprctl */

    QCoreApplication app(argc, argv);

    QThread portalThread;
    portalThread.start();
    QObject portalObject;
    new FakeRegistry(&portalObject);
    new FakeShortcuts(&portalObject);
    portalObject.moveToThread(&portalThread);

    bool registered = false;
    QMetaObject::invokeMethod(&portalObject, [&]() {
        QDBusConnection c = QDBusConnection::connectToBus(QString::fromLatin1(address),
                                                          QStringLiteral("fake-portal"));
        registered = c.registerService(QStringLiteral("org.freedesktop.portal.Desktop"))
                  && c.registerObject(QStringLiteral("/org/freedesktop/portal/desktop"),
                                      &portalObject, QDBusConnection::ExportAdaptors);
    }, Qt::BlockingQueuedConnection);
    check(registered, "fake portal on the private bus");

    {
        PortalBackend portal;

        QElapsedTimer t;
        t.start();
        const bool ok = portal.start(PttBinding{QStringLiteral("LOGO+Return")});
        const qint64 took = t.elapsed();
        std::printf("      start() took %lld ms\n", static_cast<long long>(took));
        check(ok, "start() accepted the binding");
        check(took < 200, "start() does not wait for the portal");

        spinUntil([]() { return calls().size() >= 2; }, 8000);
        const QStringList seen = calls();
        std::printf("      the portal saw: %s\n", qPrintable(seen.join(QStringLiteral(", "))));
        check(seen.size() >= 2 && seen.at(0) == QLatin1String("Register answered")
                               && seen.at(1) == QLatin1String("CreateSession"),
              "CreateSession was sent only after Register was answered");

        /* The portal is now stuck inside CreateSession for three seconds. */
        t.restart();
        const PttAvailability a = portal.probe();
        const qint64 probeTook = t.elapsed();
        std::printf("      probe() against a busy portal took %lld ms\n",
                    static_cast<long long>(probeTook));
        check(probeTook < 1000, "probe() gives up quickly on a portal that does not answer");
        Q_UNUSED(a);

        /* Let CreateSession's reply land, then stop cleanly. */
        spinUntil([]() { return false; }, 3500);
        portal.stop();
    }

    portalThread.quit();
    portalThread.wait();
    daemon.kill();
    daemon.waitForFinished(2000);

    std::printf("\n%d/%d passed\n", g_run - g_fail, g_run);
    return g_fail ? 1 : 0;
}

#include "test_portalbus.moc"
