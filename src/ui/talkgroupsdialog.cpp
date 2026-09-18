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
#include <QSet>
#include <algorithm>

#include "core/svxcore.h"   /* tglist_parse, SVX_MAX_TG, SVX_MAX_PRIO */

ReflectorTalkgroupsDialog::ReflectorTalkgroupsDialog(const QList<Entry> &entries, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Talkgroups on this reflector"));
    resize(Theme::space(620), Theme::space(560));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(Theme::space(14), Theme::space(14), Theme::space(14), Theme::space(14));
    root->setSpacing(Theme::space(10));

    auto *intro = new QLabel(
        tr("The talkgroups this reflector names, by number. "
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

    m_keptNote = new QLabel(this);
    m_keptNote->setWordWrap(true);
    Theme::setRole(m_keptNote, "hint");
    m_keptNote->hide();
    root->addWidget(m_keptNote);

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

    tools->addSpacing(Theme::space(16));

    /* The same pair for the Switch column. Ticking a Switch box ticks its
     * Monitor box through the per-row connection above, so "All switchable"
     * is also "monitor all" — which is what it has to mean: a talkgroup in the
     * sidebar's cycle that you are not listening to is a way to transmit into
     * silence. */
    auto *allSwitch = new QPushButton(tr("All switchable"), this);
    allSwitch->setFlat(true);
    allSwitch->setCursor(Qt::PointingHandCursor);
    connect(allSwitch, &QPushButton::clicked, this, [this]() {
        for (const Row &r : std::as_const(m_rows)) r.switchTo->setChecked(true);
    });
    tools->addWidget(allSwitch);

    auto *noSwitch = new QPushButton(tr("None switchable"), this);
    noSwitch->setFlat(true);
    noSwitch->setCursor(Qt::PointingHandCursor);
    connect(noSwitch, &QPushButton::clicked, this, [this]() {
        /* Only the cycle is emptied; what is monitored is a separate decision
         * and stays as it was. */
        for (const Row &r : std::as_const(m_rows)) r.switchTo->setChecked(false);
    });
    tools->addWidget(noSwitch);

    tools->addStretch(1);
    root->addLayout(tools);

    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    box->button(QDialogButtonBox::Ok)->setText(tr("Use these"));
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(box);
}

namespace {

/* One configuration field, parsed by the core's own parser so this agrees with
 * what the core will make of the result. */
struct Parsed {
    QList<quint32>      order;
    QHash<quint32, int> priority;
};

Parsed parseField(const QString &text)
{
    Parsed out;
    svx_tg_entry v[SVX_MAX_TG];
    const int n = tglist_parse(qPrintable(text), v, SVX_MAX_TG);
    for (int i = 0; i < qMax(0, n); ++i) {
        if (!out.order.contains(v[i].id))
            out.order.append(v[i].id);
        out.priority.insert(v[i].id, v[i].priority);
    }
    return out;
}

} // namespace

QList<ReflectorTalkgroupsDialog::Entry>
ReflectorTalkgroupsDialog::entriesFor(const QHash<quint32, QString> &names,
                                      const QHash<quint32, int> &nodeCount,
                                      const QString &monitoredText,
                                      const QString &switchableText,
                                      QList<quint32> *keptOut)
{
    /* The list is what the talkgroup info JSON names, and nothing else.
     *
     * It used to be the union with whatever the feed saw nodes listening to,
     * which is how 60, 10, 2300 and 145925 turned up in it: talkgroups two
     * stations agreed on, or somebody's typo, offered as if the reflector
     * vouched for them. The JSON is the reflector's — or the operator's — own
     * statement of what its talkgroups are; the feed only says how busy each
     * one is, which stays as the Nodes column. */
    QSet<quint32> ids;
    for (auto it = names.cbegin(); it != names.cend(); ++it)
        ids.insert(it.key());
    if (ids.isEmpty())
        for (auto it = nodeCount.cbegin(); it != nodeCount.cend(); ++it)
            ids.insert(it.key());          /* no JSON: nothing to filter by */

    const Parsed mon = parseField(monitoredText);
    const Parsed sw  = parseField(switchableText);

    if (keptOut) {
        keptOut->clear();
        for (quint32 id : mon.order + sw.order)
            if (!ids.contains(id) && !keptOut->contains(id))
                keptOut->append(id);
        std::sort(keptOut->begin(), keptOut->end());
    }

    QList<Entry> entries;
    entries.reserve(ids.size());
    for (quint32 id : std::as_const(ids)) {
        Entry e;
        e.id         = id;
        e.name       = names.value(id);
        e.nodes      = nodeCount.value(id, 0);
        e.monitored  = mon.priority.contains(id);
        e.switchable = sw.priority.contains(id);
        entries.append(e);
    }

    /* By talkgroup number. It was busiest-first, which put the three anyone
     * uses at the top — and made every other talkgroup impossible to find,
     * because a node count is not something anybody looks a talkgroup up by.
     * The number is; the Nodes column still says how busy each one is. */
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        return a.id < b.id;
    });
    return entries;
}

ReflectorTalkgroupsDialog::Fields
ReflectorTalkgroupsDialog::compose(const QList<Entry> &chosen,
                                   const QString &monitoredText,
                                   const QString &switchableText)
{
    const Parsed mon = parseField(monitoredText);
    const Parsed sw  = parseField(switchableText);

    QSet<quint32> listed;
    QList<quint32> monitored, switchable;
    for (const Entry &e : chosen) {
        listed.insert(e.id);
        if (e.monitored)  monitored.append(e.id);
        if (e.switchable) switchable.append(e.id);
    }

    /* What was configured but never on the list comes through untouched. */
    for (quint32 id : mon.order)
        if (!listed.contains(id) && !monitored.contains(id)) monitored.append(id);
    for (quint32 id : sw.order)
        if (!listed.contains(id) && !switchable.contains(id)) switchable.append(id);

    /* Monitored: by number, each with the priority it already had. */
    std::sort(monitored.begin(), monitored.end());
    if (monitored.size() > SVX_MAX_TG)
        monitored.resize(SVX_MAX_TG);

    QStringList monParts;
    for (quint32 id : std::as_const(monitored)) {
        const int p = qBound(0, mon.priority.value(id, 0), SVX_MAX_PRIO);
        monParts << QString::number(id) + QString(p, QLatin1Char('+'));
    }

    /* Switchable: the cycle keeps the order it had, and anything newly ticked
     * joins the end by number. Re-sorting it would silently rearrange the
     * sidebar — and move the first entry, which is the default talkgroup. */
    QList<quint32> ordered;
    for (quint32 id : sw.order)
        if (switchable.contains(id) && !ordered.contains(id))
            ordered.append(id);
    std::sort(switchable.begin(), switchable.end());
    for (quint32 id : std::as_const(switchable))
        if (!ordered.contains(id))
            ordered.append(id);
    if (ordered.size() > SVX_MAX_TG)
        ordered.resize(SVX_MAX_TG);

    QStringList swParts;
    for (quint32 id : std::as_const(ordered))
        swParts << QString::number(id);

    /* ", " is what the core's own tglist_format() writes, so a list that did
     * not really change compares equal and the file is left alone. */
    return Fields{ monParts.join(QStringLiteral(", ")), swParts.join(QStringLiteral(", ")) };
}

void ReflectorTalkgroupsDialog::setKeptNote(const QString &text)
{
    m_keptNote->setText(text);
    m_keptNote->setVisible(!text.isEmpty());
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
