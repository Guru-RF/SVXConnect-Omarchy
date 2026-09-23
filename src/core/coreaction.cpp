/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * See coreaction.h.
 */
#include "core/coreaction.h"

namespace CoreAction {

namespace {

/* GUI thread only, like the core itself. */
std::function<void()> g_before;
std::function<void()> g_after;

} // namespace

void setGuard(std::function<void()> before, std::function<void()> after)
{
    g_before = std::move(before);
    g_after  = std::move(after);
}

void run(const std::function<void()> &call)
{
    if (g_before)
        g_before();
    call();
    if (g_after)
        g_after();
}

void ptt(svx_app *app, ctl_tristate v)
{
    if (app)
        run([app, v]() { app_ptt(app, v); });
}

void toggleConnect(svx_app *app)
{
    if (app)
        run([app]() { app_toggle_connect(app); });
}

void reconnect(svx_app *app)
{
    if (app)
        run([app]() { app_reconnect(app); });
}

} // namespace CoreAction
