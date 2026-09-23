#!/bin/sh
# SPDX-License-Identifier: MIT
# SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
#
# Does the application actually start?
#
# WHY THIS EXISTS
# ---------------
# Everything in tests/ is a pure function, and SVX_WINDOW_ONLY brings the
# window up with NO core on purpose. Between them they never touch the path a
# real start takes — so 0.1.8 shipped a null dereference in MainWindow's
# constructor that was reachable only with a core, crashed every launch, and
# passed every test and every screenshot run on the way out.
#
# This runs the real thing: config loaded, core started, window built, timers
# running. It uses the screenshot hook, which quits by itself, and it asserts
# both a clean exit and that a window was actually rendered — a process that
# dies in the constructor produces no PNG.
#
# No audio and no network are needed: the core logs "no audio system available"
# and carries on, and the reflector host is deliberately one that cannot
# resolve.
#
#   usage: tools/smoketest.sh [path/to/svxconnect-omarchy]

set -eu

BIN="${1:-build/svxconnect-omarchy}"

if [ ! -x "$BIN" ]; then
    echo "smoketest: $BIN not found or not executable" >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Nothing of the user's may be read or written, and the XDG variables are not
# enough for that: the core derives pki_dir from $HOME, not from
# XDG_CONFIG_HOME, so 0.1.13's smoke test went looking in the real
# ~/.config/svxconnect/pki. HOME is sandboxed too, pki_dir is named outright,
# and the log is checked for the real home directory below.
REAL_HOME="${HOME:-}"
mkdir -p "$WORK/home"

cat > "$WORK/svxconnect.conf" <<CONF
callsign = ON0TEST
reflector = reflector.invalid
port = 5300
monitored = 8, 9
switchable = 8
pki_dir = $WORK/pki
CONF

rc=0
HOME="$WORK/home" \
XDG_CONFIG_HOME="$WORK/config" \
XDG_STATE_HOME="$WORK/state" \
XDG_CACHE_HOME="$WORK/cache" \
XDG_RUNTIME_DIR="$WORK/run" \
QT_QPA_PLATFORM=offscreen \
SVX_SCREENSHOT="$WORK/shot.png" \
SVX_SCREENSHOT_DELAY=4000 \
    "$BIN" --config "$WORK/svxconnect.conf" > "$WORK/log" 2>&1 || rc=$?

echo "--- log ---"
cat "$WORK/log" || true
echo "-----------"

if [ "$rc" -ge 128 ]; then
    echo "smoketest: killed by signal $((rc - 128)) during start-up" >&2
    exit 1
fi
if [ "$rc" -ne 0 ]; then
    echo "smoketest: exited $rc during start-up" >&2
    exit 1
fi
if [ ! -s "$WORK/shot.png" ]; then
    echo "smoketest: no window was rendered — the process never got that far" >&2
    exit 1
fi

# Surviving is not the same as working. 0.1.9 fixed the 0.1.8 crash by moving a
# call above the objects it configures, where null checks turned it into a
# no-op: the application started perfectly and never once looked for an
# enhanced reflector. "It did not crash" passed; this would not have.
if ! grep -q "reflector feed: probing wss://reflector.invalid/" "$WORK/log"; then
    echo "smoketest: started, but never probed for the reflector's portal feed" >&2
    exit 1
fi
# $WORK itself may live under the real home, when TMPDIR does.
if [ -n "$REAL_HOME" ] \
   && grep -F "$REAL_HOME/" "$WORK/log" | grep -qvF "$WORK/"; then
    echo "smoketest: the application looked in the real home directory ($REAL_HOME):" >&2
    grep -F "$REAL_HOME/" "$WORK/log" | grep -vF "$WORK/" >&2
    exit 1
fi
if grep -q "internal:" "$WORK/log"; then
    echo "smoketest: the application reported an internal wiring error" >&2
    exit 1
fi

echo "smoketest: OK — started, built its window, probed for the portal, and quit cleanly"
