/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * The talkgroup manager must keep BOTH spellings of a callsign.
 *
 * Regression test for a real bug: the interface showed "ON6URE" for a station
 * transmitting as "ON6URE-TPAD", because tgm_on_talker_start() stripped the
 * SSID before storing and the full form was gone for good.
 *
 * The stripped form still has to exist, and still has to be what MATCHING
 * uses — the reflector can report a stop with a different SSID than the start,
 * and roger-beep self-suppression must recognise your own audio coming back
 * from any of your nodes. So the fix is to keep both, not to stop stripping.
 */
#include <cstdio>
#include <cstring>

extern "C" {
#include "common/config.h"
#include "tg/tgmanager.h"
}

static int g_failures = 0;

#define CHECK(cond, what)                                                      \
    do {                                                                       \
        if (!(cond)) { std::printf("  FAIL  %s\n", (what)); ++g_failures; }    \
        else         { std::printf("  ok    %s\n", (what)); }                  \
    } while (0)

int main()
{
    svx_config cfg;
    config_defaults(&cfg);
    std::snprintf(cfg.callsign, sizeof cfg.callsign, "%s", "ON6URE");
    config_set(&cfg, "monitored",  "8, 9990");
    config_set(&cfg, "switchable", "8, 9990");

    tg_manager m;
    tgm_callbacks cb;
    std::memset(&cb, 0, sizeof cb);
    tgm_init(&m, &cfg, &cb);

    std::printf("talker callsigns\n");

    tgm_on_talker_start(&m, 9990, "ON6URE-TPAD");

    CHECK(m.n_active == 1, "the talker was recorded");
    CHECK(std::strcmp(m.active[0].full, "ON6URE-TPAD") == 0,
          "active[].full keeps the SSID, for display");
    CHECK(std::strcmp(m.active[0].call, "ON6URE") == 0,
          "active[].call is stripped, for matching");

    /* The reflector may report the stop with a different SSID; matching on the
     * base is what makes that survivable. */
    tgm_on_talker_stop(&m, 9990, "ON6URE-PI");

    CHECK(m.n_active == 0, "a stop with a different SSID still closes the entry");
    CHECK(m.n_recent == 1, "it moved to the recent list");
    CHECK(std::strcmp(m.recent[0].full, "ON6URE-TPAD") == 0,
          "recent[].full carries the START's full callsign, not the stop's");
    CHECK(std::strcmp(m.recent[0].call, "ON6URE") == 0,
          "recent[].call is stripped");

    /* Two nodes of the same operator on different talkgroups must remain two
     * distinguishable rows. */
    tgm_on_talker_start(&m, 8,    "ON6URE-PI");
    tgm_on_talker_start(&m, 9990, "ON6URE-TPAD");
    CHECK(m.n_active == 2, "two SSIDs of one callsign are two active talkers");

    bool sawPi = false, sawTpad = false;
    for (int i = 0; i < m.n_active; ++i) {
        if (std::strcmp(m.active[i].full, "ON6URE-PI")   == 0) sawPi   = true;
        if (std::strcmp(m.active[i].full, "ON6URE-TPAD") == 0) sawTpad = true;
    }
    CHECK(sawPi && sawTpad, "each row shows which node it is");

    /* A callsign with no SSID must not gain or lose anything. */
    tgm_on_talker_start(&m, 8, "PARROT");
    for (int i = 0; i < m.n_active; ++i)
        if (m.active[i].tg == 8)
            CHECK(std::strcmp(m.active[i].full, "PARROT") == 0 &&
                  std::strcmp(m.active[i].call, "PARROT") == 0,
                  "an SSID-less callsign is identical in both fields");

    std::printf("\n%s (%d failure%s)\n",
                g_failures ? "FAILED" : "all checks passed",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
