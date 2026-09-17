/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 */
#include "ui/mapview.h"
#include "ui/theme.h"

#include <QApplication>
#include <QDir>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QSet>
#include <QStandardPaths>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtMath>
#include <algorithm>

namespace {

constexpr int kTile     = 256;
constexpr int kMinZoom  = 2;
constexpr int kMaxZoom  = 12;   /* see the header: countries, not streets */
constexpr int kMaxFlight = 4;

/* The macOS app's camera, in its own units. A single talker gets a 20 km-wide
 * view; "home" is whatever the user set, 100 km by default. */
constexpr double kTalkerSpanM      = 20000.0;
constexpr qint64 kTalkerQualifyMs  = 1500;    /* before the view will move for it */
constexpr double kMetresPerDegLat  = 111000.0;

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

    /* ---- the controls, over the map at its top right ----
     * Real buttons rather than something painted: they get the theme, the
     * hover states and the tooltips for free, and a painted control that has
     * to reimplement all three always ends up looking like a painted control. */
    auto control = [this](char32_t glyph, const QString &tip, const QString &shortcut) {
        auto *b = new QToolButton(this);
        b->setText(Theme::Glyph::of(glyph));
        b->setToolTip(shortcut.isEmpty() ? tip : tr("%1  (%2)").arg(tip, shortcut));
        b->setCursor(Qt::ArrowCursor);
        b->setFocusPolicy(Qt::NoFocus);
        Theme::setRole(b, "map");
        return b;
    };

    m_zoomIn   = control(Theme::Glyph::Plus,       tr("Zoom in"),  QStringLiteral("+"));
    m_zoomOut  = control(Theme::Glyph::Minus,      tr("Zoom out"), QStringLiteral("-"));
    m_recentre = control(Theme::Glyph::Crosshairs, tr("Centre on the current talker, or on your own station"),
                         QStringLiteral("0"));

    connect(m_zoomIn,   &QToolButton::clicked, this, &MapView::zoomIn);
    connect(m_zoomOut,  &QToolButton::clicked, this, &MapView::zoomOut);
    connect(m_recentre, &QToolButton::clicked, this, &MapView::resetView);

    buildCard();

    m_clock.start();

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
        m_queued.clear();
        update();
    });
}

MapView::~MapView() = default;

QSize MapView::sizeHint() const
{
    return QSize(Theme::space(420),
                 m_preferredHeight > 0 ? m_preferredHeight : Theme::space(260));
}

void MapView::setHomeRadiusKm(int km)
{
    const int want = qBound(10, km, 2000);
    if (want == m_homeRadiusKm)
        return;
    m_homeRadiusKm = want;
    if (!m_userMoved)
        fitToMarkers();
    update();
}

void MapView::setPreferredHeight(int px)
{
    const int want = qBound(Theme::space(150), px, Theme::space(620));
    if (want == m_preferredHeight)
        return;
    m_preferredHeight = want;
    updateGeometry();
}
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

double MapView::metresPerPixel(int zoom, double latitude)
{
    return 156543.03392 * std::cos(qDegreesToRadians(qBound(-85.0, latitude, 85.0)))
         / double(1 << zoom);
}

int MapView::zoomForSpan(double spanMetres, int px, double latitude)
{
    if (spanMetres <= 0.0 || px <= 0)
        return kMaxZoom;
    const double mpp = spanMetres / double(px);
    const double z = std::log2(156543.03392
                               * std::cos(qDegreesToRadians(qBound(-85.0, latitude, 85.0)))
                               / mpp);
    if (!std::isfinite(z))
        return kMinZoom;
    return qBound(kMinZoom, int(std::floor(z)), kMaxZoom);
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
    if (m_tiles.contains(key) || m_inflight.contains(key) || m_queued.contains(key))
        return;

    /* In flight and waiting are DIFFERENT states, and conflating them was a
     * bug worth a comment: the wait list is emptied on every paint, so a tile
     * that was only ever queued would have stayed marked as "asked for" and
     * never been requested again — a permanently blank square on the map. */
    if (m_inflight.size() >= kMaxFlight) {
        m_queued.insert(key);
        m_queue.append(key);
        return;
    }

    m_inflight.insert(key);

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
    m_inflight.remove(key);

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

    /* Next from the wait list, which only ever holds tiles that were on screen
     * at the last paint. */
    while (!m_queue.isEmpty() && m_inflight.size() < kMaxFlight) {
        const QString next = m_queue.takeFirst();
        m_queued.remove(next);
        const QStringList parts = next.split(QLatin1Char('/'));
        if (parts.size() == 3)
            requestTile(parts[0].toInt(), parts[1].toInt(), parts[2].toInt());
    }
}

