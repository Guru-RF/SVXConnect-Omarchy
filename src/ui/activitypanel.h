/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * Who is talking now, and who was talking recently.
 *
 * v1 has two sections, both fed from the core's talkgroup manager:
 *
 *   LOCAL    tg_manager::active[] — talkers on the talkgroups you monitor,
 *            as reported by the reflector over TCP. Live, counts up.
 *   RECENT   tg_manager::recent[] — the last 16 finished overs.
 *
 * There is a third, REFLECTOR: 24 hours of sessions from the enhanced
 * reflector's WebSocket feed (see net/reflectorfeed.h), which is the only
 * source of history from before you connected and of traffic on talkgroups you
 * do not monitor. It replaces RECENT whenever the feed is up, because the two
 * say the same thing and the feed says it better.
 *
 * The gating rule is deliberately not the macOS one: that app shows NEITHER
 * section when enhanced mode is on but the feed is down, which leaves an empty
 * panel for no reason. Here, no feed means the local Recent list, exactly as
 * before.
 *
 * Rows are rebuilt only when the content actually changes, keyed on a cheap
 * signature. The relative-time labels are refreshed in place every tick
 * without touching the row structure — that distinction is the whole reason
 * this is not a QListView with a model reset.
 */
#ifndef SVXCONNECT_OMARCHY_ACTIVITYPANEL_H
#define SVXCONNECT_OMARCHY_ACTIVITYPANEL_H

#include <QWidget>
#include <QVector>

#include "core/svxcore.h"

class QLabel;
class QVBoxLayout;
class ReflectorFeed;
class PortalInfo;

class ActivityPanel : public QWidget {
    Q_OBJECT

public:
    explicit ActivityPanel(svx_app *app, QWidget *parent = nullptr);

    void tickModel(quint64 nowMs);

    /* The enhanced reflector's feed, or null. Not owned. */
    void setFeed(ReflectorFeed *feed);

    /* Talkgroup names, for the row tooltips. Not owned. */
    void setPortalInfo(PortalInfo *portal);

signals:
    /* A row was clicked — switch to that talkgroup. */
    void talkgroupChosen(quint32 tg);

private:
    struct Row {
        QWidget *widget = nullptr;
        QLabel  *time   = nullptr;
        quint64  stamp  = 0;      /* start_ms for live, stop_ms for recent */
        bool     live   = false;
    };

    void buildUi();
    void rebuildLocal(quint64 nowMs);
    void rebuildRecent(quint64 nowMs);
    void rebuildReflector(quint64 nowMs);
    void refreshScopeHint();
    Row  makeRow(const QString &callsign, quint32 tg, bool live, quint64 stamp);

    svx_app *m_app = nullptr;

    QLabel      *m_localHeader  = nullptr;
    QLabel      *m_localBadge   = nullptr;
    QVBoxLayout *m_localLayout  = nullptr;
    QLabel      *m_localEmpty   = nullptr;

    /* Each history section is a container, so switching between them is one
     * setVisible() rather than a hunt for loose widgets. */
    QWidget     *m_recentBox    = nullptr;
    QWidget     *m_reflectorBox = nullptr;

    QLabel      *m_recentHeader = nullptr;
    /* Why a talkgroup is missing from the list: locking and muting both
     * unsubscribe, so nothing from those talkgroups is received at all. */
    QLabel      *m_scopeHint    = nullptr;
    QVBoxLayout *m_recentLayout = nullptr;
    QLabel      *m_recentEmpty  = nullptr;

    QVBoxLayout *m_reflectorLayout = nullptr;
    QLabel      *m_reflectorEmpty  = nullptr;

    QVector<Row> m_localRows;
    QVector<Row> m_recentRows;
    QVector<Row> m_reflectorRows;

    QString m_localSig;
    QString m_recentSig;
    QString m_reflectorSig;

    ReflectorFeed *m_feed   = nullptr;
    PortalInfo    *m_portal = nullptr;
};

#endif
