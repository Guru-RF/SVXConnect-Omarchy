#!/bin/sh
# SPDX-License-Identifier: MIT
# SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
#
# Fails if the built binary links a GPL-3.0-only Qt module.
#
# cmake/LicenceGuard.cmake refuses those modules at configure time, but that
# check only sees the component list it is handed — a contributor who adds a
# second find_package(Qt6 COMPONENTS Charts) bypasses it entirely. This script
# inspects the REAL link graph of the REAL artefact, which is what actually
# determines the licence of what ships.
#
#   usage: tools/licence_guard.sh <binary> [bundle-dir]
#
# The optional bundle directory is for AppImage builds: a GPL-only module can
# arrive as a dlopen()ed Qt plugin, which never appears in ldd output.

set -eu

BIN="${1:-build/svxconnect-omarchy}"
BUNDLE="${2:-}"

if [ ! -e "$BIN" ]; then
    echo "licence_guard: $BIN not found" >&2
    exit 2
fi

# Qt Lottie's library is libQt6Bodymovin, not libQt6Lottie. A pattern matching
# "Lottie" would never fire.
FORBIDDEN='libQt6(Charts|Graphs|DataVisualization|WebEngine[A-Za-z]*|VirtualKeyboard|WaylandCompositor|Quick3D|HttpServer|Mqtt|Coap|NetworkAuth|QmlCompiler|Bodymovin)'

rc=0

hits=$(ldd "$BIN" 2>/dev/null | grep -Ei "$FORBIDDEN" || true)
if [ -n "$hits" ]; then
    echo "licence_guard: GPL-3.0-only Qt module linked into $BIN:" >&2
    echo "$hits" >&2
    rc=1
fi

if [ -n "$BUNDLE" ] && [ -d "$BUNDLE" ]; then
    hits=$(find "$BUNDLE" -name 'libQt6*' -print 2>/dev/null | grep -Ei "$FORBIDDEN" || true)
    if [ -n "$hits" ]; then
        echo "licence_guard: GPL-3.0-only Qt module bundled in $BUNDLE:" >&2
        echo "$hits" >&2
        rc=1
    fi
fi

# ncurses must never appear: src/ui/ui.c is excluded from the svxcore build, and
# if it ever creeps back in, the binary would pick up a dependency this package
# has no business carrying.
if ldd "$BIN" 2>/dev/null | grep -qiE 'libncurses'; then
    echo "licence_guard: $BIN links ncurses — src/ui/ui.c leaked into svxcore" >&2
    rc=1
fi

if [ "$rc" -eq 0 ]; then
    echo "licence_guard: OK — only LGPL Qt modules linked"
fi
exit "$rc"
