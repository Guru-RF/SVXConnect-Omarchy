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

class QNetworkAccessManager;
class QNetworkReply;

class MapView : public QWidget {
    Q_OBJECT

public:
    /* One station on the map. `self` is this client; `talking` is drawn on top
     * of everything else, because it is the only marker anyone is looking for. */
    struct Marker {
        QString callsign;
        QString detail;      /* talkgroup, or how long since it was heard */
        double  latitude  = 0.0;
        double  longitude = 0.0;
        bool    talking   = false;
        bool    self      = false;
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

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
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

    void requestTile(int z, int x, int y);
    void onTileReady(QNetworkReply *reply, const QString &key, int z, int x, int y);
    QString tileUrl(int z, int x, int y) const;
    void drawMarker(QPainter &p, const Marker &m, const QPointF &at, bool hovered);
    int  markerAt(const QPointF &pos) const;    /* index, or -1 */

    QNetworkAccessManager *m_net = nullptr;

    QHash<QString, QPixmap> m_tiles;     /* "z/x/y" -> pixmap           */
    QSet<QString>           m_inflight;  /* a request is open right now */
    QSet<QString>           m_queued;    /* waiting for a free slot     */
    QVector<QString>        m_queue;     /* ...in this order            */
    bool                    m_darkTiles = false;

    QVector<Marker> m_markers;

    double m_latitude  = 50.5;   /* somewhere over Belgium, until told better */
    double m_longitude = 4.5;
    int    m_zoom      = 6;

    bool    m_userMoved = false;
    bool    m_dragging  = false;
    QPointF m_dragFrom;
    int     m_hovered   = -1;
};

#endif
