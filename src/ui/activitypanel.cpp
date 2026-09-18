/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 */
#include "ui/activitypanel.h"
#include "ui/theme.h"
#include "ui/timefmt.h"
#include "net/reflectorfeed.h"
#include "net/portalinfo.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QDateTime>
#include <functional>

namespace {

/* A row that reports clicks without a QListView and a delegate.
 *
 * A plain std::function rather than a Q_OBJECT signal, so the class can live
 * here in the .cpp with no moc pass. AUTOMOC only scans headers and files that
 * #include a moc output, so a Q_OBJECT here would compile and then fail to
 * link on the vtable.
 *
 * Hover is the stylesheet's `*[role="row"]:hover`, which needs both
 * WA_StyledBackground (a plain QWidget paints no stylesheet background without
 * it) and WA_Hover (or :hover never matches). */
class ClickableRow : public QWidget {
public:
    ClickableRow(quint32 tg, QWidget *parent) : QWidget(parent), m_tg(tg)
    {
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_StyledBackground, true);
        setAttribute(Qt::WA_Hover, true);
        Theme::setRole(this, "row");
    }

    quint32 tg() const { return m_tg; }

    std::function<void()> onClicked;

protected:
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton
            && rect().contains(e->position().toPoint())
            && onClicked)
            onClicked();
        QWidget::mouseReleaseEvent(e);
    }

private:
    quint32 m_tg;
};

QLabel *labelWithRole(const QString &text, const char *role, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    Theme::setRole(l, role);
    return l;
}

} // namespace

ActivityPanel::ActivityPanel(svx_app *app, QWidget *parent) : QWidget(parent), m_app(app)
{
    buildUi();
}

void ActivityPanel::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(Theme::space(12), Theme::space(12), Theme::space(12), Theme::space(12));
    root->setSpacing(Theme::space(4));

    /* ---- active ---- */
    auto *localHead = new QHBoxLayout;
    localHead->setSpacing(Theme::space(6));
    m_localHeader = labelWithRole(tr("Active"), "section", this);
    localHead->addWidget(m_localHeader);

    m_localBadge = labelWithRole(QString(), "badge", this);
    m_localBadge->hide();
    localHead->addWidget(m_localBadge);
    localHead->addStretch(1);
    root->addLayout(localHead);

    m_localLayout = new QVBoxLayout;
    m_localLayout->setContentsMargins(0, 0, 0, 0);
    m_localLayout->setSpacing(Theme::space(2));
    root->addLayout(m_localLayout);

    m_localEmpty = labelWithRole(tr("Not connected"), "empty", this);
    m_localEmpty->setAlignment(Qt::AlignCenter);
    root->addWidget(m_localEmpty);

    root->addSpacing(Theme::space(12));

    /* ---- reflector: the enhanced feed's 24 hours ----
     * Built whether or not a feed exists, and shown only while one is up. It
     * carries every talkgroup the reflector saw, including the ones this
     * client does not monitor and everything that happened before it
     * connected — neither of which the core can know. */
    m_reflectorBox = new QWidget(this);
    auto *reflectorRoot = new QVBoxLayout(m_reflectorBox);
    reflectorRoot->setContentsMargins(0, 0, 0, 0);
    reflectorRoot->setSpacing(Theme::space(4));

    auto *reflectorHead = new QHBoxLayout;
    reflectorHead->setSpacing(Theme::space(6));
    reflectorHead->addWidget(labelWithRole(tr("Reflector"), "section", m_reflectorBox));
    auto *reflectorBadge = labelWithRole(tr("24h"), "badge", m_reflectorBox);
    reflectorBadge->setToolTip(tr("Straight from the reflector's portal: every talkgroup it "
                                  "saw in the last 24 hours, not only the ones you monitor."));
    reflectorHead->addWidget(reflectorBadge);
    reflectorHead->addStretch(1);
    reflectorRoot->addLayout(reflectorHead);

    m_reflectorLayout = new QVBoxLayout;
    m_reflectorLayout->setContentsMargins(0, 0, 0, 0);
    m_reflectorLayout->setSpacing(Theme::space(2));
    reflectorRoot->addLayout(m_reflectorLayout);

    m_reflectorEmpty = labelWithRole(tr("Nothing heard in the last 24 hours"), "empty", m_reflectorBox);
    m_reflectorEmpty->setAlignment(Qt::AlignCenter);
    reflectorRoot->addWidget(m_reflectorEmpty);

    m_reflectorBox->hide();
    root->addWidget(m_reflectorBox);

    /* ---- recent: the same question, answered locally ---- */
    m_recentBox = new QWidget(this);
    auto *recentRoot = new QVBoxLayout(m_recentBox);
    recentRoot->setContentsMargins(0, 0, 0, 0);
    recentRoot->setSpacing(Theme::space(4));

    m_recentHeader = labelWithRole(tr("Recent"), "section", m_recentBox);
    recentRoot->addWidget(m_recentHeader);

    /* Locking and muting are not display filters: both take the talkgroup out
     * of the subscription sent to the reflector, so nothing from it arrives at
     * all — no audio, and no activity to list here. That is worth saying,
     * because an empty Recent otherwise looks like a quiet net. */
    m_scopeHint = labelWithRole(QString(), "hint", m_recentBox);
    Theme::setTone(m_scopeHint, "warn");
    m_scopeHint->setWordWrap(true);
    m_scopeHint->hide();
    recentRoot->addWidget(m_scopeHint);

    m_recentLayout = new QVBoxLayout;
    m_recentLayout->setContentsMargins(0, 0, 0, 0);
    m_recentLayout->setSpacing(Theme::space(2));
    recentRoot->addLayout(m_recentLayout);

    m_recentEmpty = labelWithRole(tr("Nothing heard yet"), "empty", m_recentBox);
    m_recentEmpty->setAlignment(Qt::AlignCenter);
    recentRoot->addWidget(m_recentEmpty);

    root->addWidget(m_recentBox);

    root->addStretch(1);
}

