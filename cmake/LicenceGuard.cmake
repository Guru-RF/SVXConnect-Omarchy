# SPDX-License-Identifier: MIT
# SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
#
# Keeps this project MIT-licensable.
#
# SVXConnect-Omarchy's own source is MIT, and it links Qt 6 under the LGPLv3.
# That combination is legal and needs nothing but dynamic linking and the
# notices in THIRD-PARTY-NOTICES. But Qt's open-source edition is NOT uniformly
# LGPL: a handful of modules are GPL-3.0-only, and linking one silently
# converts the whole binary to GPLv3 while LICENSE still says MIT. Nothing in a
# normal build tells you this has happened — it is a licence bug that compiles,
# links, runs and ships.
#
# Qt Charts is the one people actually hit, because it is the obvious way to
# draw a meter or a level history. There is no LGPL charting module in Qt 6 at
# all. Verified: qtcharts' upstream LICENSES/ directory contains GPL-3.0-only,
# BSD-3-Clause and the commercial reference, and no LGPL text; Debian's
# qt6-charts-dev declares "License: GPL-3+". Qt's own licensing page omits
# Charts and Data Visualization from its GPL-only list only because both are
# deprecated — a future reader will misread that omission as "they are LGPL".
#
# The substitute is to draw it yourself: QPainter in paintEvent for the VU
# meters and level history, QGraphicsScene/QGraphicsView for anything
# scrollable. That is a few hundred lines, and it is what the macOS app ended
# up doing anyway for performance reasons.
#
# This file provides svx_find_qt6(), which is BOTH the guard and the
# find_package call. Keeping them as one avoids the failure mode where the deny
# list and the component list drift apart, or where a contributor adds a second
# find_package(Qt6 COMPONENTS Charts) on a new line and bypasses the check
# entirely. The CMake-time guard is still only advisory — tools/licence_guard.sh
# inspects the real link graph after the build, which is what actually counts.

set(SVX_GPL_ONLY_QT_MODULES
    # Qt's published GPL-3.0-only list
    Quick3D
    HttpServer
    Mqtt
    Coap
    NetworkAuth
    VirtualKeyboard
    WaylandCompositor
    QmlCompiler
    # Deprecated, therefore absent from that page, and still GPL-3.0-only
    Charts
    DataVisualization
    Graphs
    # Chromium; also ~186 MB installed and a 3722-line Debian copyright file
    WebEngineCore
    WebEngineWidgets
    WebEngineQuick
    # The CMake target for Qt Lottie is Bodymovin, not Lottie. Listing "Lottie"
    # here would be a deny-list entry that can never match anything.
    Bodymovin
    CACHE INTERNAL "Qt 6 modules that are GPL-3.0-only in the open-source edition")

# svx_find_qt6(<min-version> COMPONENTS <c> [<c>...])
#
# Refuses any GPL-3.0-only component, then does the find_package. Every Qt
# component this project uses must come through here.
#
# A MACRO, not a function, and that is load-bearing. find_package() sets its
# result variables — Qt6_VERSION, Qt6_VERSION_MAJOR, the QT_KNOWN_POLICY_*
# flags — in the scope it runs in. Inside a function those die at the closing
# paren, and the very next call fails with the memorable and unhelpful
#     CMake Error ... Qt6CoreMacros.cmake: Can not determine Qt version.
# A macro is textually inlined, so find_package runs in the caller's scope and
# everything downstream sees Qt normally.
macro(svx_find_qt6 MINVER)
    # Plain-signature cmake_parse_arguments: the PARSE_ARGV form is only
    # supported in function scope, which is exactly what we just gave up.
    cmake_parse_arguments(SVXQT "" "" "COMPONENTS" ${ARGN})

    foreach(m IN LISTS SVXQT_COMPONENTS)
        # Quoted, because an unquoted variable containing a ';' or an empty
        # string makes IN_LIST behave in ways that are entertaining at 2 a.m.
        if("${m}" IN_LIST SVX_GPL_ONLY_QT_MODULES)
            message(FATAL_ERROR
                "\n"
                "  Qt6::${m} is GPL-3.0-only in Qt's open-source edition.\n"
                "  Linking it makes this binary GPLv3 while LICENSE says MIT.\n"
                "\n"
                "  If you need a chart or a meter: draw it with QPainter in a\n"
                "  paintEvent. See cmake/LicenceGuard.cmake and THIRD-PARTY-NOTICES.\n")
        endif()
    endforeach()

    find_package(Qt6 ${MINVER} REQUIRED COMPONENTS ${SVXQT_COMPONENTS})

    # The resolved list, so the caller can link it without repeating itself —
    # the second half of "one list, not two". No PARENT_SCOPE needed: a macro
    # already runs in the caller's scope, which is the whole point.
    set(SVX_QT_COMPONENTS "${SVXQT_COMPONENTS}")
endmacro()
