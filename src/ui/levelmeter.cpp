/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 */
#include "ui/levelmeter.h"
#include "ui/theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <cmath>

namespace {
constexpr int kHeight = 6;

/* Below this, a repaint would not change a pixel. At 30 Hz that saves a lot of
 * composited frames on an idle desktop. */
constexpr float kEpsilon = 0.004f;
} // namespace

LevelMeter::LevelMeter(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setFocusPolicy(Qt::NoFocus);
}

QSize LevelMeter::sizeHint() const        { return QSize(Theme::space(120), Theme::space(14)); }
QSize LevelMeter::minimumSizeHint() const { return QSize(Theme::space(40),  Theme::space(kHeight)); }

void LevelMeter::setLevel(float level)
{
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;

    if (std::fabs(level - m_level) < kEpsilon)
        return;

    m_level = level;
    update();
}

void LevelMeter::paintEvent(QPaintEvent *)
{
    const OmarchyTheme::Palette &pal = Theme::palette();
    const OmarchyTheme::Tokens  &tok = Theme::tokens();

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const qreal h = Theme::space(kHeight);
    const QRectF track(0, (height() - h) / 2.0, width(), h);

    /* The track is the shell's PanelSlider track: the selected-fill wash of
     * the foreground, fully rounded. */
    QPainterPath clip;
    clip.addRoundedRect(track, h / 2.0, h / 2.0);
    p.fillPath(clip, Theme::wash(pal.foreground, tok.selectedFill));

    if (m_level <= 0.0f)
        return;

    /* The gradient spans the FULL track, not the filled portion. That is what
     * makes a given colour mean a given level: if it spanned the fill, a quiet
     * signal would still paint red at its own right-hand edge. */
    QLinearGradient g(track.left(), 0, track.right(), 0);
    g.setColorAt(0.00, Theme::meterLow());
    g.setColorAt(0.62, Theme::meterLow());
    g.setColorAt(0.82, Theme::meterMid());
    g.setColorAt(1.00, Theme::meterHigh());

    QRectF fill = track;
    fill.setWidth(track.width() * static_cast<qreal>(m_level));

    p.save();
    p.setClipPath(clip);
    p.fillRect(fill, g);
    p.restore();
}
