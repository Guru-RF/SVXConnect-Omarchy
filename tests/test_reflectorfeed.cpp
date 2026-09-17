/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * The enhanced reflector's feed, without a reflector.
 *
 * Two things here are worth a test and the rest follows from them. The URL is
 * derived from a host that may carry a port and a fallback list, and getting
 * that wrong means probing a name that does not exist — indistinguishable,
 * from the outside, from a plain reflector. And the message schema is loose in
 * ways that are invisible until a real portal sends the awkward variant: a
 * talkgroup as a string, a partial update that would blank a marker, a
 * portable station whose coordinates are under a different key with a
 * different spelling.
 *
 * The payloads below are the shapes the other SVXConnect clients parse.
 */
#include <cstdio>
#include <cstdlib>

#include <QByteArray>
#include <QUrl>

#include "net/reflectorfeed.h"

namespace {

int g_fail;
int g_run;

void check(bool ok, const char *what)
{
    ++g_run;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

const ReflectorFeed::Session *sessionOf(const ReflectorFeed &feed, const char *call)
{
    for (const ReflectorFeed::Session &s : feed.sessions())
        if (s.callsign == QLatin1String(call))
            return &s;
    return nullptr;
}

} // namespace

int main()
{
    /* ---- the URL, which is the whole of discovery ---- */

    check(ReflectorFeed::feedUrl(QStringLiteral("be.svx.link")).toString()
              == QStringLiteral("wss://reflector.be.svx.link/"),
          "a bare host becomes wss://reflector.<host>/");
    check(ReflectorFeed::feedUrl(QStringLiteral("reflector.be.svx.link")).toString()
              == QStringLiteral("wss://reflector.be.svx.link/"),
          "a host that already says reflector is not doubled");
    check(ReflectorFeed::feedUrl(QStringLiteral("be.svx.link:5300")).toString()
              == QStringLiteral("wss://reflector.be.svx.link/"),
          "the reflector's TCP port is not the portal's port");
    check(ReflectorFeed::feedUrl(QStringLiteral("be.svx.link:5300,backup.svx.link:5301")).toString()
              == QStringLiteral("wss://reflector.be.svx.link/"),
          "a fallback list probes the first host only");
    check(!ReflectorFeed::feedUrl(QStringLiteral("   ")).isValid(),
          "no host, no feed");

    /* ---- the snapshot ---- */

    ReflectorFeed feed;
    check(!feed.isAvailable(), "a feed with no snapshot is not available");

    const bool wasSnapshot = feed.handleJson(R"json({
      "type": "snapshot",
      "nodes": [
        {"callsign":"on0abc","online":true,"isTalker":false,"tg":2061,
         "monitoredTGs":[2061,"9990"],"location":"Brussels, Belgium","lat":50.85,"lon":4.35}
      ],
      "sessions": [
        {"callsign":"ON3URE","start_ms":1700000000000,"end_ms":1700000005000,
         "node":{"nodeLocation":"Asse","tg":2061}}
      ],
      "active": [
        {"callsign":"F/ON3XYZ/P","start_ms":1700000010000,"active":true,
         "node":{"nodeLocation":"Mobile","tg":2062,"qth":{"lat":"50.9","long":"4.2"}}}
      ]
    })json");

    check(wasSnapshot, "a snapshot reports itself as one");
    check(feed.isAvailable(), "and makes the feed available");

    check(feed.nodes().contains(QStringLiteral("ON0ABC")), "callsigns are upper-cased on the way in");
    check(feed.nodes().value(QStringLiteral("ON0ABC")).hasPos, "a listed node carries a position");
    check(qAbs(feed.nodes().value(QStringLiteral("ON0ABC")).latitude - 50.85) < 1e-9,
          "with the latitude it was sent");
    check(feed.nodes().value(QStringLiteral("ON0ABC")).monitoredTgs == QVector<int>({2061, 9990}),
          "a monitored talkgroup counts whether it is a number or a string");

    /* The one the nodes list never mentions. */
    check(feed.nodes().contains(QStringLiteral("F/ON3XYZ/P")),
          "a portable station is synthesised from its session");
    check(qAbs(feed.nodes().value(QStringLiteral("F/ON3XYZ/P")).longitude - 4.2) < 1e-9,
          "reading qth.long — a different key, and a string");

