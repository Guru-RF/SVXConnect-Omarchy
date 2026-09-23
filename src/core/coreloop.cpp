/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * See coreloop.h for the design and the three failure modes it avoids.
 */
#include "core/coreloop.h"
#include "core/coreaction.h"

#include <QSocketNotifier>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QDebug>

#include <poll.h>
#include <array>

namespace {

/* app_poll_fds() is given this as its `max`. The core currently returns at
 * most four (wake pipe, TLS, UDP, control FIFO); 16 is headroom that costs a
 * stack array. */
constexpr int kMaxFds = 16;

/* Never sleep forever, even if the core ever returns something odd. */
constexpr int kMaxTimeoutMs = 1000;

/* Prune an fd entry once the core has not asked for it for this long. Measured
 * in time, not in passes: the core services every 5 ms while transmitting and
 * every 20 ms while idle, so a pass count means 3 s on one and 12 s on the
 * other — never the "minutes" it was meant to be. Minutes is the point: fd
 * numbers are recycled quickly, so an entry that has genuinely gone away is
 * rare and pruning it eagerly buys nothing. */
constexpr qint64 kPruneAfterMs = 5 * 60 * 1000;

/* Spin guard: more than this many service passes inside the window means
 * something is level-triggering without being drained. During transmit the
 * legitimate rate is roughly 20 timer wakeups plus ~5 UDP packets per 100 ms,
 * so this leaves an order of magnitude of headroom. */
constexpr int   kSpinLimit    = 200;
constexpr qint64 kSpinWindowMs = 100;

} // namespace

void CoreLoop::observerTrampoline(void *user)
{
    /* Fires SYNCHRONOUSLY on the GUI thread, from inside app_service() and
     * from inside app_set_volume() / app_toggle_output_mute() / banner_set()
     * and friends.
     *
     * Never emit from here. A slot that called back into any app_* function
     * would re-enter the core in the middle of an operation. Just mark dirty;
     * serviceOnce() queues exactly one coreChanged() afterwards. */
    static_cast<CoreLoop *>(user)->m_dirty = true;
}

CoreLoop::CoreLoop(svx_app *app, QObject *parent)
    : QObject(parent), m_app(app), m_pruneAfterMs(kPruneAfterMs)
{
    m_timer.setSingleShot(true);
    /* The 5-20 ms transmit cadence must not be coalesced with other timers.
     * PreciseTimer costs a real timerfd rather than a coarse one; that is the
     * intended trade. */
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &CoreLoop::serviceOnce);

    m_prune.setInterval(30'000);
    m_prune.setTimerType(Qt::VeryCoarseTimer);
    connect(&m_prune, &QTimer::timeout, this, &CoreLoop::pruneStaleNotifiers);
    m_prune.start();

    app_set_observer(m_app, &CoreLoop::observerTrampoline, this);

    /* Every interface call that can close a socket goes through CoreAction;
     * see coreaction.h for the window this closes. */
    CoreAction::setGuard([this]() { silenceNotifiers(); },
                         [this]() { afterAction(); });
}

CoreLoop::~CoreLoop()
{
    /* Detach from the core before our members die: app_free() logs, and
     * anything that reached the observer after this point would use a
     * half-destroyed object. */
    if (m_app)
        app_set_observer(m_app, nullptr, nullptr);
    CoreAction::setGuard({}, {});

    m_timer.stop();
    m_prune.stop();

    for (auto &e : m_fds) {
        if (e.read)  { e.read->setEnabled(false);  delete e.read;  }
        if (e.write) { e.write->setEnabled(false); delete e.write; }
    }
    m_fds.clear();
}

void CoreLoop::kick()
{
    serviceOnce();
}

void CoreLoop::silenceNotifiers()
{
    for (auto &e : m_fds) {
        if (e.read)  e.read->setEnabled(false);
        if (e.write) e.write->setEnabled(false);
    }
}

void CoreLoop::afterAction()
{
    /* Inside a service pass the pass itself reconciles on the way out. */
    if (m_inService)
        return;

    /* The call may have closed descriptors and started a connect worker with a
     * new wake pipe; watch exactly what the core now wants, straight away,
     * rather than leaving everything silenced until the timer fires. And re-arm
     * the timer: a PTT press wants the 5 ms transmit cadence now, not after
     * the idle 20 ms. */
    reconcile();
    armTimer();
}

void CoreLoop::serviceOnce()
{
    if (m_inService) {
        /* Do NOT drop the activation. The notifiers are level-triggered and
         * the timer re-arms, so we would recover — but only after a whole
         * timer period, which during transmit is a dropped 20 ms audio frame.
         * Remember it and run again at the end of the outer pass. */
        m_pending = true;
        return;
    }

    m_inService = true;

    do {
        m_pending = false;
        noteSpin();

        /* (1) Silence every notifier we hold before entering the core.
         *     app_service() can close and reopen sockets; a notifier left
         *     enabled on a closed fd spins on POLLNVAL. reconcile() re-enables
         *     exactly the set the core now wants. */
        silenceNotifiers();
        m_timer.stop();

        /* (2) UNCONDITIONAL. This is the whole contract. It drains the control
         *     FIFO, runs rc_service() (firing every reflector callback
         *     synchronously on this thread), ticks the jitter buffer and the
         *     talkgroup manager, pumps the microphone ring into Opus and onto
         *     the wire, and exports the status file. */
        app_service(m_app, now_ms());

        if (app_should_quit(m_app)) {
            m_inService = false;
            if (!m_quitEmitted) {
                m_quitEmitted = true;
                QMetaObject::invokeMethod(this, &CoreLoop::quitRequested,
                                          Qt::QueuedConnection);
            }
            return;
        }

        /* (3) The fd set may have changed underneath us. */
        reconcile();

        /* (4) Re-arm. now_ms() is deliberately sampled HERE, after
         *     app_service() has returned, not reused from step 2: a pass that
         *     blocked for tens of milliseconds inside status export or a
         *     device operation must not then over-sleep by that much. */
        armTimer();

    } while (m_pending);

    /* (5) Queue the repaint hint. It must land OUTSIDE this service frame: a
     *     slot connected to coreChanged() that opens a modal dialog spins a
     *     nested event loop, the timer fires inside it, and serviceOnce()
     *     would re-enter with the guard already cleared. Queuing it means the
     *     nested loop sees m_inService == true and defers via m_pending. */
    if (m_dirty) {
        m_dirty = false;
        QMetaObject::invokeMethod(this, &CoreLoop::coreChanged, Qt::QueuedConnection);
    }

    m_inService = false;
}

