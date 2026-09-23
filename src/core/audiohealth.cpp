/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * See audiohealth.h.
 */
#include "core/audiohealth.h"

/* ---------------------------------------------------------------- TX */

void TxMonitor::sample(bool txActive, quint64 txPackets, qint64 nowMs)
{
    /* The counter restarts with the connection. Treat a step backwards as a
     * fresh baseline rather than as a huge burst of frames. */
    if (txPackets < m_lastPackets)
        m_window.clear();
    m_lastPackets = txPackets;

    if (!txActive) {
        m_state = State::Idle;
        m_wasActive = false;
        m_flowing = false;
        m_silentFor = 0;
        m_stallPending = false;
        m_window.clear();
        m_window.emplace_back(nowMs, txPackets);
        return;
    }

    if (!m_wasActive) {
        /* Key-up. The window restarts here so a heartbeat sent just before
         * the over cannot count towards its first frames. */
        m_wasActive = true;
        m_everFlowed = false;
        m_stallTaken = false;
        m_stallPending = false;
        m_keyedAt = nowMs;
        m_lastFlowAt = nowMs;
        m_window.clear();
    }

    /* Keep ONE sample at or beyond the window's edge as the reference. Dropping
     * everything older than the window would, after the GUI thread was busy
     * for a second, leave only the newest sample — no delta, "no audio" — for
     * an over that was sending the whole time. */
    m_window.emplace_back(nowMs, txPackets);
    while (m_window.size() > 2 && m_window[1].first <= nowMs - kFlowWindowMs)
        m_window.pop_front();

    m_flowing = m_window.back().second - m_window.front().second >= quint64(kFlowPackets);

    if (m_flowing) {
        m_everFlowed = true;
        m_lastFlowAt = nowMs;
        m_silentFor = 0;
        m_state = State::OnAir;
        return;
    }

    /* Keyed, nothing leaving. Measured from key-up if nothing ever left, else
     * from the last time something did — a capture stream can die mid-over
     * as well as before the first word. */
    m_silentFor = nowMs - m_lastFlowAt;
    if (m_silentFor < kGraceMs) {
        m_state = m_everFlowed ? State::OnAir : State::Keying;
        return;
    }

    m_state = State::NoAudio;
    if (m_silentFor >= kStallMs && !m_stallTaken)
        m_stallPending = true;
}

bool TxMonitor::takeStall()
{
    if (!m_stallPending)
        return false;
    m_stallPending = false;
    m_stallTaken = true;
    return true;
}

/* ---------------------------------------------------------- playback */

void PlaybackMonitor::sample(bool audioReady, quint32 bufferedMs, qint64 nowMs)
{
    if (!audioReady || bufferedMs < kFullMs) {
        m_fullSince = -1;
        m_stalledNow = false;
        return;
    }

    if (m_fullSince < 0)
        m_fullSince = nowMs;

    if (!m_stalledNow && nowMs - m_fullSince >= kStallMs) {
        m_stalledNow = true;
        if (nowMs - m_lastAction >= kRetryMs) {
            m_lastAction = nowMs;
            m_pending = true;
        }
    }
    /* Still full a whole retry period after the last attempt: try again. */
    if (m_stalledNow && !m_pending && nowMs - m_lastAction >= kRetryMs) {
        m_lastAction = nowMs;
        m_pending = true;
    }
}

bool PlaybackMonitor::takeStall()
{
    if (!m_pending)
        return false;
    m_pending = false;
    return true;
}

/* ------------------------------------------------------------ meters */

float StaleLevel::filter(float raw, qint64 nowMs)
{
    if (raw != m_last) {
        m_last = raw;
        m_since = nowMs;
        return raw;
    }
    if (raw != 0.0f && nowMs - m_since >= kStaleMs)
        return 0.0f;
    return raw;
}