/* ------------------------------------------------------------ station card */

/* What kind of station this is, in the reflector's own terms. The order is
 * load-bearing and is the one the macOS app settled on: a suffixed callsign is
 * a portable or a mobile whatever else it says (those are always flagged
 * offline, because they are not registered nodes at all), and only then does
 * the ON0 repeater prefix or the online flag get a say. */
QString MapView::kindOf(const Marker &m)
{
    if (m.callsign.contains(QLatin1Char('/')))
        return tr("PORTABLE");
    if (!m.online)
        return tr("OFFLINE");
    if (m.callsign.startsWith(QLatin1String("ON0")))
        return tr("REPEATER");
    return tr("NODE");
}

void MapView::buildCard()
{
    m_card = new QFrame(this);
    Theme::setRole(m_card, "mapcard");
    m_card->setAttribute(Qt::WA_StyledBackground, true);
    m_card->hide();

    auto *lay = new QVBoxLayout(m_card);
    lay->setContentsMargins(Theme::space(10), Theme::space(8), Theme::space(8), Theme::space(10));
    lay->setSpacing(Theme::space(3));

    auto *head = new QHBoxLayout;
    head->setSpacing(Theme::space(6));

    m_cardTitle = new QLabel(m_card);
    Theme::setRole(m_cardTitle, "value");
    head->addWidget(m_cardTitle);

    m_cardKind = new QLabel(m_card);
    Theme::setRole(m_cardKind, "chip");
    head->addWidget(m_cardKind);
    head->addStretch(1);

    auto *close = new QToolButton(m_card);
    close->setText(Theme::Glyph::of(Theme::Glyph::Close));
    close->setToolTip(tr("Close  (Esc)"));
    close->setCursor(Qt::ArrowCursor);
    close->setFocusPolicy(Qt::NoFocus);
    Theme::setRole(close, "icon");
    connect(close, &QToolButton::clicked, this, &MapView::closeCard);
    head->addWidget(close);
    lay->addLayout(head);

    m_cardWhere = new QLabel(m_card);
    m_cardWhere->setWordWrap(true);
    lay->addWidget(m_cardWhere);

    m_cardTgs = new QLabel(m_card);
    Theme::setRole(m_cardTgs, "hint");
    m_cardTgs->setWordWrap(true);
    lay->addWidget(m_cardTgs);

    m_cardPos = new QLabel(m_card);
    Theme::setRole(m_cardPos, "time");
    lay->addWidget(m_cardPos);

    m_cardInfo = new QLabel(m_card);
    Theme::setRole(m_cardInfo, "hint");
    m_cardInfo->setWordWrap(true);
    m_cardInfo->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_cardInfo->setOpenExternalLinks(true);
    m_cardInfo->hide();
    lay->addWidget(m_cardInfo);

    m_card->setMaximumWidth(Theme::space(320));
}

void MapView::openCard(int markerIndex)
{
    if (markerIndex < 0 || markerIndex >= m_markers.size()) {
        closeCard();
        return;
    }

    m_cardCall = m_markers[markerIndex].callsign;
    m_cardInfo->clear();
    m_cardInfo->hide();

    refreshCard();
    m_card->show();
    m_card->raise();
    placeCard();
    update();

    /* Whoever owns this view may know more about the callsign than the
     * reflector does — see setStationInfo(). */
    emit stationOpened(m_cardCall);
}

bool MapView::openStation(const QString &callsign)
{
    for (int i = 0; i < m_markers.size(); ++i) {
        if (m_markers[i].callsign.compare(callsign, Qt::CaseInsensitive) != 0)
            continue;

        /* Bring it into view if it is not already — a card pointing at a
         * marker off the edge of the pane explains nothing. */
        const QPointF at = toWidget(m_markers[i].latitude, m_markers[i].longitude);
        if (!QRectF(rect()).adjusted(Theme::space(20), Theme::space(20),
                                     -Theme::space(20), -Theme::space(20)).contains(at)) {
            m_latitude  = m_markers[i].latitude;
            m_longitude = m_markers[i].longitude;
            m_userMoved = true;
            refreshControls();
        }
        openCard(i);
        return true;
    }
    return false;
}

