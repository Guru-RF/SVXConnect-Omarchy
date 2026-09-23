/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 */
#include "ui/trayicon.h"
#include "core/coreaction.h"
#include "ui/theme.h"

#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>
#include <QPainter>
#include <QPixmap>

TrayIcon::TrayIcon(svx_app *app, QObject *parent)
    : QObject(parent), m_app(app)
{
    buildMenu();

    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;

    m_tray = new QSystemTrayIcon(this);
    m_tray->setContextMenu(m_menu);
    m_tray->setIcon(iconFor(false, TxMonitor::State::Idle));
    m_tray->setToolTip(tr("SVXConnect"));

    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                /* Trigger is a left click. DoubleClick too, because several
                 * hosts send only one or the other. */
                if (reason == QSystemTrayIcon::Trigger
                    || reason == QSystemTrayIcon::DoubleClick)
                    emit showWindowRequested();
            });

    /* The state dot is drawn in the theme's colours; redraw it on a switch. */
    connect(&OmarchyTheme::get(), &OmarchyTheme::changed, this, [this]() {
        m_stateInit = false;
        tickModel(m_shownTx);
        if (!m_app && m_tray)
            m_tray->setIcon(iconFor(false, TxMonitor::State::Idle));
    });

    m_tray->show();
}

TrayIcon::~TrayIcon()
{
    /* The tray first. Its platform icon (QDBusTrayIcon) was handed this menu's
     * platform menu, and would otherwise outlive the menu by the length of
     * QObject's child teardown — the "destruction order at quit" shape that
     * has crashed this application before. Qt's own QPointer probably covers
     * it; this does not rely on that. */
    if (m_tray) {
        m_tray->setContextMenu(nullptr);
        delete m_tray;
        m_tray = nullptr;
    }
    delete m_menu;
}

bool TrayIcon::isAvailable() const
{
    return m_tray != nullptr;
}

void TrayIcon::buildMenu()
{
    m_menu = new QMenu;

    QAction *show = m_menu->addAction(tr("Show SVXConnect"));
    connect(show, &QAction::triggered, this, &TrayIcon::showWindowRequested);

    m_menu->addSeparator();

    /* Toggle, not hold: a menu item cannot report a release, so offering
     * "hold to talk" here would be a lie. The global shortcut does a real hold. */
    m_pttAction = m_menu->addAction(tr("Transmit"));
    m_pttAction->setCheckable(true);
    connect(m_pttAction, &QAction::triggered, this, [this]() {
        CoreAction::ptt(m_app, CTL_TOGGLE);
    });

    m_connectAction = m_menu->addAction(tr("Disconnect"));
    connect(m_connectAction, &QAction::triggered, this, [this]() {
        CoreAction::toggleConnect(m_app);
    });

    m_menu->addSeparator();

    QAction *quit = m_menu->addAction(tr("Quit"));
    connect(quit, &QAction::triggered, this, &TrayIcon::quitRequested);
}

QIcon TrayIcon::iconFor(bool connected, TxMonitor::State tx) const
{
    /* The application icon with a state dot in the corner, in the theme's
     * red / green / muted — the same colours as the window's own status dot. */
    QIcon base;
    for (int size : {22, 24, 32, 48, 64})
        base.addFile(QStringLiteral(":/icons/app-%1.png").arg(size), QSize(size, size));

    const int px = 64;
    QPixmap pm = base.pixmap(px, px);
    if (pm.isNull())
        return base;

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    /* Red only while audio is leaving. Keyed with nothing leaving is the
     * warning colour: it is not "on the air", and must not look like it. */
    const QColor dot = tx == TxMonitor::State::OnAir ? Theme::tx()
                     : tx != TxMonitor::State::Idle  ? Theme::busy()
                     : connected                     ? Theme::connected()
                                                     : Theme::down();

    const qreal r = px * 0.22;
    const QPointF c(px - r - 2, px - r - 2);

    /* A ring of the theme background — the bar is drawn in it — so the dot
     * stays separate from the icon artwork underneath. */
    p.setPen(Qt::NoPen);
    p.setBrush(Theme::palette().background);
    p.drawEllipse(c, r + 3.0, r + 3.0);
    p.setBrush(dot);
    p.drawEllipse(c, r, r);
    p.end();

    return QIcon(pm);
}

void TrayIcon::tickModel(TxMonitor::State tx)
{
    if (!m_app)
        return;

    const rc_state st = rc_get_state(app_rc(m_app));
    const bool connected = (st == RC_CONNECTED);

    /* Every tick; both setters return at once when nothing changed. The check
     * mark is whether the core is KEYED — it is what a click would undo — and
     * the label is what app_toggle_connect() will do in THIS state. */
    m_pttAction->setChecked(app_tx_active(m_app) != 0);
    m_connectAction->setText(st == RC_IDLE ? tr("Connect") : tr("Disconnect"));

    if (!m_tray)
        return;

    if (!m_stateInit || connected != m_shownConnected || tx != m_shownTx) {
        m_stateInit      = true;
        m_shownConnected = connected;
        m_shownTx        = tx;
        m_tray->setIcon(iconFor(connected, tx));
    }

    /* The tooltip carries the callsign and talkgroup: a StatusNotifierItem has
     * no text label next to its icon. */
    const svx_config *cfg = app_config(m_app);
    const uint32_t tg = tgm_selected(app_tgm(m_app));

    QString state = QString::fromUtf8(rc_state_name(st));
    if (!state.isEmpty())
        state[0] = state[0].toUpper();

    QString tip = QStringLiteral("SVXConnect — %1\n%2")
                      .arg(QString::fromUtf8(cfg->callsign), state);
    if (connected)
        tip += tg ? tr("  ·  TG %1").arg(tg) : tr("  ·  monitoring");
    if (tx == TxMonitor::State::OnAir)
        tip += tr("\nTRANSMITTING");
    else if (tx == TxMonitor::State::NoAudio)
        tip += tr("\nKEYED — NOTHING IS BEING SENT (no microphone audio)");

    if (tip != m_tip) {
        m_tip = tip;
        m_tray->setToolTip(tip);
    }
}

void TrayIcon::notifyStillRunning()
{
    if (!m_tray || !QSystemTrayIcon::supportsMessages())
        return;

    m_tray->showMessage(
        tr("SVXConnect is still running"),
        tr("The window is closed but SVXConnect keeps receiving, and the "
           "push-to-talk shortcut keeps working. Quit from the tray icon."),
        QSystemTrayIcon::Information, 6000);
}
