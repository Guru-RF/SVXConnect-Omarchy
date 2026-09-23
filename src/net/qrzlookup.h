/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * Who is behind a callsign, from QRZ, through ham-tools.
 *
 * WHY NOT TALK TO QRZ DIRECTLY
 * ----------------------------
 * QRZ's XML API needs a subscription login, and asking every application on a
 * machine to hold the operator's QRZ password is how passwords end up in
 * config files. ham-tools already solves this: the `qrz` command holds the
 * credentials, does the login dance, and caches every answer in a small SQLite
 * database. So this asks ham-tools rather than QRZ, and if ham-tools is not
 * installed the whole feature simply is not there — no prompts, no settings,
 * nothing to configure.
 *
 * HOW IT WORKS
 * ------------
 * The cache comes first: ~/.config/ham-tools/qrz.db is read read-only, which
 * costs about a millisecond and cannot disturb a `qrz` running in another
 * terminal. Only on a miss — or a record older than a month — is `qrz <call>`
 * run, which does the lookup and writes the row; then the row is read back.
 *
 * `qrz` prints for a human: ANSI colour, fields that vanish when empty, and a
 * newline that is not there when the last field of a group is missing. The
 * database holds the same data as `key=value` lines, so the database is what
 * this parses. Its stdout is only a fallback.
 *
 * CALLSIGNS ARE NOT KEYS
 * ----------------------
 * The reflector says ON3URE-7, F/ON3XYZ/P and ON0CK/ON3TTR; QRZ knows ON3URE,
 * ON3XYZ and ON3TTR. homeCall() does that reduction, and it is the one piece
 * of this file that is pure and tested — everything else is a subprocess and a
 * database, and getting the key wrong means every lookup misses.
 */
#ifndef SVXCONNECT_OMARCHY_QRZLOOKUP_H
#define SVXCONNECT_OMARCHY_QRZLOOKUP_H

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThreadPool>

#include <functional>

class QrzLookup : public QObject {
    Q_OBJECT

public:
    /* The fields worth showing on a map. QRZ returns far more; these are the
     * ones the macOS app shows, plus the grid square, which a radio amateur
     * reads faster than a pair of coordinates. */
    struct Record {
        QString callsign;       /* the home call this answers for */
        QString fullName;       /* "fname name", either part may be missing */
        QString city;           /* addr2 */
        QString country;
        QString grid;
        QString licenceClass;
        QString email;

        bool isValid() const
        {
            return !callsign.isEmpty()
                && (!fullName.isEmpty() || !city.isEmpty() || !grid.isEmpty());
        }
    };

    explicit QrzLookup(QObject *parent = nullptr);

    /* Waits for a cache read still in flight: they are bounded by a 100 ms
     * busy timeout, and each posts its answer back to this object. */
    ~QrzLookup() override;

    /* How long `qrz` may run before it is killed and the lookup answered
     * with nothing. Ten seconds; the tests shorten it. */
    void setTimeoutMs(int ms) { m_timeoutMs = ms; }

    /* True when ham-tools is installed and configured. Everything else here is
     * a no-op when this is false. */
    static bool available();

    /* Where the pieces live, so the reasons can be reported rather than just
     * "unavailable". */
    static QString binaryPath();
    static QString configPath();
    static QString cachePath();

    /* Ask about a callsign. A cached answer is emitted on the next turn of the
     * event loop; a miss runs `qrz` and emits when it lands. Repeated calls
     * for the same station while one is in flight are folded together. */
    void lookup(const QString &callsign);

    /* ---- pure, and unit-tested ---- */

    /* ON3URE-7 → ON3URE, F/ON3XYZ/P → ON3XYZ, ON0CK/ON3TTR → ON3TTR. */
    static QString homeCall(const QString &raw);

    /* The `data` column: key=value lines, with \\ and \n escaped. */
    static QHash<QString, QString> parseFields(const QString &data);

    /* Fields → the record above. */
    static Record recordFrom(const QString &homeCall, const QHash<QString, QString> &fields);

signals:
    /* `record` is invalid when QRZ has nothing — which is a normal answer for
     * a repeater callsign and must not look like an error. */
    void resolved(const QString &homeCall, const QrzLookup::Record &record);

private:
    /* ham-tools' SQLite file, read on a pool thread: it belongs to another
     * program that may be writing it, and a locked database used to cost the
     * GUI thread — and the core it runs — QSQLITE's default 5 s busy wait.
     * `then` runs on this object's thread. */
    static bool readCache(const QString &path, const QString &homeCall, Record *out);
    void readCacheAsync(const QString &homeCall, std::function<void(bool, const Record &)> then);
    void runQrz(const QString &homeCall);

    QThreadPool m_pool;
    int         m_timeoutMs = 10000;

    /* One lookup per station at a time, and a short memory for the ones QRZ
     * does not know: a map full of repeaters would otherwise spawn the same
     * fruitless process every time a card opens. */
    QSet<QString>          m_inFlight;
    QHash<QString, qint64> m_missedAt;   /* home call -> epoch seconds */
};

Q_DECLARE_METATYPE(QrzLookup::Record)

#endif
