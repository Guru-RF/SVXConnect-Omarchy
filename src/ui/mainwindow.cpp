/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 */
#include "ui/mainwindow.h"
#include "ui/statusbar.h"
#include "ui/sidebar.h"
#include "ui/activitypanel.h"
#include "ui/theme.h"
#include "ui/configfile.h"
#include "ui/preferencesdialog.h"
#include "ui/trayicon.h"
#include "ptt/pttmanager.h"
#include "ptt/hyprlandbinding.h"
#include "core/logbridge.h"

#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QPlainTextEdit>
#include <QTimer>
#include <QMenu>
#include <QAction>
#include <QKeyEvent>
#include <QScrollBar>
#include <QFrame>
#include <QDesktopServices>
#include <QUrl>
#include <QMessageBox>
#include <QSettings>
#include <QCloseEvent>
#include <QFileSystemWatcher>
#include <QFileInfo>

namespace {

QFrame *rule(QWidget *parent, bool vertical = false)
{
    auto *f = new QFrame(parent);
    f->setFrameShape(QFrame::NoFrame);
    Theme::setRole(f, vertical ? "vrule" : "rule");
    return f;
}

QToolButton *iconButton(char32_t glyph, const QString &tip, QWidget *parent)
{
    auto *b = new QToolButton(parent);
    Theme::setRole(b, "icon");
    b->setText(Theme::Glyph::of(glyph));
    b->setToolTip(tip);
    b->setCursor(Qt::PointingHandCursor);
    b->setFocusPolicy(Qt::NoFocus);   /* Space is transmit in this window */
    b->setToolButtonStyle(Qt::ToolButtonTextOnly);
    return b;
}

QLabel *notice(const char *tone, QWidget *parent)
{
    auto *l = new QLabel(parent);
    Theme::setRole(l, "banner");
    Theme::setTone(l, tone);
    l->setWordWrap(true);
    l->setContentsMargins(Theme::space(12), Theme::space(6), Theme::space(12), Theme::space(6));
    l->setCursor(Qt::PointingHandCursor);
    l->setToolTip(QCoreApplication::translate("MainWindow", "Click to dismiss"));
    l->hide();
    return l;
}

} // namespace

MainWindow::MainWindow(svx_app *app, QWidget *parent)
    : QMainWindow(parent), m_app(app)
{
    setWindowTitle(QStringLiteral("SVXConnect"));
    buildUi();
    buildActions();

    /* Global push-to-talk. Constructed even without a core so the settings
     * page can probe backends in SVX_WINDOW_ONLY mode; the manager simply
     * never calls app_ptt() when m_app is null. */
    m_pttManager = new PttManager(m_app, this);
    connect(m_pttManager, &PttManager::backendLost, this, [this](const QString &id, const QString &why) {
        Q_UNUSED(id);
        m_pttBanner->setText(tr("Push-to-talk stopped working: %1").arg(why));
        m_pttBanner->show();
    });
    connect(m_pttManager, &PttManager::triggerChanged, this, [this](const QString &id, const QString &human) {
        if (id == QLatin1String("portal")) {
            m_portalTrigger = human;
            refreshPttHint();
        }
    });
    applyPttBindings();

    /* The tray, and with it the ability to close the window without killing
     * the push-to-talk shortcut. */
    m_tray = new TrayIcon(m_app, this);
    connect(m_tray, &TrayIcon::showWindowRequested, this, [this]() {
        showNormal();
        raise();
        activateWindow();
    });
    connect(m_tray, &TrayIcon::quitRequested, this, [this]() {
        m_reallyQuit = true;
        qApp->quit();
    });

    /* Only decouple the lifetime from the window if there is somewhere to
     * bring it back from. With no tray — the omarchy.tray widget removed from
     * the bar — hiding the window would strand the process with no interface
     * and no way to quit it. */
    qApp->setQuitOnLastWindowClosed(!m_tray->isAvailable());

    QSettings s;
    if (s.contains(QStringLiteral("window/geometry")))
        restoreGeometry(s.value(QStringLiteral("window/geometry")).toByteArray());
    else
        resize(Theme::space(860), Theme::space(580));

    m_modelTick = new QTimer(this);
    m_modelTick->setInterval(100);
    connect(m_modelTick, &QTimer::timeout, this, &MainWindow::tickModel);
    m_modelTick->start();

    m_meterTick = new QTimer(this);
    m_meterTick->setInterval(33);
    connect(m_meterTick, &QTimer::timeout, this, &MainWindow::tickMeters);
    m_meterTick->start();

    refreshPttHint();
    tickModel();
}

