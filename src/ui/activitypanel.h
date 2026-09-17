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
 * The macOS app has a third, REFLECTOR: 24 hours of sessions from the enhanced
 * reflector's WebSocket feed, which is the only source of other nodes'
 * coordinates and of history from before you connected. That feed is beyond
 * v1, so the section does not exist yet. When it lands, this is where it goes,
 * and the gating rule to copy is: show Reflector when the feed is up, and fall
 * back to Recent when it is not — the macOS app shows NEITHER when enhanced
 * mode is on but the feed is down, which leaves an empty panel for no reason.
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

class ActivityPanel : public QWidget {
    Q_OBJECT

public:
    explicit ActivityPanel(svx_app *app, QWidget *parent = nullptr);

    void tickModel(quint64 nowMs);

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
    void refreshScopeHint();
    Row  makeRow(const QString &callsign, quint32 tg, bool live, quint64 stamp);

    svx_app *m_app = nullptr;

    QLabel      *m_localHeader  = nullptr;
    QLabel      *m_localBadge   = nullptr;
    QVBoxLayout *m_localLayout  = nullptr;
    QLabel      *m_localEmpty   = nullptr;

    QLabel      *m_recentHeader = nullptr;
    /* Why a talkgroup is missing from the list: locking and muting both
     * unsubscribe, so nothing from those talkgroups is received at all. */
    QLabel      *m_scopeHint    = nullptr;
    QVBoxLayout *m_recentLayout = nullptr;
    QLabel      *m_recentEmpty  = nullptr;

    QVector<Row> m_localRows;
    QVector<Row> m_recentRows;

    QString m_localSig;
    QString m_recentSig;
};

#endif
