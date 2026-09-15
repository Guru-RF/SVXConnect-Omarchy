/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * A C++-clean view of the SVXConnect-CLI core.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * No CLI header carries `extern "C"` guards, and four of them cannot be parsed
 * by a C++ compiler at all. Verified with g++ 14.2.0 against the pinned core:
 *
 *     BROKEN   common/ring.h   audio/dev.h   audio/codec.h   audio/jitter.h
 *              src/app.h       (transitively, via all four)
 *     CLEAN    common/{config,status,lock,log,util,pki,net,crypto,tls,proto}.h
 *              ctl/ctlfifo.h   reflector/{client,enroll,handshake,frameio,
 *                                        nodeinfo}.h        tg/tgmanager.h
 *
 * The break is always `_Atomic`, which is C11 and has no C++ spelling:
 *     ring.h:32-33   _Atomic uint32_t head, tail;
 *     dev.h:78-79    svx_dev *svx_dev_open_capture(..., _Atomic float *peak);
 *
 * So this header includes only the ten C++-clean headers and re-declares, by
 * hand, the handful of `app_*` and `svx_audio_*` prototypes the GUI actually
 * calls. Every one of them takes and returns only C++-clean types — that is
 * not luck, it is the reason this approach works at all: no `app_*` entry
 * point mentions a ring, a codec, a jitter buffer or an atomic.
 *
 * `-D_Atomic=` on the C++ translation units would also "work" and is a
 * struct-layout landmine (it changes the size and alignment of svx_ring in one
 * half of the program and not the other). Do not do it.
 *
 * THE DRIFT GUARD
 * ---------------
 * A shim that silently lies about a signature is worse than no shim. Every
 * declaration below is duplicated from a real header, so it must be checked
 * against that header. tools/shim_check.c includes app.h, audio/dev.h AND this
 * file, and is compiled as C on every build; if any prototype here drifts from
 * upstream, gcc fails with "conflicting types" and the build stops.
 *
 * That check is why the type definitions below are wrapped in the *upstream*
 * include guards rather than guards of our own. `svx_devinfo`, `app_log_line`
 * and the two enums are ANONYMOUS struct/enum typedefs upstream:
 *
 *     dev.h:44   typedef struct { char id[256]; ... } svx_devinfo;
 *     app.h:37   typedef struct { int level; char text[240]; } app_log_line;
 *
 * C11 permits repeating a typedef only when it names the *same* type, and two
 * anonymous struct definitions are always distinct types. Re-defining them
 * unconditionally makes shim_check.c itself fail to compile. Keying off
 * SVX_AUDIO_DEV_H / SVX_APP_H means: when the real header has already been
 * included (that is, in shim_check.c, and in any C file), use its definition;
 * when it has not (every C++ translation unit), provide ours.
 *
 * MAINTENANCE
 * -----------
 * When the core's API changes, this file changes with it and shim_check.c
 * tells you so. The long-term fix is upstream — `extern "C"` guards, an
 * SVX_ATOMIC macro and tagged typedefs in the CLI's own headers — at which
 * point this file is deleted. See docs/PLAN.md, decision Q7 item 14.
 */
#ifndef SVXCONNECT_OMARCHY_SVXCORE_H
#define SVXCONNECT_OMARCHY_SVXCORE_H

#include <stdint.h>
#include <stddef.h>
#include <poll.h>   /* struct pollfd, for app_poll_fds() */

/* The C++-clean subset of the core's headers. These are safe to include
 * directly from C++ and are NOT duplicated below. */
