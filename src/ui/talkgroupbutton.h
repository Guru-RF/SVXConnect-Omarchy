/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * One row in the talkgroup sidebar.
 *
 * Carries five pieces of state at once — selected, muted, priority, someone
 * talking, when last heard — which is why it is a painted QAbstractButton
 * rather than a QPushButton wearing a stylesheet. A QSS rule set that covered
 * every combination of :checked, muted, and traffic would be longer than this
 * paintEvent and would fight the desktop theme at every step.
 *
 * The colours come from Theme:: and the surfaces from the palette, so it
 * inverts correctly in a dark theme without a second stylesheet.
 */
#ifndef SVXCONNECT_OMARCHY_TALKGROUPBUTTON_H
#define SVXCONNECT_OMARCHY_TALKGROUPBUTTON_H

#include <QAbstractButton>

class TalkgroupButton : public QAbstractButton {
    Q_OBJECT

public:
    TalkgroupButton(quint32 tg, int priority, QWidget *parent = nullptr);

    quint32 talkgroup() const { return m_tg; }

    /* Called from the 100 ms model tick. Each setter repaints only when the
     * value actually changed, so a quiet sidebar costs nothing. */
    void setMuted(bool muted);

    /* What the reflector calls this talkgroup, for the tooltip. The face stays
     * "TG 8": every SVXConnect client keeps names off the button itself,
     * because a name is as long as its sysop felt like making it. */
    void setName(const QString &name);
    void setTalker(const QString &callsign);   /* empty = nobody talking */
    void setLastHeard(quint64 ms);             /* 0 = never */
    void setPriority(int priority);

    /* The 1 Hz relative-time label has to be recomputed even when nothing
     * else changed, or "12s" sits there stale. Kept separate so the model tick
     * can call it without touching the other five properties. */
    void refreshAge(quint64 nowMs);

    QSize sizeHint() const override;

signals:
    void muteRequested(quint32 tg);

protected:
    void paintEvent(QPaintEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    quint32 m_tg;
    int     m_priority;
    bool    m_muted    = false;
    QString m_name;
    void refreshToolTip();
    bool    m_hover    = false;
    QString m_talker;
    quint64 m_lastHeard = 0;
    QString m_ageText;
};

#endif
