/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 */
#include "ui/talkgroupsdialog.h"
#include "ui/theme.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

ReflectorTalkgroupsDialog::ReflectorTalkgroupsDialog(const QList<Entry> &entries, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Talkgroups on this reflector"));
    resize(Theme::space(620), Theme::space(560));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(Theme::space(14), Theme::space(14), Theme::space(14), Theme::space(14));
    root->setSpacing(Theme::space(10));

    auto *intro = new QLabel(
        tr("Everything this reflector names or has a node listening on. "
           "<b>Monitor</b> is everything you want to hear; <b>Switch</b> is the short "
           "list the sidebar cycles through, so keep it short."), this);
    intro->setWordWrap(true);
    Theme::setRole(intro, "hint");
    root->addWidget(intro);

    auto *host = new QWidget;
    auto *grid = new QGridLayout(host);
    grid->setContentsMargins(0, 0, Theme::space(8), 0);
    grid->setHorizontalSpacing(Theme::space(12));
    grid->setVerticalSpacing(Theme::space(4));

    auto header = [&](int column, const QString &text, Qt::Alignment align = Qt::AlignLeft) {
        auto *l = new QLabel(text, host);
        Theme::setRole(l, "section");
        l->setAlignment(align | Qt::AlignVCenter);
        grid->addWidget(l, 0, column);
    };
    header(0, tr("Talkgroup"));
    header(1, tr("Name"));
    header(2, tr("Nodes"), Qt::AlignRight);
    header(3, tr("Monitor"), Qt::AlignHCenter);
    header(4, tr("Switch"), Qt::AlignHCenter);

    grid->setColumnStretch(1, 1);

    int row = 1;
    for (const Entry &e : entries) {
        Row r;
        r.entry = e;

        auto *id = new QLabel(QString::number(e.id), host);
        Theme::setRole(id, "value");
        grid->addWidget(id, row, 0);

        auto *name = new QLabel(e.name, host);
        name->setToolTip(e.name);
        grid->addWidget(name, row, 1);

        /* How many nodes are listening is the honest measure of whether a
         * talkgroup is worth having: the portal lists some nobody uses, and
         * carries none of the ones a couple of stations agreed on yesterday. */
        auto *nodes = new QLabel(e.nodes > 0 ? QString::number(e.nodes) : tr("—"), host);
        Theme::setRole(nodes, "time");
        nodes->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        nodes->setToolTip(e.nodes > 0
            ? tr("%n node(s) monitoring TG %1 right now", nullptr, e.nodes).arg(e.id)
            : tr("Named by the reflector, but nothing is listening to it at the moment"));
        grid->addWidget(nodes, row, 2);

        r.monitor = new QCheckBox(host);
        r.monitor->setChecked(e.monitored);
        grid->addWidget(r.monitor, row, 3, Qt::AlignHCenter);

        r.switchTo = new QCheckBox(host);
        r.switchTo->setChecked(e.switchable);
        grid->addWidget(r.switchTo, row, 4, Qt::AlignHCenter);

        /* Cycling to a talkgroup you are not listening to is a way to end up
         * transmitting into silence, so ticking Switch ticks Monitor too. */
        connect(r.switchTo, &QCheckBox::toggled, r.monitor, [mon = r.monitor](bool on) {
            if (on) mon->setChecked(true);
        });

        m_rows.append(r);
        ++row;
    }
    grid->setRowStretch(row, 1);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(host);
    root->addWidget(scroll, 1);

    auto *tools = new QHBoxLayout;
    tools->setSpacing(Theme::space(8));

    auto *all = new QPushButton(tr("Monitor all"), this);
    all->setFlat(true);
    all->setCursor(Qt::PointingHandCursor);
    connect(all, &QPushButton::clicked, this, [this]() {
        for (const Row &r : std::as_const(m_rows)) r.monitor->setChecked(true);
    });
    tools->addWidget(all);

    auto *none = new QPushButton(tr("Monitor none"), this);
    none->setFlat(true);
    none->setCursor(Qt::PointingHandCursor);
    connect(none, &QPushButton::clicked, this, [this]() {
        for (const Row &r : std::as_const(m_rows)) {
            r.monitor->setChecked(false);
            r.switchTo->setChecked(false);
        }
    });
    tools->addWidget(none);
    tools->addStretch(1);
    root->addLayout(tools);

    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    box->button(QDialogButtonBox::Ok)->setText(tr("Use these"));
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(box);
}

QList<ReflectorTalkgroupsDialog::Entry> ReflectorTalkgroupsDialog::chosen() const
{
    QList<Entry> out;
    out.reserve(m_rows.size());
    for (const Row &r : m_rows) {
        Entry e = r.entry;
        e.monitored  = r.monitor->isChecked();
        e.switchable = r.switchTo->isChecked();
        out.append(e);
    }
    return out;
}