void MapView::closeCard()
{
    if (m_cardCall.isEmpty() && !m_card->isVisible())
        return;
    m_cardCall.clear();
    m_card->hide();
    update();
}

void MapView::refreshCard()
{
    if (m_cardCall.isEmpty())
        return;

    const Marker *m = nullptr;
    for (const Marker &c : m_markers)
        if (c.callsign == m_cardCall) { m = &c; break; }

    if (!m) {                       /* it left the reflector while open */
        closeCard();
        return;
    }

    m_cardTitle->setText(m->callsign);
    m_cardKind->setText(m->talking ? tr("TALKING") : kindOf(*m));
    Theme::setProp(m_cardKind, "tone", m->talking ? QStringLiteral("ok") : QString());

    m_cardWhere->setText(m->location.isEmpty() ? tr("No location published") : m->location);
    m_cardWhere->setVisible(true);

    QStringList lines;
    if (m->tg > 0)
        lines << (m->tgName.isEmpty() ? tr("On TG %1").arg(m->tg)
                                      : tr("On TG %1 — %2").arg(m->tg).arg(m->tgName));
    if (!m->monitoredTgs.isEmpty()) {
        QStringList tgs;
        for (int tg : m->monitoredTgs)
            tgs << QString::number(tg);
        lines << tr("Monitors %1").arg(tgs.join(QStringLiteral(", ")));
    }
    m_cardTgs->setText(lines.join(QStringLiteral(" · ")));
    m_cardTgs->setVisible(!lines.isEmpty());

    m_cardPos->setText(tr("%1, %2").arg(m->latitude, 0, 'f', 5).arg(m->longitude, 0, 'f', 5));
}

void MapView::setStationInfo(const QString &callsign, const QString &text)
{
    /* A lookup that lands after the card moved on belongs to nobody. */
    if (m_cardCall.isEmpty() || callsign.compare(m_cardCall, Qt::CaseInsensitive) != 0)
        return;

    m_cardInfo->setText(text);
    m_cardInfo->setVisible(!text.isEmpty());
    placeCard();
}

void MapView::placeCard()
{
    if (!m_card->isVisible() || m_cardCall.isEmpty())
        return;

    const Marker *m = nullptr;
    for (const Marker &c : m_markers)
        if (c.callsign == m_cardCall) { m = &c; break; }
    if (!m)
        return;

    m_card->adjustSize();
    const QSize sz = m_card->size();
    const QPointF at = toWidget(m->latitude, m->longitude);
    const int pad = Theme::space(8);

    /* Beside the marker, flipped to whichever side has room, and never off the
     * edge of the pane. */
    int x = int(at.x()) + Theme::space(14);
    if (x + sz.width() + pad > width())
        x = int(at.x()) - Theme::space(14) - sz.width();
    int y = int(at.y()) - sz.height() / 2;

    m_card->move(qBound(pad, x, qMax(pad, width()  - sz.width()  - pad)),
                 qBound(pad, y, qMax(pad, height() - sz.height() - pad)));
}

/* ---------------------------------------------------------------- markers */

void MapView::setMarkers(const QVector<Marker> &markers)
{
    auto same = [](const QVector<Marker> &a, const QVector<Marker> &b) {
        if (a.size() != b.size())
            return false;
        for (int i = 0; i < a.size(); ++i) {
            if (a[i].callsign != b[i].callsign || a[i].talking != b[i].talking
                || a[i].detail != b[i].detail || a[i].tg != b[i].tg
                || a[i].online != b[i].online || a[i].location != b[i].location
                || !qFuzzyCompare(a[i].latitude + 1.0, b[i].latitude + 1.0)
                || !qFuzzyCompare(a[i].longitude + 1.0, b[i].longitude + 1.0))
                return false;
        }
        return true;
    };

    const bool changed = !same(markers, m_markers);
    const bool hadNone = m_markers.isEmpty();
    if (changed)
        m_markers = markers;

    /* Run even when nothing changed: a talker qualifies for a camera move by
     * the passage of time, not by a new message, and the caller ticks this
     * every half second. */
    updateTalkerClock();
    const bool moved = followIfNeeded(hadNone && changed);

    refreshControls();
    if (changed) {
        refreshCard();
        placeCard();
    }
    if (changed || moved)
        update();
}