#ifdef __cplusplus
extern "C" {
#endif

#include "common/config.h"      /* svx_config, config_*  */
#include "common/log.h"         /* log_set_sink, log_level_from_name */
#include "common/lock.h"        /* svx_lock_acquire/who/release */
#include "common/status.h"      /* svx_status, svx_status_* */
#include "common/util.h"        /* now_ms, fmt_age, maidenhead, path_expand */
#include "common/pki.h"         /* pki_build_path, pki_cert_info */
#include "ctl/ctlfifo.h"        /* ctl_tristate: CTL_OFF / CTL_ON / CTL_TOGGLE */
#include "reflector/client.h"   /* rc_client, rc_state, rc_get_stats, ... */
#include "tg/tgmanager.h"       /* tg_manager, tgm_* */

/* ------------------------------------------------------------------------
 * From audio/dev.h — the parts that do not mention _Atomic.
 * Guarded by the upstream guard so that a C file which already included
 * dev.h gets dev.h's definitions and no conflicting-types error.
 * ------------------------------------------------------------------------ */
#ifndef SVX_AUDIO_DEV_H

/* The reflector carries 16 kHz mono Opus in 20 ms frames. */
#define SVX_RATE   16000
#define SVX_FRAME  320            /* samples in 20 ms at 16 kHz, mono */

typedef struct svx_dev svx_dev;

typedef struct {
    char id[256];        /* backend-stable identifier; "" means system default */
    char name[256];      /* what to show a human */
    int  is_default;
} svx_devinfo;

typedef enum {
    SVX_DEV_OK = 0,
    SVX_DEV_STOPPED,     /* the device stopped on its own */
    SVX_DEV_REROUTED,    /* the default device changed under us */
    SVX_DEV_LOST         /* unplugged */
} svx_dev_event;

typedef enum {
    SVX_MIC_DENIED       = -1,
    SVX_MIC_UNDETERMINED =  0,
    SVX_MIC_GRANTED      =  1
} svx_mic_state;

#endif /* SVX_AUDIO_DEV_H */

/* Device enumeration. BLOCKING — svx_audio_list() spins up a miniaudio context
 * (tens to hundreds of ms). Call it from the GUI thread when a picker opens,
 * never on a repaint tick. */
int  svx_audio_list(int capture, svx_devinfo *out, int max);

/* Resolve a user-supplied device string: id, then exact name, then
 * case-insensitive substring, then the system default. Returns 1 if a specific
 * device matched, 0 if it fell through to the default. That 0 is load-bearing
 * for the GUI: it is the only way to tell that a pinned device has vanished,
 * and the picker must surface it instead of silently showing the default. */
int  svx_audio_resolve(int capture, const char *want, svx_devinfo *out);

/* The human name of an open stream. NOTE: the GUI cannot reach the app's own
 * streams — app.c keeps play_dev / cap_dev private and exposes no accessor —
 * so the "Currently playing to: X" line needs an upstream addition before it
 * can be built. See docs/PLAN.md §3.9. Declared here for completeness only. */
const char *svx_dev_name(svx_dev *d);

/* macOS TCC; on Linux mic_perm.c returns SVX_MIC_GRANTED unconditionally.
 * The real Linux substitute is the core's silent-microphone detector. */
svx_mic_state svx_mic_status(void);

/* ------------------------------------------------------------------------
 * From src/app.h — the application core.
 * ------------------------------------------------------------------------ */

typedef struct svx_app svx_app;

#ifndef SVX_APP_H
/* How many log lines the interface's log pane can scroll back through. */
#define APP_LOG_LINES 512

typedef struct {
    int  level;
    char text[240];
} app_log_line;
#endif /* SVX_APP_H */

/* ---- lifetime ----
 * `cfg` is NOT copied: app.c:583 stores the pointer, and app_set_volume(),
 * app_set_input_device() and app_set_output_device() mutate it in place.
 * It must outlive the svx_app. Never pass a stack local. */
svx_app *app_new(const svx_config *cfg, int no_tx);
void     app_free(svx_app *a);

/* Label this process in the exported status file. We use "gui-qt", NOT "gui" —
 * SVXConnect-PIOS already registers as "gui" and expects to be raised with
 * SIGUSR1, which this application does not implement. The field is a fixed
 * char[16] in app.c, so the string must stay short. */
void     app_set_owner_kind(svx_app *a, const char *kind);

/* Open the audio devices and the control FIFO, then start connecting.
 * BLOCKING: miniaudio initialisation takes tens to hundreds of ms. */
void     app_start(svx_app *a);

/* ---- poll() integration ----
 * app_service() MUST be called every time round the loop even when poll()
 * returned nothing, because it also drives the timers. See core/coreloop.cpp. */
int      app_poll_fds(svx_app *a, struct pollfd *p, int max);
int      app_next_timeout_ms(svx_app *a, uint64_t now);
void     app_service(svx_app *a, uint64_t now);

/* ---- actions ---- */
void     app_ptt(svx_app *a, ctl_tristate v);
void     app_tg_next(svx_app *a);
void     app_tg_prev(svx_app *a);
void     app_tg_select(svx_app *a, uint32_t tg);
void     app_tg_index(svx_app *a, int idx);      /* the 1..9 keys */
void     app_toggle_lock(svx_app *a);
void     app_toggle_mute(svx_app *a, uint32_t tg);
void     app_volume_delta(svx_app *a, int delta);
void     app_set_volume(svx_app *a, int pct);    /* absolute 0..100 */

/* Runtime device switch. app_set_output_device() tears down the whole
 * miniaudio context and takes 1-2 s; it must run behind a wait cursor with the
 * combo disabled, and must never be called from inside app_service(). */
void     app_set_input_device(svx_app *a, const char *dev);
void     app_set_output_device(svx_app *a, const char *dev);
void     app_toggle_output_mute(svx_app *a);
void     app_test_tone(svx_app *a);
void     app_reconnect(svx_app *a);
void     app_toggle_connect(svx_app *a);
void     app_quit(svx_app *a);
int      app_should_quit(const svx_app *a);

/* ---- state, for rendering ---- */
const svx_config *app_config(const svx_app *a);
rc_client        *app_rc(svx_app *a);
tg_manager       *app_tgm(svx_app *a);

int          app_tx_active(const svx_app *a);
uint64_t     app_tx_elapsed_ms(const svx_app *a);
int          app_tx_available(const svx_app *a);   /* 0 when --no-tx or denied */
float        app_mic_level(const svx_app *a);
float        app_spk_level(const svx_app *a);
int          app_volume(const svx_app *a);
int          app_output_muted(const svx_app *a);
uint32_t     app_jitter_ms(const svx_app *a);
const char  *app_jitter_state(const svx_app *a);
int          app_audio_ready(const svx_app *a);

/* A banner the interface should show until dismissed; NULL when there is
 * nothing to say. */
const char  *app_banner(const svx_app *a);
void         app_dismiss_banner(svx_app *a);

/* Divert logging into the core's in-memory ring.
 *
 * DO NOT CALL THIS. It is here so the drift guard checks it, and so nobody
 * reintroduces it. The Qt front end installs its own thread-safe sink with
 * log_set_sink() instead, because log.c has no locking at all and
 * handshake_run() logs from the connect worker thread. See core/logbridge.cpp.
 * app_capture_log() also has a side effect the core's own comment gets wrong:
 * emit() returns as soon as the sink has been called, so capturing the log
 * means cfg.log_file receives nothing. */
void         app_capture_log(svx_app *a);

/* Ring of recent log lines, newest last. Returns how many were written. */
int          app_log_snapshot(const svx_app *a, app_log_line *out, int max);
uint64_t     app_log_serial(const svx_app *a);    /* bumps on every new line */

/* Called whenever something worth redrawing has changed.
 *
 * Nearly inert, and NOT the change signal it sounds like: app.c:83-85 fires it
 * only from banner_set, ctl_volume, app_set_volume, app_toggle_output_mute,
 * app_set_input_device, app_set_output_device and app_dismiss_banner. It does
 * not fire on connection-state change, talker start/stop or node join/leave,
 * and the TG manager's own `changed` callback is a no-op stub (app.c:444).
 * Polling on a timer is therefore mandatory, not an optimisation. */
void         app_set_observer(svx_app *a, void (*fn)(void *), void *user);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SVXCONNECT_OMARCHY_SVXCORE_H */
