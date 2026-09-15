/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * The drift guard for src/core/svxcore.h.
 *
 * svxcore.h re-declares a subset of the core's API by hand, because src/app.h
 * and audio/dev.h cannot be parsed by a C++ compiler (they use _Atomic). A
 * hand-copied prototype that quietly disagrees with the real one is the worst
 * possible failure: the C++ side would call through a wrong signature and the
 * linker would be perfectly happy.
 *
 * So this file includes BOTH the real headers AND the shim, and is compiled as
 * C as part of the normal build. gcc then checks every duplicated declaration
 * against its original. Change a signature upstream without updating the shim
 * and the build stops here with "conflicting types for ...".
 *
 * Include order is deliberate: the real headers come first so that
 * SVX_APP_H and SVX_AUDIO_DEV_H are already defined when svxcore.h is reached,
 * which suppresses the shim's copies of the anonymous-struct typedefs
 * (svx_devinfo, app_log_line) and its two anonymous enums. Those cannot be
 * repeated in C at all — an anonymous struct definition is a distinct type
 * every time it appears — so only the function prototypes are cross-checked.
 * That is exactly the part that matters.
 */
#include "app.h"
#include "audio/dev.h"

#include "core/svxcore.h"

/* Something with external linkage, so the translation unit is not empty and
 * the object file is genuinely produced and linked. */
int svxconnect_shim_check_ok = 1;
