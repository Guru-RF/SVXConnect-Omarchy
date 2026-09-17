/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 */
#include "ui/activitypanel.h"
#include "ui/theme.h"
#include "ui/timefmt.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
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

    /* ---- recent ---- */
    m_recentHeader = labelWithRole(tr("Recent"), "section", this);
    root->addWidget(m_recentHeader);

    /* Locking and muting are not display filters: both take the talkgroup out
     * of the subscription sent to the reflector, so nothing from it arrives at
     * all — no audio, and no activity to list here. That is worth saying,
     * because an empty Recent otherwise looks like a quiet net. */
    m_scopeHint = labelWithRole(QString(), "hint", this);
    Theme::setTone(m_scopeHint, "warn");
    m_scopeHint->setWordWrap(true);
    m_scopeHint->hide();
    root->addWidget(m_scopeHint);

    m_recentLayout = new QVBoxLayout;
    m_recentLayout->setContentsMargins(0, 0, 0, 0);
    m_recentLayout->setSpacing(Theme::space(2));
    root->addLayout(m_recentLayout);

    m_recentEmpty = labelWithRole(tr("Nothing heard yet"), "empty", this);
    m_recentEmpty->setAlignment(Qt::AlignCenter);
    root->addWidget(m_recentEmpty);

    root->addStretch(1);
}

ActivityPanel::Row ActivityPanel::makeRow(const QString &callsign, quint32 tg,
                                          bool live, quint64 stamp)
{
    auto *w = new ClickableRow(tg, this);
    w->onClicked = [this, tg]() { if (tg) emit talkgroupChosen(tg); };
    w->setToolTip(tr("Switch to TG %1").arg(tg));

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

void ActivityPanel::tickModel(quint64 nowMs)
{
    if (!m_app) return;
    rebuildLocal(nowMs);
    rebuildRecent(nowMs);
    refreshScopeHint();
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
