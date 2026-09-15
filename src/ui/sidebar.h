/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * The left column: the selected talkgroup and its lock, the switchable
 * talkgroups, and the audio section — two level meters, mute and volume.
 */
#ifndef SVXCONNECT_OMARCHY_SIDEBAR_H
#define SVXCONNECT_OMARCHY_SIDEBAR_H

#include <QWidget>
#include <QVector>

#include "core/svxcore.h"

class QLabel;
class QToolButton;
class QSlider;
class QVBoxLayout;
class LevelMeter;
class TalkgroupButton;

class Sidebar : public QWidget {
    Q_OBJECT

public:
    explicit Sidebar(svx_app *app, QWidget *parent = nullptr);

    /* 100 ms: talkgroup selection, mute, traffic, ages, lock, volume. */
    void tickModel(quint64 nowMs);

    /* 33 ms: meters only. Separate entry point so the fast path touches
     * nothing but two LevelMeter::setLevel() calls. */
    void tickMeters();

    /* Re-read the configured talkgroups and rebuild the button list. Call on
     * startup and after a settings change, never on a tick. */
    void rebuildTalkgroups();

private:
    void buildUi();
    void applyLockVisuals(bool locked);
    void applyMuteVisuals(bool muted);

    svx_app *m_app = nullptr;

    QLabel      *m_activeTg  = nullptr;
    QLabel      *m_tgInfo    = nullptr;
    QLabel      *m_preempt   = nullptr;
    QToolButton *m_lock      = nullptr;
    QVBoxLayout *m_tgLayout  = nullptr;
    QWidget     *m_tgHost    = nullptr;
    LevelMeter  *m_micMeter  = nullptr;
    LevelMeter  *m_spkMeter  = nullptr;
    QToolButton *m_mute      = nullptr;
    QSlider     *m_volume    = nullptr;

    QVector<TalkgroupButton *> m_buttons;

    /* Signature of the talkgroup set the buttons were built from, so
     * rebuildTalkgroups() can be called freely and only does work when the
     * configuration genuinely changed. */
    QString m_tgSignature;

    bool m_lockInit  = false;
    bool m_lockState = false;
    bool m_muteInit  = false;
    bool m_muteState = false;

    float m_micVu = 0.0f;
    float m_spkVu = 0.0f;
};

#endif
