/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * Startup, in an order where nearly every step is load-bearing.
 */
#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QSocketNotifier>
#include <QDir>
#include <QDebug>
#include <QIcon>
#include <QProcess>
#include <QTabWidget>
#include <QTimer>

#include <clocale>
#include <cstring>
#include <cstdio>
#include <csignal>
#include <unistd.h>

#include "core/svxcore.h"
#include "core/coreloop.h"
#include "core/logbridge.h"
#include "core/runlock.h"
#include "ui/mainwindow.h"
#include "ui/omarchytheme.h"
#include "ui/preferencesdialog.h"
#include "ui/locationdialog.h"
#include "settings/configstore.h"

namespace {

/* The one config instance.
 *
 * app_new() does NOT copy this: app.c stores the pointer, and app_set_volume(),
 * app_set_input_device() and app_set_output_device() all mutate it in place
 * behind your back. It must therefore outlive the svx_app. Namespace scope
 * makes that unarguable — a local in a helper function would be a
 * use-after-free that only shows up when someone drags the volume slider. */
svx_config g_cfg;

/* SIGINT/SIGTERM handling.
 *
 * Quitting through the Qt event loop matters: _exit() from a handler would
 * skip app_free(), leaving a stale status file that makes a panel widget show
 * a client that is not running, and a held run lock that makes the CLI refuse
 * to start until the file is manually removed. A self-pipe is the standard way
 * to get from an async-signal context onto the event loop safely. */
int g_sigPipe[2] = {-1, -1};

extern "C" void onFatalSignal(int sig)
{
    const char b = static_cast<char>(sig);
    ssize_t rc = ::write(g_sigPipe[1], &b, 1);
    (void)rc;   /* nothing useful to do if this fails inside a handler */
}

void installSignalHandling(QObject *ctx)
{
    if (::pipe(g_sigPipe) != 0)
        return;

    auto *n = new QSocketNotifier(g_sigPipe[0], QSocketNotifier::Read, ctx);
    QObject::connect(n, &QSocketNotifier::activated, ctx, [n]() {
        char b = 0;
        ssize_t rc = ::read(g_sigPipe[0], &b, 1);
        (void)rc;
        n->setEnabled(false);
        log_info("caught signal %d, shutting down", int(b));
        QCoreApplication::quit();
    });

    std::signal(SIGINT,  onFatalSignal);
    std::signal(SIGTERM, onFatalSignal);
}

/* LC_NUMERIC must be "C" for the whole process lifetime.
 *
 * nodeinfo_build_json() formats latitude and longitude with snprintf("%.7f")
 * (nodeinfo.c:42-43). Under a comma locale — nl_BE, fr_FR, de_DE — that
 * produces 51,050000 instead of 51.0500000.
 *
 * The failure is quieter than it looks, and worth stating precisely because
 * the obvious guess is wrong. The coordinate is embedded as a JSON *string*,
 * not a bare number:
 *
 *     "pos":{"lat":"%s","long":"%s","loc":"%s"}      (nodeinfo.c:55-57)
 *
 * so "51,050000" is still perfectly valid JSON. Nothing is rejected, no parse
 * error occurs, and MsgNodeInfo is delivered intact. What happens instead is
 * that the reflector and its portal cannot interpret the value as a number, so
 * your node is mis-plotted or not plotted at all — with no error logged on
 * either side, and nothing in the client to suggest anything went wrong.
 * (The CLI's own comment at nodeinfo.c:40-41 says a comma would "take the
 * whole document down"; it would not. Same conclusion, wrong mechanism.)
 *
 * Setting it before QApplication is NOT sufficient, which is the part that
 * catches people: QGuiApplication loads a platform theme plugin, and the GTK3
 * theme (used on GNOME sessions) calls gtk_init(), which calls
 * setlocale(LC_ALL, ""). So it must be re-asserted afterwards, and verified.
 * Qt's own number formatting is locale-aware through QLocale and is unaffected
 * by this. */
void enforceCNumeric(const char *when)
{
    std::setlocale(LC_NUMERIC, "C");

    char probe[32];
    std::snprintf(probe, sizeof probe, "%.7f", 51.05);
    if (std::strchr(probe, ',')) {
        log_err("LC_NUMERIC is not C after %s (%.7f formats as \"%s\") — "
                "node position would be rejected by the reflector", when, 51.05, probe);
    }
}

/* Automatic station position: follow Omarchy's weather location.
 *
 * Preferences applies it when it is opened, but that is not enough on its own —
 * someone who runs omarchy-weather-location and then starts the app would be
 * published at the old position until they happened to open a dialog. So the
 * check happens at startup as well, and writes the result to svxconnect.conf,
 * which keeps the CLI in step too.
 *
 * Runs before app_new(): the live config can still be changed here without the
 * talkgroup manager holding indices into it. */
void syncAutoPosition(svx_config *cfg, const char *confPath)
{
    if (!LocationDialog::autoModeSetting())
        return;

    const LocationDialog::Place p = LocationDialog::omarchyPlace();
    if (!p.isValid()) {
        log_warn("automatic position is on, but Omarchy has no location stored "
                 "(run omarchy-weather-location) — keeping the configured one");
        return;
    }

    const QString lat = QString::number(p.latitude,  'f', 7);
    const QString lon = QString::number(p.longitude, 'f', 7);

    ConfigStore store(cfg, QString::fromUtf8(confPath));
    if (!p.name.isEmpty())
        store.set(QStringLiteral("location"), p.name);
    store.set(QStringLiteral("latitude"),  lat);
    store.set(QStringLiteral("longitude"), lon);

    if (store.isDirty()) {
        QString err;
        if (!store.save(&err)) {
            log_warn("could not write the Omarchy position to %s: %s",
                     confPath, qPrintable(err));
            return;
        }
        /* The file is right; the struct was loaded before it changed. */
        if (!p.name.isEmpty())
            config_set(cfg, "location", qPrintable(p.name));
        config_set(cfg, "latitude",  qPrintable(lat));
        config_set(cfg, "longitude", qPrintable(lon));

        log_info("position follows Omarchy: %s %s, %s",
                 p.name.isEmpty() ? "(unnamed)" : qPrintable(p.name),
                 qPrintable(lat), qPrintable(lon));
    }
}

/* SVX_SCREENSHOT=<file.png> renders the window to a file and quits.
 *
 * With QT_QPA_PLATFORM=offscreen that needs no compositor, which makes it the
 * way to check a theme (see SVX_OMARCHY_THEME_DIR) and to make documentation
 * screenshots. SVX_SCREENSHOT_SIZE=<w>x<h> sizes the window first, which is
 * how the header row's behaviour at narrow widths is checked.
 * SVX_SCREENSHOT_PREFS=<tab index> also renders the preferences dialog on that
 * tab, as <file>-prefs.png, and SVX_SCREENSHOT_LOCATION=1 the position lookup,
 * as <file>-location.png. SVX_SCREENSHOT_DELAY=<ms> waits longer before
 * grabbing, which the map needs. */
void scheduleScreenshot(QApplication &app, MainWindow &w, const QString &confFile)
{
    const QString shot = qEnvironmentVariable("SVX_SCREENSHOT");
    if (shot.isEmpty())
        return;

    const QString size = qEnvironmentVariable("SVX_SCREENSHOT_SIZE");
    const qsizetype x = size.indexOf(QLatin1Char('x'));
    if (x > 0)
        w.resize(size.left(x).toInt(), size.mid(x + 1).toInt());

    /* 900 ms is enough for a window that draws itself. The map is not that:
     * it waits for a WebSocket snapshot and then for tiles over the network,
     * so a run that wants the map in the picture asks for longer. */
    const int delay = qEnvironmentVariableIntValue("SVX_SCREENSHOT_DELAY");
    QTimer::singleShot(delay > 0 ? delay : 900, &app, [&w, &app, shot, confFile]() {
        w.grab().save(shot);
        bool hasTab = false;
        const int tab = qEnvironmentVariableIntValue("SVX_SCREENSHOT_PREFS", &hasTab);
        if (hasTab) {
            PreferencesDialog dlg(nullptr, confFile, &w);
            if (auto *tabs = dlg.findChild<QTabWidget *>())
                tabs->setCurrentIndex(tab);
            dlg.show();
            QCoreApplication::processEvents();
            QString prefsShot = shot;
            prefsShot.replace(QStringLiteral(".png"), QStringLiteral("-prefs.png"));
            dlg.grab().save(prefsShot);
        }
        if (qEnvironmentVariableIntValue("SVX_SCREENSHOT_LOCATION") > 0) {
            LocationDialog dlg(&w);
            dlg.show();
            QCoreApplication::processEvents();
            QString locShot = shot;
            locShot.replace(QStringLiteral(".png"), QStringLiteral("-location.png"));
            dlg.grab().save(locShot);
        }
        app.quit();
    });
}

} // namespace

