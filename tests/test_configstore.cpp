/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * ConfigStore round-trip tests.
 *
 * The property that matters: this file is shared verbatim with the terminal
 * client and is meant to be hand-edited, so the comments in it ARE the
 * documentation. Changing one key must not disturb anything else — not the
 * other keys, not the comments, not the blank lines, not the column
 * alignment, and not the trailing comment on the very line being edited.
 *
 * Run with:  ./build/test_configstore
 */
#include "settings/configstore.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QTextStream>
#include <QStringList>

#include <cstdio>

static int g_failures = 0;

#define CHECK(cond, what)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL  %s\n", (what));                               \
            ++g_failures;                                                      \
        } else {                                                               \
            std::printf("  ok    %s\n", (what));                               \
        }                                                                      \
    } while (0)

static QByteArray readAll(const QString &path)
{
    QFile f(path);
    f.open(QIODevice::ReadOnly);
    return f.readAll();
}

static void write(const QString &path, const QByteArray &body)
{
    QFile f(path);
    f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    f.write(body);
    f.close();
}

/* A file with every hazard the writer has to survive: aligned columns, a
 * trailing comment on a key that gets edited, a ';' comment, an escaped '#'
 * inside a value, a duplicate key where the last wins, mixed capitalisation,
 * and no trailing newline. */
