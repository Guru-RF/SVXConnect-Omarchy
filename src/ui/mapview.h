/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * A slippy map, drawn by hand.
 *
 * WHY NOT A MAP LIBRARY
 * ---------------------
 * The other ports embed one: the Windows app runs Leaflet inside WebView2.
 * Neither option exists here. Qt WebEngine is GPL-3.0-only — see
 * cmake/LicenceGuard.cmake, it would silently relicense the whole binary — and
 * Qt Location drags in QML and a Quick scene graph to put raster tiles on a
 * flat surface. So this is what the licence guard's own comment recommends:
 * draw it yourself. Web Mercator is about fifteen lines of arithmetic, and a
 * tile is a PNG at a URL.
 *
 * WHAT IT DRAWS
 * -------------
 * Raster tiles from OpenStreetMap — turned over into a dark basemap here when
 * the Omarchy theme is dark, since a light map under a dark theme is the one
 * thing that makes a themed window look broken, and the ready-made dark tile
 * services now want an API key. Over them, one marker per node the reflector
 * knows the position of, with the current talker highlighted and your own
 * station marked separately.
 *
 * TILE ETIQUETTE
 * --------------
 * The OSM tile policy is a rate limit and an identification requirement, and
 * both are honoured here: every request carries the application's User-Agent,
 * tiles are cached on disk between runs, at most a few requests are in flight,
 * and a tile that scrolled out of view before it arrived is not re-requested.
 * Zoom is capped at 12 — a reflector map is about which country a station is
 * in, not which street — which also bounds how many tiles a pan can ask for.
 */
#ifndef SVXCONNECT_OMARCHY_MAPVIEW_H
#define SVXCONNECT_OMARCHY_MAPVIEW_H

#include <QWidget>
#include <QElapsedTimer>
#include <QHash>
#include <QPixmap>
#include <QPointF>
#include <QSet>
#include <QVector>

class QFrame;
class QLabel;
class QNetworkAccessManager;
class QNetworkReply;
class QToolButton;

class MapView : public QWidget {
    Q_OBJECT

public:
    /* One station on the map. `self` is this client; `talking` is drawn on top
     * of everything else, because it is the only marker anyone is looking for. */
    struct Marker {
        QString      callsign;
        QString      detail;       /* the short line drawn beside the dot     */
        QString      location;     /* the portal's free text, e.g. "Brugge"   */
        int          tg      = 0;
        QString      tgName;       /* what the portal calls that talkgroup    */
        QVector<int> monitoredTgs;
        bool         online  = false;
        double       latitude  = 0.0;
        double       longitude = 0.0;
        bool         talking   = false;
        bool         self      = false;
    };

    explicit MapView(QWidget *parent = nullptr);
    ~MapView() override;

    /* Replaces the whole set. Cheap when nothing changed: the markers are
     * compared and a repaint only happens if they differ. */
    void setMarkers(const QVector<Marker> &markers);

    /* Centre and zoom so every marker is visible, with a margin. Does nothing
     * when there are no markers, or when the user has panned — an automatic
     * recentre that fights the hand on the mouse is worse than no recentre. */
    void fitToMarkers();

    /* Forget that the user panned, and fit again on the next marker update. */
    void resetView();

    /* Open the station card for a callsign, centring on it if it is off
     * screen. False when the reflector has no position for that station. */
    bool openStation(const QString &callsign);

    /* How tall the pane would like to be, in pixels — what the drag handle
     * above it sets. A preference, not a constraint: the layout may still give
     * it less, down to minimumSizeHint(), rather than forcing the window to
     * grow. */
    void setPreferredHeight(int px);
    int  preferredHeight() const { return m_preferredHeight; }

    /* How wide a view "home" means, in kilometres — the macOS app's
     * mapHomeRadiusKm, and the same default. Only used when nobody is
     * transmitting. */
    void setHomeRadiusKm(int km);
    int  homeRadiusKm() const { return m_homeRadiusKm; }

    /* Where the camera is. Read by tests/test_mapcamera.cpp, which is the only
     * way to know the camera logic works: it is all timing and state, and a
     * screenshot of a map says nothing about whether it would have moved. */
    int    zoom()            const { return m_zoom; }
    double centreLatitude()  const { return m_latitude; }
    double centreLongitude() const { return m_longitude; }

    /* How long a talker must transmit before the view moves for it, how long
     * the talker view lingers after the last one stops, and how long a manual
     * pan holds the camera off. The defaults are 1.5 s, 3 s and 30 s; the
     * setter exists so a test does not have to take half a minute. */
    void setCameraTiming(qint64 qualifyMs, qint64 lingerMs, qint64 holdMs);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

public slots:
    /* The on-map controls, also reachable from the window's menu. */
    void zoomIn();
    void zoomOut();

    /* Extra lines for the open station card — the answer to stationOpened().
     * Ignored when the card has moved on to another station, which is what
     * makes a slow lookup harmless. */
    void setStationInfo(const QString &callsign, const QString &text);

signals:
    /* A station was clicked. The window answers with whatever it can find out
     * about the callsign — see setStationInfo(). */
    void stationOpened(const QString &callsign);

protected:
    void keyPressEvent(QKeyEvent *) override;
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void leaveEvent(QEvent *) override;
    void resizeEvent(QResizeEvent *) override;

private:
    /* ---- Web Mercator, in tile units at the current zoom ---- */
    static QPointF project(double latitude, double longitude, int zoom);
    static double  latitudeOf(double tileY, int zoom);

