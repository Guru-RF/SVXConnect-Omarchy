/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * A VU meter that owns its own repaint.
 *
 * This is a plain QWidget with a paintEvent rather than a QProgressBar for two
 * reasons. The obvious one is that a themed QProgressBar cannot be given a
 * green-yellow-red gradient reliably across desktop themes. The important one
 * is the repaint model: at 30 Hz, this widget must be the ONLY thing that
 * repaints. The macOS app's own comment records what happens otherwise —
 * observing the audio engine from the main view re-ran the whole body,
 * including a ForEach over sixty sessions, thirty times a second.
 *
 * So setLevel() calls update() on this widget and nothing else. It never
 * emits, and nothing above it in the tree re-lays out.
 *
 * BALLISTICS LIVE IN THE CALLER, NOT HERE — deliberately, and there is exactly
 * one stage of smoothing in the whole chain. app_mic_level() and
 * app_spk_level() return a single buffer's peak: jumpy while live, and frozen
 * at the last value once the stream stops, which is why a raw reading leaves
 * the meter stuck after an over. The 33 ms tick applies instant-attack /
 * 0.75-decay and hands the result here. Adding a second attack/release filter
 * inside this widget would cascade two time constants and produce a meter that
 * visibly trails speech.
 */
#ifndef SVXCONNECT_OMARCHY_LEVELMETER_H
#define SVXCONNECT_OMARCHY_LEVELMETER_H

#include <QWidget>

class LevelMeter : public QWidget {
    Q_OBJECT

public:
    explicit LevelMeter(QWidget *parent = nullptr);

    /* `level` is 0.0 .. 1.0, already ballistically smoothed. Repaints only
     * when the value moved enough to be visible. */
    void setLevel(float level);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    float m_level = 0.0f;
};

#endif
