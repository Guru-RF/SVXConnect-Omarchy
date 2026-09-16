/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * "Someone is talking" as a desktop notification.
 *
 * The core has no talker-start callback — its own `changed` hook is a no-op
 * stub — so this works the way the rest of the interface does: it reads the
 * talkgroup manager's active list on the 100 ms tick and notices what is new
 * since the previous pass. A talker is identified by its SSID-stripped
 * callsign and talkgroup, so ON6URE-PI keying up again on the same talkgroup
 * after ON6URE-TPAD does not count as a new talker.
 *
 * WHY IT KEEPS TRACKING WHEN IT IS NOT ANNOUNCING
 * -----------------------------------------------
 * Notifications only go out while the window is closed to the tray, but the
 * active list is read on every tick regardless. If it only looked while
 * hidden, then everyone already talking at the moment you closed the window
 * would look new, and you would get a burst of notifications for a QSO you
 * were just watching.
 *
 * Your own transmissions never notify: the reflector echoes them back, and the
 * core keeps the SSID-stripped callsign precisely so they can be recognised
 * whichever of your nodes sent them.
 */
#ifndef SVXCONNECT_OMARCHY_NOTIFIER_H
#define SVXCONNECT_OMARCHY_NOTIFIER_H

#include <QObject>
#include <QSet>
#include <QString>

#include "core/svxcore.h"

class Notifier : public QObject {
    Q_OBJECT

public:
    explicit Notifier(svx_app *app, QObject *parent = nullptr);

    /* Called from the 100 ms model tick. `announce` is false while the window
     * is on screen: talkers are still tracked, but nothing is sent. */
    void tickModel(bool announce);

    void setEnabled(bool on) { m_enabled = on; }

    /* The GUI-only preference, in QSettings — the core has no key for it and
     * the terminal client cannot use one. On by default. */
    static bool enabledSetting();
    static void setEnabledSetting(bool on);

signals:
    /* The notification was clicked. */
    void showWindowRequested();

private slots:
    void onActionInvoked(uint id, const QString &actionKey);
    void onNotificationClosed(uint id, uint reason);

private:
    void announceTalkers(const QStringList &names, quint32 tg, int extra);

    svx_app       *m_app = nullptr;
    bool           m_enabled = true;
    bool           m_subscribed = false;
    bool           m_primed = false;   /* the first tick only learns the state */
    QSet<QString>  m_active;           /* "CALL@tg" of the talkers last seen   */

    /* The notification we last sent, so a new talker replaces it instead of
     * stacking up a column of cards during a net. */
    uint m_lastId = 0;
};

#endif
