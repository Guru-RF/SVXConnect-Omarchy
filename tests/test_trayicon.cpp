/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * The tray menu against a stub core.
 *
 * Two ways 0.1.13's tray menu said the opposite of the truth, both because it
 * only re-synchronised when "connected" or "transmitting" CHANGED:
 *
 *   - "Transmit" is checkable, so Qt ticks it on the click itself. A press
 *     the core refuses ("PTT refused: X is talking") changes nothing, so the
 *     tick stayed — "Transmit ✓" on an idle radio.
 *   - "Connect"/"Disconnect" followed connected-or-not. Connecting, then
 *     reconnecting, is "not connected" throughout, so the item kept saying
 *     Connect while a click would disconnect — and the reverse after a drop
 *     followed by a Disconnect.
 *
 * Runs on the offscreen platform, where there is no tray; the menu is built
 * regardless, which is what makes it testable.
 */
#include <cstdio>

#include <QAction>
#include <QApplication>

#include "ui/trayicon.h"

namespace {

int g_fail;
int g_run;

void check(bool ok, const char *what)
{
    ++g_run;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

} // namespace

/* ---- the stub core ----------------------------------------------------- */

struct svx_app {
    rc_state   state  = RC_IDLE;
    int        tx     = 0;
    bool       refuse = false;
    svx_config cfg{};
    tg_manager tgm{};
};

namespace {
svx_app g_core;
} // namespace

extern "C" {

rc_client *app_rc(svx_app *a)                  { return reinterpret_cast<rc_client *>(a); }
rc_state   rc_get_state(const rc_client *c)    { return reinterpret_cast<const svx_app *>(c)->state; }
const char *rc_state_name(rc_state s)
{
    switch (s) {
    case RC_IDLE:       return "idle";
    case RC_CONNECTING: return "connecting";
    case RC_CONNECTED:  return "connected";
    case RC_BACKOFF:    return "reconnecting";
    }
    return "?";
}
int  app_tx_active(const svx_app *a)             { return a->tx; }
const svx_config *app_config(const svx_app *a)   { return &a->cfg; }
tg_manager *app_tgm(svx_app *a)                  { return &a->tgm; }
uint32_t tgm_selected(const tg_manager *)        { return 0; }

void app_ptt(svx_app *a, ctl_tristate v)
{
    const bool start = v == CTL_ON || (v == CTL_TOGGLE && !a->tx);
    if (!start)      a->tx = 0;
    else if (!a->refuse) a->tx = 1;
}
void app_toggle_connect(svx_app *a) { a->state = a->state == RC_IDLE ? RC_CONNECTING : RC_IDLE; }
void app_reconnect(svx_app *)       {}

} // extern "C"

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication qapp(argc, argv);

    TrayIcon tray(&g_core);
    QAction *ptt  = tray.pttAction();
    QAction *conn = tray.connectAction();

    /* ---- the Transmit check mark ---- */
    g_core.state = RC_CONNECTED;
    tray.tickModel(TxMonitor::State::Idle);

    g_core.refuse = true;
    ptt->trigger();                      /* Qt ticks it; the core says no */
    tray.tickModel(TxMonitor::State::Idle);
    check(!ptt->isChecked(), "a refused Transmit does not stay ticked");

    g_core.refuse = false;
    ptt->trigger();
    tray.tickModel(TxMonitor::State::OnAir);
    check(g_core.tx == 1 && ptt->isChecked(), "an accepted Transmit is ticked");

    g_core.tx = 0;                       /* the core stops by itself */
    tray.tickModel(TxMonitor::State::Idle);
    check(!ptt->isChecked(), "a core-side stop un-ticks it");

    /* ---- the Connect / Disconnect label ---- */
    struct Step { rc_state st; const char *want; const char *what; };
    const Step steps[] = {
        {RC_IDLE,       "Connect",    "idle: Connect"},
        {RC_CONNECTING, "Disconnect", "connecting: Disconnect (a click stops it)"},
        {RC_BACKOFF,    "Disconnect", "reconnecting: Disconnect"},
        {RC_IDLE,       "Connect",    "stopped while reconnecting: back to Connect"},
        {RC_CONNECTED,  "Disconnect", "connected: Disconnect"},
        {RC_BACKOFF,    "Disconnect", "dropped, reconnecting: Disconnect"},
        {RC_IDLE,       "Connect",    "disconnected from reconnecting: Connect"},
    };
    for (const Step &s : steps) {
        g_core.state = s.st;
        tray.tickModel(TxMonitor::State::Idle);
        check(conn->text() == QLatin1String(s.want), s.what);
    }

    /* And the label is what the click does. */
    g_core.state = RC_BACKOFF;
    tray.tickModel(TxMonitor::State::Idle);
    conn->trigger();
    check(g_core.state == RC_IDLE, "clicking \"Disconnect\" while reconnecting disconnects");

    std::printf("\n%d/%d passed\n", g_run - g_fail, g_run);
    return g_fail ? 1 : 0;
}
