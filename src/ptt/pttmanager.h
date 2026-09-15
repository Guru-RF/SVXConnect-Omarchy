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
    ~PttManager() override;

    /* Backends in preference order, for the settings table. Owned by this. */
    QVector<PttBackend *> backends() const { return m_backends; }
    PttBackend *backend(const QString &id) const;

    void setMode(Mode m) { m_mode = m; }
    Mode mode() const    { return m_mode; }

    /* Start the keyboard binding. An invalid binding stops the backend rather
     * than erroring. */
    void applyKeyboardBinding(const PttBinding &b);

    /* Unkey now, whatever the reason. Safe to call when not transmitting. */
    void forceUnkey(const char *why);

    bool isKeyed() const { return m_holders > 0 || m_latched; }

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

    /* How many backends are currently holding the key down. Reference counted
     * so a foot switch released while the hotkey is still held does not unkey. */
    int  m_holders = 0;
    bool m_latched = false;   /* Toggle mode's own state */
};

#endif
