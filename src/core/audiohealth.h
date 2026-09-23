/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * Is audio actually moving? Answered from what the core does, not from what
 * it was asked to do.
 *
 * WHAT 0.1.13 GOT WRONG
 * ---------------------
 * At 19:49:38 the capture stream stopped delivering samples while miniaudio
 * still reported it started. The core logged "TX ON", sent nothing — "TX OFF
 * 8 s, 0 frames", four times over — and the interface said the opposite on
 * every surface it has: the PTT button red with TRANSMITTING, the tray dot
 * red, and the microphone meter frozen on the last peak of the previous over,
 * because app_mic_level() is written only by the capture callback and nothing
 * clears it. The operator retried for fifty seconds, disconnect and reconnect
 * included, before restarting. The same thing happened to playback in the
 * session before it: the jitter buffer sat full at 512 ms for hours with
 * nothing draining it, and the speaker meter held its last value.
 *
 * app_tx_active() says the transmitter is KEYED. It does not say anything is
 * being sent. The core exposes no frame counter, but it does count every UDP
 * datagram that leaves (rc_stats.tx_packets), and while keyed that is 50
 * audio frames a second plus a heartbeat every ten seconds. So the packet
 * counter is the ground truth for "on the air", and TxMonitor turns it into
 * the four states the interface draws.
 *
 * Everything here is pure — samples in, verdicts out, time passed in — so
 * tests/test_audiohealth.cpp can replay the 19:49 over frame by frame.
 */
#ifndef SVXCONNECT_OMARCHY_AUDIOHEALTH_H
#define SVXCONNECT_OMARCHY_AUDIOHEALTH_H

#include <QtGlobal>
#include <deque>
#include <utility>

class TxMonitor {
public:
    enum class State {
        Idle,      /* not keyed                                              */
        Keying,    /* keyed, first frames not out yet (the first few 10 ms)  */
        OnAir,     /* keyed and audio frames are leaving                     */
        NoAudio    /* keyed and NOTHING is leaving — the 0.1.13 hang         */
    };

    /* How long keyed-but-silent is tolerated before it is NoAudio: the first
     * frame needs 20 ms of capture plus a service pass, and a device start can
     * take a little longer than that. */
    static constexpr qint64 kGraceMs = 400;

    /* How long NoAudio lasts before the interface acts on it (un-key, reopen
     * the microphone). Longer than a core-side watchdog would need, so that
     * one gets the first go if the core ever grows its own. */
    static constexpr qint64 kStallMs = 1500;

    /* "Frames are leaving" = at least this many datagrams in this window. A
     * heartbeat or the end-of-over flush is one datagram, never three. */
    static constexpr qint64 kFlowWindowMs = 300;
    static constexpr int    kFlowPackets  = 3;

    /* Feed app_tx_active() and rc_stats.tx_packets. Call as often as the
     * interface ticks; every call is cheap. */
    void sample(bool txActive, quint64 txPackets, qint64 nowMs);

    State state() const { return m_state; }

    /* Frames left within the flow window: what the microphone meter gates on,
     * so it falls to zero instead of holding a stale peak. */
    bool audioFlowing() const { return m_flowing; }

    /* True exactly once per over, the first time NoAudio has lasted kStallMs. */
    bool takeStall();

    /* How long the current over has been keyed without audio leaving, 0 when
     * it is not. For the log line. */
    qint64 silentForMs() const { return m_silentFor; }

private:
    State   m_state = State::Idle;
    bool    m_wasActive = false;
    bool    m_flowing = false;
    bool    m_everFlowed = false;
    bool    m_stallPending = false;
    bool    m_stallTaken = false;
    qint64  m_keyedAt = 0;
    qint64  m_lastFlowAt = 0;
    qint64  m_silentFor = 0;
    quint64 m_lastPackets = 0;

    /* (time, packets) over the last kFlowWindowMs. */
    std::deque<std::pair<qint64, quint64>> m_window;
};

/* Playback that has stopped draining.
 *
 * The playback callback is what empties the jitter buffer's ring; the jitter
 * buffer keeps it near its 80 ms target and drops above 300 ms. With the
 * callback dead nothing drains and nothing drops, so the ring sits full —
 * 512 ms, the "512 ms buffered" of every stats line in the 22 September log.
 * A ring that stays over kFullMs for kStallMs is that, and nothing else. */
class PlaybackMonitor {
public:
    static constexpr quint32 kFullMs   = 450;
    static constexpr qint64  kStallMs  = 3000;

    /* One recovery attempt a minute at most: reopening the output takes a
     * second or two and must not become a loop if the device stays dead. */
    static constexpr qint64  kRetryMs  = 60'000;

    void sample(bool audioReady, quint32 bufferedMs, qint64 nowMs);

    /* True once when the ring has been full for kStallMs, then not again
     * until it has drained and filled again, or kRetryMs has passed. */
    bool takeStall();

    bool stalled() const { return m_fullSince >= 0 && m_stalledNow; }

private:
    qint64 m_fullSince = -1;
    qint64 m_lastAction = -kRetryMs;
    bool   m_stalledNow = false;
    bool   m_pending = false;
};

/* A level meter fed by a value that only an audio callback writes will hold
 * its last value for ever when that callback stops. A real signal's peak is
 * never bit-identical for long; a stuck one is. After kStaleMs of the same
 * non-zero value, report silence so the meter decays. */
class StaleLevel {
public:
    static constexpr qint64 kStaleMs = 250;

    float filter(float raw, qint64 nowMs);

private:
    float  m_last = 0.0f;
    qint64 m_since = 0;
};

#endif
