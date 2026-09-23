/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * See logbridge.h for why this exists and what the sink contract really is.
 *
 * EVERYTHING IN THIS FILE RUNS ON TWO THREADS: the GUI thread, and the
 * reflector client's transient connect worker. Therefore nothing here touches
 * a QObject, emits a signal, or reads a QWidget. The only communication with
 * the GUI is a monotonically increasing serial that the model tick polls.
 */
#include "core/logbridge.h"
#include "core/svxcore.h"

#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QByteArray>
#include <QAtomicInteger>

#include <atomic>
#include <cstdio>
#include <ctime>

namespace {

constexpr int kRingCap = 2000;

QMutex                 g_mx;
QVector<LogLine>       g_ring;
QAtomicInteger<quint64> g_serial{0};
std::FILE             *g_file = nullptr;   /* guarded by g_mx — see below */
/* Atomic, not under g_mx: the sink reads it on every line — including from
 * the connect worker thread — before it takes the lock. A plain bool written
 * under the lock and read outside it is a data race. */
std::atomic<bool>      g_redact{true};
bool                   g_installed = false;

/* Redaction.
 *
 * Deliberately cheap: both patterns are anchored on a character that is rare
 * in log lines, so the common case is a failed first-character test rather
 * than a regex engine run. This is on the connect worker's path during a
 * handshake, so it must not be expensive.
 *
 * Coordinates are rounded rather than removed: a bug report about "my node is
 * in the wrong place on the map" needs the grid square, and 4 decimals is
 * ~11 m — the same precision maidenhead() reduces to anyway. */
QString redactLine(QString s)
{
    if (s.contains(QLatin1Char('@'))) {
        static const QRegularExpression re(
            QStringLiteral(R"(\b([A-Za-z0-9._%+-])[A-Za-z0-9._%+-]*(@[A-Za-z0-9.-]+\.[A-Za-z]{2,}))"));
        s.replace(re, QStringLiteral("\\1***\\2"));
    }

    if (s.contains(QLatin1Char('.'))) {
        static const QRegularExpression re(
            QStringLiteral(R"((-?\d{1,3}\.\d{4})\d{2,})"));
        s.replace(re, QStringLiteral("\\1"));
    }

    return s;
}

/* The sink. Called from the GUI thread AND from the connect worker. */
void svxcLogSink(int level, const char *body, void *)
{
    /* Reproduce emit()'s format exactly, so a GUI-written log file is
     * byte-comparable with a CLI-written one. localtime_r is thread-safe
     * (it is not async-signal-safe, which does not apply here). */
    char ts[32];
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    std::strftime(ts, sizeof ts, "%H:%M:%S", &tm);

    QString text = QString::fromUtf8(body);
    if (g_redact.load(std::memory_order_relaxed))
        text = redactLine(std::move(text));

    const QString line = QLatin1String(ts) + QLatin1String(" [")
                       + QLatin1String(log_level_name(level)) + QLatin1String("] ")
                       + text;

    {
        QMutexLocker lk(&g_mx);

        g_ring.append(LogLine{level, line});
        if (g_ring.size() > kRingCap)
            g_ring.remove(0, g_ring.size() - kRingCap);

        /* g_file is read under the same lock that setFile() writes it under.
         * fputs() itself is thread-safe on glibc, but the POINTER load is not
         * ordered against a concurrent reopen without this. */
        if (g_file) {
            const QByteArray utf8 = line.toUtf8();
            std::fputs(utf8.constData(), g_file);
            std::fputc('\n', g_file);
            std::fflush(g_file);
        }
    }

    g_serial.fetchAndAddOrdered(1);

    /* Keep journald and `svxconnect-omarchy 2>&1 | less` useful. The core's default
     * path would have done this; we took it over, so we owe it. */
    std::fputs(qPrintable(line), stderr);
    std::fputc('\n', stderr);
}

} // namespace

namespace LogBridge {

void install()
{
    QMutexLocker lk(&g_mx);
    if (g_installed)
        return;
    g_installed = true;
    lk.unlock();

    log_set_sink(svxcLogSink, nullptr);
}

void setFile(const QString &path)
{
    std::FILE *f = nullptr;
    if (!path.isEmpty()) {
        /* Append, never truncate: the CLI may have written to this file five
         * minutes ago and the user may be reading it right now. */
        f = std::fopen(qPrintable(path), "ae");
        if (!f) {
            log_warn("log file %s could not be opened, logging to stderr only",
                     qPrintable(path));
            return;
        }
    }

    QMutexLocker lk(&g_mx);
    if (g_file)
        std::fclose(g_file);
    g_file = f;
}

void setRedaction(bool on)
{
    g_redact.store(on, std::memory_order_relaxed);
}

bool redaction()
{
    return g_redact.load(std::memory_order_relaxed);
}

quint64 serial()
{
    return g_serial.loadAcquire();
}

QVector<LogLine> snapshot(int max)
{
    QMutexLocker lk(&g_mx);
    if (max >= g_ring.size())
        return g_ring;
    return g_ring.mid(g_ring.size() - max);
}

void clear()
{
    QMutexLocker lk(&g_mx);
    g_ring.clear();
}

void shutdown()
{
    /* Restore the default sink FIRST, so anything the core logs between here
     * and process exit still lands on stderr rather than into a closed FILE*. */
    log_set_sink(nullptr, nullptr);

    QMutexLocker lk(&g_mx);
    if (g_file) {
        std::fclose(g_file);
        g_file = nullptr;
    }
    g_installed = false;
}

} // namespace LogBridge