MainWindow::~MainWindow()
{
    QSettings s;
    s.setValue(QStringLiteral("window/geometry"), saveGeometry());
}

void MainWindow::buildUi()
{
    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    /* ---- header ---- */
    m_status = new ConnectionBar(m_app, central);

    m_logButton = iconButton(Theme::Glyph::Log, tr("Log  (Ctrl+L)"), m_status);
    m_logButton->setCheckable(true);
    m_prefsButton = iconButton(Theme::Glyph::Settings, tr("Preferences  (Ctrl+,)"), m_status);
    m_menuButton = iconButton(Theme::Glyph::Menu, tr("More"), m_status);
    m_menuButton->setPopupMode(QToolButton::InstantPopup);

    m_status->addTrailing(m_logButton);
    m_status->addTrailing(m_prefsButton);
    m_status->addTrailing(m_menuButton);

    root->addWidget(m_status);
    root->addWidget(rule(central));

    /* ---- notices ----
     * The core raises a banner for things the operator must see: a silent
     * microphone, a certificate about to expire, a refused transmit. A lost
     * push-to-talk control gets its own bar, because the core's bar is
     * re-synchronised from app_banner() every tick and would erase it. */
    m_banner = notice("warn", central);
    m_banner->installEventFilter(this);
    root->addWidget(m_banner);

    m_pttBanner = notice("error", central);
    m_pttBanner->installEventFilter(this);
    root->addWidget(m_pttBanner);

    /* Shown when svxconnect.conf changes on disk while running.
     *
     * Nothing is reloaded in place, deliberately: app_new() keeps the
     * svx_config POINTER and the talkgroup manager holds indices into its
     * lists, so rewriting those under a live connection is a use-after-free
     * waiting for the next talker. A real button, not a clickable bar:
     * restarting drops the connection, and that should not be something you
     * trigger while dismissing a notice. */
    m_reload = new QWidget(central);
    m_reload->setAttribute(Qt::WA_StyledBackground, true);
    Theme::setRole(m_reload, "banner");
    Theme::setTone(m_reload, "ok");

    auto *reloadLay = new QHBoxLayout(m_reload);
    reloadLay->setContentsMargins(Theme::space(12), Theme::space(4), Theme::space(8), Theme::space(4));
    reloadLay->setSpacing(Theme::space(8));

    auto *reloadText = new QLabel(tr("Configuration changed. Restart to apply it."), m_reload);
    reloadText->setWordWrap(true);
    reloadLay->addWidget(reloadText, 1);

    auto *restartBtn = new QPushButton(tr("Restart now"), m_reload);
    restartBtn->setCursor(Qt::PointingHandCursor);
    restartBtn->setToolTip(tr("Quit and start again, applying the new configuration"));
    connect(restartBtn, &QPushButton::clicked, this, &MainWindow::onRestartRequested);
    reloadLay->addWidget(restartBtn);

    auto *laterBtn = new QPushButton(tr("Later"), m_reload);
    laterBtn->setFlat(true);
    laterBtn->setCursor(Qt::PointingHandCursor);
    connect(laterBtn, &QPushButton::clicked, m_reload, &QWidget::hide);
    reloadLay->addWidget(laterBtn);

    m_reload->hide();
    root->addWidget(m_reload);

    /* ---- body: sidebar | activity ---- */
    auto *body = new QWidget(central);
    auto *bodyLay = new QHBoxLayout(body);
    bodyLay->setContentsMargins(0, 0, 0, 0);
    bodyLay->setSpacing(0);

    m_sidebar = new Sidebar(m_app, body);
    bodyLay->addWidget(m_sidebar);
    bodyLay->addWidget(rule(body, true));

    m_activity = new ActivityPanel(m_app, body);
    connect(m_activity, &ActivityPanel::talkgroupChosen, this, [this](quint32 tg) {
        if (m_app) app_tg_select(m_app, tg);
    });
    bodyLay->addWidget(m_activity, 1);

    /* ---- log ---- */
    m_log = new QPlainTextEdit(central);
    Theme::setRole(m_log, "log");
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);
    m_log->setFrameShape(QFrame::NoFrame);
    m_log->hide();

    m_body = new QSplitter(Qt::Vertical, central);
    m_body->addWidget(body);
    m_body->addWidget(m_log);
    m_body->setStretchFactor(0, 1);
    m_body->setStretchFactor(1, 0);
    m_body->setChildrenCollapsible(false);
    m_body->restoreState(QSettings().value(QStringLiteral("window/splitter")).toByteArray());
    connect(m_body, &QSplitter::splitterMoved, this, [this]() {
        QSettings().setValue(QStringLiteral("window/splitter"), m_body->saveState());
    });
    root->addWidget(m_body, 1);

    root->addWidget(rule(central));

    /* ---- PTT ----
     * Hold-to-talk on the mouse: pressed() keys, released() unkeys — the same
     * gesture the global shortcut uses, so the two match.
     *
     * The core owns every refusal: no link, no talkgroup, or somebody else
     * already talking each produce a distinct beep pattern and no carrier. */
    auto *pttWrap = new QWidget(central);
    auto *pttLay = new QVBoxLayout(pttWrap);
    pttLay->setContentsMargins(Theme::space(12), Theme::space(10), Theme::space(12), Theme::space(8));
    pttLay->setSpacing(Theme::space(6));

    m_ptt = new QPushButton(tr("PUSH TO TALK"), pttWrap);
    Theme::setRole(m_ptt, "ptt");
    m_ptt->setFocusPolicy(Qt::NoFocus);
    m_ptt->setCursor(Qt::PointingHandCursor);
    m_ptt->setToolTip(tr("Hold to transmit. Space toggles, Escape stops."));
    connect(m_ptt, &QPushButton::pressed,  this, &MainWindow::onPttPressed);
    connect(m_ptt, &QPushButton::released, this, &MainWindow::onPttReleased);
    pttLay->addWidget(m_ptt);

    auto *hintRow = new QHBoxLayout;
    hintRow->setSpacing(Theme::space(8));
    m_pttHint = new QLabel(tr("hold to talk · space toggles · esc stops"), pttWrap);
    Theme::setRole(m_pttHint, "hint");
    hintRow->addWidget(m_pttHint);
    hintRow->addStretch(1);
    m_pttGlobal = new QLabel(pttWrap);
    Theme::setRole(m_pttGlobal, "hint");
    hintRow->addWidget(m_pttGlobal);
    pttLay->addLayout(hintRow);

    root->addWidget(pttWrap);

    setCentralWidget(central);
    refreshPttButton();
}

