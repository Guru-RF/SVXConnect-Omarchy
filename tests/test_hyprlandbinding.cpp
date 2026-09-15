/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * The Hyprland push-to-talk binding writes into the user's own bindings.lua,
 * so the string handling has to be exactly right: a chord must round-trip,
 * a typed key must never be accepted as a global PTT key, and rewriting the
 * block must leave every other byte of the file alone.
 */
#include <cstdio>
#include <cstdlib>

#include <QByteArray>
#include <QString>

#include "ptt/hyprlandbinding.h"

namespace {

int g_failures = 0;

void check(bool ok, const char *what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

void checkEq(const QString &got, const QString &want, const char *what)
{
    const bool ok = got == want;
    check(ok, what);
    if (!ok)
        std::printf("      got:  [%s]\n      want: [%s]\n",
                    qPrintable(got), qPrintable(want));
}

} // namespace

int main()
{
    using namespace HyprlandBinding;

    /* ---- chords ---- */
    checkEq(normalizeKeys(QStringLiteral("super+grave")), QStringLiteral("SUPER + GRAVE"),
            "normalises case and spacing");
    checkEq(normalizeKeys(QStringLiteral("SUPER CTRL + P")), QStringLiteral("SUPER + CTRL + P"),
            "accepts omarchy's printed form");
    checkEq(normalizeKeys(QStringLiteral("ctrl + logo + F12")), QStringLiteral("SUPER + CTRL + F12"),
            "folds LOGO into SUPER and orders modifiers");
    checkEq(normalizeKeys(QStringLiteral("Control+Mod1+x")), QStringLiteral("CTRL + ALT + X"),
            "folds CONTROL and MOD1");
    checkEq(modsToString(64 | 4 | 1), QStringLiteral("SUPER + CTRL + SHIFT"), "modmask to names");

    check(!parseKeys(QStringLiteral("SUPER + CTRL")).ok, "modifiers without a key are not a chord");
    check(!parseKeys(QStringLiteral("A + B")).ok,        "two keys are not a chord");
    check(!parseKeys(QString()).ok,                      "empty is not a chord");

    /* ---- safety ---- */
    QString why;
    check(isSafeKeys(QStringLiteral("SUPER + GRAVE"), &why),  "SUPER + GRAVE is safe");
    check(isSafeKeys(QStringLiteral("F12"), &why),            "bare F12 is safe");
    check(isSafeKeys(QStringLiteral("PAUSE"), &why),          "bare PAUSE is safe");
    check(!isSafeKeys(QStringLiteral("RETURN"), &why),        "bare RETURN is refused");
    check(!why.isEmpty(),                                     "a refusal explains itself");
    check(!isSafeKeys(QStringLiteral("SHIFT + RETURN"), &why), "SHIFT alone does not make a key safe");
    check(!isSafeKeys(QStringLiteral("CAPS + RETURN"), &why),  "CAPS alone does not make a key safe");
    check(!isSafeKeys(QStringLiteral("SPACE"), &why),          "bare SPACE is refused");

    /* ---- hyprctl binds -j ---- */
    const QByteArray json = R"([
        {"modmask":64,"submap":"","key":"RETURN","description":"Terminal","dispatcher":"__lua"},
        {"modmask":64,"submap":"","key":"grave","description":"SVXConnect push-to-talk","dispatcher":"__lua"},
        {"modmask":68,"submap":"","key":"P","description":"Power","dispatcher":"__lua"},
        {"modmask":64,"submap":"resize","key":"X","description":"Resize","dispatcher":"__lua"}
    ])";
    checkEq(keysFromBindsJson(json), QStringLiteral("SUPER + GRAVE"), "finds our bind by description");
    checkEq(keysFromBindsJson(QByteArray("[]")), QString(), "no bind, no keys");
    checkEq(keysFromBindsJson(QByteArray("not json")), QString(), "garbage does not crash");

    checkEq(conflictFromBindsJson(json, QStringLiteral("SUPER + CTRL + P")), QStringLiteral("Power"),
            "reports what a chord is already bound to");
    checkEq(conflictFromBindsJson(json, QStringLiteral("super + return")), QStringLiteral("Terminal"),
            "conflict matching ignores case");
    checkEq(conflictFromBindsJson(json, QStringLiteral("SUPER + GRAVE")), QString(),
            "our own bind is not a conflict");
    checkEq(conflictFromBindsJson(json, QStringLiteral("SUPER + X")), QString(),
            "a bind in another submap is not a conflict");

    /* ---- bindings.lua ---- */
    const QString user = QStringLiteral(
        "-- my bindings\n"
        "o.bind(\"SUPER + B\", \"Browser\", \"chromium\")\n");

    const QString once = withManagedBlock(user, QStringLiteral("super+grave"), QString());
    check(once.startsWith(user), "appending keeps the user's text as it was");
    check(once.contains(QStringLiteral(
              "hl.bind(\"SUPER + GRAVE\", hl.dsp.global(\"SVXConnect:ptt\"),\n"
              "        { description = \"SVXConnect push-to-talk\" })\n")),
          "the block binds the global shortcut with our description");
    checkEq(keysFromLua(once), QStringLiteral("SUPER + GRAVE"), "reads the key back from Lua");

    const QString twice = withManagedBlock(once, QStringLiteral("SUPER + GRAVE"), QString());
    checkEq(twice, once, "re-applying the same key is a no-op");

    const QString moved = withManagedBlock(once + QStringLiteral("-- after\n"),
                                           QStringLiteral("SUPER + F12"), QString());
    check(moved.startsWith(user) && moved.endsWith(QStringLiteral("-- after\n")),
          "replacing the block preserves text before and after it");
    checkEq(keysFromLua(moved), QStringLiteral("SUPER + F12"), "the replaced block has the new key");
    check(moved.count(QStringLiteral(">>> SVXConnect")) == 1, "there is only ever one block");

    const QString taken = managedBlock(QStringLiteral("SUPER + CTRL + P"), QStringLiteral("Power"));
    check(taken.contains(QStringLiteral("-- SUPER + CTRL + P was: Power\n"
                                        "hl.unbind(\"SUPER + CTRL + P\")\n")),
          "a taken chord is unbound first, and the block says what it was");

    checkEq(withoutManagedBlock(once), user, "removing the block restores the file exactly");
    checkEq(withoutManagedBlock(user), user, "removing from a file with no block changes nothing");
    checkEq(withManagedBlock(QString(), QStringLiteral("F12"), QString()),
            managedBlock(QStringLiteral("F12"), QString()),
            "an empty file gets just the block");

    checkEq(keysFromLua(QStringLiteral("-- hl.bind(\"F1\", hl.dsp.global(\"SVXConnect:ptt\"))\n")),
            QString(), "a commented-out bind is not a binding");

    const QString fifo = fifoExample(QStringLiteral("/run/user/1000/svxconnect/ctl"),
                                     QStringLiteral("f12"));
    check(fifo.contains(QStringLiteral("{ release = true }")), "the FIFO example unkeys on release");
    check(fifo.contains(QStringLiteral("echo 'ptt on' > /run/user/1000/svxconnect/ctl")),
          "the FIFO example writes to the configured FIFO");

    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "passed",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
