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

class QSystemTrayIcon;
class QMenu;
class QAction;

class TrayIcon : public QObject {
    Q_OBJECT

public:
    explicit TrayIcon(svx_app *app, QObject *parent = nullptr);
    ~TrayIcon() override;

    bool isAvailable() const;

    /* Called from the 100 ms model tick. Cheap: it only touches the icon and
     * tooltip when the state it renders has actually changed. */
    void tickModel();

    /* Shown once, the first time the window is closed, so "it did not quit"
     * is an explanation rather than a surprise. */
    void notifyStillRunning();

signals:
    void showWindowRequested();
    void quitRequested();

private:
    void rebuildMenu();
    QIcon iconFor(bool connected, bool transmitting) const;

    svx_app         *m_app  = nullptr;
    QSystemTrayIcon *m_tray = nullptr;
    QMenu           *m_menu = nullptr;
    QAction         *m_connectAction = nullptr;
    QAction         *m_pttAction     = nullptr;

    /* What the icon currently depicts, so a 10 Hz tick does not reassign the
     * same QIcon forever — on some StatusNotifierItem hosts that flickers. */
    bool m_shownConnected = false;
    bool m_shownTx        = false;
    bool m_stateInit      = false;
    QString m_tip;
};

#endif