void MainWindow::buildActions()
{
    auto action = [this](const QString &text, const QString &keys) {
        auto *a = new QAction(text, this);
        if (!keys.isEmpty())
            a->setShortcut(QKeySequence(keys));
        a->setShortcutContext(Qt::WindowShortcut);
        addAction(a);   /* so the shortcut works with no menu bar */
        return a;
    };

    QAction *prefs = action(tr("Preferences…"), QStringLiteral("Ctrl+,"));
    connect(prefs, &QAction::triggered, this, &MainWindow::onPreferences);
    connect(m_prefsButton, &QToolButton::clicked, this, &MainWindow::onPreferences);

    QAction *editConf = action(tr("Edit configuration…"), QStringLiteral("Ctrl+E"));
    connect(editConf, &QAction::triggered, this, &MainWindow::onEditConfig);

    QAction *confDir = action(tr("Show configuration folder"), QString());
    connect(confDir, &QAction::triggered, this, [this]() {
        if (!m_configPath.isEmpty())
            ConfigFile::openContainingFolder(m_configPath, this);
    });

    QAction *certs = action(tr("Show certificates"), QString());
    connect(certs, &QAction::triggered, this, [this]() {
        if (!m_app) return;
        QDesktopServices::openUrl(
            QUrl::fromLocalFile(QString::fromUtf8(app_config(m_app)->pki_dir)));
    });

    QAction *reconnect = action(tr("Reconnect"), QStringLiteral("Ctrl+R"));
    connect(reconnect, &QAction::triggered, this, [this]() {
        if (m_app) app_reconnect(m_app);
    });

    QAction *lock = action(tr("Lock talkgroup"), QStringLiteral("Ctrl+K"));
    connect(lock, &QAction::triggered, this, [this]() {
        if (m_app) app_toggle_lock(m_app);
    });

    m_actShowSidebar = action(tr("Show sidebar"), QStringLiteral("Ctrl+Shift+S"));
    m_actShowSidebar->setCheckable(true);
    m_actShowSidebar->setChecked(true);
    connect(m_actShowSidebar, &QAction::toggled, this, [this](bool on) {
        m_sidebar->setVisible(on);
    });

    m_actShowLog = action(tr("Show log"), QStringLiteral("Ctrl+L"));
    m_actShowLog->setCheckable(true);
    connect(m_actShowLog, &QAction::toggled, this, [this](bool on) {
        m_log->setVisible(on);
        const QSignalBlocker b(m_logButton);
        m_logButton->setChecked(on);
        if (on) {
            m_lastLogSerial = 0;
            refreshLog();
        }
    });
    connect(m_logButton, &QToolButton::toggled, m_actShowLog, &QAction::setChecked);

    QAction *docs = action(tr("Documentation"), QString());
    connect(docs, &QAction::triggered, this, []() {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://svxconnect.app")));
    });

    QAction *about = action(tr("About SVXConnect"), QString());
    connect(about, &QAction::triggered, this, &MainWindow::showAbout);

    QAction *quit = action(tr("Quit"), QStringLiteral("Ctrl+Q"));
    connect(quit, &QAction::triggered, this, [this]() {
        m_reallyQuit = true;
        qApp->quit();
    });

    auto *menu = new QMenu(this);
    menu->addAction(prefs);
    menu->addAction(editConf);
    menu->addAction(confDir);
    menu->addAction(certs);
    menu->addSeparator();
    menu->addAction(reconnect);
    menu->addAction(lock);
    menu->addSeparator();
    menu->addAction(m_actShowSidebar);
    menu->addAction(m_actShowLog);
    menu->addSeparator();
    menu->addAction(docs);
    menu->addAction(about);
    menu->addSeparator();
    menu->addAction(quit);
    m_menuButton->setMenu(menu);
}