static const char *kSample =
    "# SVXConnect configuration\n"
    "# The comments in this file are the documentation.\n"
    "\n"
    "callsign             = ON6URE      # your callsign\n"
    "email                = a@b.example\n"
    "Reflector            = be.svx.link ; mixed-case key, ';' comment\n"
    "port                 = 5300\n"
    "\n"
    "; --- talkgroups ---\n"
    "switchable           = 8, 1745, 8000\n"
    "monitored            = 8++, 1745+, 8000\n"
    "linger_seconds       = 30\n"
    "\n"
    "location             = Gent \\# 1   # an escaped hash in the value\n"
    "tx_timeout_sec       = 120\n"
    "tx_timeout_sec       = 90          # duplicate: this one wins\n"
    "jitter_ms            = 80";

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    QTemporaryDir dir;
    if (!dir.isValid()) {
        std::printf("could not create a temporary directory\n");
        return 2;
    }
    const QString path = dir.filePath(QStringLiteral("svxconnect.conf"));

    svx_config cfg;
    config_defaults(&cfg);

    std::printf("ConfigStore\n");

    /* ---- 1. a save with nothing staged must not touch the file ---- */
    write(path, kSample);
    const QByteArray before = readAll(path);
    {
        ConfigStore cs(&cfg, path);
        QString err;
        CHECK(cs.save(&err), "empty save succeeds");
        CHECK(!cs.isDirty(), "empty save leaves the store clean");
    }
    CHECK(readAll(path) == before, "empty save leaves the file byte-identical");

    /* ---- 2. setting a key to its existing value stages nothing ---- */
    write(path, kSample);
    config_load(&cfg, qPrintable(path));
    {
        ConfigStore cs(&cfg, path);
        cs.setInt(QStringLiteral("jitter_ms"), 80);
        CHECK(!cs.isDirty(), "setting a key to its current value stages nothing");
        cs.set(QStringLiteral("callsign"), QStringLiteral("ON6URE"));
        CHECK(!cs.isDirty(), "same for a string key");
        QString err;
        cs.save(&err);
    }
    CHECK(readAll(path) == before, "a no-op edit leaves the file byte-identical");

    /* ---- 3. one changed key changes exactly one line ---- */
    write(path, kSample);
    config_load(&cfg, qPrintable(path));
    {
        ConfigStore cs(&cfg, path);
        cs.setInt(QStringLiteral("jitter_ms"), 120);
        CHECK(cs.isDirty(), "a real change is staged");
        QString err;
        CHECK(cs.save(&err), "save succeeds");
    }
    {
        const QStringList a = QString::fromUtf8(before).split(QLatin1Char('\n'));
        const QStringList b = QString::fromUtf8(readAll(path)).split(QLatin1Char('\n'));
        CHECK(a.size() == b.size(), "line count unchanged");
        int differing = 0;
        for (int i = 0; i < qMin(a.size(), b.size()); ++i)
            if (a[i] != b[i]) ++differing;
        CHECK(differing == 1, "exactly one line differs");
        CHECK(b.last().startsWith(QLatin1String("jitter_ms")) &&
              b.last().contains(QLatin1String("120")), "the edited line holds the new value");
    }

    /* ---- 4. comments, alignment and escapes survive ---- */
    write(path, kSample);
    config_load(&cfg, qPrintable(path));
    {
        ConfigStore cs(&cfg, path);
        cs.set(QStringLiteral("callsign"), QStringLiteral("ON4TEST"));
        QString err;
        cs.save(&err);
    }
    {
        const QString out = QString::fromUtf8(readAll(path));
        CHECK(out.contains(QLatin1String("# your callsign")),
              "the trailing comment on the edited line survives");
        CHECK(out.contains(QLatin1String("callsign             = ON4TEST")),
              "column alignment is preserved");
        CHECK(out.contains(QLatin1String("# The comments in this file are the documentation.")),
              "header comments survive");
        CHECK(out.contains(QLatin1String("; --- talkgroups ---")),
              "';' section comments survive");
        CHECK(out.contains(QLatin1String("Gent \\# 1")),
              "an escaped '#' inside a value is not treated as a comment");
    }

    /* ---- 5. a duplicated key: EVERY occurrence is rewritten, because the
     *         last one is the one config_load() ends up honouring ---- */
    write(path, kSample);
    config_load(&cfg, qPrintable(path));
    {
        ConfigStore cs(&cfg, path);
        cs.setInt(QStringLiteral("tx_timeout_sec"), 45);
        QString err;
        cs.save(&err);
    }
    {
        const QString out = QString::fromUtf8(readAll(path));
        CHECK(out.count(QLatin1String("tx_timeout_sec")) == 2,
              "both duplicate lines are still present");
        CHECK(!out.contains(QLatin1String("= 120")) && !out.contains(QLatin1String("= 90 ")),
              "neither stale value survives");
        CHECK(out.contains(QLatin1String("# duplicate: this one wins")),
              "the duplicate's comment survives");

        /* The real proof: the core must read back what we intended. */
        svx_config check;
        config_defaults(&check);
        config_load(&check, qPrintable(path));
        CHECK(check.tx_timeout_sec == 45, "the core reads back the new value");
    }

    /* ---- 6. a key the file never mentioned is appended ---- */
    write(path, kSample);
    config_load(&cfg, qPrintable(path));
    {
        ConfigStore cs(&cfg, path);
        cs.setInt(QStringLiteral("mic_gain"), 6);
        cs.setBool(QStringLiteral("mic_agc"), true);
        QString err;
        cs.save(&err);
    }
    {
        svx_config check;
        config_defaults(&check);
        config_load(&check, qPrintable(path));
        CHECK(check.mic_gain == 6,  "an appended int key reads back");
        CHECK(check.mic_agc == 1,   "an appended bool key reads back");
        CHECK(QString::fromUtf8(readAll(path))
                  .contains(QLatin1String("# --- added by SVXConnect ---")),
              "appended keys are in a marked block");
    }

    /* ---- 7. mixed-case key in the file is matched case-insensitively ---- */
    write(path, kSample);
    config_load(&cfg, qPrintable(path));
    {
        ConfigStore cs(&cfg, path);
        cs.set(QStringLiteral("reflector"), QStringLiteral("nl.svx.link"));
        QString err;
        cs.save(&err);
    }
    {
        const QString out = QString::fromUtf8(readAll(path));
        CHECK(out.contains(QLatin1String("Reflector            = nl.svx.link")),
              "the file's own capitalisation of the key is kept");
        CHECK(!out.contains(QLatin1String("# --- added by SVXConnect ---")),
              "a mixed-case key is not duplicated into the appended block");
        CHECK(out.contains(QLatin1String("; mixed-case key")),
              "its trailing ';' comment survives");
    }

    /* ---- 8. a value containing a '#' gets quoted so it round-trips ---- */
    write(path, kSample);
    config_load(&cfg, qPrintable(path));
    {
        ConfigStore cs(&cfg, path);
        cs.set(QStringLiteral("location"), QStringLiteral("Gent #2"));
        QString err;
        cs.save(&err);

        svx_config check;
        config_defaults(&check);
        config_load(&check, qPrintable(path));
        CHECK(QString::fromUtf8(check.location) == QLatin1String("Gent #2"),
              "a value containing '#' survives the round trip");
    }

    std::printf("\n%s (%d failure%s)\n",
                g_failures ? "FAILED" : "all checks passed",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