    check(feed.sessions().size() == 2, "both lists contribute, deduplicated by callsign");
    check(feed.sessions().first().callsign == QStringLiteral("F/ON3XYZ/P"),
          "whoever is talking now sorts first");
    check(feed.sessions().first().active, "and is marked active");
    check(sessionOf(feed, "ON3URE") && !sessionOf(feed, "ON3URE")->active,
          "a session with an end time is history");
    check(sessionOf(feed, "ON3URE")->lastActivityMs() == 1700000005000LL,
          "whose last activity is when it ended, in epoch milliseconds");
    check(sessionOf(feed, "ON3URE")->tg == 2061, "the talkgroup comes from the nested node");

    /* ---- partial updates ---- */

    feed.handleJson(R"json({"type":"node_upsert","node":{"callsign":"ON0ABC","isTalker":true}})json");
    check(feed.nodes().value(QStringLiteral("ON0ABC")).isTalker, "an upsert applies what it carries");
    check(qAbs(feed.nodes().value(QStringLiteral("ON0ABC")).latitude - 50.85) < 1e-9,
          "and leaves the coordinates it does not mention alone");
    check(feed.nodes().value(QStringLiteral("ON0ABC")).tg == 2061,
          "the talkgroup survives an upsert that omits it");

    feed.handleJson(R"json({"type":"node_upsert","node":{"callsign":"ON0ABC","lat":null,"tg":"7"}})json");
    check(qAbs(feed.nodes().value(QStringLiteral("ON0ABC")).latitude - 50.85) < 1e-9,
          "an explicit null is absent, not a position of zero");
    check(feed.nodes().value(QStringLiteral("ON0ABC")).tg == 7,
          "a talkgroup sent as a string still counts");

    /* ---- one row per station ---- */

    feed.handleJson(R"json({"type":"talk_start","session":
        {"callsign":"ON3URE","start_ms":1700000100000,"active":true,"node":{"tg":8}}})json");
    check(sessionOf(feed, "ON3URE") && sessionOf(feed, "ON3URE")->active,
          "a new over replaces that station's old row");
    check(feed.sessions().size() == 2, "rather than adding a second one");

    feed.handleJson(R"json({"type":"talk_stop","session":
        {"callsign":"ON3URE","start_ms":1700000100000,"end_ms":1700000120000,"node":{"tg":8}}})json");
    check(sessionOf(feed, "ON3URE") && !sessionOf(feed, "ON3URE")->active,
          "and the stop closes it");
    check(sessionOf(feed, "ON3URE")->lastActivityMs() == 1700000120000LL,
          "with the end time as its last activity");

    /* A talker that only ever appears in a session still gets a marker. */
    feed.handleJson(R"json({"type":"talk_start","session":
        {"callsign":"F/ON9MOB/M","start_ms":1700000200000,"active":true,
         "node":{"qth":{"lat":51.0,"long":4.5},"tg":2063}}})json");
    check(feed.nodes().contains(QStringLiteral("F/ON9MOB/M")),
          "a mobile appearing mid-session lands on the map");
    check(feed.nodes().value(QStringLiteral("F/ON9MOB/M")).isTalker,
          "and is drawn as the talker it is");

    /* Null Island: the portal lists unconfigured nodes at exactly 0,0, and a
     * marker there drags the map's automatic zoom out across an ocean. */
    feed.handleJson(R"json({"type":"node_upsert","node":
        {"callsign":"ON0XXX-1745","online":true,"tg":1745,"lat":0.0,"lon":0.0}})json");
    check(feed.nodes().contains(QStringLiteral("ON0XXX-1745")),
          "a node with no usable position is still a node");
    check(!feed.nodes().value(QStringLiteral("ON0XXX-1745")).hasPos,
          "but 0,0 does not count as one");

    /* ---- removal ---- */

    feed.handleJson(R"json({"type":"node_remove","callsign":"ON0ABC"})json");
    check(!feed.nodes().contains(QStringLiteral("ON0ABC")), "a removed node leaves no stale marker");
    check(sessionOf(feed, "ON3URE") != nullptr, "but its history is still history");
    feed.handleJson(R"json({"type":"node_remove","callsign":"ON0NOPE"})json");
    check(true, "removing a station that was never there is harmless");

    /* ---- rubbish ---- */

    check(!feed.handleJson(QByteArrayLiteral("{\"type\":\"keepalive\"}")),
          "an unknown type is not a snapshot");
    check(!feed.handleJson(QByteArrayLiteral("<html>502 Bad Gateway</html>")),
          "nor is a proxy error page");
    check(feed.isAvailable(), "and neither takes the feed down");

    std::printf("\n%d/%d passed\n", g_run - g_fail, g_run);
    return g_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
