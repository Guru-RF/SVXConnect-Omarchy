/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 */
#include "ui/mapview.h"
#include "ui/theme.h"

#include <QApplication>
#include <QDir>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QStandardPaths>
#include <QWheelEvent>
#include <QtMath>

namespace {

constexpr int kTile     = 256;
constexpr int kMinZoom  = 2;
constexpr int kMaxZoom  = 12;   /* see the header: countries, not streets */
constexpr int kMaxFlight = 4;

QString keyOf(int z, int x, int y)
{
    return QStringLiteral("%1/%2/%3").arg(z).arg(x).arg(y);
}

/* Wrap x around the date line; y is clamped, since there is nothing above the
 * top tile. Returns false when the tile does not exist at this zoom. */
bool normalise(int z, int *x, int *y)
{
    const int n = 1 << z;
    if (*y < 0 || *y >= n)
        return false;
    *x = ((*x % n) + n) % n;
    return true;
}

/* A light basemap under a dark theme is the one thing that makes a themed
 * window look broken, so the tiles are turned over: lightness inverted, hue
 * kept, saturation pulled back, and the result compressed into the dark half
 * of the range. That is the same transform the CSS dark-map filters do
 * (invert + hue-rotate), and it keeps water blue and parks green instead of
 * producing a photographic negative.
 *
 * Done once per tile, on arrival; the disk cache holds the original PNG. */
QImage darkened(const QImage &src)
{
    QImage img = src.convertToFormat(QImage::Format_RGB32);

    for (int y = 0; y < img.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            float h = 0.0f, sat = 0.0f, l = 0.0f, a = 1.0f;
            QColor::fromRgb(line[x]).getHslF(&h, &sat, &l, &a);
            if (h < 0.0f)
                h = 0.0f;                        /* achromatic: hue is unset */
            l = 0.06f + (1.0f - l) * 0.48f;      /* invert, then keep it dark */
            sat *= 0.55f;
            line[x] = QColor::fromHslF(h, sat, l).rgb();
        }
    }
    return img;
}

} // namespace

MapView::MapView(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(Theme::space(170));
    setMouseTracking(true);
    setCursor(Qt::OpenHandCursor);
    setAttribute(Qt::WA_StyledBackground, false);

    m_net = new QNetworkAccessManager(this);

    /* Tiles survive a restart. Without this every launch re-downloads the same
     * dozen tiles, which is exactly what the OSM policy asks people not to do. */
    auto *cache = new QNetworkDiskCache(this);
    cache->setCacheDirectory(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                             + QStringLiteral("/tiles"));
    cache->setMaximumCacheSize(64ll * 1024 * 1024);
    m_net->setCache(cache);

    m_darkTiles = Theme::palette().dark;
    connect(&OmarchyTheme::get(), &OmarchyTheme::changed, this, [this]() {
        const bool dark = Theme::palette().dark;
        if (dark == m_darkTiles) {
            update();
            return;
        }
        /* The basemap follows the theme, so everything cached is the wrong
         * colour now. */
        m_darkTiles = dark;
        m_tiles.clear();
        m_queue.clear();
        update();
    });
}

MapView::~MapView() = default;

QSize MapView::sizeHint() const        { return QSize(Theme::space(420), Theme::space(260)); }
QSize MapView::minimumSizeHint() const { return QSize(Theme::space(200), Theme::space(170)); }

/* ------------------------------------------------------------- projection */

QPointF MapView::project(double latitude, double longitude, int zoom)
{
    const double n   = 1 << zoom;
    const double lat = qBound(-85.05112878, latitude, 85.05112878);
    const double rad = qDegreesToRadians(lat);
    return QPointF(n * (longitude + 180.0) / 360.0,
                   n * (1.0 - std::log(std::tan(rad) + 1.0 / std::cos(rad)) / M_PI) / 2.0);
}

double MapView::latitudeOf(double tileY, int zoom)
{
    const double n = 1 << zoom;
    return qRadiansToDegrees(std::atan(std::sinh(M_PI * (1.0 - 2.0 * tileY / n))));
}