void MainWindow::showAbout()
{
    const OmarchyTheme &theme = OmarchyTheme::get();
    const QString themeLine = theme.isOmarchy()
        ? tr("Styled live from the Omarchy theme <b>%1</b>, in %2.")
              .arg(theme.name().toHtmlEscaped(), theme.fontFamily().toHtmlEscaped())
        : tr("No Omarchy theme was found, so the built-in palette is used.");

    QMessageBox::about(this, tr("About SVXConnect"),
        tr("<h3>SVXConnect %1</h3>"
           "<p>A desktop client for SvxLink reflectors, for Omarchy.</p>"
           "<p>%3</p>"
           "<p>Copyright © 2026 Diëlectricum BV.<br>"
           "Written by Joeri Van Dooren, ON6URE.</p>"
           "<p>Released under the MIT licence.</p>"
           "<p>Built with the Qt toolkit %2, © The Qt Company Ltd and "
           "contributors, used under the GNU Lesser General Public License "
           "version 3. Qt is linked dynamically and unmodified; you may "
           "replace the Qt libraries with modified versions and relink.</p>")
            .arg(QString::fromLatin1(SVXCONNECT_VERSION),
                 QString::fromLatin1(qVersion()),
                 themeLine));
}

void MainWindow::keyPressEvent(QKeyEvent *e)
{
    if (!m_app) { QMainWindow::keyPressEvent(e); return; }

    /* Space is a TOGGLE, not a hold, and Escape is an unconditional stop.
     *
     * Keyboard auto-repeat under Wayland makes an in-window hold-to-talk
     * unreliable — the release may not arrive in the order you expect. A toggle
     * is deterministic. True hold-to-talk comes from the global shortcut, which
     * gets its release from Hyprland rather than from a focused widget, and from
     * the mouse on the PTT button.
     *
     * isAutoRepeat() is checked anyway, so holding Space does not chatter the
     * transmitter on and off. */
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) {
        app_ptt(m_app, CTL_TOGGLE);
        e->accept();
        return;
    }
    if (e->key() == Qt::Key_Escape) {
        app_ptt(m_app, CTL_OFF);
        e->accept();
        return;
    }
    QMainWindow::keyPressEvent(e);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonRelease) {
        if (watched == m_banner) {
            /* Tell the core, not just the widget: app_banner() would hand the
             * same string straight back on the next tick otherwise. */
            if (m_app) app_dismiss_banner(m_app);
            m_banner->hide();
            return true;
        }
        if (watched == m_pttBanner) {
            m_pttBanner->hide();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

bool MainWindow::event(QEvent *event)
{
    if (event->type() == QEvent::WindowActivate)
        refreshPttHint();
    return QMainWindow::event(event);
}

void MainWindow::onCoreChanged()
{
    refreshBanner();
}

void MainWindow::tickModel()
{
    const quint64 now = m_app ? now_ms() : 0;

    if (m_tray) m_tray->tickModel();
    m_status->tickModel(now);
    m_sidebar->tickModel(now);
    m_activity->tickModel(now);

    refreshBanner();
    refreshPttButton();

    if (m_log->isVisible())
        refreshLog();
}

void MainWindow::tickMeters()
{
    m_sidebar->tickMeters();
}

void MainWindow::refreshBanner()
{
    if (!m_app) return;

    const char *banner = app_banner(m_app);
    if (banner && *banner) {
        const QString text = QString::fromUtf8(banner);
        if (m_banner->text() != text)
            m_banner->setText(text);
        m_banner->show();
    } else if (m_banner->isVisible()) {
        m_banner->hide();
    }
}

void MainWindow::refreshPttButton()
{
    const bool tx = m_app && app_tx_active(m_app);
    if (m_txInit && tx == m_lastTxActive)
        return;
    m_txInit = true;
    m_lastTxActive = tx;

    /* The theme's red, solid, with the background as text — it must read as
     * "on the air" at a glance, including on an unfocused window. */
    Theme::setProp(m_ptt, "tx", tx);
    m_ptt->setText(tx ? tr("TRANSMITTING") : tr("PUSH TO TALK"));
}

void MainWindow::refreshPttHint()
{
    QString text;
    const char *tone = "";

    if (HyprlandBinding::isHyprland()) {
        const QString keys = HyprlandBinding::boundKeys();
        if (!keys.isEmpty()) {
            text = tr("global  %1").arg(keys);
        } else {
            text = tr("no global key — Preferences › Push-to-talk");
            tone = "warn";
        }
    } else if (!m_portalTrigger.isEmpty()) {
        text = tr("global  %1").arg(m_portalTrigger);
    } else {
        text = tr("no global key");
        tone = "warn";
    }

    m_pttGlobal->setText(text);
    Theme::setTone(m_pttGlobal, tone);
}

void MainWindow::refreshLog()
{
    const quint64 serial = LogBridge::serial();
    if (serial == m_lastLogSerial)
        return;
    m_lastLogSerial = serial;

    QScrollBar *bar = m_log->verticalScrollBar();
    const bool atBottom = bar->value() >= bar->maximum() - 4;

    const QVector<LogLine> lines = LogBridge::snapshot(500);
    m_log->clear();
    for (const LogLine &l : lines)
        m_log->appendPlainText(l.text);

    if (atBottom)
        bar->setValue(bar->maximum());
}

void MainWindow::onPttPressed()
{
    if (m_app) app_ptt(m_app, CTL_ON);
}

void MainWindow::onPttReleased()
{
    if (m_app) app_ptt(m_app, CTL_OFF);
}

void MainWindow::setConfigPath(const QString &path)
{
    m_configPath = path;
    watchConfig();
}

void MainWindow::watchConfig()
{
    if (m_configPath.isEmpty())
        return;

    if (!m_configWatch) {
        m_configWatch = new QFileSystemWatcher(this);
        connect(m_configWatch, &QFileSystemWatcher::fileChanged,
                this, &MainWindow::onConfigFileChanged);
    }

    if (!m_configWatch->files().contains(m_configPath)
        && QFileInfo::exists(m_configPath))
        m_configWatch->addPath(m_configPath);
}

void MainWindow::onConfigFileChanged(const QString &path)
{
    Q_UNUSED(path);

    /* Most editors — Neovim included — save by writing a temporary file and
     * renaming it over the target. That REMOVES the inode the watcher was
     * holding, so the path is silently dropped from the watch list and a
     * second save would go unnoticed. Re-adding it is not optional. */
    watchConfig();

    log_info("configuration file changed on disk");
    m_reload->show();
}

void MainWindow::onEditConfig()
{
    if (m_configPath.isEmpty()) {
        QMessageBox::warning(this, tr("No configuration file"),
            tr("SVXConnect does not know which configuration file to edit."));
        return;
    }

    if (!ConfigFile::exists(m_configPath)) {
        const QMessageBox::StandardButton answer = QMessageBox::question(this,
            tr("Create a configuration file?"),
            tr("There is no configuration file at:\n\n%1\n\n"
               "Create one now, filled in with this build's defaults?")
                .arg(m_configPath),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes);

        if (answer != QMessageBox::Yes)
            return;

        QString error;
        if (!ConfigFile::createDefault(m_configPath,
                                       m_app ? app_config(m_app) : nullptr, &error)) {
            QMessageBox::critical(this, tr("Could not create the configuration"), error);
            return;
        }
        watchConfig();
    }

    ConfigFile::openInEditor(m_configPath, this);
}

void MainWindow::onRestartRequested()
{
    /* Never restart with the transmitter keyed. Drop the carrier first,
     * explicitly, so the reflector sees a clean end of transmission. */
    if (m_app && app_tx_active(m_app)) {
        app_ptt(m_app, CTL_OFF);
        log_info("unkeyed before restarting");
    }

    log_info("restarting to apply the new configuration");
    emit restartRequested();
}

void MainWindow::onPreferences()
{
    if (m_configPath.isEmpty()) {
        QMessageBox::warning(this, tr("No configuration file"),
            tr("SVXConnect does not know which configuration file to edit."));
        return;
    }

    /* Modal: the dialog writes the shared config file and applies device
     * changes through the core, and the core is main-thread-only with no
     * re-entrancy guarantees. */
    PreferencesDialog dlg(m_app, m_configPath, this);

    if (m_pttManager) {
        if (PttBackend *portal = m_pttManager->backend(QStringLiteral("portal")))
            dlg.setCurrentShortcut(portal->activeTrigger());
    }

    QMetaObject::Connection trigConn;
    if (m_pttManager)
        trigConn = connect(m_pttManager, &PttManager::triggerChanged, &dlg,
                           [&dlg](const QString &id, const QString &human) {
                               if (id == QLatin1String("portal"))
                                   dlg.setCurrentShortcut(human);
                           });

    connect(&dlg, &PreferencesDialog::restartNeeded, this, [this]() {
        m_reload->show();
    });
    connect(&dlg, &PreferencesDialog::pttBindingChanged, this, [this]() {
        applyPttBindings();
        refreshPttHint();
    });
    connect(&dlg, &PreferencesDialog::talkgroupsChanged, this, [this]() {
        /* The buttons are rebuilt from the LIVE config, which the dialog did
         * not touch — so this only takes effect after the restart. Calling it
         * anyway is harmless and keeps the one code path. */
        m_sidebar->rebuildTalkgroups();
    });

    dlg.exec();

    if (trigConn)
        disconnect(trigConn);

    refreshPttHint();
}

void MainWindow::applyPttBindings()
{
    if (!m_pttManager)
        return;

    /* A screenshot run is a throwaway process. Registering would make it the
     * newest portal session for SVXConnect:ptt, and xdg-desktop-portal-hyprland
     * delivers the key to the newest session only — so an instance that is
     * really running would silently stop receiving push-to-talk. */
    if (qEnvironmentVariableIsSet("SVX_SCREENSHOT"))
        return;

    m_pttManager->setMode(PreferencesDialog::holdMode() ? PttManager::Mode::Hold
                                                        : PttManager::Mode::Toggle);
    m_pttManager->applyKeyboardBinding(PreferencesDialog::keyboardBinding());
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    /* Quit and the tray's Quit set m_reallyQuit; everything else means "close
     * the window", which for this application means hide. */
    if (m_reallyQuit || !m_tray || !m_tray->isAvailable()) {
        QMainWindow::closeEvent(e);
        return;
    }

    e->ignore();
    hide();

    /* Say so once. A window that does not go away when closed is surprising. */
    QSettings s;
    if (!s.value(QStringLiteral("window/toldAboutTray"), false).toBool()) {
        s.setValue(QStringLiteral("window/toldAboutTray"), true);
        m_tray->notifyStillRunning();
    }
}
