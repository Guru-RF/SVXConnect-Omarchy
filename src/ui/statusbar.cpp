/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 */
#include "ui/statusbar.h"
#include "ui/theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QFontMetrics>
#include <algorithm>

namespace {

QLabel *dim(QWidget *parent)
{
    auto *l = new QLabel(parent);
    Theme::setRole(l, "dim");
    return l;
}

} // namespace

ConnectionBar::ConnectionBar(svx_app *app, QWidget *parent) : QWidget(parent), m_app(app)
{
    buildUi();
}

void ConnectionBar::buildUi()
{
    m_row = new QHBoxLayout(this);
    m_row->setContentsMargins(Theme::space(14), Theme::space(6), Theme::space(6), Theme::space(6));
    m_row->setSpacing(Theme::space(10));

    m_dot = new QLabel(this);
    Theme::setRole(m_dot, "dot");
    Theme::setTone(m_dot, "down");
    m_dot->setFixedSize(Theme::space(8), Theme::space(8));
    m_row->addWidget(m_dot);

    m_state = new QLabel(tr("Idle"), this);
    Theme::setRole(m_state, "state");
    m_row->addWidget(m_state);

    m_detail = dim(this);
    m_detail->setMinimumWidth(Theme::space(40));
    m_row->addWidget(m_detail, 1);

    m_identity = dim(this);
    m_identity->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_row->addWidget(m_identity);

    m_rx = dim(this);
    m_rx->setToolTip(tr("Received audio packets per second"));
    m_row->addWidget(m_rx);

    m_tx = dim(this);
    m_tx->setToolTip(tr("Transmitted audio packets per second"));
    m_row->addWidget(m_tx);

    m_nodes = dim(this);
    m_row->addWidget(m_nodes);

    m_grid = dim(this);
    m_grid->setToolTip(tr("The grid square the reflector portal plots you at"));
    m_row->addWidget(m_grid);

    m_connect = new QPushButton(tr("Connect"), this);
    m_connect->setCursor(Qt::PointingHandCursor);
    m_connect->setFocusPolicy(Qt::NoFocus);   /* Space is transmit in this window */
    connect(m_connect, &QPushButton::clicked, this, [this]() {
        if (m_app) app_toggle_connect(m_app);
    });
    m_row->addWidget(m_connect);
}

void ConnectionBar::addTrailing(QWidget *w)
{
    m_row->addWidget(w);
}

void ConnectionBar::tickModel(quint64 nowMs)
{
    Q_UNUSED(nowMs);
    if (!m_app) return;

    const svx_config *cfg = app_config(m_app);
    rc_client *rc = app_rc(m_app);
    const rc_state st = rc_get_state(rc);

    if (!m_stateInit || st != m_lastState) {
        m_stateInit = true;
        m_lastState = st;

        const char *tone = "down";
        switch (st) {
        case RC_CONNECTED:  tone = "ok";   break;
        case RC_CONNECTING:
        case RC_BACKOFF:    tone = "warn"; break;
        case RC_IDLE:       tone = "down"; break;
        }
        Theme::setTone(m_dot, tone);

        /* rc_state_name() returns lower case — "idle", "connecting",
         * "connected", "reconnecting". Capitalise for display rather than
         * maintaining a second table that would drift from the core's. */
        QString label = QString::fromUtf8(rc_state_name(st));
        if (!label.isEmpty())
            label[0] = label[0].toUpper();
        m_state->setText(label);

        m_connect->setText(st == RC_IDLE ? tr("Connect") : tr("Disconnect"));
    }

    const char *err = rc_last_error(rc);
    if (st != RC_CONNECTED && err && *err) {
        const QString msg = QString::fromUtf8(err);
        m_detail->setText(m_detail->fontMetrics().elidedText(
            msg, Qt::ElideRight, std::max(60, m_detail->width())));
        m_detail->setToolTip(msg);
        Theme::setTone(m_detail, "error");
    } else if (!m_detail->text().isEmpty()) {
        m_detail->clear();
        m_detail->setToolTip(QString());
        Theme::setTone(m_detail, "");
    }

    m_identity->setText(QStringLiteral("%1 · %2:%3")
        .arg(QString::fromUtf8(cfg->callsign), QString::fromUtf8(rc_host(rc)))
        .arg(rc_port(rc)));

    rc_stats s{};
    rc_get_stats(rc, &s);
    m_rx->setText(tr("RX %1/s").arg(s.rx_pps, 3));
    m_tx->setText(tr("TX %1/s").arg(s.tx_pps, 3));

    /* Loss, replay and auth failures only appear once they are non-zero — a
     * permanently visible "loss 0.0%" trains people to stop reading it. */
    QString rxTip = tr("Received audio packets per second\nlost %1  (%2%)")
                        .arg(s.rx_lost).arg(s.loss_pct, 0, 'f', 1);
    const bool suspicious = s.rx_replayed || s.rx_auth_fail;
    if (suspicious)
        rxTip += tr("\nreplayed %1  ·  auth failures %2")
                     .arg(s.rx_replayed).arg(s.rx_auth_fail);
    Theme::setTone(m_rx, suspicious ? "warn" : "");
    m_rx->setToolTip(rxTip);

    m_nodes->setText(st == RC_CONNECTED
        ? tr("id %1 · %2 nodes").arg(rc_client_id(rc)).arg(rc_node_count(rc))
        : QString());

    /* The grid square is what the reflector portal plots you at, so showing it
     * here is a standing check that the position actually parsed. */
    if (cfg->latitude != 0.0 || cfg->longitude != 0.0) {
        char grid[16] = {0};
        maidenhead(grid, sizeof grid, cfg->latitude, cfg->longitude);
        QString loc = QString::fromUtf8(grid);
        if (cfg->location[0])
            loc += QStringLiteral(" · ") + QString::fromUtf8(cfg->location);
        m_grid->setText(loc);
    } else {
        m_grid->setText(tr("no location"));
    }
}