QPointF MapView::centreTile() const
{
    return project(m_latitude, m_longitude, m_zoom);
}

QPointF MapView::toWidget(double lat, double lon) const
{
    const QPointF t = project(lat, lon, m_zoom) - centreTile();
    return QPointF(width() / 2.0 + t.x() * kTile, height() / 2.0 + t.y() * kTile);
}

/* ----------------------------------------------------------------- tiles */

QString MapView::tileUrl(int z, int x, int y) const
{
    /* One provider. The ready-made dark basemaps — CARTO's dark_all, which the
     * Windows port uses, and Stadia's — now want an API key, and a map that
     * stops working when a key expires is worse than no map. OpenStreetMap's
     * own tiles need nothing but attribution and a User-Agent, so they are what
     * this uses, and the dark variant is made here (see darkened()). */
    return QStringLiteral("https://tile.openstreetmap.org/%1/%2/%3.png").arg(z).arg(x).arg(y);
}

void MapView::requestTile(int z, int x, int y)
{
    const QString key = keyOf(z, x, y);
    if (m_tiles.contains(key) || m_pending.contains(key))
        return;

    m_pending.insert(key);
    if (m_inFlight >= kMaxFlight) {
        m_queue.append(key);
        return;
    }

    ++m_inFlight;

    QNetworkRequest req{QUrl(tileUrl(z, x, y))};
    /* The tile policy requires an identifying User-Agent. A request with Qt's
     * default one is what gets a whole application blocked. */
    req.setRawHeader(QByteArrayLiteral("User-Agent"),
                     QByteArrayLiteral("SVXConnect-Omarchy/") + SVXCONNECT_VERSION
                         + QByteArrayLiteral(" (+https://svxconnect.app)"));
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                     QVariant::fromValue(QNetworkRequest::PreferCache));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QVariant::fromValue(QNetworkRequest::NoLessSafeRedirectPolicy));
    req.setTransferTimeout(15000);

    QNetworkReply *reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, key, z, x, y]() {
        onTileReady(reply, key, z, x, y);
    });
}

void MapView::onTileReady(QNetworkReply *reply, const QString &key, int z, int x, int y)
{
    reply->deleteLater();
    m_pending.remove(key);
    --m_inFlight;

    if (reply->error() == QNetworkReply::NoError) {
        QImage img;
        if (img.loadFromData(reply->readAll()) && !img.isNull()) {
            const QPixmap pm = QPixmap::fromImage(m_darkTiles ? darkened(img) : img);
            m_tiles.insert(key, pm);
            /* A cache that grows without bound is a leak with a nice name. */
            if (m_tiles.size() > 600)
                m_tiles.clear();
            update();
        }
    }
    Q_UNUSED(z); Q_UNUSED(x); Q_UNUSED(y);

    /* Next from the queue, skipping anything that scrolled out of view while
     * it waited — m_queue is drained on every paint. */
    while (!m_queue.isEmpty() && m_inFlight < kMaxFlight) {
        const QString next = m_queue.takeFirst();
        const QStringList parts = next.split(QLatin1Char('/'));
        if (parts.size() != 3)
            continue;
        m_pending.remove(next);   /* requestTile re-inserts it */
        requestTile(parts[0].toInt(), parts[1].toInt(), parts[2].toInt());
    }
}

/* ---------------------------------------------------------------- markers */