void MapView::updateTalkerClock()
{
    const qint64 now = m_clock.elapsed();

    QSet<QString> talking;
    for (const Marker &m : m_markers) {
        if (!m.talking)
            continue;
        talking.insert(m.callsign);
        if (!m_talkerSince.contains(m.callsign))
            m_talkerSince.insert(m.callsign, now);
    }

    for (auto it = m_talkerSince.begin(); it != m_talkerSince.end(); ) {
        if (talking.contains(it.key())) ++it;
        else it = m_talkerSince.erase(it);
    }
}

bool MapView::followIfNeeded(bool firstMarkers)
{
    if (m_userMoved || m_markers.isEmpty())
        return false;

    if (firstMarkers) {
        fitToMarkers();
        return true;
    }

    /* Someone has been transmitting for a moment and cannot be seen: go to
     * them. A talker already on screen does not move the view — the marker is
     * right there, and a map that jumps at every over is unusable. */
    const qint64 now = m_clock.elapsed();
    const QRectF visible = QRectF(rect()).adjusted(Theme::space(20), Theme::space(20),
                                                   -Theme::space(20), -Theme::space(20));
    for (const Marker &m : m_markers) {
        if (!m.talking)
            continue;
        if (now - m_talkerSince.value(m.callsign, now) < kTalkerQualifyMs)
            continue;
        if (!visible.contains(toWidget(m.latitude, m.longitude))) {
            fitToMarkers();
            return true;
        }
    }
    return false;
}

void MapView::fitToMarkers()
{
    if (m_markers.isEmpty())
        return;

    /* What to look at, in order of what anyone actually wants to see:
     *
     *   1. whoever has been transmitting for more than a moment;
     *   2. failing that, your own station, at the home radius;
     *   3. failing that, everything.
     *
     * The spans are the macOS app's, converted from its MapKit regions: a
     * single talker gets a 20 km-wide view, home gets whatever the home radius
     * setting says. Fitting everything sounds neutral and is not — one station
     * on holiday in Egypt and one placeholder in Sweden are enough to zoom a
     * Belgian reflector out to a view of the Atlantic. */
    const qint64 now = m_clock.elapsed();

    QVector<Marker> subject;
    for (const Marker &m : m_markers)
        if (m.talking && now - m_talkerSince.value(m.callsign, now) >= kTalkerQualifyMs)
            subject.append(m);

    double minSpanM = kTalkerSpanM;
    double widen    = 1.5;
    bool   everything = false;

    if (subject.isEmpty()) {
        for (const Marker &m : m_markers)
            if (m.self) subject.append(m);
        minSpanM = double(m_homeRadiusKm) * 1000.0;
        widen    = 1.0;
    }
    if (subject.isEmpty()) {
        subject    = m_markers;
        minSpanM   = kTalkerSpanM;
        widen      = 1.4;
        everything = true;
    }

    double north = -90.0, south = 90.0, east = -180.0, west = 180.0;

    if (everything && subject.size() >= 8) {
        /* Where most of the stations are, not where the two furthest ones are.
         * A reflector's list always has a few of those — a holiday in Egypt, a
         * node left at a placeholder in Sweden — and fitting the extremes puts
         * a Belgian network on a map of three continents. The middle 90% is
         * the network; the rest is a pan away. */
        QVector<double> lats, lons;
        lats.reserve(subject.size());
        lons.reserve(subject.size());
        for (const Marker &m : subject) {
            lats.append(m.latitude);
            lons.append(m.longitude);
        }
        std::sort(lats.begin(), lats.end());
        std::sort(lons.begin(), lons.end());

        const int lo = subject.size() / 20;              /* 5th percentile */
        const int hi = subject.size() - 1 - lo;          /* 95th           */
        south = lats[lo]; north = lats[hi];
        west  = lons[lo]; east  = lons[hi];
    } else {
        for (const Marker &m : subject) {
            north = qMax(north, m.latitude);
            south = qMin(south, m.latitude);
            east  = qMax(east,  m.longitude);
            west  = qMin(west,  m.longitude);
        }
    }

    m_latitude  = (north + south) / 2.0;
    m_longitude = (east + west) / 2.0;

    /* The box in metres, widened, and never tighter than the minimum span for
     * this kind of view. */
    const double cosLat = qMax(std::cos(qDegreesToRadians(m_latitude)), 0.1);
    const double spanLatM = qMax((north - south) * kMetresPerDegLat * widen, minSpanM);
    const double spanLonM = qMax((east - west) * kMetresPerDegLat * cosLat * widen, minSpanM);

    m_zoom = qMin(zoomForSpan(spanLatM, height(), m_latitude),
                  zoomForSpan(spanLonM, width(),  m_latitude));
    refreshControls();
    update();
}