ActivityPanel::Row ActivityPanel::makeRow(const QString &callsign, quint32 tg,
                                          bool live, quint64 stamp)
{
    auto *w = new ClickableRow(tg, this);
    w->onClicked = [this, tg]() { if (tg) emit talkgroupChosen(tg); };
    /* The chip on the row stays "TG 8" — it is a column, and names are as long
     * as their sysop felt like — but the tooltip has room for what it is. */
    const QString tgName = m_portal ? m_portal->talkgroupName(tg) : QString();
    w->setToolTip(tgName.isEmpty() ? tr("Switch to TG %1").arg(tg)
                                   : tr("Switch to TG %1 — %2").arg(tg).arg(tgName));

    auto *lay = new QHBoxLayout(w);
    lay->setContentsMargins(Theme::space(8), Theme::space(5), Theme::space(8), Theme::space(5));
    lay->setSpacing(Theme::space(8));

    /* Live rows get a green dot, finished rows a faint one, so the two states
     * are distinguishable without reading the time column. */
    auto *dot = new QLabel(w);
    Theme::setRole(dot, "dot");
    if (live)
        Theme::setTone(dot, "ok");
    dot->setFixedSize(Theme::space(8), Theme::space(8));
    lay->addWidget(dot);

    auto *call = labelWithRole(callsign, "call", w);
    Theme::setProp(call, "live", live);
    lay->addWidget(call);

    lay->addWidget(labelWithRole(tr("TG %1").arg(tg), "chip", w));

    lay->addStretch(1);

    auto *time = labelWithRole(QString(), "time", w);
    time->setMinimumWidth(Theme::space(52));
    time->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    lay->addWidget(time);

    return Row{w, time, stamp, live};
}