void MapView::setMarkers(const QVector<Marker> &markers)
{
    auto same = [](const QVector<Marker> &a, const QVector<Marker> &b) {
        if (a.size() != b.size())
            return false;
        for (int i = 0; i < a.size(); ++i) {
            if (a[i].callsign != b[i].callsign || a[i].talking != b[i].talking
                || a[i].detail != b[i].detail
                || !qFuzzyCompare(a[i].latitude + 1.0, b[i].latitude + 1.0)
                || !qFuzzyCompare(a[i].longitude + 1.0, b[i].longitude + 1.0))
                return false;
        }
        return true;
    };

    if (same(markers, m_markers))
        return;

    const bool hadNone = m_markers.isEmpty();
    m_markers = markers;

    if (!m_userMoved) {
        if (hadNone) {
            fitToMarkers();
        } else {
            /* Someone keyed up outside the visible area: follow them. A talker
             * already on screen does not move the view — a map that jumps at
             * every over is unusable, and the marker is right there. */
            const QRectF visible = QRectF(rect()).adjusted(Theme::space(20), Theme::space(20),
                                                           -Theme::space(20), -Theme::space(20));
            for (const Marker &m : m_markers) {
                if (m.talking && !visible.contains(toWidget(m.latitude, m.longitude))) {
                    fitToMarkers();
                    break;
                }
            }
        }
    }

    update();
}

void MapView::fitToMarkers()
{
    if (m_markers.isEmpty())
        return;

    /* What to look at, in order of what anyone actually wants to see:
     *
     *   1. whoever is transmitting;
     *   2. failing that, your own station and its surroundings;
     *   3. failing that, everything.
     *
     * Fitting everything sounds neutral and is not: one station on holiday in
     * Egypt and one placeholder in Sweden are enough to zoom a Belgian
     * reflector out to a view of the Atlantic. */
    QVector<Marker> subject;
    for (const Marker &m : m_markers)
        if (m.talking) subject.append(m);

    double padLat = 0.35, padLon = 0.5;    /* ≈ 40 km around a talker */
    if (subject.isEmpty()) {
        for (const Marker &m : m_markers)
            if (m.self) subject.append(m);
        padLat = 1.1; padLon = 1.7;        /* ≈ 120 km around home    */
    }
    if (subject.isEmpty()) {
        subject = m_markers;
        padLat = 0.3; padLon = 0.45;
    }

    double north = -90.0, south = 90.0, east = -180.0, west = 180.0;
    for (const Marker &m : subject) {
        north = qMax(north, m.latitude);
        south = qMin(south, m.latitude);
        east  = qMax(east,  m.longitude);
        west  = qMin(west,  m.longitude);
    }

    m_latitude  = (north + south) / 2.0;
    m_longitude = (east + west) / 2.0;

    /* Widen the box so a single marker does not ask for the maximum zoom. */
    north += padLat; south -= padLat;
    east  += padLon; west  -= padLon;

    int zoom = kMinZoom;
    for (int z = kMaxZoom; z >= kMinZoom; --z) {
        const QPointF a = project(north, west, z);
        const QPointF b = project(south, east, z);
        const double w = qAbs(b.x() - a.x()) * kTile;
        const double h = qAbs(b.y() - a.y()) * kTile;
        /* The floors matter: in a short pane the height test alone would drop
         * the zoom until a country fits in eighty pixels, which is how a map of
         * Belgium ends up showing the Atlantic. */
        const double marginW = qMax(width()  - Theme::space(60), Theme::space(220));
        const double marginH = qMax(height() - Theme::space(50), Theme::space(150));
        if (w <= marginW && h <= marginH) {
            zoom = z;
            break;
        }
    }
    m_zoom = qBound(kMinZoom, zoom, kMaxZoom);
    update();
}

void MapView::resetView()
{
    m_userMoved = false;
    fitToMarkers();
}

int MapView::markerAt(const QPointF &pos) const
{
    const double r = Theme::space(9);
    /* Last first: the talker is drawn last and so is on top. */
    for (int i = m_markers.size() - 1; i >= 0; --i) {
        if (QLineF(toWidget(m_markers[i].latitude, m_markers[i].longitude), pos).length() <= r)
            return i;
    }
    return -1;
}

/* ---------------------------------------------------------------- painting */

