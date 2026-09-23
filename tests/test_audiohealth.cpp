/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * TxMonitor, PlaybackMonitor and StaleLevel, fed the way the window feeds
 * them: app_tx_active() and rc_stats.tx_packets every 33 ms.
 *
 * The case that matters is 19:49:38 in the 0.1.13 field log: keyed, "TX ON",
 * and 0 frames for eight seconds, with nothing but a UDP heartbeat leaving.
 * The interface said TRANSMITTING the whole time. Here it must say Keying,
 * then NoAudio, and ask for a stall action once — and never OnAir.
 */
#include <cstdio>

#include "core/audiohealth.h"

namespace {

int g_fail;
int g_run;

void check(bool ok, const char *what)
{
    ++g_run;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

/* A transmitter: keyed or not, and a datagram counter that advances one per
 * 20 ms frame while the microphone delivers. */
struct Radio {
    bool    keyed = false;
    bool    micAlive = true;
    quint64 packets = 1000;
    qint64  t = 0;
    qint64  frameDebt = 0;

    void advance(qint64 ms)
    {
        t += ms;
        if (keyed && micAlive) {
            frameDebt += ms;
            packets += quint64(frameDebt / 20);
            frameDebt %= 20;
        }
    }
};

} // namespace

int main()
{
    /* ---- a healthy over ---- */
    {
        TxMonitor m;
        Radio r;
        m.sample(r.keyed, r.packets, r.t);
        check(m.state() == TxMonitor::State::Idle, "healthy: idle before key-up");

        r.keyed = true;
        m.sample(r.keyed, r.packets, r.t);
        check(m.state() == TxMonitor::State::Keying, "healthy: Keying at the instant of key-up");

        bool onAir = false, stall = false, everNoAudio = false;
        for (int i = 0; i < 300; ++i) {          /* 10 s */
            r.advance(33);
            m.sample(r.keyed, r.packets, r.t);
            if (r.t >= 150 && m.state() == TxMonitor::State::OnAir) onAir = true;
            if (m.state() == TxMonitor::State::NoAudio) everNoAudio = true;
            stall |= m.takeStall();
        }
        check(onAir && m.state() == TxMonitor::State::OnAir, "healthy: OnAir while frames leave");
        check(m.audioFlowing(), "healthy: the mic meter is fed");
        check(!everNoAudio && !stall, "healthy: never NoAudio, never a stall");

        r.keyed = false;
        r.packets += 1;                          /* the flush */
        m.sample(r.keyed, r.packets, r.t);
        check(m.state() == TxMonitor::State::Idle && !m.audioFlowing(),
              "healthy: idle and meter gated after key-down");
    }

    /* ---- 19:49:38: keyed, the capture stream delivers nothing ---- */
    {
        TxMonitor m;
        Radio r;
        r.micAlive = false;
        m.sample(false, r.packets, r.t);
        r.keyed = true;

        int stalls = 0;
        bool everOnAir = false, everFlowing = false;
        TxMonitor::State at300 = TxMonitor::State::Idle, at600 = TxMonitor::State::Idle;
        qint64 stallAt = -1;
        for (int i = 0; i < 250; ++i) {          /* ~8 s, like the log */
            r.advance(33);
            if (r.t >= 5000 && r.t < 5033)
                r.packets += 1;                  /* a heartbeat is not audio */
            m.sample(r.keyed, r.packets, r.t);
            if (m.state() == TxMonitor::State::OnAir) everOnAir = true;
            if (m.audioFlowing()) everFlowing = true;
            if (r.t >= 300 && r.t < 333) at300 = m.state();
            if (r.t >= 600 && r.t < 633) at600 = m.state();
            if (m.takeStall()) {
                ++stalls;
                if (stallAt < 0) stallAt = r.t;
            }
        }
        check(!everOnAir, "hang: never shown as OnAir (the 0.1.13 TRANSMITTING)");
        check(!everFlowing, "hang: the mic meter is never fed a stale peak");
        check(at300 == TxMonitor::State::Keying, "hang: Keying within the start-up grace");
        check(at600 == TxMonitor::State::NoAudio, "hang: NoAudio once the grace is over");
        check(stalls == 1, "hang: exactly one stall action per over");
        check(stallAt >= TxMonitor::kStallMs && stallAt < TxMonitor::kStallMs + 100,
              "hang: the stall comes at kStallMs, not later");

        /* The next over gets its own verdict. */
        r.keyed = false;
        m.sample(false, r.packets, r.t);
        r.keyed = true;
        int again = 0;
        for (int i = 0; i < 80; ++i) {
            r.advance(33);
            m.sample(r.keyed, r.packets, r.t);
            again += m.takeStall() ? 1 : 0;
        }
        check(again == 1, "hang: a second dead over is caught too");
    }

    /* ---- the stream dies mid-over ---- */
    {
        TxMonitor m;
        Radio r;
        m.sample(false, r.packets, r.t);
        r.keyed = true;
        for (int i = 0; i < 30; ++i) { r.advance(33); m.sample(true, r.packets, r.t); }
        check(m.state() == TxMonitor::State::OnAir, "mid-over: OnAir before the stream dies");

        r.micAlive = false;
        const qint64 died = r.t;
        qint64 noAudioAt = -1, stallAt = -1;
        for (int i = 0; i < 100; ++i) {
            r.advance(33);
            m.sample(true, r.packets, r.t);
            if (noAudioAt < 0 && m.state() == TxMonitor::State::NoAudio) noAudioAt = r.t;
            if (stallAt < 0 && m.takeStall()) stallAt = r.t;
        }
        check(noAudioAt > died && noAudioAt - died < 1000, "mid-over: NoAudio within a second");
        check(stallAt > 0 && stallAt - died < 2200, "mid-over: a stall action follows");
    }

    /* ---- the GUI thread was busy: a long gap between samples ---- */
    {
        TxMonitor m;
        Radio r;
        m.sample(false, r.packets, r.t);
        r.keyed = true;
        for (int i = 0; i < 30; ++i) { r.advance(33); m.sample(true, r.packets, r.t); }
        r.advance(2000);                         /* a 2 s freeze; frames kept leaving */
        m.sample(true, r.packets, r.t);
        check(m.state() == TxMonitor::State::OnAir && !m.takeStall(),
              "gap: a stall of the interface is not a stall of the microphone");
    }

    /* ---- the counter restarts with a reconnect ---- */
    {
        TxMonitor m;
        m.sample(false, 5000, 0);
        m.sample(false, 3, 100);                 /* reconnected: counter from 0 */
        quint64 p = 3;
        qint64 t = 100;
        for (int i = 0; i < 20; ++i) { t += 20; p += 1; m.sample(true, p, t); }
        check(m.state() == TxMonitor::State::OnAir, "reset: an over after a reconnect is OnAir");
    }

    /* ---- playback ---- */
    {
        PlaybackMonitor p;
        int stalls = 0;
        for (qint64 t = 0; t < 30'000; t += 100) {
            p.sample(true, 80 + quint32(t % 200), t);    /* 80-280 ms: a working buffer */
            stalls += p.takeStall() ? 1 : 0;
        }
        check(stalls == 0, "playback: a working jitter buffer is never a stall");

        PlaybackMonitor q;
        int n = 0;
        qint64 firstAt = -1;
        for (qint64 t = 0; t < 30'000; t += 100) {
            q.sample(true, 512, t);                      /* the 22 September log */
            if (q.takeStall()) { ++n; if (firstAt < 0) firstAt = t; }
        }
        check(n == 1, "playback: a ring stuck full is one stall, not one per tick");
        check(firstAt >= PlaybackMonitor::kStallMs && firstAt < PlaybackMonitor::kStallMs + 200,
              "playback: after kStallMs");

        for (qint64 t = 30'000; t < 70'000; t += 100) {
            q.sample(true, 512, t);
            n += q.takeStall() ? 1 : 0;
        }
        check(n == 2, "playback: still dead after the retry period: one more attempt");

        PlaybackMonitor r;
        for (qint64 t = 0; t < 2000; t += 100) r.sample(true, 512, t);
        r.sample(false, 0, 2100);                        /* audio closed */
        int k = 0;
        for (qint64 t = 2200; t < 4000; t += 100) { r.sample(true, 512, t); k += r.takeStall() ? 1 : 0; }
        check(k == 0, "playback: the full time restarts when audio was reopened");
    }

    /* ---- meters ---- */
    {
        StaleLevel s;
        float out = 1.0f;
        for (qint64 t = 0; t <= 400; t += 33)
            out = s.filter(0.42f, t);
        check(out == 0.0f, "stale: a value no callback has refreshed decays to silence");

        StaleLevel v;
        bool passed = true;
        for (int i = 0; i < 30; ++i) {
            const float in = 0.3f + 0.01f * float(i % 7);
            passed &= v.filter(in, i * 33) == in;
        }
        check(passed, "stale: a live signal passes untouched");

        StaleLevel z;
        check(z.filter(0.0f, 0) == 0.0f && z.filter(0.0f, 1000) == 0.0f, "stale: silence stays silence");
    }

    std::printf("\n%d/%d passed\n", g_run - g_fail, g_run);
    return g_fail ? 1 : 0;
}
