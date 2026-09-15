/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 */
#include "ui/sidebar.h"
#include "ui/talkgroupbutton.h"
#include "ui/levelmeter.h"
#include "ui/theme.h"
#include "ui/timefmt.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QFrame>
#include <QToolButton>
#include <QSlider>
#include <QScrollArea>
#include <QSignalBlocker>

namespace {

constexpr int kWidth = 230;

QLabel *section(const QString &text, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    Theme::setRole(l, "section");
    return l;
}

} // namespace

Sidebar::Sidebar(svx_app *app, QWidget *parent) : QWidget(parent), m_app(app)
{
    setFixedWidth(Theme::space(kWidth));
    buildUi();
    rebuildTalkgroups();

    /* The width follows the font: `omarchy display text size` scales the
     * spacing, and a fixed 230 px column would clip larger text. */
    connect(&OmarchyTheme::get(), &OmarchyTheme::changed, this, [this]() {
        setFixedWidth(Theme::space(kWidth));
    });
}

void Sidebar::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(Theme::space(12), Theme::space(12), Theme::space(12), Theme::space(12));
    root->setSpacing(Theme::space(8));

    /* ---- which talkgroup, and the lock ---- */
    auto *tgRow = new QHBoxLayout;
    tgRow->setSpacing(Theme::space(6));

    m_activeTg = new QLabel(QStringLiteral("—"), this);
    Theme::setRole(m_activeTg, "headline");
    tgRow->addWidget(m_activeTg);

    tgRow->addStretch(1);

    m_lock = new QToolButton(this);
    Theme::setRole(m_lock, "lock");
    m_lock->setCheckable(true);
    m_lock->setCursor(Qt::PointingHandCursor);
    m_lock->setFocusPolicy(Qt::NoFocus);
    m_lock->setToolButtonStyle(Qt::ToolButtonTextOnly);
    connect(m_lock, &QToolButton::clicked, this, [this]() {
        if (m_app) app_toggle_lock(m_app);
    });
    tgRow->addWidget(m_lock);
    root->addLayout(tgRow);

    applyLockVisuals(false);

    /* Free-form label a sysop can attach to a talkgroup. Populated from the
     * portal's talkgroups.json once a portal fetcher exists; the widget ships
     * now so the layout does not shift when it arrives. */
    m_tgInfo = new QLabel(this);
    Theme::setRole(m_tgInfo, "hint");
    m_tgInfo->setWordWrap(true);
    m_tgInfo->hide();
    root->addWidget(m_tgInfo);

    /* The preemption notice. tgm_preempt_banner() returns the talkgroup you
     * were moved AWAY from and expires itself after five seconds. */
    m_preempt = new QLabel(this);
    Theme::setRole(m_preempt, "hint");
    Theme::setTone(m_preempt, "warn");
    m_preempt->setWordWrap(true);
    m_preempt->hide();
    root->addWidget(m_preempt);

    root->addSpacing(Theme::space(4));
    auto *tgHead = section(tr("Talkgroups"), this);
    tgHead->setToolTip(tr("Click to switch. Right-click to mute."));
    root->addWidget(tgHead);

    /* ---- the talkgroup list ---- */
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_tgHost = new QWidget(scroll);
    m_tgLayout = new QVBoxLayout(m_tgHost);
    m_tgLayout->setContentsMargins(0, 0, 0, 0);
    m_tgLayout->setSpacing(Theme::space(2));
    m_tgLayout->addStretch(1);
    scroll->setWidget(m_tgHost);
    root->addWidget(scroll, 1);

    /* ---- audio ---- */
    root->addSpacing(Theme::space(4));
    root->addWidget(section(tr("Audio"), this));

    auto meterRow = [this](char32_t glyph, const QString &tip, LevelMeter **out) {
        auto *row = new QHBoxLayout;
        row->setSpacing(Theme::space(8));
        auto *cap = new QLabel(Theme::Glyph::of(glyph), this);
        Theme::setRole(cap, "glyph");
        cap->setToolTip(tip);
        cap->setAlignment(Qt::AlignCenter);
        cap->setFixedWidth(Theme::space(20));
        row->addWidget(cap);
        *out = new LevelMeter(this);
        (*out)->setToolTip(tip);
        row->addWidget(*out, 1);
        return row;
    };
    root->addLayout(meterRow(Theme::Glyph::Microphone, tr("Microphone level, while transmitting"), &m_micMeter));
    root->addLayout(meterRow(Theme::Glyph::Speaker,    tr("Speaker level"), &m_spkMeter));

    auto *volRow = new QHBoxLayout;
    volRow->setSpacing(Theme::space(6));

    m_mute = new QToolButton(this);
    Theme::setRole(m_mute, "icon");
    m_mute->setCheckable(true);
    m_mute->setCursor(Qt::PointingHandCursor);
    m_mute->setFocusPolicy(Qt::NoFocus);
    m_mute->setFixedWidth(Theme::space(28));
    connect(m_mute, &QToolButton::clicked, this, [this]() {
        if (m_app) app_toggle_output_mute(m_app);
    });
    volRow->addWidget(m_mute);
    applyMuteVisuals(false);

    m_volume = new QSlider(Qt::Horizontal, this);
    m_volume->setRange(0, 100);
    m_volume->setValue(m_app ? app_volume(m_app) : 80);
    m_volume->setCursor(Qt::PointingHandCursor);
    m_volume->setFocusPolicy(Qt::NoFocus);
    m_volume->setToolTip(tr("Output volume"));
    connect(m_volume, &QSlider::valueChanged, this, [this](int v) {
        if (m_app) app_set_volume(m_app, v);
    });
    volRow->addWidget(m_volume, 1);
    root->addLayout(volRow);
}

