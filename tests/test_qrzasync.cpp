/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * QRZ lookups must not hold the GUI thread, and must end.
 *
 * Two ways 0.1.13's lookup could misbehave, both reproduced here against a
 * ham-tools install made up in a temporary HOME:
 *
 *   - the cache is somebody else's SQLite file. Read on the GUI thread with
 *     QSQLITE's default 5 s busy timeout, a database ham-tools was writing
 *     froze the window — and the core, which runs on the same thread — for up
 *     to five seconds per card opened.
 *   - `qrz` ran with no time limit. One that hung on the network kept its
 *     station "in flight" for the life of the process: that card never got
 *     QRZ data, however often it was opened.
 */
#include <cstdio>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "net/qrzlookup.h"

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
    QTemporaryDir home, bin;
    qputenv("HOME", home.path().toUtf8());
    qputenv("PATH", bin.path().toUtf8() + ":/usr/bin:/bin");

    QCoreApplication app(argc, argv);

    /* A ham-tools install: configuration, cache, and a qrz that exits at once
     * having found nothing. */
    const QString dir = home.path() + QStringLiteral("/.config/ham-tools");
    QDir().mkpath(dir);
    writeFile(dir + QStringLiteral("/config.yaml"), "qrz: {}\n");
    writeFile(bin.path() + QStringLiteral("/qrz"), "#!/bin/sh\nexit 0\n", true);
    check(QrzLookup::available(), "a ham-tools install in the temporary HOME");

    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("writer"));
        db.setDatabaseName(QrzLookup::cachePath());
        db.open();
        QSqlQuery q(db);
        q.exec(QStringLiteral("CREATE TABLE qrz (call TEXT PRIMARY KEY, data TEXT, updated INTEGER)"));
        q.prepare(QStringLiteral("INSERT INTO qrz VALUES (?, ?, ?)"));
        q.addBindValue(QStringLiteral("ON6URE"));
        q.addBindValue(QStringLiteral("fname=Joeri\nname=Van Dooren\naddr2=Gent\ngrid=JO11"));
        q.addBindValue(QDateTime::currentSecsSinceEpoch());
        q.exec();
    }

    /* ---- a cache hit still works, and still arrives later ---- */
    {
        QrzLookup qrz;
        bool done = false;
        QrzLookup::Record got;
        QObject::connect(&qrz, &QrzLookup::resolved, [&](const QString &, const QrzLookup::Record &r) {
            got = r;
            done = true;
        });
        qrz.lookup(QStringLiteral("ON6URE-7"));
        check(!done, "the answer is not delivered inside lookup()");
        check(waitFor(done, 2000) && got.fullName == QLatin1String("Joeri Van Dooren"),
              "a cached record is found");
    }

    /* ---- ham-tools is writing: the database is locked ---- */
    {
        QSqlDatabase w = QSqlDatabase::database(QStringLiteral("writer"));
        QSqlQuery lock(w);
        check(lock.exec(QStringLiteral("BEGIN EXCLUSIVE")), "the cache is locked by another writer");

        QrzLookup qrz;
        bool done = false;
        QObject::connect(&qrz, &QrzLookup::resolved, [&]() { done = true; });

        QElapsedTimer t;
        t.start();
        qrz.lookup(QStringLiteral("ON6URE"));
        const qint64 took = t.elapsed();
        std::printf("      lookup() against a locked cache took %lld ms\n",
                    static_cast<long long>(took));
        check(took < 50, "lookup() does not wait for the lock");
        check(waitFor(done, 3000), "...and still answers (as a miss) soon after");

        lock.exec(QStringLiteral("ROLLBACK"));
    }

    /* ---- qrz hangs ---- */
    {
        writeFile(bin.path() + QStringLiteral("/qrz"), "#!/bin/sh\nexec sleep 60\n", true);

        QrzLookup qrz;
        qrz.setTimeoutMs(500);
        bool done = false;
        QrzLookup::Record got;
        QObject::connect(&qrz, &QrzLookup::resolved, [&](const QString &, const QrzLookup::Record &r) {
            got = r;
            done = true;
        });

        QElapsedTimer t;
        t.start();
        qrz.lookup(QStringLiteral("ON4XYZ"));
        check(waitFor(done, 3000), "a hung qrz is killed and the lookup answered");
        std::printf("      answered after %lld ms\n", static_cast<long long>(t.elapsed()));
        check(!got.isValid(), "...with nothing, as for an unknown callsign");
    }

    std::printf("\n%d/%d passed\n", g_run - g_fail, g_run);
    return g_fail ? 1 : 0;
}