void ActivityPanel::rebuildLocal(quint64 nowMs)
{
    const tg_manager *tgm = app_tgm(m_app);

    QString sig;
    for (int i = 0; i < tgm->n_active; ++i)
        sig += QStringLiteral("%1@%2,").arg(QString::fromUtf8(tgm->active[i].full))
                                        .arg(tgm->active[i].tg);

    if (sig != m_localSig) {
        m_localSig = sig;
        for (const Row &r : std::as_const(m_localRows))
            r.widget->deleteLater();
        m_localRows.clear();

        for (int i = 0; i < tgm->n_active; ++i) {
            const tgm_talker &t = tgm->active[i];
            /* .full, not .call: ON6URE-TPAD and ON6URE-PI are different
             * stations, and a list that renders both as "ON6URE" cannot tell
             * you which one is on the air. */
            Row r = makeRow(QString::fromUtf8(t.full), t.tg, true, t.start_ms);
            m_localLayout->addWidget(r.widget);
            m_localRows.append(r);
        }

        const int n = m_localRows.size();
        m_localBadge->setText(QString::number(n));
        m_localBadge->setVisible(n > 0);

        const bool connected = rc_get_state(app_rc(m_app)) == RC_CONNECTED;
        m_localEmpty->setText(connected ? tr("No active traffic") : tr("Not connected"));
        m_localEmpty->setVisible(n == 0);
    }

    /* Live rows count up, so the time label is refreshed every tick even when
     * the row set did not change. */
    for (const Row &r : std::as_const(m_localRows))
        r.time->setText(TimeFmt::elapsed(r.stamp, nowMs));
}

void ActivityPanel::rebuildRecent(quint64 nowMs)
{
    const tg_manager *tgm = app_tgm(m_app);

    QString sig;
    for (int i = 0; i < tgm->n_recent; ++i)
        sig += QStringLiteral("%1@%2@%3,").arg(QString::fromUtf8(tgm->recent[i].full))
                                           .arg(tgm->recent[i].tg)
                                           .arg(tgm->recent[i].stop_ms);

    if (sig != m_recentSig) {
        m_recentSig = sig;
        for (const Row &r : std::as_const(m_recentRows))
            r.widget->deleteLater();
        m_recentRows.clear();

        for (int i = 0; i < tgm->n_recent; ++i) {
            const tgm_recent &t = tgm->recent[i];
            Row r = makeRow(QString::fromUtf8(t.full), t.tg, false, t.stop_ms);
            m_recentLayout->addWidget(r.widget);
            m_recentRows.append(r);
        }
        m_recentEmpty->setVisible(m_recentRows.isEmpty());
    }

    for (const Row &r : std::as_const(m_recentRows))
        r.time->setText(TimeFmt::ago(r.stamp, nowMs));
}

void ActivityPanel::setFeed(ReflectorFeed *feed)
{
    m_feed = feed;
}

void ActivityPanel::setPortalInfo(PortalInfo *portal)
{
    m_portal = portal;
    if (!m_portal)
        return;
    /* Rows are built once and kept, so names arriving later need a rebuild. */
    connect(m_portal, &PortalInfo::changed, this, [this]() {
        m_localSig.clear();
        m_recentSig.clear();
        m_reflectorSig.clear();
    });
}

