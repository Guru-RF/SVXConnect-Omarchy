/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * The push-to-talk key, as a Hyprland binding.
 *
 * HOW GLOBAL PUSH-TO-TALK WORKS ON HYPRLAND
 * ----------------------------------------
 * On KDE the GlobalShortcuts portal owns the key: the application suggests a
 * trigger, the desktop assigns one, and the user changes it in System Settings.
 * xdg-desktop-portal-hyprland works the other way round. The portal only
 * REGISTERS the shortcut — it appears in `hyprctl globalshortcuts` as
 * "SVXConnect:ptt" — and ignores preferred_trigger entirely. Nothing fires
 * until the user's Hyprland config binds a key to it with the `global`
 * dispatcher:
 *
 *     hl.bind("SUPER + GRAVE", hl.dsp.global("SVXConnect:ptt"))
 *
 * Hyprland then sends the press AND the release to the portal, which emits
 * Activated and Deactivated — a real hold-to-talk, with no permission to read
 * the keyboard.
 *
 * So on Omarchy the binding lives in ~/.config/hypr/bindings.lua, which is the
 * user's file. This module writes one clearly-marked block there on request
 * (and never otherwise), validates it by asking Hyprland for config errors,
 * rolls back if Hyprland rejects it, and reads back which key is bound from
 * `hyprctl binds`, matching on the bind's description.
 *
 * The string functions are pure and unit-tested (tests/test_hyprlandbinding.cpp);
 * only the functions marked LIVE run hyprctl.
 */
#ifndef SVXCONNECT_OMARCHY_HYPRLANDBINDING_H
#define SVXCONNECT_OMARCHY_HYPRLANDBINDING_H

#include <QByteArray>
#include <QString>

#include <functional>

class QObject;

namespace HyprlandBinding {

/* Hyprland names a portal shortcut "<app id>:<shortcut id>". The app id is the
 * .desktop basename PortalBackend registers with. */
inline constexpr char kShortcutId[]  = "SVXConnect:ptt";

/* The description on our bind. It is how the binding is found again in
 * `hyprctl binds -j`, where every Lua bind's dispatcher reads "__lua". */
inline constexpr char kDescription[] = "SVXConnect push-to-talk";

/* Free in a stock Omarchy, next to 1 under the left hand, and not a key
 * anything types — so a held Super+` can only ever mean "transmit". */
inline constexpr char kDefaultKeys[] = "SUPER + GRAVE";

/* Is this session Hyprland? */
bool isHyprland();

/* ~/.config/hypr/bindings.lua (honouring XDG_CONFIG_HOME). */
QString bindingsFile();

/* The answer the last refreshBoundKeys() got, without asking again. Empty
 * until the first refresh has come back. */
QString cachedBoundKeys();

/* LIVE, asynchronous. The key bound to push-to-talk, "SUPER + GRAVE", or
 * empty: asks Hyprland in the background, falling back to reading
 * bindings.lua for a bind written without our description, and calls `done`
 * with the answer on `context`'s thread once it is in — or with the fallback
 * if hyprctl has not answered in 3 s. Returns immediately. `done` is dropped
 * if `context` is destroyed first.
 *
 * There is deliberately no synchronous version. The interface asked on every
 * window activation, and waiting up to 3 s for hyprctl on the GUI thread
 * stalls the core it also runs. */
void refreshBoundKeys(QObject *context, std::function<void(const QString &keys)> done);

/* LIVE. What `keys` is already bound to, as its description, or empty. */
QString conflictFor(const QString &keys);

/* ---- pure ---- */

struct Keys {
    int     mods = 0;   /* Hyprland modmask: SHIFT 1, CAPS 2, CTRL 4, ALT 8, SUPER 64 */
    QString key;        /* upper case, e.g. "GRAVE" */
    bool    ok = false;
};

/* "super ctrl+p" -> "SUPER + CTRL + P". Aliases fold: CONTROL -> CTRL,
 * WIN / LOGO / MOD4 -> SUPER, MOD1 -> ALT. */
QString normalizeKeys(const QString &keys);
Keys    parseKeys(const QString &keys);
QString modsToString(int modmask);

/* Refuses a chord that would transmit while typing: a bare key, or one with
 * only Shift, unless it is a key nobody types (F1-F24, Pause, Scroll_Lock,
 * Insert, Menu). */
bool isSafeKeys(const QString &keys, QString *why);

QString keysFromBindsJson(const QByteArray &json);
QString conflictFromBindsJson(const QByteArray &json, const QString &keys);
QString keysFromLua(const QString &luaText);

/* The marked block. `replaces`, when not empty, is the description of the
 * binding currently on `keys`; the block then unbinds it first, and says so. */
QString managedBlock(const QString &keys, const QString &replaces);

/* `fileText` with the marked block replaced, or appended if there is none.
 * Everything outside the markers is preserved byte for byte. */
QString withManagedBlock(const QString &fileText, const QString &keys, const QString &replaces);
QString withoutManagedBlock(const QString &fileText);

/* A ready-to-paste bind that drives the control FIFO instead of the portal. */
QString fifoExample(const QString &fifoPath, const QString &keys);

/* ---- actions ---- */

/* Write the block into bindings.lua (backing the file up first), reload
 * Hyprland, and roll back if that introduced config errors. */
bool install(const QString &keys, const QString &replaces, QString *error);

/* Remove the block, if there is one, and reload. */
bool remove(QString *error);

/* Open bindings.lua in the Omarchy editor (omarchy-launch-editor), else xdg-open. */
bool openBindingsFile();

} // namespace HyprlandBinding

#endif
