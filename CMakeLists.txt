# SPDX-License-Identifier: MIT
# SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
#
# SVXConnect for Omarchy: a Qt 6 desktop client for SvxLink v3 reflectors,
# wrapping the SVXConnect-CLI core, styled live from the Omarchy theme and with
# push-to-talk bound through Hyprland. A port of SVXConnect-Qt.
#
# Build:
#     cmake -B build
#     cmake --build build
#
# Against a sibling CLI working tree instead of the pinned submodule:
#     cmake -B build -DCLI_DIR=../SVXConnect-CLI

cmake_minimum_required(VERSION 3.22)

project(svxconnect-omarchy
    VERSION 0.1.4
    DESCRIPTION "SvxLink reflector client for Omarchy"
    HOMEPAGE_URL "https://svxconnect.app"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# Handy for clangd / clang-tidy; costs nothing.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# The C core
# ---------------------------------------------------------------------------
set(CLI_DIR "${CMAKE_SOURCE_DIR}/third_party/svxconnect-cli"
    CACHE PATH "Path to the SVXConnect-CLI source tree")
get_filename_component(CLI_DIR "${CLI_DIR}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")

if(NOT EXISTS "${CLI_DIR}/src/app.c")
    message(FATAL_ERROR
        "\n"
        "  The SVXConnect-CLI core was not found at:\n"
        "      ${CLI_DIR}\n"
        "\n"
        "  Either initialise the submodule:\n"
        "      git submodule update --init\n"
        "  or point at a working tree:\n"
        "      cmake -B build -DCLI_DIR=../SVXConnect-CLI\n")
endif()

# MUST precede SvxCore.cmake, which calls pkg_check_modules().
find_package(PkgConfig REQUIRED)

include(cmake/Version.cmake)
include(cmake/LicenceGuard.cmake)
include(cmake/SvxCore.cmake)

svx_resolve_version(SVXCONNECT_VERSION)

# ---------------------------------------------------------------------------
# Qt
#
# Every component goes through svx_find_qt6(), which refuses the GPL-3.0-only
# modules. See cmake/LicenceGuard.cmake for why that matters here.
#
# Svg is not only for the icon: the stylesheet draws its arrows and check marks
# from SVG files generated in the theme's colours, and those load through the
# Qt SVG image-format plugin.
# ---------------------------------------------------------------------------
svx_find_qt6(6.4 COMPONENTS Core Gui Widgets Network DBus Svg)

qt_standard_project_setup()

# ---------------------------------------------------------------------------
# The application
# ---------------------------------------------------------------------------
set(APP_SOURCES
    src/main.cpp

    src/core/coreloop.cpp        src/core/coreloop.h
    src/core/logbridge.cpp       src/core/logbridge.h
    src/core/runlock.cpp         src/core/runlock.h
    src/core/devlist.cpp         src/core/devlist.h
    src/core/svxcore.h

    src/ui/omarchytheme.cpp      src/ui/omarchytheme.h
    src/ui/theme.h
    src/ui/mainwindow.cpp        src/ui/mainwindow.h
    src/ui/levelmeter.cpp        src/ui/levelmeter.h
    src/ui/talkgroupbutton.cpp   src/ui/talkgroupbutton.h
    src/ui/timefmt.cpp           src/ui/timefmt.h
    src/ui/statusbar.cpp         src/ui/statusbar.h
    src/ui/sidebar.cpp           src/ui/sidebar.h
    src/ui/activitypanel.cpp     src/ui/activitypanel.h
    src/ui/trayicon.cpp          src/ui/trayicon.h
    src/ui/notifier.cpp          src/ui/notifier.h
    src/ui/configfile.cpp        src/ui/configfile.h
    src/ui/preferencesdialog.cpp src/ui/preferencesdialog.h
    src/ui/locationdialog.cpp    src/ui/locationdialog.h

    src/settings/configstore.cpp src/settings/configstore.h

    src/ptt/pttbackend.h
    src/ptt/pttmanager.cpp       src/ptt/pttmanager.h
    src/ptt/portalbackend.cpp    src/ptt/portalbackend.h
    src/ptt/hyprlandbinding.cpp  src/ptt/hyprlandbinding.h
)

qt_add_executable(svxconnect-omarchy ${APP_SOURCES} resources/icons.qrc)

target_include_directories(svxconnect-omarchy PRIVATE "${CMAKE_SOURCE_DIR}/src")

target_compile_definitions(svxconnect-omarchy PRIVATE
    SVXCONNECT_VERSION="${SVXCONNECT_VERSION}"
    # Refuse to build against a Qt that would silently narrow QString
    # conversions, and keep tr() honest.
    QT_NO_CAST_FROM_ASCII
    QT_NO_CAST_TO_ASCII
    QT_USE_QSTRINGBUILDER)

target_compile_options(svxconnect-omarchy PRIVATE -Wall -Wextra -Wno-unused-parameter)

target_link_libraries(svxconnect-omarchy PRIVATE
    svxcore
    Qt6::Widgets
    Qt6::Network
    Qt6::DBus
    Qt6::Svg)

# ---------------------------------------------------------------------------
# Tests
#
# Plain executables rather than QtTest: the properties worth checking here are
# about bytes in a file, and a test that needs no framework is a test that
# still runs when someone builds this in five years.
# ---------------------------------------------------------------------------
enable_testing()

add_executable(test_configstore
    tests/test_configstore.cpp
    src/settings/configstore.cpp)
target_include_directories(test_configstore PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(test_configstore PRIVATE svxcore Qt6::Core)
add_test(NAME configstore COMMAND test_configstore)

add_executable(test_talkercall tests/test_talkercall.cpp)
target_link_libraries(test_talkercall PRIVATE svxcore)
add_test(NAME talkercall COMMAND test_talkercall)

add_executable(test_notifier
    tests/test_notifier.cpp
    src/ui/notifier.cpp)
target_include_directories(test_notifier PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(test_notifier PRIVATE svxcore Qt6::Core Qt6::DBus)
add_test(NAME notifier COMMAND test_notifier)

add_executable(test_locationlookup
    tests/test_locationlookup.cpp
    src/ui/locationdialog.cpp
    src/ui/omarchytheme.cpp)
target_include_directories(test_locationlookup PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(test_locationlookup PRIVATE
    SVXCONNECT_VERSION="${SVXCONNECT_VERSION}"
    QT_NO_CAST_FROM_ASCII)
target_link_libraries(test_locationlookup PRIVATE
    svxcore Qt6::Widgets Qt6::Network Qt6::Svg)
add_test(NAME locationlookup COMMAND test_locationlookup)

add_executable(test_hyprlandbinding
    tests/test_hyprlandbinding.cpp
    src/ptt/hyprlandbinding.cpp)
target_include_directories(test_hyprlandbinding PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(test_hyprlandbinding PRIVATE QT_NO_CAST_FROM_ASCII)
target_link_libraries(test_hyprlandbinding PRIVATE svxcore Qt6::Core)
add_test(NAME hyprlandbinding COMMAND test_hyprlandbinding)

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS svxconnect-omarchy RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

install(FILES THIRD-PARTY-NOTICES LICENSE
        DESTINATION "${CMAKE_INSTALL_DOCDIR}")

# The basename is load-bearing: the GlobalShortcuts portal identifies the
# application by it, and Hyprland binds the shortcut as "SVXConnect:ptt".
install(FILES data/SVXConnect.desktop
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")

foreach(sz 16 22 24 32 48 64 128 256 512 1024)
    install(FILES "data/hicolor/${sz}x${sz}/apps/SVXConnect.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${sz}x${sz}/apps")
endforeach()

message(STATUS "")
message(STATUS "  SVXConnect-Omarchy ${SVXCONNECT_VERSION}")
message(STATUS "    core        ${CLI_DIR}")
message(STATUS "    Qt          ${Qt6_VERSION}")
message(STATUS "    build type  ${CMAKE_BUILD_TYPE}")
message(STATUS "")
