/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * CoreLoop against a stub core whose "sockets" are socketpairs the test owns.
 *
 * The case that matters is the one 0.1.13's journal shows at every
 * "disconnected by you": the interface closes the core's sockets between
 * service passes, under notifiers that are still enabled. Qt then reports
 *
 *     QSocketNotifier: Invalid socket 26 and type 'Read', disabling...
 *
 * and, worse, the freed fd number can be handed to the next socket anybody
 * opens while a stale notifier still claims it. The first check below makes
 * the call directly, to prove this harness reproduces the warning at all; the
 * second makes it through CoreAction, which must leave Qt nothing to say.
 */
#include <cstdio>
#include <vector>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSocketNotifier>
#include <QString>

#include <sys/socket.h>
#include <unistd.h>

#include "core/coreaction.h"
#include "core/coreloop.h"

namespace {

int g_fail;
int g_run;

void check(bool ok, const char *what)
{
    ++g_run;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

QStringList g_warnings;

void handler(QtMsgType, const QMessageLogContext &, const QString &msg)
{
    if (msg.contains(QLatin1String("QSocketNotifier")))
        g_warnings << msg;
}

void spin(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

} // namespace

/* ---- the stub core ----------------------------------------------------- */

struct svx_app {
    std::vector<int> fds;      /* what app_poll_fds() reports       */
    int              peer[2] = {-1, -1};
    int              services = 0;
};

namespace {

/* A "connection": one end watched by the loop, the other kept so the watched
 * end has something to be readable from. */
void openConnection(svx_app *a)
{
    int sv[2];
    ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    a->fds = {sv[0]};
    a->peer[0] = sv[0];
    a->peer[1] = sv[1];
}

/* What rc_stop() does: close both descriptors, synchronously. */
void dropConnection(svx_app *a)
{
    ::close(a->peer[0]);
    ::close(a->peer[1]);
    a->peer[0] = a->peer[1] = -1;
    a->fds.clear();
}

} // namespace

extern "C" {

int app_poll_fds(svx_app *a, struct pollfd *p, int max)
{
    int n = 0;
    for (int fd : a->fds)
        if (n < max)
            p[n++] = pollfd{fd, POLLIN, 0};
    return n;
}

int  app_next_timeout_ms(svx_app *, uint64_t) { return 20; }
void app_service(svx_app *a, uint64_t)        { a->services++; }
int  app_should_quit(const svx_app *)         { return 0; }
void app_set_observer(svx_app *, void (*)(void *), void *) {}

/* The "disconnect" the tray and the connection bar make. */
void app_toggle_connect(svx_app *a) { dropConnection(a); }

/* A reconnect: close, then open a fresh connection straight away — which the
 * kernel gives the lowest free numbers, i.e. the ones just closed. */
void app_reconnect(svx_app *a)
{
    dropConnection(a);
    openConnection(a);
}

void app_ptt(svx_app *, ctl_tristate) {}

} // extern "C"

int main(int argc, char **argv)
{
    QCoreApplication qapp(argc, argv);
    qInstallMessageHandler(handler);

    svx_app app;
    openConnection(&app);

    {
        CoreLoop loop(&app);
        loop.kick();
        spin(60);

        /* 1. The harness reproduces the bug: a close made directly, with the
         *    notifier enabled, is what Qt complains about. */
        g_warnings.clear();
        app_toggle_connect(&app);
        spin(100);
        check(!g_warnings.isEmpty(),
              "a direct close under an enabled notifier reproduces the warning");

        /* 2. Through CoreAction: nothing enabled on the descriptor while it is
         *    closed, so Qt has nothing to report. */
        openConnection(&app);
        loop.kick();
        spin(60);

        g_warnings.clear();
        CoreAction::toggleConnect(&app);
        spin(100);
        check(g_warnings.isEmpty(), "CoreAction::toggleConnect leaves no QSocketNotifier warning");
        for (const QString &w : std::as_const(g_warnings))
            std::printf("      %s\n", qPrintable(w));

        /* 3. Reconnect: the replacement socket takes the freed number before
         *    the next service pass. It must be watched, and only once. */
        openConnection(&app);
        loop.kick();
        spin(60);

        g_warnings.clear();
        const int before = app.peer[0];
        CoreAction::reconnect(&app);
        check(app.peer[0] == before, "the reconnect reused the closed descriptor's number");

        const int servicesBefore = app.services;
        ::write(app.peer[1], "x", 1);
        spin(10);
        check(app.services > servicesBefore,
              "the reused descriptor is watched at once, not only after the next timer");
        spin(100);
        check(g_warnings.isEmpty(), "reconnect through CoreAction leaves no warning");
        for (const QString &w : std::as_const(g_warnings))
            std::printf("      %s\n", qPrintable(w));

        /* 4. Pruning is by age, not by pass count. The disconnected fd numbers
         *    from above are unwanted now; with a short age they go. */
        dropConnection(&app);
        loop.kick();
        const int notifiersBefore = int(loop.findChildren<QSocketNotifier *>().size());
        loop.setPruneAfterMs(50);
        spin(80);
        QMetaObject::invokeMethod(&loop, "pruneStaleNotifiers");
        const int notifiersAfter = int(loop.findChildren<QSocketNotifier *>().size());
        check(notifiersBefore > 0 && notifiersAfter == 0,
              "notifiers unwanted for longer than the prune age are deleted");

        /* And not before: a 5-minute age at the transmit cadence keeps them. */
        openConnection(&app);
        loop.kick();
        dropConnection(&app);
        loop.kick();
        loop.setPruneAfterMs(5 * 60 * 1000);
        spin(300);
        QMetaObject::invokeMethod(&loop, "pruneStaleNotifiers");
        check(!loop.findChildren<QSocketNotifier *>().isEmpty(),
              "a recently wanted descriptor is not pruned");
    }

    /* The guard goes with the loop: a stray call after it is harmless. */
    openConnection(&app);
    CoreAction::toggleConnect(&app);
    check(app.fds.empty(), "with no loop, CoreAction simply makes the call");

    std::printf("\n%d/%d passed\n", g_run - g_fail, g_run);
    return g_fail ? 1 : 0;
}
