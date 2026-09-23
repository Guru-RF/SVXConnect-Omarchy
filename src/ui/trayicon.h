/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * The tray icon, which on Omarchy lives in the shell bar's tray widget
 * (StatusNotifierItem; the menu is drawn by the shell, in the shell's style).
 *
 * It exists because the global push-to-talk shortcut is registered by the
 * running process: closing the window must not quietly disable it, so closing
 * hides to the tray and quitting is an explicit action.
 */
#ifndef SVXCONNECT_OMARCHY_TRAYICON_H
#define SVXCONNECT_OMARCHY_TRAYICON_H

#include <QObject>
#include <QIcon>

#include "core/svxcore.h"
#include "core/audiohealth.h"

class QSystemTrayIcon;
class QMenu;
class QAction;

class TrayIcon : public QObject {
    Q_OBJECT

public:
    explicit TrayIcon(svx_app *app, QObject *parent = nullptr);
    ~TrayIcon() override;

    bool isAvailable() const;

    /* Called from the 100 ms model tick with the window's verdict on whether
     * audio is leaving (see core/audiohealth.h): the dot is red only while it
     * is, never merely because the transmitter is keyed. Cheap: the icon and
     * tooltip are only touched when what they show has changed.
     *
     * The menu items are re-synchronised on EVERY tick, not only on a change.
     * "Transmit" is checkable and Qt flips its check mark on the click itself,
     * so a press the core refuses left it checked while idle; and the
     * Connect/Disconnect label followed "connected or not", so across
     * connecting and reconnecting it said the opposite of what a click did. */
    void tickModel(TxMonitor::State tx);

    /* The menu's items, for the tests. The menu exists even where no tray
     * does. */
    QAction *pttAction() const     { return m_pttAction; }
    QAction *connectAction() const { return m_connectAction; }

    /* Shown once, the first time the window is closed, so "it did not quit"
     * is an explanation rather than a surprise. */
    void notifyStillRunning();

signals:
    void showWindowRequested();
    void quitRequested();

private:
    void buildMenu();
    QIcon iconFor(bool connected, TxMonitor::State tx) const;

    svx_app         *m_app  = nullptr;
    QSystemTrayIcon *m_tray = nullptr;
    QMenu           *m_menu = nullptr;
    QAction         *m_connectAction = nullptr;
    QAction         *m_pttAction     = nullptr;

    /* What the icon currently depicts, so a 10 Hz tick does not reassign the
     * same QIcon forever — on some StatusNotifierItem hosts that flickers. */
    bool m_shownConnected = false;
    TxMonitor::State m_shownTx = TxMonitor::State::Idle;
    bool m_stateInit      = false;
    QString m_tip;
};

#endif