void MapView::drawMarker(QPainter &p, const Marker &m, const QPointF &at, bool hovered)
{
    const OmarchyTheme::Palette &pal = Theme::palette();

    const qreal r = Theme::space(m.talking ? 6 : 4);
    const QColor fill = m.talking ? Theme::connected()
                      : m.self    ? Theme::palette().accent
                                  : Theme::wash(pal.foreground, 0.55);

    if (m.talking) {
        /* A halo, so the one marker that matters is findable without reading
         * any labels. */
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::wash(Theme::connected(), 0.22));
        p.drawEllipse(at, r * 2.6, r * 2.6);
    }

    QPen pen(pal.background);
    pen.setWidthF(1.5);
    p.setPen(pen);
    p.setBrush(fill);
    p.drawEllipse(at, r, r);

    if (!m.talking && !m.self && !hovered)
        return;

    /* Labels only where they earn their space: the talker, you, and whatever
     * the pointer is over. Every node labelled is an unreadable map. */
    const QFont f = Theme::font(Theme::Font::Caption, m.talking);
    const QFontMetrics fm(f);
    const QString text = m.detail.isEmpty()
                       ? m.callsign
                       : QStringLiteral("%1 · %2").arg(m.callsign, m.detail);

    const QRectF box(at.x() + r + Theme::space(4),
                     at.y() - fm.height() / 2.0 - Theme::space(2),
                     fm.horizontalAdvance(text) + Theme::space(10),
                     fm.height() + Theme::space(4));

    p.setFont(f);
    p.setPen(Qt::NoPen);
    p.setBrush(Theme::wash(pal.background, 0.82));
    p.drawRoundedRect(box, Theme::space(3), Theme::space(3));
    p.setPen(m.talking ? Theme::connected() : pal.foreground);
    p.drawText(box, Qt::AlignCenter, text);
}

void MapView::paintEvent(QPaintEvent *)
{
    const OmarchyTheme::Palette &pal = Theme::palette();

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.fillRect(rect(), pal.darkBackground);

    /* ---- tiles ---- */
    const QPointF c = centreTile();
    const double originX = width()  / 2.0 - c.x() * kTile;
    const double originY = height() / 2.0 - c.y() * kTile;

    const int firstX = int(std::floor(-originX / kTile));
    const int firstY = int(std::floor(-originY / kTile));
    const int lastX  = int(std::floor((width()  - originX) / kTile));
    const int lastY  = int(std::floor((height() - originY) / kTile));

    m_queue.clear();   /* only ask for what is on screen right now */

    bool missing = false;
    for (int ty = firstY; ty <= lastY; ++ty) {
        for (int tx = firstX; tx <= lastX; ++tx) {
            int nx = tx, ny = ty;
            if (!normalise(m_zoom, &nx, &ny))
                continue;

            const QRectF dest(originX + tx * kTile, originY + ty * kTile, kTile, kTile);
            const auto it = m_tiles.constFind(keyOf(m_zoom, nx, ny));
            if (it != m_tiles.constEnd()) {
                p.drawPixmap(dest, *it, QRectF(0, 0, kTile, kTile));
            } else {
                p.fillRect(dest, Theme::wash(pal.foreground, 0.04));
                requestTile(m_zoom, nx, ny);
                missing = true;
            }
        }
    }

    /* ---- markers, talker last so it lands on top ---- */
    QVector<int> order;
    order.reserve(m_markers.size());
    for (int i = 0; i < m_markers.size(); ++i)
        if (!m_markers[i].talking) order.append(i);
    for (int i = 0; i < m_markers.size(); ++i)
        if (m_markers[i].talking) order.append(i);

    for (int i : order) {
        const Marker &m = m_markers[i];
        const QPointF at = toWidget(m.latitude, m.longitude);
        if (at.x() < -50 || at.y() < -50 || at.x() > width() + 50 || at.y() > height() + 50)
            continue;
        drawMarker(p, m, at, i == m_hovered);
    }

    /* ---- attribution, which is a licence condition and not decoration ---- */
    const QFont small = Theme::font(Theme::Font::Caption);
    p.setFont(small);
    const QString credit = tr("© OpenStreetMap contributors");
    const QFontMetrics fm(small);
    const QRectF creditBox(width() - fm.horizontalAdvance(credit) - Theme::space(10),
                           height() - fm.height() - Theme::space(6),
                           fm.horizontalAdvance(credit) + Theme::space(6),
                           fm.height() + Theme::space(2));
    p.setPen(Qt::NoPen);
    p.setBrush(Theme::wash(pal.background, 0.7));
    p.drawRoundedRect(creditBox, Theme::space(2), Theme::space(2));
    p.setPen(Theme::wash(pal.foreground, 0.65));
    p.drawText(creditBox, Qt::AlignCenter, credit);

    if (m_markers.isEmpty()) {
        p.setPen(Theme::wash(pal.foreground, 0.55));
        p.setFont(Theme::font(Theme::Font::Body));
        p.drawText(rect(), Qt::AlignCenter,
                   missing ? tr("Loading the map…")
                           : tr("No station positions from this reflector."));
    }
}