    /* The zoom at which `spanMetres` fills `px` pixels at that latitude —
     * how a camera described in kilometres becomes a slippy zoom level. */
    static int zoomForSpan(double spanMetres, int px, double latitude);
    static double metresPerPixel(int zoom, double latitude);

    /* ---- the camera ----
     *
     * The view is decided by a SIGNATURE — who has been transmitting long
     * enough to count, where you are, and the home radius — and the map moves
     * when that signature changes, never otherwise. That single rule is what
     * the Android and macOS apps both do, and it gives all the behaviour at
     * once: a talker pulls the view in (even one already on screen, which the
     * first version of this refused to do — on a 100 km home view every
     * Belgian repeater is "already on screen", so the map never moved at
     * all); the end of the talking sends it home again; and an unchanged
     * situation never yanks the view on a tick.
     *
     * Two refinements on top. The talker view LINGERS for a moment after the
     * last talker stops, so the gap between two overs of one QSO does not
     * bounce the map home and back. And a manual pan or zoom HOLDS the camera
     * off for a while — the reference apps do not protect a pan at all, and
     * having the map snatched from under the cursor is the one thing about
     * them not worth copying — but the hold expires, so the map does not stay
     * dead for the rest of the session because someone scrolled once. */
    void    updateTalkerClock();
    QVector<int> qualifiedTalkers() const;          /* indices into m_markers */
    bool    userHolding();                          /* also expires the hold  */
    void    noteUserMove();
    bool    refreshCamera(bool force);
    void    frame(const QVector<int> &talkers);     /* choose what to look at */
    void    applyTarget();                          /* and turn it into a zoom */

    QPointF centreTile() const;                 /* map centre, in tile units  */
    QPointF toWidget(double lat, double lon) const;

    /* Change zoom keeping `anchor` (in widget coordinates) over the same point
     * on the ground. The centre is the anchor for the buttons; the pointer is
     * the anchor for the wheel. */
    void zoomTo(int zoom, const QPointF &anchor);
    void layOutControls();
    void refreshControls();

    /* The station card: what the reflector knows about one marker, opened by
     * clicking it. */
    void buildCard();
    void openCard(int markerIndex);
    void closeCard();
    void refreshCard();
    void placeCard();
    static QString kindOf(const Marker &m);

    void requestTile(int z, int x, int y);
    void onTileReady(QNetworkReply *reply, const QString &key, int z, int x, int y);
    QString tileUrl(int z, int x, int y) const;
    void drawMarker(QPainter &p, const Marker &m, const QPointF &at, bool hovered);
    int  markerAt(const QPointF &pos) const;    /* index, or -1 */

    QNetworkAccessManager *m_net = nullptr;

    QToolButton *m_zoomIn   = nullptr;
    QToolButton *m_zoomOut  = nullptr;
    QToolButton *m_recentre = nullptr;

    QFrame *m_card      = nullptr;
    QLabel *m_cardTitle = nullptr;
    QLabel *m_cardKind  = nullptr;
    QLabel *m_cardWhere = nullptr;
    QLabel *m_cardTgs   = nullptr;
    QLabel *m_cardPos   = nullptr;
    QLabel *m_cardInfo  = nullptr;   /* filled by setStationInfo() */
    QString m_cardCall;              /* which station it is open on */

    QHash<QString, QPixmap> m_tiles;     /* "z/x/y" -> pixmap           */
    QSet<QString>           m_inflight;  /* a request is open right now */
    QSet<QString>           m_queued;    /* waiting for a free slot     */
    QVector<QString>        m_queue;     /* ...in this order            */
    bool                    m_darkTiles = false;

    QVector<Marker> m_markers;

    int    m_preferredHeight = 0;   /* 0 = the built-in default */
    int    m_homeRadiusKm    = 100;

    /* callsign -> when it started transmitting, on m_clock's scale. A talker
     * earns a camera move only after it has been going for a moment; brief
     * key-ups would otherwise throw the view around. */
    QHash<QString, qint64> m_talkerSince;
    QElapsedTimer          m_clock;

    /* What the camera is looking at, in ground units: the zoom is derived when
     * it is applied, so a resized pane keeps its subject and only re-zooms. */
    struct CameraTarget {
        bool   valid = false;
        double latitude = 0.0, longitude = 0.0;
        double spanLatM = 0.0, spanLonM = 0.0;
    };
    CameraTarget m_target;
    QString      m_cameraSig;               /* last signature framed; empty = must frame */
    bool         m_followingTalkers = false;
    qint64       m_talkEndedAt = -1;        /* when the talker set went empty, for the linger */
    qint64       m_userMovedAt = 0;         /* last manual pan or zoom, for the hold          */
    int          m_wheelAccum  = 0;         /* high-resolution wheels send fractions of a step */

    qint64 m_qualifyMs = 1500;              /* see setCameraTiming() */
    qint64 m_lingerMs  = 3000;
    qint64 m_holdMs    = 30000;

    double m_latitude  = 50.5;   /* somewhere over Belgium, until told better */
    double m_longitude = 4.5;
    int    m_zoom      = 6;

    bool    m_userMoved = false;
    bool    m_dragging  = false;
    QPointF m_dragFrom;
    QPointF m_pressAt;      /* to tell a click from the start of a pan */
    int     m_hovered   = -1;
};

#endif