void ActivityPanel::rebuildReflector(quint64 nowMs)
{
    /* The feed keeps 60 sessions. The panel scrolls, so showing more than the
     * core's own 16 costs nothing but rows — 30 is about as far back as anyone
     * scrolls looking for who was on. */
    constexpr int kMaxRows = 30;

    const QVector<ReflectorFeed::Session> &all = m_feed->sessions();
    const int n = qMin(all.size(), kMaxRows);

    QString sig;
    for (int i = 0; i < n; ++i)
        sig += QStringLiteral("%1@%2@%3@%4,").arg(all[i].callsign)
                                             .arg(all[i].tg)
                                             .arg(all[i].lastActivityMs())
                                             .arg(all[i].active ? 1 : 0);

    if (sig != m_reflectorSig) {
        m_reflectorSig = sig;
        for (const Row &r : std::as_const(m_reflectorRows))
            r.widget->deleteLater();
        m_reflectorRows.clear();

        for (int i = 0; i < n; ++i) {
            const ReflectorFeed::Session &s = all[i];
            Row r = makeRow(s.callsign, quint32(qMax(0, s.tg)), s.active,
                            quint64(s.active ? s.startMs : s.endMs));
            if (!s.location.isEmpty()) {
                const QString tgName = (m_portal && s.tg > 0)
                                     ? m_portal->talkgroupName(quint32(s.tg)) : QString();
                const QString tgText = tgName.isEmpty()
                                     ? tr("TG %1").arg(s.tg)
                                     : tr("TG %1 — %2").arg(s.tg).arg(tgName);
                r.widget->setToolTip(s.tg > 0
                    ? tr("%1 — %2. Click to switch to %3.").arg(s.callsign, s.location, tgText)
                    : tr("%1 — %2").arg(s.callsign, s.location));
            }
            m_reflectorLayout->addWidget(r.widget);
            m_reflectorRows.append(r);
        }
        m_reflectorEmpty->setVisible(m_reflectorRows.isEmpty());
    }

    /* The feed's timestamps are wall-clock epoch milliseconds, while the core's
     * are a monotonic clock — so these rows are aged against the wall clock,
     * not against the tick's `nowMs`. Mixing the two is how a session that
     * ended a minute ago ends up claiming it happened in 1970. */
    Q_UNUSED(nowMs);
    const quint64 epochNow = quint64(QDateTime::currentMSecsSinceEpoch());
    for (const Row &r : std::as_const(m_reflectorRows))
        r.time->setText(r.live ? TimeFmt::elapsed(r.stamp, epochNow)
                               : TimeFmt::ago(r.stamp, epochNow));
}

void ActivityPanel::tickModel(quint64 nowMs)
{
    /* One history section at a time. The feed's is better in every way — it
     * sees talkgroups this client never subscribed to — so when it is up, the
     * local one goes away rather than sitting underneath saying less. */
    const bool enhanced = m_feed && m_feed->isAvailable();
    m_reflectorBox->setVisible(enhanced);
    m_recentBox->setVisible(!enhanced);

    if (enhanced)
        rebuildReflector(nowMs);

    if (!m_app) return;
    rebuildLocal(nowMs);

    if (!enhanced) {
        rebuildRecent(nowMs);
        /* Locking and muting only narrow what this CLIENT receives, which is
         * why the warning belongs to the local list alone. */
        refreshScopeHint();
    }
}

void ActivityPanel::refreshScopeHint()
{
    const tg_manager *tgm = app_tgm(m_app);
    const svx_config *cfg = app_config(m_app);

    QString text;
    if (tgm_locked(tgm)) {
        const uint32_t sel = tgm_selected(tgm);
        text = sel ? tr("Locked to TG %1 — no other talkgroup is received while it is locked.").arg(sel)
                   : tr("Locked — no talkgroup is received while it is locked.");
    } else {
        /* Muted talkgroups, named, so it is obvious which ones are silent. */
        QStringList muted;
        for (int i = 0; i < cfg->n_monitored; ++i) {
            const uint32_t id = cfg->monitored[i].id;
            if (tgm_is_muted(tgm, id) && !muted.contains(QString::number(id)))
                muted << QString::number(id);
        }
        for (int i = 0; i < cfg->n_switchable; ++i) {
            const uint32_t id = cfg->switchable[i].id;
            if (tgm_is_muted(tgm, id) && !muted.contains(QString::number(id)))
                muted << QString::number(id);
        }
        if (!muted.isEmpty())
            text = tr("TG %1 muted — not received, so nothing from it is listed.")
                       .arg(muted.join(QStringLiteral(", ")));
    }

    if (text.isEmpty()) {
        m_scopeHint->hide();
        return;
    }
    if (m_scopeHint->text() != text)
        m_scopeHint->setText(text);
    m_scopeHint->show();
}
