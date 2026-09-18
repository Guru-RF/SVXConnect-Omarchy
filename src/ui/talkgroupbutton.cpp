/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 */
#include "ui/talkgroupbutton.h"
#include "ui/theme.h"
#include "ui/timefmt.h"

#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QMenu>
#include <QContextMenuEvent>
#include <QFontMetrics>
#include <cmath>

namespace {

constexpr int kRowHeight = 40;
constexpr int kPadX      = 8;
constexpr qreal kDotR    = 4.0;   /* 8 px dot */
constexpr qreal kHaloR   = 6.0;   /* + 2 px halo */

/* A five-pointed star, drawn rather than taken from an icon font: it needs to
 * be tinted with the theme's yellow and sit on the text baseline exactly. */
QPolygonF starPolygon(QPointF c, qreal r)
{
    QPolygonF poly;
    for (int i = 0; i < 10; ++i) {
        const qreal rr = (i % 2 == 0) ? r : r * 0.42;
        const qreal a  = -M_PI / 2.0 + i * M_PI / 5.0;
        poly << QPointF(c.x() + rr * std::cos(a), c.y() + rr * std::sin(a));
    }
    return poly;
}

} // namespace

TalkgroupButton::TalkgroupButton(quint32 tg, int priority, QWidget *parent)
    : QAbstractButton(parent), m_tg(tg), m_priority(priority)
{
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    /* No keyboard focus: Space is the window's transmit toggle, and a
     * focused button would take it and switch talkgroup instead. */
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    refreshToolTip();

    /* The row height follows the font size. */
    connect(&OmarchyTheme::get(), &OmarchyTheme::changed, this, [this]() { updateGeometry(); });
}

QSize TalkgroupButton::sizeHint() const
{
    return QSize(Theme::space(180), Theme::space(kRowHeight));
}

void TalkgroupButton::setMuted(bool muted)
{
    if (m_muted == muted) return;
    m_muted = muted;

    refreshToolTip();
    update();
}

void TalkgroupButton::setName(const QString &name)
{
    if (m_name == name) return;
    m_name = name;
    refreshToolTip();
}

void TalkgroupButton::refreshToolTip()
{
    /* Muting is not a volume control: the talkgroup leaves the subscription
     * sent to the reflector, so nothing from it arrives — no audio, and no
     * entries in Recent either. Say so, rather than leaving someone to wonder
     * why a talkgroup has been quiet all afternoon. */
    const QString hint = m_muted
        ? tr("TG %1 is muted: it is not received at all, so no activity from it "
             "is listed. Right-click to unmute.").arg(m_tg)
        : tr("Switch to TG %1. Right-click to mute.").arg(m_tg);

    /* The reflector's name for it first, when there is one — the way the
     * Windows client does it. */
    setToolTip(m_name.isEmpty() ? hint : m_name + QLatin1Char('\n') + hint);
}

void TalkgroupButton::setTalker(const QString &callsign)
{
    if (m_talker == callsign) return;
    m_talker = callsign;
    update();
}

void TalkgroupButton::setLastHeard(quint64 ms)
{
    if (m_lastHeard == ms) return;
    m_lastHeard = ms;
    update();
}

void TalkgroupButton::setPriority(int priority)
{
    if (m_priority == priority) return;
    m_priority = priority;
    update();
}

void TalkgroupButton::refreshAge(quint64 nowMs)
{
    const QString next = m_lastHeard ? TimeFmt::compactAge(m_lastHeard, nowMs) : QString();
    if (next == m_ageText) return;
    m_ageText = next;
    update();
}

void TalkgroupButton::enterEvent(QEnterEvent *) { m_hover = true;  update(); }
void TalkgroupButton::leaveEvent(QEvent *)      { m_hover = false; update(); }

void TalkgroupButton::contextMenuEvent(QContextMenuEvent *e)
{
    QMenu menu(this);
    QAction *mute = menu.addAction(m_muted ? tr("Unmute TG %1").arg(m_tg)
                                           : tr("Mute TG %1").arg(m_tg));
    if (menu.exec(e->globalPos()) == mute)
        emit muteRequested(m_tg);
}

