/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * Thread-safe logging for the Qt front end.
 *
 * WHY NOT app_capture_log()
 * -------------------------
 * The core offers app_capture_log() plus app_log_snapshot(), which is what the
 * ncurses UI uses. We do not use it, for two independent reasons.
 *
 * 1. THREADING. common/log.c has no locking of any kind — g_level, g_file,
 *    g_sink and g_sink_user are bare file statics. And logging is genuinely
 *    concurrent: handshake_run() executes on the connect worker thread
 *    (client.c:142, and handshake.h says so in as many words) and calls
 *    log_info()/log_dbg() from at least eight places during every connect.
 *    The core's own ring has the same problem. Ours is mutex-guarded.
 *
 * 2. THE FILE. common/log.c's emit() calls the sink and RETURNS:
 *
 *        if (g_sink) { g_sink(level, body, g_sink_user); return; }
 *
 *    so once any sink is installed, cfg.log_file receives nothing at all. The
 *    core writes the file only on the default path. Owning the sink means we
 *    own the file too, which is the only way to get both a log pane and a log
 *    file at the same time.
 *
 * THE SINK CONTRACT, WHICH log.h DESCRIBES INCORRECTLY
 * ----------------------------------------------------
 * log.h:36 says the sink receives "a complete, already-formatted line without
 * a trailing newline". It does not. Verified in log.c:75-95: `body` is the
 * bare vsnprintf() of the caller's format string — no timestamp, no level tag,
 * no newline. The timestamp and "[info]" prefix are applied only in the branch
 * that runs when NO sink is installed.
 *
 * So this bridge synthesises the prefix itself, reproducing emit()'s exact
 * format ("%H:%M:%S [%s] %s\n"), so that a log file written by the GUI is
 * byte-comparable with one written by the CLI. Forget this and you get one
 * unbroken multi-megabyte line in both the file and the journal.
 */
#ifndef SVXCONNECT_OMARCHY_LOGBRIDGE_H
#define SVXCONNECT_OMARCHY_LOGBRIDGE_H

#include <QString>
#include <QVector>

struct LogLine {
    int     level;      /* LOG_ERR / LOG_WARN / LOG_INFO / LOG_DBG */
    QString text;       /* the formatted line, without the trailing newline */
};

namespace LogBridge {

/* Install the sink. MUST be called before app_new(), so that the connect
 * worker never races an install. Idempotent. */
void install();

/* Route lines to `path` as well, in addition to the ring and stderr. Pass an
 * empty string to stop. Safe to call at any time; takes the same lock the sink
 * does. This is cfg.log_file — nothing else in the process ever opens it,
 * because the core cannot (see the header comment). */
void setFile(const QString &path);

/* Redaction. On by default: email local-parts are masked and coordinates are
 * rounded to 4 decimals (grid-square precision), because the Preferences pane
 * invites users to attach this file to a bug report. A "verbose diagnostic
 * log" checkbox turns it off for one session. Key material is never logged by
 * the core in the first place. */
void setRedaction(bool on);
bool redaction();

/* Bumps on every accepted line. The log pane polls this on the 100 ms model
 * tick and only calls snapshot() when it has changed — no queued signals and
 * no QObject touched from the connect worker. */
quint64 serial();

/* Newest last, at most `max` lines. */
QVector<LogLine> snapshot(int max = 2000);

/* Drop everything in the ring (the log pane's Clear action). */
void clear();

/* Uninstall the sink and close the file. Call after app_free(), never before:
 * the core logs during teardown. */
void shutdown();

} // namespace LogBridge

#endif
