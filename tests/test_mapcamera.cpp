/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * Does the map move when it should, and stay put when it should not?
 *
 * The camera is all timing and state, which is why two earlier versions of it
 * were wrong in ways no screenshot could show: a rendered map says nothing
 * about whether it WOULD have moved. The first version only followed a talker
 * that was off screen — and on a 100 km home view of Belgium every repeater is
 * on screen, so in practice it never moved at all. That exact case is the
 * first thing checked here.
 *
 * A QApplication on the offscreen platform, because MapView is a widget; the
 * widget is never shown, so no tile is ever requested and no network is
 * touched. The timings are shortened through setCameraTiming() so the hold
 * does not take half a minute.
 */
#include <cstdio>
#include <cstdlib>

#include <QApplication>
#include <QEventLoop>
#include <QTimer>

#include "ui/mapview.h"

namespace {

int g_fail;
int g_run;

void check(bool ok, const char *what)
{
    ++g_run;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

void wait(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

MapView::Marker station(const char *call, double lat, double lon,
                        bool talking = false, bool self = false)
{
    MapView::Marker m;
    m.callsign  = QString::fromLatin1(call);
    m.latitude  = lat;
    m.longitude = lon;
    m.online    = true;
    m.talking   = talking;
    m.self      = self;
    return m;
}

bool near(double a, double b, double tolerance = 0.02)
{
    return qAbs(a - b) <= tolerance;
}

/* The network as the feed reports it: home in Merchtem, and two repeaters that
 * are both comfortably inside a 100 km home view. */
QVector<MapView::Marker> network(bool gentTalking, bool oostendeTalking)
{
    return {
        station("ON0GRC", 51.0450, 3.7018, gentTalking),        /* Gent      */
        station("ON0OST", 51.2265, 2.9226, oostendeTalking),    /* Oostende  */
        station("ON6URE", 50.9513, 4.2320, false, true),        /* home      */
    };
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    MapView map;
    map.resize(420, 260);
    map.setCameraTiming(/*qualify*/ 150, /*linger*/ 200, /*hold*/ 400);

    /* ---- home ---- */

    map.setMarkers(network(false, false));
    const int homeZoom = map.zoom();
    check(near(map.centreLatitude(), 50.9513) && near(map.centreLongitude(), 4.2320),
          "with nobody talking the map sits on your own station");

    /* ---- a talker who is ALREADY ON SCREEN ---- */

    map.setMarkers(network(true, false));
    check(near(map.centreLatitude(), 50.9513),
          "a key-up does not move the map at once — it might be a kerchunk");

    wait(220);
    map.setMarkers(network(true, false));
    check(near(map.centreLatitude(), 51.0450) && near(map.centreLongitude(), 3.7018),
          "after the qualify delay the map goes to the talker, though it was already visible");
    check(map.zoom() > homeZoom, "and zooms IN on them, which is the whole point");
    const int talkerZoom = map.zoom();

    /* Nothing changed, so nothing moves: the signature is what gates it. */
    map.zoomOut();                        /* a manual nudge, to have something to detect */
    const int nudged = map.zoom();
    map.resetView();                      /* back on the talker, hold cleared */
    check(map.zoom() == talkerZoom, "recentre returns to the talker view");
    Q_UNUSED(nudged);

    /* ---- between two overs ---- */

    map.setMarkers(network(false, false));
    check(near(map.centreLatitude(), 51.0450),
          "when the talker stops, the view lingers rather than bouncing home");

    wait(260);
    map.setMarkers(network(false, false));
    check(near(map.centreLatitude(), 50.9513) && map.zoom() == homeZoom,
          "and once the linger has passed it goes home again");

    /* ---- a second station ---- */

    map.setMarkers(network(false, true));
    wait(220);
    map.setMarkers(network(false, true));
    check(near(map.centreLatitude(), 51.2265) && near(map.centreLongitude(), 2.9226),
          "a different talker gets the view in turn");

    /* ---- the hand on the mouse wins, for a while ---- */

    map.zoomOut();                                   /* the user takes over */
    const double heldLat = map.centreLatitude();
    map.setMarkers(network(true, false));            /* Gent keys up instead */
    wait(220);
    map.setMarkers(network(true, false));
    check(near(map.centreLatitude(), heldLat),
          "a manual zoom holds the camera off, even for a new talker");

    wait(450);                                       /* the hold expires */
    map.setMarkers(network(true, false));
    check(near(map.centreLatitude(), 51.0450) && near(map.centreLongitude(), 3.7018),
          "but the hold expires, and the change that happened meanwhile is framed then");

    /* ---- recentre cuts a hold short ---- */

    map.zoomOut();
    map.setMarkers(network(true, false));
    check(map.zoom() != talkerZoom, "held again after another manual zoom");
    map.resetView();
    check(map.zoom() == talkerZoom && near(map.centreLatitude(), 51.0450),
          "recentre ends the hold at once and frames the talker");

    /* ---- the home radius is part of the view ---- */

    map.setMarkers(network(false, false));
    wait(260);
    map.setMarkers(network(false, false));
    const int z100 = map.zoom();
    map.setHomeRadiusKm(1000);
    check(map.zoom() < z100, "a wider home radius re-frames home, without waiting for a change");

    std::printf("\n%d/%d passed\n", g_run - g_fail, g_run);
    return g_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
