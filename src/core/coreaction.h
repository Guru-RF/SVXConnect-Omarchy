/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * The one door for core calls made from the interface rather than from inside
 * CoreLoop's service pass.
 *
 * WHY THIS EXISTS
 * ---------------
 * CoreLoop keeps a QSocketNotifier ENABLED on every descriptor the core asked
 * it to watch, between service passes. Most app_* calls leave that set alone,
 * but a few close sockets there and then:
 *
 *     app_toggle_connect  -> rc_stop          -> close(TLS fd), close(UDP fd)
 *     app_reconnect       -> rc_reconnect_now -> the same, then a new worker
 *     app_ptt(ON/TOGGLE)  -> tx_start while not connected -> rc_reconnect_now
 *
 * Made straight from a click, those closes land under enabled notifiers. The
 * next poll() returns POLLNVAL and Qt prints "QSocketNotifier: Invalid socket
 * 26 and type 'Read', disabling..." — seen at every "disconnected by you" in
 * the 0.1.13 journal. That much is noise. The hazard is the window between the
 * close() and the next reconcile(): the connect worker, QWebSocket or D-Bus can
 * be handed the freed fd NUMBER, and a notifier that still believes it owns it
 * is either a second notifier on a Qt socket ("Multiple socket notifiers",
 * undefined dispatch) or a wake-up for a socket the core does not own.
 *
 * run() closes the window from both ends: CoreLoop silences every notifier
 * BEFORE the call, so nothing is ever enabled on a descriptor while it is
 * closed, and reconciles AFTER it, so the new set is watched at once.
 *
 * A free-standing hook rather than a CoreLoop pointer handed to every widget:
 * the tray, the connection bar, the window and the push-to-talk manager all
 * make these calls, SVX_WINDOW_ONLY has no loop at all, and the unit tests
 * drive PttManager against a stub core with no loop either. With no guard
 * installed, run() simply makes the call.
 */
#ifndef SVXCONNECT_OMARCHY_COREACTION_H
#define SVXCONNECT_OMARCHY_COREACTION_H

#include <functional>

#include "core/svxcore.h"

namespace CoreAction {

/* Installed by CoreLoop for its lifetime. `before` runs immediately before
 * the call, `after` immediately after. Pass empty functions to remove. */
void setGuard(std::function<void()> before, std::function<void()> after);

/* Make a core call that may close or open the core's sockets. */
void run(const std::function<void()> &call);

/* The calls that can, spelled out so no caller has to remember which. */
void ptt(svx_app *app, ctl_tristate v);
void toggleConnect(svx_app *app);
void reconnect(svx_app *app);

} // namespace CoreAction

#endif