void CoreLoop::reconcile()
{
    struct pollfd want[kMaxFds];
    int n = app_poll_fds(m_app, want, kMaxFds);
    if (n < 0)
        n = 0;

    /* Driving setEnabled() from the computed desired set — rather than
     * "re-enable everything I still hold" — is what keeps this correct now
     * that entries outlive their membership of the set. */
    const qint64 now = qint64(now_ms());

    for (int i = 0; i < n; ++i) {
        const int fd = want[i].fd;
        if (fd < 0)
            continue;

        FdEntry &e = m_fds[fd];   /* default-constructs on first sight */
        e.lastWantedMs = now;

        /* The Read notifier is created UNCONDITIONALLY, even for an fd the
         * core asked to watch only for POLLOUT.
         *
         * Two reasons. First, Qt splits what poll() unifies and offers no
         * Exception notifier for POLLERR/POLLHUP — but the kernel reports
         * those regardless of the requested events, and a Read notifier wakes
         * on them, which is how we get what PIOS gets from its unconditional
         * G_IO_ERR | G_IO_HUP. Second, fail-safe beats fail-loud here: an
         * assertion would compile out under QT_NO_DEBUG, which is exactly the
         * build a user runs and exactly when a hang would matter. Today
         * rc_poll_fds() never sets POLLOUT without POLLIN, so the warning
         * below should never fire; if the core ever changes, we service the fd
         * anyway and say so. */
        if (!e.read) {
            e.read = new QSocketNotifier(fd, QSocketNotifier::Read, this);
            e.read->setEnabled(false);
            connect(e.read, &QSocketNotifier::activated, this, &CoreLoop::serviceOnce);
        }
        if (!(want[i].events & POLLIN)) {
            qWarning("CoreLoop: fd %d requested without POLLIN (events=0x%x); "
                     "servicing it via the Read notifier anyway", fd, want[i].events);
        }
        e.read->setEnabled(true);

        if (want[i].events & POLLOUT) {
            /* Appears and vanishes with tls_want_write(). */
            if (!e.write) {
                e.write = new QSocketNotifier(fd, QSocketNotifier::Write, this);
                e.write->setEnabled(false);
                connect(e.write, &QSocketNotifier::activated, this, &CoreLoop::serviceOnce);
            }
            e.write->setEnabled(true);
        } else if (e.write) {
            e.write->setEnabled(false);
        }
    }
}

void CoreLoop::armTimer()
{
    int to = app_next_timeout_ms(m_app, now_ms());
    if (to < 0)
        to = kMaxTimeoutMs;
    else if (to > kMaxTimeoutMs)
        to = kMaxTimeoutMs;

    m_timer.start(to);
}

void CoreLoop::noteSpin()
{
    static thread_local QElapsedTimer clock;
    if (!clock.isValid())
        clock.start();

    const qint64 nowEl = clock.elapsed();
    if (nowEl - m_spinWindowStart > kSpinWindowMs) {
        m_spinWindowStart = nowEl;
        m_spinCount = 0;
        m_spinWarned = false;
    }

    if (++m_spinCount <= kSpinLimit || m_spinWarned)
        return;

    m_spinWarned = true;

    /* Name the offending descriptors and what they are actually reporting.
     * A bare "spinning" line is not actionable; poll() revents tells you
     * immediately whether this is POLLNVAL on a closed fd (our bug) or a
     * readable fd the core is not draining (a core bug worth seeing). */
    QString detail;
    struct pollfd probe[kMaxFds];
    int n = 0;
    for (auto it = m_fds.constBegin(); it != m_fds.constEnd() && n < kMaxFds; ++it) {
        const bool enabled = (it->read && it->read->isEnabled())
                          || (it->write && it->write->isEnabled());
        if (enabled)
            probe[n++] = pollfd{it.key(), POLLIN | POLLOUT, 0};
    }
    if (n > 0 && ::poll(probe, static_cast<nfds_t>(n), 0) >= 0) {
        for (int i = 0; i < n; ++i)
            detail += QStringLiteral(" fd=%1 revents=0x%2")
                          .arg(probe[i].fd).arg(probe[i].revents, 0, 16);
    }

    qWarning("CoreLoop: %d service passes in %lld ms — possible level-triggered "
             "runaway.%s", m_spinCount, kSpinWindowMs, qPrintable(detail));
}

void CoreLoop::pruneStaleNotifiers()
{
    /* Runs from its own timer, so it is never nested inside a notifier's emit.
     * The guard covers the case where a future change makes serviceOnce() spin
     * a nested loop. */
    if (m_inService)
        return;

    const qint64 now = qint64(now_ms());
    for (auto it = m_fds.begin(); it != m_fds.end(); ) {
        if (now - it->lastWantedMs > m_pruneAfterMs) {
            delete it->read;
            delete it->write;
            it = m_fds.erase(it);
        } else {
            ++it;
        }
    }
}