void MapView::resetView()
{
    m_userMoved = false;
    fitToMarkers();
    refreshControls();
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

    /* The wait list is rebuilt from what is on screen now, so a tile that
     * scrolled away before its turn came is simply forgotten. */
    m_queue.clear();
    m_queued.clear();

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
                continue;
            }

            /* Not here yet. Rather than a grey hole, blow up the matching
             * quarter of a tile from a zoom level already in the cache — blurry
             * for a moment, which is what every slippy map does, and it means
             * zooming never flashes the background. */
            bool stood_in = false;
            for (int up = 1; up <= 4 && !stood_in; ++up) {
                const int pz = m_zoom - up;
                if (pz < kMinZoom)
                    break;
                const auto pit = m_tiles.constFind(keyOf(pz, nx >> up, ny >> up));
                if (pit == m_tiles.constEnd())
                    continue;
                const int span = 1 << up;                 /* children per side */
                const qreal sub = qreal(kTile) / span;
                p.drawPixmap(dest, *pit, QRectF((nx % span) * sub, (ny % span) * sub, sub, sub));
                stood_in = true;
            }
            if (!stood_in)
                p.fillRect(dest, Theme::wash(pal.foreground, 0.04));

            requestTile(m_zoom, nx, ny);
            missing = true;
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

    /* ---- scale bar, bottom left ----
     * A map with no sense of distance is a picture. The bar is a round number
     * of metres — 1, 2 or 5 times a power of ten — drawn at whatever pixel
     * length that works out to here. */
    {
        const double mpp = metresPerPixel(m_zoom, m_latitude);
        const double maxPx = qMin(double(width()) * 0.3, double(Theme::space(140)));

        static const double kLadder[] = {
            10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000,
            20000, 50000, 100000, 200000, 500000, 1000000, 2000000, 5000000,
        };
        double metres = kLadder[0];
        for (double candidate : kLadder) {
            if (candidate / mpp > maxPx)
                break;
            metres = candidate;
        }

        const int barPx = int(metres / mpp);
        if (barPx >= Theme::space(30)) {
            const QString text = metres >= 1000.0
                ? tr("%1 km").arg(metres / 1000.0, 0, 'g', 3)
                : tr("%1 m").arg(int(metres));

            const QFont f = Theme::font(Theme::Font::Caption);
            const QFontMetrics fm(f);
            const int x = Theme::space(10);
            const int y = height() - Theme::space(12);

            QPen pen(Theme::wash(pal.foreground, 0.75));
            pen.setWidthF(1.4);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawLine(x, y, x + barPx, y);
            p.drawLine(x, y - Theme::space(3), x, y);
            p.drawLine(x + barPx, y - Theme::space(3), x + barPx, y);

            p.setFont(f);
            p.setPen(Theme::wash(pal.foreground, 0.75));
            p.drawText(x, y - Theme::space(5), text);
        }
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
    m_pressAt  = e->position();
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
    placeCard();
    update();
}

void MapView::mouseReleaseEvent(QMouseEvent *e)
{
    const bool wasDrag = m_dragging
        && (e->position() - m_pressAt).manhattanLength() > Theme::space(4);

    m_dragging = false;
    setCursor(Qt::OpenHandCursor);

    if (e->button() != Qt::LeftButton || wasDrag)
        return;

    /* A click, not a pan: open the station under it, or dismiss the card. */
    const int hit = markerAt(e->position());
    if (hit >= 0)
        openCard(hit);
    else
        closeCard();
}

