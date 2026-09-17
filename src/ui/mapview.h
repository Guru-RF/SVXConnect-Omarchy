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

public slots:
    /* The on-map controls, also reachable from the window's menu. */
    void zoomIn();
    void zoomOut();

    /* Extra lines for the open station card — the answer to stationOpened().
     * Ignored when the card has moved on to another station, which is what
     * makes a slow lookup harmless. */
    void setStationInfo(const QString &callsign, const QString &text);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

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