void Sidebar::applyLockVisuals(bool locked)
{
    /* Text that names the STATE, not the action: "Lock" on a button that is
     * already locked reads as "click to lock", which is exactly backwards.
     *
     * Yellow when locked rather than the accent: the accent reads as
     * "selected", which is what every other checked control means, whereas
     * this is a hold that changes how the radio behaves. It is the same yellow
     * the status dot uses for "not in the normal state". */
    m_lock->setText(locked ? Theme::Glyph::of(Theme::Glyph::Lock) + tr("  Locked")
                           : Theme::Glyph::of(Theme::Glyph::Unlock) + tr("  Lock"));
    Theme::setProp(m_lock, "locked", locked);

    m_lock->setToolTip(locked
        ? tr("Locked to this talkgroup. Nothing will move you automatically — "
             "click to unlock.")
        : tr("Click to lock. While locked, a talker on a higher-priority "
             "talkgroup will not pull you away."));
}

void Sidebar::applyMuteVisuals(bool muted)
{
    m_mute->setText(Theme::Glyph::of(muted ? Theme::Glyph::SpeakerOff : Theme::Glyph::Speaker));
    m_mute->setToolTip(muted ? tr("Output muted — click to unmute") : tr("Mute output"));
}

void Sidebar::rebuildTalkgroups()
{
    if (!m_app) return;
    const svx_config *cfg = app_config(m_app);

    QString sig;
    for (int i = 0; i < cfg->n_switchable; ++i)
        sig += QStringLiteral("%1:%2,")
                   .arg(cfg->switchable[i].id).arg(cfg->switchable[i].priority);
    if (sig == m_tgSignature)
        return;
    m_tgSignature = sig;

    for (auto *b : std::as_const(m_buttons))
        b->deleteLater();
    m_buttons.clear();

    for (int i = 0; i < cfg->n_switchable; ++i) {
        const uint32_t tg = cfg->switchable[i].id;

        /* Priority comes from config_tg_priority(), not from the switchable
         * entry's own suffix: the monitored list wins where both name a
         * talkgroup, and that is the priority the state machine acts on. */
        auto *b = new TalkgroupButton(tg, config_tg_priority(cfg, tg), m_tgHost);
        connect(b, &TalkgroupButton::clicked, this, [this, tg]() {
            if (m_app) app_tg_select(m_app, tg);
        });
        connect(b, &TalkgroupButton::muteRequested, this, [this](quint32 t) {
            if (m_app) app_toggle_mute(m_app, t);
        });
        m_tgLayout->insertWidget(m_tgLayout->count() - 1, b);
        m_buttons.append(b);
    }
}

void Sidebar::tickModel(quint64 nowMs)
{
    if (!m_app) return;

    tg_manager *tgm = app_tgm(m_app);
    const rc_state st = rc_get_state(app_rc(m_app));
    const uint32_t sel = tgm_selected(tgm);

    if (st != RC_CONNECTED)
        m_activeTg->setText(QStringLiteral("—"));
    else if (sel == 0)
        m_activeTg->setText(tr("Monitor"));
    else
        m_activeTg->setText(tr("TG %1").arg(sel));

    const bool locked = tgm_locked(tgm) != 0;
    if (!m_lockInit || locked != m_lockState) {
        m_lockInit  = true;
        m_lockState = locked;
        const QSignalBlocker blocker(m_lock);
        m_lock->setChecked(locked);
        applyLockVisuals(locked);
    }

    const uint32_t from = tgm_preempt_banner(tgm, nowMs);
    if (from) {
        m_preempt->setText(tr("Moved from TG %1").arg(from));
        m_preempt->show();
    } else {
        m_preempt->hide();
    }

    for (auto *b : std::as_const(m_buttons)) {
        const uint32_t tg = b->talkgroup();
        b->setChecked(tg == sel);
        b->setMuted(tgm_is_muted(tgm, tg) != 0);

        const tgm_talker *t = tgm_talker_on(tgm, tg);
        b->setTalker(t ? QString::fromUtf8(t->full) : QString());
        b->setLastHeard(tgm_last_heard(tgm, tg));
        b->refreshAge(nowMs);
    }

    if (!m_volume->isSliderDown() && m_volume->value() != app_volume(m_app)) {
        const QSignalBlocker blocker(m_volume);
        m_volume->setValue(app_volume(m_app));
    }

    const bool muted = app_output_muted(m_app) != 0;
    if (!m_muteInit || muted != m_muteState) {
        m_muteInit  = true;
        m_muteState = muted;
        const QSignalBlocker blocker(m_mute);
        m_mute->setChecked(muted);
        applyMuteVisuals(muted);
    }
}

void Sidebar::tickMeters()
{
    if (!m_app) return;

    /* app_mic_level() holds its last peak after capture stops, so gate it on
     * transmit; the speaker meter must read zero while muted rather than
     * showing what would have played. */
    const float mic = app_tx_active(m_app)    ? app_mic_level(m_app) : 0.0f;
    const float spk = app_output_muted(m_app) ? 0.0f : app_spk_level(m_app);

    auto ballistic = [](float in, float &vu) {
        vu = (in > vu) ? in : vu * 0.75f;
        if (vu < 0.001f) vu = 0.0f;
        return vu;
    };

    m_micMeter->setLevel(ballistic(mic, m_micVu));
    m_spkMeter->setLevel(ballistic(spk, m_spkVu));
}
