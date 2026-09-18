/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * What "Load from reflector" lists, and what it writes back.
 *
 * This dialog edits somebody's configuration, so both halves are pure
 * functions and both are pinned here. Two properties matter more than the
 * rest:
 *
 *   - the list is what the talkgroup info JSON names, and nothing else. It
 *     used to include every talkgroup the feed saw a node listening to, which
 *     is how 60, 10 and 145925 were offered as if the reflector vouched for
 *     them;
 *   - filtering that list must not be a way to delete from a configuration. A
 *     talkgroup the operator configured that the reflector does not name is
 *     not shown — and is still there afterwards, priority and position intact.
 *
 * The data is be.svx.link's, shortened.
 */
#include <cstdio>
#include <cstdlib>

#include <QHash>
#include <QList>
#include <QString>

#include "ui/talkgroupsdialog.h"

namespace {

int g_fail;
int g_run;

void check(bool ok, const char *what)
{
    ++g_run;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

using Dialog = ReflectorTalkgroupsDialog;

const Dialog::Entry *find(const QList<Dialog::Entry> &list, quint32 id)
{
    for (const Dialog::Entry &e : list)
        if (e.id == id) return &e;
    return nullptr;
}

QList<Dialog::Entry> tick(QList<Dialog::Entry> list, quint32 id, bool monitor, bool switchable)
{
    for (Dialog::Entry &e : list)
        if (e.id == id) { e.monitored = monitor; e.switchable = switchable; }
    return list;
}

} // namespace

int main()
{
    const QHash<quint32, QString> names = {
        {8,    QStringLiteral("70cm Repeaters")},
        {1745, QStringLiteral("145.450 ON0ORA-S Simplex Club Opwijk")},
        {9990, QStringLiteral("Parrot, test your audio here")},
        {4,    QStringLiteral("4m Repeaters")},
    };
    /* What the feed sees nodes listening to — including four the JSON has
     * never heard of. */
    const QHash<quint32, int> nodes = {
        {8, 73}, {1745, 40}, {9990, 25}, {60, 4}, {10, 3}, {2300, 1}, {145925, 1},
    };

    /* ---- what is listed ---- */

    QList<quint32> kept;
    QList<Dialog::Entry> list = Dialog::entriesFor(names, nodes, QStringLiteral("8++, 60+, 1745"),
                                                   QStringLiteral("1745, 60, 8"), &kept);

    check(list.size() == 4, "the list is exactly the talkgroups the JSON names");
    check(!find(list, 60) && !find(list, 10) && !find(list, 145925),
          "a talkgroup only the feed has seen is not offered");
    check(find(list, 4) && find(list, 4)->nodes == 0,
          "a named talkgroup nobody is on is still listed, with a count of none");
    check(list.at(0).id == 4 && list.at(1).id == 8 && list.at(2).id == 1745 && list.at(3).id == 9990,
          "ordered by talkgroup number, not by how busy each one is");
    check(find(list, 8)->nodes == 73, "the feed's count annotates what the JSON lists");
    check(find(list, 8)->name == QStringLiteral("70cm Repeaters"), "with its name");

    check(find(list, 8)->monitored && find(list, 8)->switchable,
          "what is configured arrives ticked");
    check(find(list, 9990) && !find(list, 9990)->monitored, "and what is not, does not");

    check(kept == QList<quint32>({60}),
          "a configured talkgroup the JSON does not name is reported as kept, not listed");

    /* ---- what is written back ---- */

    /* The operator adds the parrot to both, and touches nothing else. */
    Dialog::Fields f = Dialog::compose(tick(list, 9990, true, true),
                                       QStringLiteral("8++, 60+, 1745"),
                                       QStringLiteral("1745, 60, 8"));

    check(f.monitored == QStringLiteral("8++, 60+, 1745, 9990"),
          "monitored: by number, priorities intact, the unlisted 60+ still there");
    check(f.switchable == QStringLiteral("1745, 60, 8, 9990"),
          "switchable: the cycle keeps its order — 1745 stays first, so the default "
          "talkgroup does not move — and the new entry joins the end");

    /* Unticking removes; it is the unlisted ones that are untouchable. */
    f = Dialog::compose(tick(list, 8, false, false),
                        QStringLiteral("8++, 60+, 1745"), QStringLiteral("1745, 60, 8"));
    check(f.monitored == QStringLiteral("60+, 1745"), "unticking a listed talkgroup removes it");
    check(f.switchable == QStringLiteral("1745, 60"), "from both fields");

    /* Accepting without touching anything must be a no-op, or merely opening
     * the dialog would dirty the configuration file. */
    f = Dialog::compose(list, QStringLiteral("8++, 60+, 1745"), QStringLiteral("1745, 60, 8"));
    check(f.monitored == QStringLiteral("8++, 60+, 1745"), "accepting untouched changes nothing");
    check(f.switchable == QStringLiteral("1745, 60, 8"), "in either field");

    /* ---- no JSON at all ---- */

    kept.clear();
    list = Dialog::entriesFor({}, nodes, QString(), QString(), &kept);
    check(list.size() == nodes.size(),
          "with no JSON there is nothing to filter by, so the feed's talkgroups are the fallback");
    check(kept.isEmpty(), "and nothing is being carried past the list");

    check(Dialog::entriesFor({}, {}, QStringLiteral("8"), QString()).isEmpty(),
          "a plain reflector with no JSON has nothing to offer");

    std::printf("\n%d/%d passed\n", g_run - g_fail, g_run);
    return g_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