/* ------------------------------------------------------------- interaction */

void MapView::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton)
        return;
    m_dragging = true;
    m_dragFrom = e->position();
    setCursor(Qt::ClosedHandCursor);
}

void MapView::mouseMoveEvent(QMouseEvent *e)
{
    if (!m_dragging) {
        const int hit = markerAt(e->position());
        if (hit != m_hovered) {
            m_hovered = hit;
            setToolTip(hit < 0 ? QString()
                               : tr("%1 — %2, %3")
                                     .arg(m_markers[hit].callsign)
                                     .arg(m_markers[hit].latitude,  0, 'f', 4)
                                     .arg(m_markers[hit].longitude, 0, 'f', 4));
            update();
        }
        return;
    }

    const QPointF delta = e->position() - m_dragFrom;
    m_dragFrom = e->position();

    QPointF c = centreTile() - QPointF(delta.x() / kTile, delta.y() / kTile);
    const double n = 1 << m_zoom;
    c.setY(qBound(0.0, c.y(), n));

    m_latitude  = latitudeOf(c.y(), m_zoom);
    m_longitude = c.x() / n * 360.0 - 180.0;
    while (m_longitude >  180.0) m_longitude -= 360.0;
    while (m_longitude < -180.0) m_longitude += 360.0;

    m_userMoved = true;
    update();
}

void MapView::mouseReleaseEvent(QMouseEvent *)
{
    m_dragging = false;
    setCursor(Qt::OpenHandCursor);
}

void MapView::mouseDoubleClickEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton)
        return;
    if (m_zoom < kMaxZoom) {
        ++m_zoom;
        m_userMoved = true;
        update();
    }
}

void MapView::wheelEvent(QWheelEvent *e)
{
    const int steps = e->angleDelta().y() > 0 ? 1 : -1;
    const int next = qBound(kMinZoom, m_zoom + steps, kMaxZoom);
    if (next == m_zoom) {
        e->accept();
        return;
    }

    /* Zoom about the pointer, not the centre: zooming towards a station and
     * watching it slide off screen is the thing that makes a hand-written map
     * feel hand-written. */
    const QPointF before = e->position();
    const double n0 = 1 << m_zoom;
    const QPointF c0 = centreTile();
    const QPointF world0(c0.x() + (before.x() - width() / 2.0) / kTile,
                         c0.y() + (before.y() - height() / 2.0) / kTile);

    m_zoom = next;
    const double scale = double(1 << m_zoom) / n0;
    const QPointF world1 = world0 * scale;

    const QPointF c1(world1.x() - (before.x() - width() / 2.0) / kTile,
                     world1.y() - (before.y() - height() / 2.0) / kTile);

    m_latitude  = latitudeOf(qBound(0.0, c1.y(), double(1 << m_zoom)), m_zoom);
    m_longitude = c1.x() / double(1 << m_zoom) * 360.0 - 180.0;
    while (m_longitude >  180.0) m_longitude -= 360.0;
    while (m_longitude < -180.0) m_longitude += 360.0;

    m_userMoved = true;
    update();
    e->accept();
}

void MapView::leaveEvent(QEvent *)
{
    if (m_hovered >= 0) {
        m_hovered = -1;
        setToolTip(QString());
        update();
    }
}
