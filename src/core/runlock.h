/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * The single-owner run lock.
 *
 * Exactly one process may hold the reflector connection: the same client
 * certificate and node id cannot be used twice at once. The ncurses TUI,
 * `svxconnect --headless`, the GTK GUI on Raspberry Pi OS and this application
 * all take the same advisory flock() on cfg.lock_file before connecting.
 *
 * WHO ELSE IS OUT THERE
 * ---------------------
 * The lock file records a `kind` string, and the three existing owners use:
 *
 *     "cli"       the ncurses client
 *     "headless"  svxconnect --headless, i.e. the systemd unit
 *     "gui"       SVXConnect-PIOS, the GTK client on Raspberry Pi OS
 *
 * We register as "gui-omarchy", NOT "gui". This is not cosmetic. PIOS's main.c
 * takes the lock as "gui" and expects an incumbent to be raised by
 * kill(pid, SIGUSR1). We raise over a QLocalSocket and do not install a
 * SIGUSR1 handler. If both were "gui", then on a Pi with both installed the
 * second launch would connect to a socket that does not exist, hang until the
 * connect timed out, and then report the wrong thing. Distinct kinds let the
 * raise path pick the right mechanism per holder — and it must, because a Pi
 * with both installed is a case the packaging explicitly contemplates.
 *
 * The status file's owner field is a fixed char[16], so any kind string must
 * stay short.
 *
 * `svxconnect --enroll` deliberately does NOT take this lock (verified: the
 * CLI's main.c calls enroll_run() with no acquire), so the enrolment wizard
 * works while this application holds it.
 */
#ifndef SVXCONNECT_OMARCHY_RUNLOCK_H
#define SVXCONNECT_OMARCHY_RUNLOCK_H

#include <QString>

namespace RunLock {

/* The kind this application records in the lock and status files. */
inline constexpr char kOwnerKind[] = "gui-omarchy";

enum class Result {
    Acquired,      /* we own the connection */
    HeldByOther,   /* someone else does — see holderKind()/holderPid() */
    Error          /* the lock file could not be created or opened */
};

struct Holder {
    QString kind;   /* "cli" | "headless" | "gui" | "gui-qt" | "gui-omarchy" | "" if unknown */
    long    pid = 0;

    /* Is the holder another instance of THIS application, i.e. one we know how
     * to raise over a QLocalSocket? */
    bool isSelfKind() const { return kind == QLatin1String(kOwnerKind); }

    /* Is it SVXConnect-PIOS, which wants SIGUSR1 instead? */
    bool isPios() const { return kind == QLatin1String("gui"); }

    /* A sentence naming who is in the way, for a dialog or a status line. */
    QString describe() const;
};

/* Try to take the lock at `lockPath`. On HeldByOther, `out` is filled in. */
Result acquire(const QString &lockPath, Holder *out);

/* Release if held. Idempotent; also called automatically at process exit by
 * the core's own atexit path. Must run AFTER app_free(), which clears the
 * status file and joins the connect worker. */
void release();

} // namespace RunLock

#endif
