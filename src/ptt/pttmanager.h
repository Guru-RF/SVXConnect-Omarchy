/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * Owns the PTT backends and turns their edges into app_ptt().
 *
 * Presses are reference-counted rather than boolean, so that a second source
 * releasing while a first is still held does not drop the carrier mid-word.
 * Only the portal backend exists today; the counting is what makes adding
 * another (a foot switch, say) a change of one file rather than of this
 * class's logic.
 *
 * THE SAFETY RULES, WHICH ARE THE POINT OF THIS CLASS
 * ---------------------------------------------------
 * A missed release is an unattended transmitter. Three independent defences,
 * all required, none sufficient alone:
 *
 *   1. tx_timeout_sec in the core (120 s by default) hard-unkeys regardless of
 *      what any of this does. It is the backstop and must never default to 0.
 *   2. Any backend emitting lost() unkeys immediately. A control that has just
 *      told you it can no longer see the release is not a control.
 *   3. Every error path unkeys. If this class is ever unsure of the state, it
 *      stops transmitting.
 */
#ifndef SVXCONNECT_OMARCHY_PTTMANAGER_H
#define SVXCONNECT_OMARCHY_PTTMANAGER_H

#include <QObject>
#include <QVector>
#include <QHash>

#include "core/svxcore.h"
#include "ptt/pttbackend.h"

class PttManager : public QObject {
    Q_OBJECT

public:
    enum class Mode { Hold, Toggle };

    explicit PttManager(svx_app *app, QObject *parent = nullptr);

    /* With the backends given instead of the portal. Takes ownership. For the
     * tests, which drive the manager with a scripted backend and a stub core. */
    PttManager(svx_app *app, const QVector<PttBackend *> &backends, QObject *parent = nullptr);
    ~PttManager() override;

    /* Backends in preference order, for the settings table. Owned by this. */
    QVector<PttBackend *> backends() const { return m_backends; }
    PttBackend *backend(const QString &id) const;

    /* Switching mode while keyed un-keys: a press made under Hold is waiting
     * for a release that Toggle would ignore. */
    void setMode(Mode m);
    Mode mode() const    { return m_mode; }

    /* Start the keyboard binding. An invalid binding stops the backend rather
     * than erroring. Stopping or restarting the backend un-keys first: a
     * session that is torn down never delivers the release it owes. */
    void applyKeyboardBinding(const PttBinding &b);

    /* Unkey now, whatever the reason. Safe to call when not transmitting. */
    void forceUnkey(const char *why);

    /* The CORE's answer, not ours. The transmitter can be keyed by Space, the
     * mouse, the tray or the control FIFO, and un-keyed by the core itself —
     * transmit timeout, a dropped link, a talkgroup change — none of which
     * passes through here. */
    bool isKeyed() const { return m_app && app_tx_active(m_app) != 0; }

signals:
    /* A backend can no longer guarantee a release; the UI should say so. */
    void backendLost(const QString &backendId, const QString &why);
    void triggerChanged(const QString &backendId, const QString &human);

private:
    void wire(PttBackend *b);
    void onPressed(PttBackend *b);
    void onReleased(PttBackend *b);

    svx_app               *m_app = nullptr;
    QVector<PttBackend *>  m_backends;
    Mode                   m_mode = Mode::Hold;

    /* How many backends are currently holding the key down, in Hold mode.
     * Reference counted so a foot switch released while the hotkey is still
     * held does not unkey.
     *
     * There is deliberately no Toggle-mode equivalent. 0.1.13 kept its own
     * "latched" flag and sent ON or OFF from it; the core then ended an over by
     * itself ("the connection dropped", 19:50:30 in the field log) and the flag
     * still said keyed five seconds later, so the next press sent OFF to an
     * idle transmitter and did nothing. The reverse — keyed with Space, then
     * the global key sends ON — left the transmitter on. Toggle now asks the
     * core. */
    int  m_holders = 0;
};

#endif
