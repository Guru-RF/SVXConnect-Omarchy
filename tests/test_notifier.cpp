/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * "Someone is talking" detection.
 *
 * The notification must fire when a talker STARTS — that is, when a station
 * appears in the talkgroup manager's `active` list — and never when one stops.
 * A stop only moves the entry to `recent`, which this code never reads, but
 * "the code does not read it" is not a test. So: drive the same list the core
 * maintains through a QSO, and check exactly when a notification comes out.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <QSet>
#include <QString>
#include <QStringList>

#include "ui/notifier.h"

namespace {

int g_fail;
int g_run;

void check(bool ok, const char *what)
{
    ++g_run;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

/* One entry of the manager's active list, as the core fills it in. */
void talking(tg_manager &tgm, int slot, quint32 tg, const char *call, const char *full)
{
    std::snprintf(tgm.active[slot].call, sizeof(tgm.active[slot].call), "%s", call);
    std::snprintf(tgm.active[slot].full, sizeof(tgm.active[slot].full), "%s", full);
    tgm.active[slot].tg = tg;
    tgm.active[slot].start_ms = 1000 + slot;
}

} // namespace

int main()
{
    tg_manager tgm;
    std::memset(&tgm, 0, sizeof(tgm));

    QSet<QString> seen;
    quint32 tg = 0;
    const QString mine = QStringLiteral("ON6URE");

    /* Nothing on the air. */
    check(Notifier::freshTalkers(&tgm, mine, &seen, &tg).isEmpty(), "silence notifies nothing");

    /* ON4ABC-7 keys up on TG 8. */
    talking(tgm, 0, 8, "ON4ABC", "ON4ABC-7");
    tgm.n_active = 1;
    QStringList fresh = Notifier::freshTalkers(&tgm, mine, &seen, &tg);
    check(fresh == QStringList{QStringLiteral("ON4ABC-7")}, "a talker starting notifies, with its full callsign");
    check(tg == 8, "the talkgroup comes with it");

    /* Still talking on the next tick. */
    check(Notifier::freshTalkers(&tgm, mine, &seen, &tg).isEmpty(), "the same talker does not notify again");

    /* A second station joins on another talkgroup. */
    talking(tgm, 1, 1745, "ON7XYZ", "ON7XYZ");
    tgm.n_active = 2;
    fresh = Notifier::freshTalkers(&tgm, mine, &seen, &tg);
    check(fresh == QStringList{QStringLiteral("ON7XYZ")}, "only the new station notifies");
    check(tg == 1745, "on its own talkgroup");

    /* ON4ABC-7 stops: the core drops it from `active` and files it under
     * `recent`. A stop must never notify. */
    talking(tgm, 0, 1745, "ON7XYZ", "ON7XYZ");
    tgm.n_active = 1;
    check(Notifier::freshTalkers(&tgm, mine, &seen, &tg).isEmpty(), "a talker STOPPING notifies nothing");

    /* And the one still talking is not re-announced by the list shifting. */
    check(Notifier::freshTalkers(&tgm, mine, &seen, &tg).isEmpty(), "a shifted list does not re-notify");

    /* ON4ABC comes back for the next over — a new start, so it notifies. */
    talking(tgm, 1, 8, "ON4ABC", "ON4ABC-7");
    tgm.n_active = 2;
    fresh = Notifier::freshTalkers(&tgm, mine, &seen, &tg);
    check(fresh == QStringList{QStringLiteral("ON4ABC-7")}, "the next over notifies again");

    /* Everyone stops. */
    tgm.n_active = 0;
    check(Notifier::freshTalkers(&tgm, mine, &seen, &tg).isEmpty(), "everyone stopping notifies nothing");

    /* Our own transmission, echoed back by the reflector, from any of our
     * nodes: the core strips the SSID into `call` precisely so this works. */
    talking(tgm, 0, 8, "ON6URE", "ON6URE-TPAD");
    tgm.n_active = 1;
    check(Notifier::freshTalkers(&tgm, mine, &seen, &tg).isEmpty(), "our own transmission notifies nothing");

    talking(tgm, 1, 8, "ON4ABC", "ON4ABC-7");
    tgm.n_active = 2;
    fresh = Notifier::freshTalkers(&tgm, mine, &seen, &tg);
    check(fresh == QStringList{QStringLiteral("ON4ABC-7")}, "someone else talking beside us still notifies");

    /* Two stations keying up between two ticks. */
    std::memset(&tgm, 0, sizeof(tgm));
    seen.clear();
    Notifier::freshTalkers(&tgm, mine, &seen, &tg);
    talking(tgm, 0, 8, "ON4ABC", "ON4ABC-7");
    talking(tgm, 1, 9, "ON7XYZ", "ON7XYZ");
    tgm.n_active = 2;
    fresh = Notifier::freshTalkers(&tgm, mine, &seen, &tg);
    check(fresh.size() == 2, "two starts in one tick are both reported");

    /* A station that moves talkgroup mid-over is the same transmission: the
     * core upserts it by callsign, and closes it by callsign whatever
     * talkgroup the stop names. So it must not announce itself twice. */
    std::memset(&tgm, 0, sizeof(tgm));
    seen.clear();
    talking(tgm, 0, 8, "ON4ABC", "ON4ABC-7");
    tgm.n_active = 1;
    Notifier::freshTalkers(&tgm, mine, &seen, &tg);
    talking(tgm, 0, 1745, "ON4ABC", "ON4ABC-7");
    check(Notifier::freshTalkers(&tgm, mine, &seen, &tg).isEmpty(),
          "a talker moving talkgroup does not notify again");

    std::printf("\n%d/%d passed\n", g_run - g_fail, g_run);
    return g_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
