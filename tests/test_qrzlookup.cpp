/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * Turning a callsign heard on a reflector into one QRZ has heard of.
 *
 * This is the whole reason the lookup is testable at all: everything else in
 * qrzlookup.cpp is a subprocess and somebody else's SQLite file, but the key
 * it looks things up by is pure text manipulation — and getting it wrong does
 * not fail loudly, it just means every lookup quietly misses. A reflector says
 * ON3URE-7, F/ON3XYZ/P and ON0CK/ON3TTR; QRZ has ON3URE, ON3XYZ and ON3TTR.
 */
#include <cstdio>
#include <cstdlib>

#include <QHash>
#include <QString>

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

void sameCall(const char *in, const char *want)
{
    const QString got = QrzLookup::homeCall(QString::fromLatin1(in));
    const bool ok = got == QLatin1String(want);
    ++g_run;
    std::printf("%s  %s -> %s%s\n", ok ? "ok  " : "FAIL", in, qPrintable(got),
                ok ? "" : QStringLiteral(" (wanted %1)").arg(QLatin1String(want)).toUtf8().constData());
    if (!ok) ++g_fail;
}

} // namespace

int main()
{
    /* ---- the home call ---- */

    sameCall("ON6URE",      "ON6URE");
    sameCall("  on6ure  ",  "ON6URE");        /* trimmed and upper-cased */
    sameCall("ON3URE-7",    "ON3URE");        /* an SSID is this network's idea */
    sameCall("ON6URE-TPAD", "ON6URE");
    sameCall("ON3URE/P",    "ON3URE");        /* portable */
    sameCall("ON3URE/M",    "ON3URE");        /* mobile */
    sameCall("ON3URE/MM",   "ON3URE");        /* maritime mobile, not /M + M */
    sameCall("ON3URE/QRP",  "ON3URE");
    sameCall("F/ON3XYZ",    "ON3XYZ");        /* operating in France */
    sameCall("F/ON3XYZ/P",  "ON3XYZ");        /* ...and portable while doing it */
    sameCall("ON0CK/ON3TTR","ON3TTR");        /* heard through a node: the operator wins */
    sameCall("PA/ON6URE-9", "ON6URE");        /* every rule at once */
    sameCall("",            "");

    /* A callsign with no digit anywhere is not a callsign, but it must still
     * come back as something rather than empty. */
    check(!QrzLookup::homeCall(QStringLiteral("BROADCAST")).isEmpty(),
          "a pseudo-station still yields a key");

    /* ---- the cache's own format ---- */

    /* ham-tools stores QRZ's fields as key=value lines, escaping backslashes
     * and newlines. This is one real record, shortened. */
    const QHash<QString, QString> f = QrzLookup::parseFields(QStringLiteral(
        "call=ON6URE\n"
        "fname=Joeri\n"
        "name=Van Dooren\n"
        "addr1=Some street 1\n"
        "addr2=Merchtem\n"
        "country=Belgium\n"
        "grid=JO20CW\n"
        "class=HAREC\n"
        "email=ure@example.be\n"
        "bio=line one\\nline two\n"
        "moddate=2025-01-02 03:04:05\n"));

    check(f.value(QStringLiteral("call")) == QStringLiteral("ON6URE"), "fields parse by key");
    check(f.value(QStringLiteral("name")) == QStringLiteral("Van Dooren"),
          "a value may contain spaces");
    check(f.value(QStringLiteral("bio")) == QStringLiteral("line one\nline two"),
          "an escaped newline becomes a newline");
    check(f.value(QStringLiteral("moddate")).contains(QStringLiteral("03:04:05")),
          "only the FIRST equals sign splits the line");

    check(QrzLookup::parseFields(QString()).isEmpty(), "an empty record parses to nothing");
    check(QrzLookup::parseFields(QStringLiteral("rubbish\n=nokey\n")).isEmpty(),
          "lines without a key are dropped, not guessed at");

    /* ---- what the card shows ---- */

    const QrzLookup::Record r = QrzLookup::recordFrom(QStringLiteral("ON6URE"), f);
    check(r.isValid(), "a record with a name is worth showing");
    check(r.fullName == QStringLiteral("Joeri Van Dooren"), "the name is first and last together");
    check(r.city == QStringLiteral("Merchtem"), "the city is addr2, not addr1");
    check(r.grid == QStringLiteral("JO20CW"), "the grid square comes through");
    check(r.licenceClass == QStringLiteral("HAREC"), "so does the licence class");
    check(r.email == QStringLiteral("ure@example.be"), "and the address to write to");

    QHash<QString, QString> half;
    half.insert(QStringLiteral("name"), QStringLiteral("Van Dooren"));
    check(QrzLookup::recordFrom(QStringLiteral("ON6URE"), half).fullName
              == QStringLiteral("Van Dooren"),
          "a missing first name leaves no leading space");

    check(!QrzLookup::recordFrom(QStringLiteral("ON0TEST"), {}).isValid(),
          "no fields is not a record");

    QHash<QString, QString> anonymous;
    anonymous.insert(QStringLiteral("call"), QStringLiteral("ON0ABC"));
    check(!QrzLookup::recordFrom(QStringLiteral("ON0ABC"), anonymous).isValid(),
          "a record with nothing but the callsign says nothing, so it is not shown");

    std::printf("\n%d/%d passed\n", g_run - g_fail, g_run);
    return g_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
