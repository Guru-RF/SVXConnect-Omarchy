# SPDX-License-Identifier: MIT
# SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
#
# Builds the SVXConnect-CLI core as a static library, `svxcore`.
#
# This is the same decision SVXConnect-PIOS made for GTK3: the protocol, mTLS,
# AES-GCM, Opus, the jitter buffer, the talkgroup state machine, the control
# FIFO, the run lock and the status export are all already written, in C, and
# are the shipped-and-on-the-air implementation. The Qt front end presents them;
# it does not re-derive them.
#
# Requires, from the caller, BEFORE include()ing this file:
#     find_package(PkgConfig REQUIRED)
# because pkg_check_modules() below is defined by that module. Getting this
# order wrong yields `Unknown CMake command "pkg_check_modules"`.

if(NOT COMMAND pkg_check_modules)
    message(FATAL_ERROR
        "SvxCore.cmake needs find_package(PkgConfig REQUIRED) to have run first.")
endif()

# ---------------------------------------------------------------------------
# Source selection
#
# Exactly two files in the CLI tree must be excluded, and both for a concrete
# reason rather than tidiness:
#   src/main.c    — the only int main() in the tree; linking it would collide
#                   with ours and pull in the whole CLI argument parser.
#   src/ui/ui.c   — the only <ncurses.h> consumer; linking it would add a
#                   dependency on libncursesw to a GUI that never draws a
#                   terminal, and would show up in dpkg-shlibdeps output.
# Everything else — all 24 remaining translation units — is wanted.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE SVXCORE_SRC CONFIGURE_DEPENDS "${CLI_DIR}/src/*.c")
list(FILTER SVXCORE_SRC EXCLUDE REGEX "/src/main\\.c$")
list(FILTER SVXCORE_SRC EXCLUDE REGEX "/src/ui/ui\\.c$")

list(LENGTH SVXCORE_SRC SVXCORE_N)
if(SVXCORE_N LESS 20)
    message(FATAL_ERROR
        "Only ${SVXCORE_N} core source files found under ${CLI_DIR}/src.\n"
        "  The submodule is probably empty — run 'git submodule update --init',\n"
        "  or point at a working tree with -DCLI_DIR=../SVXConnect-CLI")
endif()

# The drift guard. Compiled as C, alongside the core, on every build: it
# includes app.h, audio/dev.h AND our C++-clean shim, so gcc cross-checks every
# prototype the shim duplicates. See tools/shim_check.c and src/core/svxcore.h.
list(APPEND SVXCORE_SRC "${CMAKE_SOURCE_DIR}/tools/shim_check.c")

add_library(svxcore STATIC ${SVXCORE_SRC})

# gnu11, NOT c11: the core uses strcasestr, strdup and friends, which are glibc
# extensions hidden behind _GNU_SOURCE and unavailable under -std=c11.
set_target_properties(svxcore PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON
    POSITION_INDEPENDENT_CODE ON)

target_compile_definitions(svxcore PUBLIC _GNU_SOURCE)

target_include_directories(svxcore
    PUBLIC  "${CLI_DIR}/src"            # the core's own headers
            "${CMAKE_SOURCE_DIR}/src"   # so shim_check.c finds core/svxcore.h
    PRIVATE "${CLI_DIR}/third_party")   # vendored miniaudio.h

# The core is warning-clean under these; the exclusions are upstream style
# choices, not defects we are papering over.
target_compile_options(svxcore PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation)

# miniaudio is a 90k-line single-header library and is not our code to tidy.
set_source_files_properties("${CLI_DIR}/src/audio/dev_miniaudio.c"
    PROPERTIES COMPILE_OPTIONS
    "-Wno-unused-function;-Wno-unused-variable;-Wno-sign-compare;-Wno-unused-but-set-variable")

# ---------------------------------------------------------------------------
# Dependencies
#
# libopus's headers live in /usr/include/opus, so the include path is NOT
# optional — codec.c does #include <opus.h> and fails without it. That is what
# PkgConfig::OPUS carries, and it is the one thing a hand-written -lopus misses.
#
# -lresolv is required: common/net.c uses res_query() and ns_initparse() for
# SRV lookups. On glibc 2.34+ these moved back into libc for some symbols but
# not all; link it explicitly rather than depending on which.
#
# There is deliberately NO ALSA or PulseAudio dependency here. miniaudio
# dlopen()s libasound.so.2 / libpulse.so.0 at runtime — that is what -ldl is
# for — which is why no -dev package is needed to build, and equally why
# no linker-based dependency scan can see them and the package must name them
# by hand (packaging/aur/*/PKGBUILD).
# ---------------------------------------------------------------------------
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)

target_link_libraries(svxcore PUBLIC
    OpenSSL::SSL
    OpenSSL::Crypto
    PkgConfig::OPUS
    Threads::Threads
    resolv
    m
    ${CMAKE_DL_LIBS})

message(STATUS "svxcore: ${SVXCORE_N} core sources from ${CLI_DIR}")
