/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * Runs the SVXConnect-CLI core inside the Qt event loop.
 *
 * The core is written around poll(). app.h states the contract in one
 * sentence: "app_service() must be called every time round the loop even when
 * poll() returned nothing, because it also drives the timers."
 * SVXConnect-PIOS folds this into a custom GSource; Qt gets a set of
 * QSocketNotifiers plus one single-shot QTimer, both funnelling into one slot.
 *
 * Three facts drive the design, and each has a specific known failure mode.
 *
 * 1. THE FD SET IS DYNAMIC. rc_poll_fds() always returns the connect worker's
 *    wake pipe, and adds the TLS and UDP sockets only while a connection
 *    exists — which the worker opens and closes as it reconnects. The events
 *    mask is dynamic too: POLLOUT appears on the TLS fd only while
 *    tls_want_write().
 *
 * 2. THE TIMER MUST BE AN ABSOLUTE DEADLINE, RE-ARMED EVERY PASS — not "ask
 *    again how long". PIOS's appsource.c documents the failure verbatim: while
 *    transmitting, app_next_timeout_ms() never reaches 0 because the core keeps
 *    asking for a short poll to drain the mic ring. A naive implementation then
 *    only ever services on inbound fd activity, so RECEIVE WORKS AND TRANSMIT
 *    SILENTLY NEVER SENDS. QTimer::start(ms) is a real deadline, which makes
 *    this correct by construction.
 *
 *    Related, and worth knowing before someone "optimises" it: app.c clamps the
 *    timeout to 20 ms whenever audio is ready, NOT only while connected. An
 *    idle, disconnected, tray-resident SVXConnect therefore wakes at 50 Hz
 *    forever. That is deliberate — it is the jitter buffer's tick — and
 *    removing it breaks playback.
 *
 * 3. QSocketNotifier IS LEVEL-TRIGGERED AND MUST NEVER BE LEFT ENABLED ON A
 *    CLOSED FD, or the event loop spins at 100% on POLLNVAL.
 *
 * ON NOT DESTROYING NOTIFIERS
 * ---------------------------
 * The obvious implementation rebuilds the notifier set whenever the fd set
 * changes, with deleteLater() for safety. That has a real, reachable bug.
 * deleteLater() defers to the event-loop level at which it was called. Open a
 * modal dialog (Preferences, a QMessageBox), let the connection drop and
 * reconnect while it is up, and the pending DeferredDelete events do not run
 * until the nested loop exits — by which time reconcile() has already created a
 * replacement notifier on the same fd NUMBER, because the kernel reuses the
 * lowest free descriptor after a close(). Qt then prints
 *
 *     QSocketNotifier: Multiple socket notifiers for same socket 9 and type Read
 *
 * and behaviour is undefined.
 *
 * So notifiers are never destroyed on the reconcile path. They are keyed by fd
 * number, created on first sight, and thereafter only enabled and disabled. A
 * disabled notifier on a closed fd is inert and costs nothing. Entries that
 * have been absent for a long time are pruned on a slow maintenance timer that
 * cannot be running inside a notifier's own emit.
 */
#ifndef SVXCONNECT_OMARCHY_CORELOOP_H
#define SVXCONNECT_OMARCHY_CORELOOP_H

#include <QObject>
#include <QHash>
#include <QTimer>

#include "core/svxcore.h"

class QSocketNotifier;

class CoreLoop : public QObject {
    Q_OBJECT

public:
    /* `app` must already exist; app_start() may be called before or after
     * construction, but see main.cpp for why the observer wants to be
     * installed first. */
    explicit CoreLoop(svx_app *app, QObject *parent = nullptr);
    ~CoreLoop() override;

    /* Service once immediately and arm the loop. Call once, after app_start(). */
    void kick();

signals:
    /* Coalesced "something changed, consider repainting". Always delivered
     * queued, never from inside a service pass — see serviceOnce(). This is
     * only a hint: the core's observer is nearly inert (it does not fire on
     * connection state, talker start/stop or node join/leave), so the UI polls
     * on a timer regardless. */
    void coreChanged();

    /* app_should_quit() went true — the control FIFO's "quit", or app_quit(). */
    void quitRequested();

private slots:
    void serviceOnce();
    void pruneStaleNotifiers();

private:
    struct FdEntry {
        QSocketNotifier *read  = nullptr;
        QSocketNotifier *write = nullptr;
        int              absentPasses = 0;
    };

    void reconcile();
    void armTimer();
    void noteSpin();

    /* The core's observer callback. A static member rather than a friend free
     * function: a friend declaration inside the class does not make the name
     * findable by ordinary lookup, and there is no ADL on void*. */
    static void observerTrampoline(void *user);

    svx_app             *m_app = nullptr;
    QTimer               m_timer;
    QTimer               m_prune;
    QHash<int, FdEntry>  m_fds;

    bool    m_dirty     = false;   /* set by the observer, on this thread */
    bool    m_inService = false;   /* re-entrancy guard */
    bool    m_pending   = false;   /* work arrived while in service */
    bool    m_quitEmitted = false;

    /* Spin detection */
    int     m_spinCount = 0;
    qint64  m_spinWindowStart = 0;
    bool    m_spinWarned = false;
};

#endif
