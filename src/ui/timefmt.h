/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * The three time labels the interface needs.
 *
 * The core has fmt_age() and fmt_duration() in common/util.c, and they are
 * good, but they do not cover what the display needs:
 *
 *   fmt_age()       "12s" / "4m" / "2h" / "3d", and "--" for zero. No " ago",
 *                   and no way to translate it — it is snprintf into a C
 *                   buffer.
 *   fmt_duration()  "m:ss" / "h:mm:ss". Correct, but again untranslatable.
 *
 * The macOS app shows three distinct things and they must stay distinct,
 * because a row showing "12s" means something different in each:
 *
 *   elapsed(start)  a talker who is STILL speaking — how long they have been
 *                   going. Counts up.
 *   duration(secs)  a finished over — how long it lasted.
 *   ago(stop)       how long since it ended.
 *
 * So these are reimplemented with tr(), which also gets plurals right in
 * languages that need them.
 */
#ifndef SVXCONNECT_OMARCHY_TIMEFMT_H
#define SVXCONNECT_OMARCHY_TIMEFMT_H

#include <QString>
#include <QCoreApplication>

namespace TimeFmt {

/* "12s", "1:04", "1:02:03" — a live or finished span. */
QString duration(quint64 seconds);

/* Live talker: seconds since start_ms, formatted as duration(). */
QString elapsed(quint64 startMs, quint64 nowMs);

/* "just now", "12s ago", "4m ago", "2h ago", "3d ago". */
QString ago(quint64 stampMs, quint64 nowMs);

/* Compact form for the talkgroup button's sub-line, where width is tight:
 * "12s", "4m", "2h", "3d", or an em dash when never heard. */
QString compactAge(quint64 stampMs, quint64 nowMs);

} // namespace TimeFmt

#endif
