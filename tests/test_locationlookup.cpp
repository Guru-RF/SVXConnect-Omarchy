/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * Address lookup: the URL that goes out, and the JSON that comes back.
 *
 * Neither half is testable by hand. A malformed query silently returns the
 * wrong town rather than an error, and Nominatim sends its coordinates as
 * JSON *strings* — "51.05" and not 51.05 — so a parser written the obvious way
 * yields 0, which the reflector plots in the Gulf of Guinea. Both payloads
 * below are real answers, trimmed to the fields this code reads.
 */
#include <cstdio>
#include <cstdlib>

#include <QByteArray>
#include <QUrl>
#include <QUrlQuery>

#include "ui/locationdialog.h"

namespace {

int g_fail;
int g_run;

void check(bool ok, const char *what)
{
    ++g_run;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

QString item(const QUrl &url, const char *key)
{
    return QUrlQuery(url).queryItemValue(QString::fromLatin1(key), QUrl::FullyDecoded);
}

} // namespace

int main()
{
    /* ---- the outgoing query ---- */

    const QUrl url = LocationDialog::nominatimUrl(
        QStringLiteral("  Kerkstraat 12 "), QStringLiteral("9000"),
        QStringLiteral("Gent"), QStringLiteral("België"));

    check(url.host() == QStringLiteral("nominatim.openstreetmap.org"), "asks Nominatim");
    check(url.scheme() == QStringLiteral("https"), "over https");
    check(item(url, "street") == QStringLiteral("Kerkstraat 12"), "street is trimmed, not dropped");
    check(item(url, "postalcode") == QStringLiteral("9000"), "postcode is sent as postalcode");
    check(item(url, "city") == QStringLiteral("Gent"), "city is sent");
    check(item(url, "country") == QStringLiteral("België"), "a non-ASCII country survives");
    check(url.toEncoded().contains("Belgi%C3%AB"), "and is percent-encoded as UTF-8");
    check(item(url, "format") == QStringLiteral("jsonv2"), "asks for jsonv2");
    check(item(url, "addressdetails") == QStringLiteral("1"), "asks for address details");

    /* An empty field must not become an empty query item: Nominatim treats
     * street="" as a constraint and finds nothing. */
    const QUrl coarse = LocationDialog::nominatimUrl(
        QString(), QStringLiteral("9000"), QStringLiteral("Gent"), QString());
    check(!QUrlQuery(coarse).hasQueryItem(QStringLiteral("street")), "an empty street is omitted");
    check(!QUrlQuery(coarse).hasQueryItem(QStringLiteral("country")), "an empty country is omitted");
    check(QUrlQuery(coarse).hasQueryItem(QStringLiteral("city")), "the filled fields remain");

    /* ---- the answer ---- */

    const QByteArray nominatim = R"json([
      {
        "place_id": 1,
        "lat": "51.0538286",
        "lon": "3.7250121",
        "name": "12",
        "display_name": "12, Kerkstraat, Gent, Oost-Vlaanderen, 9000, België",
        "address": {
          "house_number": "12",
          "road": "Kerkstraat",
          "city": "Gent",
          "county": "Oost-Vlaanderen",
          "postcode": "9000",
          "country": "België"
        }
      },
      {
        "place_id": 2,
        "lat": "50.9376",
        "lon": "4.0350",
        "display_name": "Kerkstraat, Aalst, Oost-Vlaanderen, 9300, België",
        "address": { "town": "Aalst", "country": "België" }
      }
    ])json";

    const QList<LocationDialog::Place> places = LocationDialog::parseNominatim(nominatim);
    check(places.size() == 2, "both candidates are offered, not just the first");

    if (places.size() == 2) {
        /* The string-versus-number trap. */
        check(qAbs(places[0].latitude  - 51.0538286) < 1e-9, "latitude parses out of a JSON string");
        check(qAbs(places[0].longitude -  3.7250121) < 1e-9, "longitude parses out of a JSON string");
        check(places[0].isValid(), "and the result counts as a position");

        /* `location` is a place name. "12" — the OSM name of a house number —
         * is not one; the city is. */
        check(places[0].name == QStringLiteral("Gent"), "the city becomes the location name");
        check(places[1].name == QStringLiteral("Aalst"), "a town does just as well as a city");
        check(places[0].detail.contains(QStringLiteral("Kerkstraat")),
              "the full address is kept, to tell two streets apart");
    }

    check(LocationDialog::parseNominatim(QByteArrayLiteral("[]")).isEmpty(),
          "no matches yields no places");
    check(LocationDialog::parseNominatim(QByteArrayLiteral("<html>rate limited</html>")).isEmpty(),
          "a non-JSON body yields no places rather than a crash");

    /* A result without usable coordinates is not a position, however well
     * named, and must not reach the list. */
    check(LocationDialog::parseNominatim(
              QByteArrayLiteral(R"([{"display_name":"Nowhere","lat":"0","lon":"0"}])")).isEmpty(),
          "a result at 0,0 is dropped");

    /* ---- Omarchy's own weather location ---- */

    check(LocationDialog::omarchyLocationPath().endsWith(
              QStringLiteral("/omarchy/settings/weather.json")),
          "reads the file omarchy-weather-location writes");

    const LocationDialog::Place omarchy = LocationDialog::parseOmarchyLocation(
        QByteArrayLiteral(R"({"name":"Gent","latitude":51.05,"longitude":3.72})"));
    check(omarchy.isValid(), "Omarchy coordinates are a position");
    check(omarchy.name == QStringLiteral("Gent"), "and carry the name Omarchy shows");
    check(qAbs(omarchy.latitude - 51.05) < 1e-9, "latitude comes through as a JSON number too");

    /* omarchy-weather-location can store a name with no coordinates. A name is
     * not a position, and must fall through to the IP lookup instead of
     * quietly offering 0,0. */
    check(!LocationDialog::parseOmarchyLocation(
               QByteArrayLiteral(R"({"name":"Gent"})")).isValid(),
          "a name without coordinates is not a position");
    check(!LocationDialog::parseOmarchyLocation(QByteArray()).isValid(),
          "no weather file at all is not a position");

    /* ---- the IP fallback ---- */

    const LocationDialog::Place ip = LocationDialog::parseIpLookup(
        QByteArrayLiteral(R"({"city":"Ghent","region":"East Flanders",
                              "country_name":"Belgium","latitude":51.0536,"longitude":3.7253})"));
    check(ip.isValid(), "the IP lookup yields a position");
    check(ip.name == QStringLiteral("Ghent"), "named after the city it reports");
    check(ip.detail.contains(QStringLiteral("Belgium")), "with the country spelled out");
    check(ip.detail.contains(QStringLiteral("IP")), "and labelled as the guess it is");
    check(!LocationDialog::parseIpLookup(
               QByteArrayLiteral(R"({"error":true,"reason":"RateLimited"})")).isValid(),
          "a rate-limit reply is not a position");

    std::printf("\n%d/%d passed\n", g_run - g_fail, g_run);
    return g_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
