/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * The reflector portal's two metadata files.
 *
 * The URL is derived, not configured, so a mistake there is indistinguishable
 * from "this reflector has no portal" — worth pinning down. And both files are
 * somebody else's JSON: the parsers have to skip what they do not recognise
 * rather than throwing the whole file away, because a portal that adds a field
 * must not blank the talkgroup names that were working yesterday.
 *
 * The payloads below are the shapes portal.be.svx.link actually serves.
 */
#include <cstdio>
#include <cstdlib>

#include <QByteArray>
#include <QUrl>

#include "net/portalinfo.h"

namespace {

int g_fail;
int g_run;

void check(bool ok, const char *what)
{
    ++g_run;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

} // namespace

int main()
{
    /* ---- the URL ---- */

    check(PortalInfo::portalBaseUrl(QStringLiteral("be.svx.link")).toString()
              == QStringLiteral("https://portal.be.svx.link/"),
          "the portal is https://portal.<host>/");
    check(PortalInfo::portalBaseUrl(QStringLiteral("reflector.be.svx.link")).toString()
              == QStringLiteral("https://portal.be.svx.link/"),
          "the portal is a sibling of the reflector, not a child of it");
    check(PortalInfo::portalBaseUrl(QStringLiteral("be.svx.link:5300")).toString()
              == QStringLiteral("https://portal.be.svx.link/"),
          "the reflector's TCP port has nothing to do with it");
    check(PortalInfo::portalBaseUrl(QStringLiteral("be.svx.link,backup.svx.link")).toString()
              == QStringLiteral("https://portal.be.svx.link/"),
          "a fallback list asks the first host");
    check(!PortalInfo::portalBaseUrl(QStringLiteral("  ")).isValid(), "no host, no portal");
    check(!PortalInfo::portalBaseUrl(QStringLiteral("reflector.")).isValid(),
          "a host that is nothing but the prefix is not a host");

    /* ---- talkgroups.json ---- */

    const QHash<quint32, QString> tgs = PortalInfo::parseTalkgroups(R"json({
      "4": "Chat 1",
      "8": "70cm Repeaters",
      "1745": "145.450 ON0ORA-S",
      "9990": "Parrot",
      "notanumber": "ignored",
      "12": 42
    })json");

    check(tgs.size() == 4, "every numeric key with a text name is taken, and only those");
    check(tgs.value(8) == QStringLiteral("70cm Repeaters"), "a talkgroup has a name");
    check(tgs.value(1745) == QStringLiteral("145.450 ON0ORA-S"),
          "four-digit talkgroups are not truncated");
    check(!tgs.contains(12), "a name that is not text is skipped, not coerced");
    check(PortalInfo::parseTalkgroups(QByteArrayLiteral("<html>404</html>")).isEmpty(),
          "an error page yields no names rather than a crash");
    check(PortalInfo::parseTalkgroups(QByteArrayLiteral("{}")).isEmpty(),
          "an empty portal is simply empty");

    /* ---- callsigns.json ---- */

    const QHash<QString, QString> calls = PortalInfo::parseCallsigns(R"json({
      "on0ora": "TX:438.8000 RX:431.2000\nCTCSS-OUT: 131.8\nSysop: ON4XYZ",
      "ON0BRK": "TX/RX:145.4000\nSimplex Club Frequency"
    })json");

    check(calls.size() == 2, "both stations are listed");
    check(calls.contains(QStringLiteral("ON0ORA")),
          "keys are upper-cased, because the feed's callsigns are");
    check(calls.value(QStringLiteral("ON0ORA")).contains(QLatin1Char('\n')),
          "the description keeps its line breaks");
    check(calls.value(QStringLiteral("ON0ORA")).startsWith(QStringLiteral("TX:438.8000")),
          "and its contents");

    std::printf("\n%d/%d passed\n", g_run - g_fail, g_run);
    return g_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