void MapView::keyPressEvent(QKeyEvent *e)
{
    switch (e->key()) {
    case Qt::Key_Escape: closeCard();  break;
    case Qt::Key_Plus:
    case Qt::Key_Equal:  zoomIn();     break;
    case Qt::Key_Minus:  zoomOut();    break;
    case Qt::Key_0:      resetView();  break;
    default:             QWidget::keyPressEvent(e); return;
    }
    e->accept();
}

void MapView::mouseDoubleClickEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton)
        return;
    zoomTo(m_zoom + 1, e->position());
}

void MapView::zoomTo(int zoom, const QPointF &anchor)
{
    const int next = qBound(kMinZoom, zoom, kMaxZoom);
    if (next == m_zoom)
        return;

    /* Keep whatever is under `anchor` under it afterwards. Zooming towards a
     * station and watching it slide off screen is the thing that makes a
     * hand-written map feel hand-written. */
    const double n0 = 1 << m_zoom;
    const QPointF c0 = centreTile();
    const QPointF world0(c0.x() + (anchor.x() - width()  / 2.0) / kTile,
                         c0.y() + (anchor.y() - height() / 2.0) / kTile);

    m_zoom = next;
    const QPointF world1 = world0 * (double(1 << m_zoom) / n0);

    const QPointF c1(world1.x() - (anchor.x() - width()  / 2.0) / kTile,
                     world1.y() - (anchor.y() - height() / 2.0) / kTile);

    m_latitude  = latitudeOf(qBound(0.0, c1.y(), double(1 << m_zoom)), m_zoom);
    m_longitude = c1.x() / double(1 << m_zoom) * 360.0 - 180.0;
    while (m_longitude >  180.0) m_longitude -= 360.0;
    while (m_longitude < -180.0) m_longitude += 360.0;

    /* Zooming is taking over the view, from the buttons as much as from the
     * wheel — otherwise the next marker update would undo it. */
    m_userMoved = true;
    refreshControls();
    placeCard();
    update();
}

void MapView::zoomIn()  { zoomTo(m_zoom + 1, QPointF(width() / 2.0, height() / 2.0)); }
void MapView::zoomOut() { zoomTo(m_zoom - 1, QPointF(width() / 2.0, height() / 2.0)); }

void MapView::refreshControls()
{
    if (!m_zoomIn)
        return;
    m_zoomIn->setEnabled(m_zoom < kMaxZoom);
    m_zoomOut->setEnabled(m_zoom > kMinZoom);
    /* Recentring is only meaningful when there is something to centre on, and
     * only a change when the view has been moved off it. */
    m_recentre->setEnabled(!m_markers.isEmpty() && m_userMoved);
}

void MapView::layOutControls()
{
    if (!m_zoomIn)
        return;

    const int pad = Theme::space(8);
    const int gap = Theme::space(4);

    /* Top right, stacked. Hidden altogether in a pane too short to hold them
     * without covering the map it is meant to control. */
    const QSize a = m_zoomIn->sizeHint();
    const bool room = height() >= a.height() * 3 + gap * 2 + pad * 2;
    for (QToolButton *b : {m_zoomIn, m_zoomOut, m_recentre})
        b->setVisible(room);
    if (!room)
        return;

    int y = pad;
    for (QToolButton *b : {m_zoomIn, m_zoomOut, m_recentre}) {
        const QSize sz = b->sizeHint();
        b->setGeometry(width() - pad - sz.width(), y, sz.width(), sz.height());
        b->raise();
        y += sz.height() + gap;
    }
}

void MapView::wheelEvent(QWheelEvent *e)
{
    zoomTo(m_zoom + (e->angleDelta().y() > 0 ? 1 : -1), e->position());
    e->accept();
}

void MapView::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);

    /* The zoom that fits the markers depends on the size of this widget, and
     * when the pane is first unfolded that size is not yet the one it will be
     * shown at — so the fit computed then is for the wrong pane and leaves the
     * stations off screen. Refit on every resize until the user takes over. */
    if (!m_userMoved)
        fitToMarkers();
    layOutControls();
    placeCard();
    update();
}

void MapView::leaveEvent(QEvent *)
{
    if (m_hovered >= 0) {
        m_hovered = -1;
        setToolTip(QString());
        update();
    }
}
