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
#include <QCoreApplication>
#include <QTemporaryDir>
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

int main(int argc, char **argv)
{
    /* The operator's own documents are stored in QSettings, so the settings
     * are pointed at a directory that is thrown away — a test must never be
     * able to overwrite somebody's real talkgroup names. */
    QTemporaryDir sandbox;
    qputenv("XDG_CONFIG_HOME", sandbox.path().toUtf8());
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SVXConnect-test"));
    QCoreApplication::setApplicationName(QStringLiteral("portalinfo"));

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

    /* ---- the operator's own documents ---- */

    PortalInfo info;
    info.setNetworkEnabled(false);              /* nothing here may touch a network */
    info.setReflector(QStringLiteral("plain.example"));
    check(!info.hasTalkgroups(), "a plain reflector starts with no names at all");

    QString error;
    check(info.setManualJson(R"({"8": "70cm Repeaters", "9990": "Parrot"})", QByteArray(), &error),
          "a hand-written talkgroup document is accepted");
    check(info.talkgroupName(8) == QStringLiteral("70cm Repeaters"),
          "and names a talkgroup on a reflector that publishes nothing");
    check(info.rawTalkgroups().contains("Parrot"), "the text is kept as written, for the editor");

    /* The trap this exists to avoid: a typo must not blank what was working. */
    check(!info.setManualJson(R"({"8": "70cm Repeaters",})", QByteArray(), &error),
          "a document that does not parse is refused");
    check(!error.isEmpty(), "with a reason the dialog can show");
    check(info.talkgroupName(8) == QStringLiteral("70cm Repeaters"),
          "and the names that were working are still there");

    check(!info.setManualJson(R"(["8", "9990"])", QByteArray(), &error),
          "a JSON array is refused: it has to be an object");
    check(!info.setManualJson(R"({"8": "ok"})", R"(not json)", &error),
          "a bad callsign document refuses the pair");
    check(info.talkgroupName(9990) == QStringLiteral("Parrot"),
          "so the talkgroups were not half-saved next to it");

    check(info.setManualJson(QByteArray(), R"({"on0ora": "TX:438.8000\nCTCSS: 131.8"})", &error),
          "clearing one document and writing the other is fine");
    check(!info.hasTalkgroups(), "empty text clears the talkgroup names");
    check(info.callsignInfo(QStringLiteral("ON0ORA")).contains(QLatin1Char('\n')),
          "and the station description keeps its line break");

    /* It belongs to one reflector: a list of Belgian names is wrong, not stale,
     * on another network. */
    PortalInfo other;
    other.setNetworkEnabled(false);
    other.setReflector(QStringLiteral("elsewhere.example"));
    check(other.callsignInfo(QStringLiteral("ON0ORA")).isEmpty(),
          "another reflector does not inherit this one's documents");

    PortalInfo again;
    again.setNetworkEnabled(false);
    again.setReflector(QStringLiteral("plain.example"));
    check(!again.callsignInfo(QStringLiteral("ON0ORA")).isEmpty(),
          "but the same reflector gets them back after a restart");

    std::printf("\n%d/%d passed\n", g_run - g_fail, g_run);
    return g_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
