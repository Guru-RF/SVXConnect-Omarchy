/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * Reading the push-to-talk key from Hyprland without waiting for hyprctl.
 *
 * The window asked on every activation, and ran `hyprctl binds -j` with a
 * 3 s wait on the GUI thread — the thread that also runs the core, so a slow
 * Hyprland socket stalled audio and heartbeats with it. A fake hyprctl on
 * PATH here takes its time; asking must return at once, and the answer (or
 * the bindings.lua fallback, if hyprctl never answers) must still arrive.
 */
#include <cstdio>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include "ptt/hyprlandbinding.h"

using namespace HyprlandBinding;

namespace {

int g_fail;
int g_run;

void check(bool ok, const char *what)
{
    ++g_run;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

void writeFile(const QString &path, const QByteArray &text, bool exec = false)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.write(text);
    f.close();
    if (exec)
        f.setPermissions(f.permissions() | QFileDevice::ExeOwner);
}

/* A hyprctl that answers `binds -j` after `delay` seconds. */
void fakeHyprctl(const QString &dir, const char *delay)
{
    writeFile(dir + QStringLiteral("/hyprctl"),
              QByteArray("#!/bin/sh\nsleep ") + delay + QByteArray(
              "\necho '[{\"modmask\":64,\"submap\":\"\",\"key\":\"grave\","
              "\"description\":\"SVXConnect push-to-talk\",\"dispatcher\":\"__lua\"}]'\n"),
              /*exec=*/true);
}

bool waitFor(const bool &flag, int ms)
{
    QElapsedTimer t;
    t.start();
    while (!flag && t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return flag;
}

} // namespace

int main(int argc, char **argv)
{
    QTemporaryDir bin, conf;
    qputenv("PATH", bin.path().toUtf8() + ":/usr/bin:/bin");
    qputenv("HYPRLAND_INSTANCE_SIGNATURE", "test");
    qputenv("XDG_CONFIG_HOME", conf.path().toUtf8());

    QCoreApplication app(argc, argv);

    /* ---- a slow but working Hyprland ---- */
    {
        fakeHyprctl(bin.path(), "1");
        QObject ctx;
        bool done = false;
        QString got;

        QElapsedTimer t;
        t.start();
        refreshBoundKeys(&ctx, [&](const QString &keys) { got = keys; done = true; });
        const qint64 took = t.elapsed();
        std::printf("      refreshBoundKeys() returned in %lld ms\n", static_cast<long long>(took));
        check(took < 100, "asking does not wait for hyprctl");
        check(!done, "...and the answer is not there yet");

        check(waitFor(done, 3000), "the answer arrives");
        check(got == QLatin1String("SUPER + GRAVE"), "it is the bind Hyprland reports");
        check(cachedBoundKeys() == QLatin1String("SUPER + GRAVE"), "and it is cached");
    }

    /* ---- a Hyprland that never answers: fall back to bindings.lua ---- */
    {
        fakeHyprctl(bin.path(), "30");
        QDir().mkpath(conf.path() + QStringLiteral("/hypr"));
        writeFile(bindingsFile(), managedBlock(QStringLiteral("SUPER + F12"), QString()).toUtf8());

        QObject ctx;
        bool done = false;
        QString got;
        QElapsedTimer t;
        t.start();
        refreshBoundKeys(&ctx, [&](const QString &keys) { got = keys; done = true; });
        check(waitFor(done, 5000), "a hung hyprctl is given up on");
        std::printf("      gave up after %lld ms\n", static_cast<long long>(t.elapsed()));
        check(t.elapsed() < 4000, "...within the 3 s allowance");
        check(got == QLatin1String("SUPER + F12"), "...and the file is read instead");
    }

    /* ---- the asker goes away first ---- */
    {
        fakeHyprctl(bin.path(), "1");
        bool called = false;
        {
            QObject ctx;
            refreshBoundKeys(&ctx, [&](const QString &) { called = true; });
        }
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 1500)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        check(!called, "no answer is delivered to an asker that is gone");
    }

    std::printf("\n%d/%d passed\n", g_run - g_fail, g_run);
    return g_fail ? 1 : 0;
}