int main(int argc, char **argv)
{
    /* 1. Locale and SIGPIPE, before anything else can format a number or
     *    write to a socket whose peer went away. */
    std::setlocale(LC_ALL, "");
    enforceCNumeric("startup");
    std::signal(SIGPIPE, SIG_IGN);

    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SVXConnect"));
    QCoreApplication::setApplicationName(QStringLiteral("SVXConnect"));
    QCoreApplication::setApplicationVersion(QString::fromLatin1(SVXCONNECT_VERSION));
    QGuiApplication::setDesktopFileName(QStringLiteral("SVXConnect"));

    /* The Omarchy theme — palette, font, rounding and control tokens — applied
     * to the whole application and followed live. Before any window or dialog
     * exists, including the "already running" message box below. */
    OmarchyTheme::get().install();

    /* The window icon.
     *
     * setDesktopFileName() above is what lets a Wayland compositor match the
     * window to its .desktop entry and find the installed hicolor icon, and on
     * a proper install that is enough. It is NOT enough when running from a
     * build tree, or on a desktop that does not do that lookup, so the icon is
     * also compiled in and set explicitly.
     *
     * Several sizes rather than one: Qt picks the closest and scales, and a
     * 16 px title-bar icon downscaled from 256 px looks noticeably worse than
     * the purpose-made 16 px version. */
    QIcon icon;
    for (int size : {16, 22, 24, 32, 48, 64, 128, 256, 512, 1024})
        icon.addFile(QStringLiteral(":/icons/app-%1.png").arg(size), QSize(size, size));
    if (!icon.isNull())
        QGuiApplication::setWindowIcon(icon);

    /* 2. Re-assert. See enforceCNumeric(). */
    enforceCNumeric("QApplication construction");

    /* 3. Arguments are parsed BEFORE the run lock, so that a forwarded
     *    svxconnect:// URL from a second launch is not thrown away on the
     *    "already running" branch. (Forwarding itself is M5; parsing here is
     *    what makes adding it a small change rather than a restructure.) */
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QCoreApplication::translate("main", "SvxLink reflector client for Omarchy"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption confOpt({QStringLiteral("c"), QStringLiteral("config")},
        QCoreApplication::translate("main", "Configuration file to use."),
        QStringLiteral("file"));
    parser.addOption(confOpt);
    parser.addPositionalArgument(QStringLiteral("url"),
        QCoreApplication::translate("main", "An svxconnect:// URL to act on."));
    parser.process(app);

    /* 4. Logging, before app_new(), so the connect worker never races the
     *    sink install. */
    LogBridge::install();

    /* 5. Configuration. */
    config_defaults(&g_cfg);

    char confPath[512];
    if (parser.isSet(confOpt))
        qstrncpy(confPath, qPrintable(parser.value(confOpt)), sizeof confPath);
    else
        config_default_path(confPath, sizeof confPath);

    const bool haveConf = (config_load(&g_cfg, confPath) == 0);

    /* log_level_from_name() returns -1 for an unknown name, and log_set_level()
     * clamps that to LOG_ERR — so a typo in the config would silently reduce
     * the whole application to errors only. The CLI guards this; so do we. */
    const int lvl = log_level_from_name(g_cfg.log_level);
    if (lvl >= 0)
        log_set_level(lvl);

    if (g_cfg.log_file[0])
        LogBridge::setFile(QString::fromUtf8(g_cfg.log_file));

    log_info("SVXConnect-Omarchy %s starting (Qt %s)", SVXCONNECT_VERSION, qVersion());
    if (!haveConf) {
        log_warn("no configuration at %s — run 'svxconnect --enroll' first", confPath);
    } else {
        syncAutoPosition(&g_cfg, confPath);
    }

    /* 6. Layout-only mode: bring the window up without touching the core.
     *    Borrowed from SVXConnect-PIOS. Invaluable for UI work on a machine
     *    with no certificate and no microphone. */
    if (qEnvironmentVariableIsSet("SVX_WINDOW_ONLY")) {
        log_warn("SVX_WINDOW_ONLY set — the reflector core will not be started");
        MainWindow w(nullptr);
        w.setConfigPath(QString::fromUtf8(confPath));
        w.setReflectorHost(QString::fromUtf8(g_cfg.reflector));
        w.show();

        scheduleScreenshot(app, w, QString::fromUtf8(confPath));

        const int rc = app.exec();
        LogBridge::shutdown();
        return rc;
    }

    /* 7. The run lock, BEFORE app_new(), so we never open an audio device or
     *    a socket we are not entitled to hold. */
    RunLock::Holder holder;
    switch (RunLock::acquire(QString::fromUtf8(g_cfg.lock_file), &holder)) {
    case RunLock::Result::Acquired:
        break;

    case RunLock::Result::HeldByOther: {
        /* M5 turns this into "raise the existing window" for our own kind, and
         * SIGUSR1 for SVXConnect-PIOS. Until then, say plainly who is in the
         * way — which is more than the bare "already running" most programs
         * manage. */
        const QString who = holder.describe();
        log_err("the reflector connection is already held by %s", qPrintable(who));
        QMessageBox::critical(nullptr,
            QCoreApplication::translate("main", "SVXConnect is already running"),
            QCoreApplication::translate("main",
                "The reflector connection is already held by %1.\n\n"
                "Only one client may use your certificate and node id at a "
                "time. Quit that one first.").arg(who));
        LogBridge::shutdown();
        return 1;
    }

    case RunLock::Result::Error:
        log_err("could not create the run lock at %s", g_cfg.lock_file);
        QMessageBox::critical(nullptr,
            QCoreApplication::translate("main", "SVXConnect cannot start"),
            QCoreApplication::translate("main",
                "The run lock at %1 could not be created.\n\n"
                "Check that the directory exists and is writable.")
                .arg(QString::fromUtf8(g_cfg.lock_file)));
        LogBridge::shutdown();
        return 1;
    }

    /* 8. The core. */
    svx_app *core = app_new(&g_cfg, /*no_tx=*/0);
    if (!core) {
        log_err("app_new() failed");
        RunLock::release();
        LogBridge::shutdown();
        return 1;
    }
    app_set_owner_kind(core, RunLock::kOwnerKind);

    /* Restart-after-editing.
     *
     * The whole point of routing this through main() rather than doing it in
     * the window is ORDER. The replacement process must not be launched until
     * this one has released the run lock, or it will find the lock held, refuse
     * itself, and leave the user with no application running at all.
     *
     * svx_lock_release() drops the flock by closing the fd it holds, and that
     * cannot happen until app_free() has returned. So the launch is deferred
     * all the way past teardown, at the bottom of this function. */
    bool restartWanted = false;

    /* Captured before exec() so a restart preserves whatever the user
     * originally passed — notably -c <file>. */
    const QStringList relaunchArgs = QCoreApplication::arguments().mid(1);
    const QString     relaunchExe  = QCoreApplication::applicationFilePath();

    int rc = 0;
    {
        /* THE SCOPE IS LOAD-BEARING. Both the window and the loop must be
         * destroyed while `core` is still alive: ~CoreLoop detaches the core's
         * observer with app_set_observer(), and ~PttManager un-keys the
         * transmitter with app_ptt(), each through `core`. As stack objects of
         * main() they would otherwise die AFTER app_free() below — a
         * use-after-free that segfaulted on every quit, in ~CoreLoop. */
        MainWindow win(core);
        win.setConfigPath(QString::fromUtf8(confPath));
        win.show();
        scheduleScreenshot(app, win, QString::fromUtf8(confPath));

        QObject::connect(&win, &MainWindow::restartRequested, &app, [&]() {
            restartWanted = true;
            app.quit();
        });

        /* 9. CoreLoop is constructed BEFORE app_start(), so that the observer is
         *    installed before the core can fire it. app_start() is blocking —
         *    miniaudio initialisation takes tens to hundreds of milliseconds —
         *    so show the window first and let it paint. */
        CoreLoop loop(core);
        QObject::connect(&loop, &CoreLoop::coreChanged, &win, &MainWindow::onCoreChanged);
        QObject::connect(&loop, &CoreLoop::quitRequested, &app, &QCoreApplication::quit);

        installSignalHandling(&app);

        app.processEvents();     /* let the window paint before we block */
        app_start(core);
        loop.kick();

        rc = app.exec();
    }

    /* 10. Teardown order matters: app_free() clears the status file and joins
     *     the connect worker (and logs while doing it), so the lock is
     *     released after it, and the log sink is torn down after that. */
    app_free(core);
    RunLock::release();

    /* The lock is now genuinely free, so the replacement can take it. */
    if (restartWanted) {
        log_info("relaunching %s", qPrintable(relaunchExe));
        if (!QProcess::startDetached(relaunchExe, relaunchArgs))
            log_err("could not relaunch %s — start SVXConnect again by hand",
                    qPrintable(relaunchExe));
    }

    LogBridge::shutdown();
    return rc;
}