void TalkgroupButton::paintEvent(QPaintEvent *)
{
    const OmarchyTheme::Palette &pal = Theme::palette();
    const OmarchyTheme::Tokens  &tok = Theme::tokens();

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal radius = tok.radius;
    const bool active = isChecked();
    const int padX = Theme::space(kPadX);

    /* Surface: the shell's Button.qml ladder — pressed, then hover, then
     * selected — with the hover-cursor border on hover and the active border
     * on keyboard focus. */
    QPainterPath path;
    path.addRoundedRect(r, radius, radius);

    if (isDown())
        p.fillPath(path, Theme::wash(pal.foreground, tok.pressedFill));
    else if (m_hover)
        p.fillPath(path, Theme::wash(pal.foreground, tok.hoverFill));
    else if (active)
        p.fillPath(path, Theme::wash(pal.foreground, tok.selectedFill));

    if (active && m_hover)
        p.fillPath(path, Theme::wash(pal.foreground, tok.selectedFill - tok.hoverFill));

    if (hasFocus() || m_hover) {
        QPen pen(hasFocus() ? tok.activeBorder : Theme::wash(pal.foreground, tok.hoverBorderAlpha));
        pen.setWidthF(1.0);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    }

    /* The selected row takes the menu's selected-text colour — the accent in
     * every stock theme — the same way the shell marks the current item. */
    QColor fg = active ? tok.menuSelectedText : pal.foreground;
    /* Muted rows dim rather than vanish — you still need to see that the
     * talkgroup exists and that it is the reason you are not hearing it. */
    if (m_muted)
        fg.setAlphaF(0.5f);

    /* ---- title ---- */
    QFont title = Theme::font(Theme::Font::Body, active);
    title.setStrikeOut(m_muted);
    p.setFont(title);
    p.setPen(fg);

    const QFontMetrics fm(title);
    const QString label = tr("TG %1").arg(m_tg);
    const int titleY = static_cast<int>(r.top()) + Theme::space(4) + fm.ascent();
    p.drawText(padX, titleY, label);

    int x = padX + fm.horizontalAdvance(label) + Theme::space(6);

    /* ---- priority stars, one per '+' ---- */
    if (m_priority > 0) {
        const qreal starR = Theme::space(5);
        p.setPen(Qt::NoPen);
        p.setBrush(m_muted ? Theme::wash(Theme::star(), 0.5) : Theme::star());
        for (int i = 0; i < m_priority; ++i) {
            p.drawPolygon(starPolygon(QPointF(x + starR, titleY - fm.xHeight() / 2.0 - 1), starR));
            x += Theme::space(12);
        }
        p.setBrush(Qt::NoBrush);
    }

    /* ---- traffic dot, right-aligned, with a halo of the window background so
     *      it reads against the selected surface as well ---- */
    if (!m_talker.isEmpty()) {
        const qreal haloR = Theme::space(kHaloR);
        const qreal dotR  = Theme::space(kDotR);
        const QPointF c(r.right() - padX - haloR, r.top() + haloR + Theme::space(6));
        p.setPen(Qt::NoPen);
        p.setBrush(pal.background);
        p.drawEllipse(c, haloR, haloR);
        p.setBrush(Theme::connected());
        p.drawEllipse(c, dotR, dotR);
        p.setBrush(Qt::NoBrush);
    }

    /* ---- sub-line: who is talking, else how long since anyone was ---- */
    const QFont sub = Theme::font(Theme::Font::Caption);
    p.setFont(sub);

    QColor subFg = m_talker.isEmpty() ? Theme::wash(fg, 0.62) : fg;
    p.setPen(subFg);

    const QFontMetrics sfm(sub);
    const int subY = static_cast<int>(r.bottom()) - Theme::space(6);

    const QString subText = !m_talker.isEmpty() ? m_talker : m_ageText;
    if (!subText.isEmpty()) {
        const int avail = static_cast<int>(r.width()) - padX * 2
                        - (m_muted ? Theme::space(18) : 0);
        p.drawText(padX, subY, sfm.elidedText(subText, Qt::ElideRight, avail));
    }

    /* ---- mute mark at the right of the sub-line, clear of the traffic dot ---- */
    if (m_muted) {
        const qreal gx = r.right() - padX - Theme::space(6);
        const qreal gy = subY - sfm.xHeight() / 2.0;
        const qreal k  = Theme::space(4);
        QPen pen(Theme::wash(pal.foreground, 0.62));
        pen.setWidthF(1.4);
        p.setPen(pen);
        p.drawLine(QPointF(gx - k, gy - k), QPointF(gx + k, gy + k));
        p.drawLine(QPointF(gx + k, gy - k), QPointF(gx - k, gy + k));
    }
}
