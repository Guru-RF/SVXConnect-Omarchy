/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * The window shell: header actions, layout, the two timers, and the PTT button.
 *
 * THE REPAINT MODEL, WHICH IS NOT OBVIOUS
 * ---------------------------------------
 * app_set_observer() sounds like the change notification you want. It is
 * nearly inert: app.c fires it only from banner_set, ctl_volume,
 * app_set_volume, app_toggle_output_mute, app_set_input_device,
 * app_set_output_device and app_dismiss_banner. It does NOT fire on connection
 * state change, talker start/stop, or node join/leave, and the talkgroup
 * manager's own `changed` callback is a no-op stub.
 *
 * So polling on a timer is mandatory, not an optimisation. Two timers,
 * deliberately separate, and the split is the whole design:
 *
 *     100 ms  everything textual — status, talkgroups, activity, log serial
 *      33 ms  level meters ONLY, which own their own repaint
 *
 * Never connect a high-rate source to a full relayout.
 *
 * NO MENU BAR
 * -----------
 * Omarchy's own surfaces have none, and under Hyprland a tiled window with a
 * File/View/Help strip looks like it wandered in from another desktop. Every
 * action is a QAction on the window — so its shortcut works without a menu —
 * and they are reachable from the icon buttons at the right of the header.
 */
#ifndef SVXCONNECT_OMARCHY_MAINWINDOW_H
#define SVXCONNECT_OMARCHY_MAINWINDOW_H

#include <QMainWindow>

#include "core/svxcore.h"

class QLabel;
class QPushButton;
class QToolButton;
class QPlainTextEdit;
class QTimer;
class QAction;
class QSplitter;
class QFileSystemWatcher;

class PttManager;
class TrayIcon;
class Notifier;
class ConnectionBar;
class Sidebar;
class ActivityPanel;
class MapView;
class ReflectorFeed;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    /* `app` may be null, for SVX_WINDOW_ONLY=1 — a layout-only mode that brings
     * the window up without starting the core. Everything below must therefore
     * tolerate a null core. */
    explicit MainWindow(svx_app *app, QWidget *parent = nullptr);
    ~MainWindow() override;

    /* The configuration file actually in use, as resolved in main() — which
     * may have taken it from -c. */
    void setConfigPath(const QString &path);

    /* The loaded configuration, for the window's own read-only uses — the
     * portal probe and your station's position on the map. With a core this is
     * app_config(); SVX_WINDOW_ONLY has no core, and main() passes the same
     * struct in so both still work without one. */
    void setOfflineConfig(const svx_config *cfg);

public slots:
    /* Connected to CoreLoop::coreChanged() — a hint, not the primary path. */
    void onCoreChanged();

signals:
    /* The user asked to restart after editing the configuration. A signal and
     * not a restart done here: main() is the only place that knows the correct
     * order — quit the loop, app_free(), release the run lock, and only then
     * exec the replacement, which would otherwise find the lock held. */
    void restartRequested();

protected:
    void keyPressEvent(QKeyEvent *) override;
    /* The map folds out when the window is tall enough to hold one. */
    void resizeEvent(QResizeEvent *) override;
    /* Closing hides to the tray instead of quitting — the global shortcut is
     * registered by the running process. See trayicon.h. */
    void closeEvent(QCloseEvent *) override;
    /* Click-to-dismiss for the notice bars. */
    bool eventFilter(QObject *watched, QEvent *event) override;
    /* Re-reads the Hyprland binding when the window is activated, which is
     * when a user who has just edited bindings.lua comes back to look. */
    bool event(QEvent *event) override;

private slots:
    void tickModel();     /* 100 ms */
    void tickMeters();    /*  33 ms */

    void onPttPressed();
    void onPttReleased();

    void onPreferences();
    void onEditConfig();
    void onConfigFileChanged(const QString &path);
    void onRestartRequested();

private:
    void buildUi();
    void buildActions();
    void refreshBanner();
    void refreshLog();
    void refreshPttButton();
    void refreshPttHint();
    void watchConfig();

    /* The map pane.
     *
     * Three states, not two: "auto" is the default and means the map appears
     * when there is room for it and a feed to fill it, which is what makes it
     * unobtrusive on a tiled half-screen window and present on a big one. The
     * menu item switches to an explicit on or off, and that choice sticks. */
    void applyMapVisibility();
    void refreshMapMarkers();
    QString mapMode() const;
    void setMapMode(const QString &mode);
    void applyPttBindings();
    void showAbout();

    svx_app          *m_app = nullptr;
    const svx_config *m_cfg = nullptr;
    PttManager *m_pttManager = nullptr;
    TrayIcon   *m_tray       = nullptr;
    Notifier   *m_notifier   = nullptr;
    bool        m_reallyQuit = false;

    ConnectionBar  *m_status    = nullptr;
    Sidebar        *m_sidebar   = nullptr;
    ActivityPanel  *m_activity  = nullptr;
    ReflectorFeed  *m_feed      = nullptr;
    MapView        *m_map       = nullptr;
    QWidget        *m_mapPane   = nullptr;   /* map + its rule, shown as one */
    QTimer         *m_mapTick   = nullptr;   /* 500 ms while the map is up   */
    QLabel         *m_banner    = nullptr;   /* the core's banner          */
    QLabel         *m_pttBanner = nullptr;   /* a lost push-to-talk control */
    QWidget        *m_reload    = nullptr;
    QPushButton    *m_ptt       = nullptr;
    QLabel         *m_pttHint   = nullptr;
    QLabel         *m_pttGlobal = nullptr;
    QPlainTextEdit *m_log       = nullptr;
    QSplitter      *m_body      = nullptr;

    QToolButton *m_logButton   = nullptr;
    QToolButton *m_prefsButton = nullptr;
    QToolButton *m_menuButton  = nullptr;

    QAction *m_actShowSidebar = nullptr;
    QAction *m_actShowLog     = nullptr;
    QAction *m_actShowMap     = nullptr;

    QTimer *m_modelTick = nullptr;
    QTimer *m_meterTick = nullptr;

    QString             m_configPath;
    QFileSystemWatcher *m_configWatch = nullptr;

    /* What the portal reports as bound, on desktops where it knows. */
    QString m_portalTrigger;

    quint64 m_lastLogSerial = 0;
    bool    m_lastTxActive  = false;
    bool    m_txInit        = false;
};

#endif
