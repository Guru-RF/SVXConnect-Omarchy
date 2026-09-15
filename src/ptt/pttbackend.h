/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * Global push-to-talk: one interface, several backends.
 *
 * This is the feature the macOS app was never allowed to ship — its
 * HotkeyManager still carries the comment explaining that
 * NSEvent.addGlobalMonitorForEvents was flagged under App Store guideline
 * 2.4.5 — and the one a terminal cannot offer at all, because a terminal
 * delivers characters, not key events, so there is no release to detect.
 *
 * WHAT MAKES THIS HARDER THAN A NORMAL GLOBAL SHORTCUT
 * ----------------------------------------------------
 * A shortcut needs one edge: the activation. Push-to-talk needs BOTH edges,
 * and the release is the safety-critical one. A missed press is an annoyance;
 * a missed release is an unattended transmitter sitting on the air. Every
 * backend below is therefore judged first on whether it can report a release
 * at all, and PttManager treats "unkey" as the safe default in every error
 * path.
 *
 * THE FOUR BACKENDS
 * -----------------
 *   portal   org.freedesktop.portal.GlobalShortcuts. Works on Wayland and
 *            X11, needs no elevated permission, and — verified on Plasma
 *            6.3.6 with xdg-desktop-portal 1.20.3 — emits both Activated and
 *            Deactivated. The default.
 *
 *   fifo     The core's existing control FIFO, driven by a compositor key
 *            binding. Already works with no code at all; it is listed so the
 *            interface can report on it and the settings page can explain it.
 *
 * An evdev backend reading /dev/input/event* directly was written and then
 * REMOVED. It was the only way to bind a chord using CapsLock, because at that
 * layer CapsLock is just KEY_CAPSLOCK going down and up rather than a lock
 * state the shortcut layer discards — but it cost read access to every key the
 * device produces, which for a keyboard is every keystroke the user types.
 *
 * That turned out to be solving the problem at the wrong layer. The desktop
 * already offers it: the xkb option `caps:hyper` ("Make Caps Lock an
 * additional Hyper", in KDE's Keyboard → Key Bindings settings) turns CapsLock
 * into a real modifier, Hyper and Super share Mod4, and CapsLock+Enter is then
 * simply LOGO+Return — an ordinary portal binding, with no elevated permission
 * and no udev rule, that benefits every application rather than this one.
 *
 * A MEASURED WARNING ABOUT preferred_trigger
 * ------------------------------------------
 * The portal's preferred_trigger is a HINT. Tested against
 * xdg-desktop-portal-kde 6.3.5, with a distinct app id per probe so nothing
 * was served from kglobalaccel's cache:
 *
 *     CTRL+SHIFT+t  ->  "Ctrl+Shift+T"     honoured
 *     CTRL+Return   ->  "Ctrl+Return"      honoured
 *     F12           ->  "F12"              honoured
 *     SUPER+p       ->  ""                 rejected outright (the spec's
 *                                          modifier is LOGO, not SUPER)
 *     CAPS+Return   ->  "Return"           CapsLock SILENTLY DROPPED
 *
 * That last row is a live footgun. Asking for CapsLock+Enter and not checking
 * the reply leaves you bound to bare Enter — every time the operator presses
 * Enter in any application, anywhere, the transmitter keys. PortalBackend
 * therefore validates what came back and refuses a binding that lost its
 * modifiers, rather than trusting the request.
 */
#ifndef SVXCONNECT_OMARCHY_PTTBACKEND_H
#define SVXCONNECT_OMARCHY_PTTBACKEND_H

#include <QObject>
#include <QString>

/* What the user bound.
 *
 * Expressed in the freedesktop Shortcuts syntax the portal takes: modifiers
 * CTRL / ALT / SHIFT / LOGO joined with '+', then an xkbcommon keysym name
 * without its XKB_KEY_ prefix — "LOGO+Return", "CTRL+SHIFT+t".
 */
struct PttBinding {
    QString trigger;

    bool isValid() const { return !trigger.isEmpty(); }
};

/* Whether a backend can be used here, and if not, what the user must do. */
struct PttAvailability {
    enum State { Available, NeedsSetup, Unavailable };

    State   state = Unavailable;
    QString reason;          /* shown verbatim in the settings table       */
    QString instructions;    /* paste-able fix: a udev rule, a config line */
    bool    hasRelease = false;   /* false => toggle only; say so loudly   */
};

class PttBackend : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    ~PttBackend() override = default;

    virtual QString id() const = 0;            /* "portal" | "fifo" */
    virtual QString displayName() const = 0;

    /* Cheap, side-effect-free, and safe to call repeatedly — the settings
     * table re-runs it on a timer so a udev rule applied in another window
     * shows up without reopening the dialog. */
    virtual PttAvailability probe() const = 0;

    virtual bool start(const PttBinding &binding) = 0;
    virtual void stop() = 0;

    /* What the backend ACTUALLY bound, which for the portal may differ from
     * what was asked for. Display this, never the requested trigger. */
    virtual QString activeTrigger() const { return QString(); }

    /* Running and able to deliver presses. The default suits a backend whose
     * trigger is only known once it is live. */
    virtual bool isActive() const { return !activeTrigger().isEmpty(); }

signals:
    void pressed();
    void released();

    /* The backend can no longer guarantee it will see a release — the portal
     * session closed, for instance.
     * PttManager unkeys immediately on this — a transmitter that cannot be
     * stopped by its own button must not stay keyed. */
    void lost(const QString &why);

    /* The bound trigger changed under us (the user reassigned it in the
     * desktop's own shortcut settings). */
    void triggerChanged(const QString &human);
};

#endif
