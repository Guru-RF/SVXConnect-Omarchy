/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 */
#include "net/reflectorfeed.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSettings>
#include <QTimer>
#include <QWebSocket>
#include <algorithm>

#include "core/svxcore.h"   /* log_info / log_warn */

namespace {

constexpr char kEnabledKey[]  = "feed/enhanced";

constexpr int kSessionLimit   = 60;
constexpr int kSnapshotMs     = 5000;
constexpr int kReconnectMs    = 15000;

/* Every accessor below is lenient on purpose: portal versions disagree about
 * whether a talkgroup is a number or a string, and a client that insists on
 * one of them shows an empty map against the other. */

bool numeric(const QJsonValue &v, double *out)
{
    if (v.isDouble()) { *out = v.toDouble(); return true; }
    if (v.isString()) {
        bool ok = false;
        const double d = v.toString().toDouble(&ok);
        if (ok) { *out = d; return true; }
    }
    return false;
}

double numberOr(const QJsonObject &o, const char *key, double fallback)
{
    double d = 0.0;
    return numeric(o.value(QLatin1String(key)), &d) ? d : fallback;
}

int intOf(const QJsonObject &o, const char *key)
{
    return int(numberOr(o, key, 0.0));
}

qint64 longOf(const QJsonObject &o, const char *key)
{
    return qint64(numberOr(o, key, 0.0));
}

QString stringOf(const QJsonObject &o, const char *key)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isString() ? v.toString() : QString();
}

bool boolOf(const QJsonObject &o, const char *key)
{
    return o.value(QLatin1String(key)).toBool(false);
}

QJsonObject objectOf(const QJsonObject &o, const char *key)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isObject() ? v.toObject() : QJsonObject();
}

/* A position from the first of several spellings that carries one. The portal
 * writes "lon" for a listed node and "long" for a session's qth, and the
 * session's values are strings. */
bool positionFrom(const QJsonObject &o, const char *latKey, const char *lonKey,
                  double *lat, double *lon)
{
    if (!numeric(o.value(QLatin1String(latKey)), lat)
        || !numeric(o.value(QLatin1String(lonKey)), lon))
        return false;

    /* Exactly 0,0 is not a position, it is an unconfigured node — the portal
     * has several. Drawing them puts a marker in the Gulf of Guinea and, worse,
     * stretches the map's automatic zoom across an ocean to include it. */
    return *lat != 0.0 || *lon != 0.0;
}

} // namespace

ReflectorFeed::ReflectorFeed(QObject *parent) : QObject(parent)
{
    m_snapshot = new QTimer(this);
    m_snapshot->setSingleShot(true);
    m_snapshot->setInterval(kSnapshotMs);
    connect(m_snapshot, &QTimer::timeout, this, [this]() {
        if (m_gotSnapshot)
            return;
        /* Silence is the answer: this is a plain reflector. */
        log_info("reflector feed: no snapshot from %s within 5s — plain reflector",
                 qPrintable(m_url.toString()));
        markUnavailable();
        teardown();
        scheduleReconnect();
    });

    m_reconnect = new QTimer(this);
    m_reconnect->setSingleShot(true);
    m_reconnect->setInterval(kReconnectMs);
    connect(m_reconnect, &QTimer::timeout, this, [this]() { connectNow(); });
}

ReflectorFeed::~ReflectorFeed()
{
    teardown();
}

bool ReflectorFeed::enabledSetting()
{
    return QSettings().value(QLatin1String(kEnabledKey), true).toBool();
}

void ReflectorFeed::setEnabledSetting(bool on)
{
    QSettings().setValue(QLatin1String(kEnabledKey), on);
}

/* ------------------------------------------------------------------- URL */

QUrl ReflectorFeed::feedUrl(const QString &host)
{
    const QString trimmed = host.trimmed();
    if (trimmed.isEmpty())
        return QUrl();

    /* The configured host may be a comma-separated fallback list, and may
     * carry the reflector's TCP port. Neither belongs in the portal URL. */
    QString bare = trimmed.section(QLatin1Char(','), 0, 0).section(QLatin1Char(':'), 0, 0).trimmed();
    if (bare.isEmpty())
        return QUrl();

    if (!bare.startsWith(QLatin1String("reflector."), Qt::CaseInsensitive))
        bare = QStringLiteral("reflector.") + bare;

    const QUrl url(QStringLiteral("wss://%1/").arg(bare));
    return url.isValid() ? url : QUrl();
}

/* -------------------------------------------------------------- lifecycle */

void ReflectorFeed::setReflector(const QString &host)
{
    const QUrl next = feedUrl(host);
    if (next == m_url && m_host == host)
        return;

    m_host = host;
    m_url  = next;

    teardown();
    markUnavailable();
    connectNow();
}

void ReflectorFeed::setEnabled(bool on)
{
    if (m_enabled == on)
        return;
    m_enabled = on;

    if (!on) {
        teardown();
        markUnavailable();
        log_info("reflector feed: disabled — using the reflector's own history only");
        return;
    }
    connectNow();
}

void ReflectorFeed::connectNow()
{
    if (!m_enabled || !m_url.isValid())
        return;

    teardown();

    m_gotSnapshot = false;
    m_socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);

    connect(m_socket, &QWebSocket::textMessageReceived, this, [this](const QString &text) {
        if (handleJson(text.toUtf8()))
            m_snapshot->stop();
    });
    /* Some portals send the same JSON as a binary frame. */
    connect(m_socket, &QWebSocket::binaryMessageReceived, this, [this](const QByteArray &data) {
        if (handleJson(data))
            m_snapshot->stop();
    });

    connect(m_socket, &QWebSocket::disconnected, this, [this]() {
        if (m_available)
            log_info("reflector feed: connection lost");
        markUnavailable();
        teardown();
        scheduleReconnect();
    });

    connect(m_socket, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        /* Logged at debug volume: a plain reflector produces one of these
         * every fifteen seconds, and it is not an error for the operator. */
        markUnavailable();
        teardown();
        scheduleReconnect();
    });

    log_info("reflector feed: probing %s", qPrintable(m_url.toString()));
    m_socket->open(m_url);
    m_snapshot->start();
}

void ReflectorFeed::teardown()
{
    m_snapshot->stop();
    if (!m_socket)
        return;

    /* Disconnect first: close() would otherwise re-enter through
     * disconnected() and schedule a second reconnect. */
    m_socket->disconnect(this);
    m_socket->close();
    m_socket->deleteLater();
    m_socket = nullptr;
}

void ReflectorFeed::markUnavailable()
{
    const bool was = m_available;
    const bool had = !m_nodes.isEmpty() || !m_sessions.isEmpty();

    m_available = false;
    m_nodes.clear();
    m_sessions.clear();

    if (was)
        emit availabilityChanged(false);
    if (was || had)
        emit changed();
}

void ReflectorFeed::scheduleReconnect()
{
    if (!m_enabled || !m_url.isValid())
        return;
    if (!m_reconnect->isActive())
        m_reconnect->start();
}

/* ---------------------------------------------------------------- parsing */

bool ReflectorFeed::hasValue(const QJsonObject &o, const char *key)
{
    const auto it = o.constFind(QLatin1String(key));
    return it != o.constEnd() && !it->isNull();
}

ReflectorFeed::Node ReflectorFeed::parseNode(const QJsonObject &o, bool *ok)
{
    Node n;
    n.callsign = stringOf(o, "callsign").toUpper();
    *ok = !n.callsign.isEmpty();
    if (!*ok)
        return n;

    n.online   = boolOf(o, "online");
    n.isTalker = boolOf(o, "isTalker");
    n.tg       = intOf(o, "tg");
    n.location = stringOf(o, "location");

    const QJsonValue mon = o.value(QLatin1String("monitoredTGs"));
    if (mon.isArray()) {
        for (const QJsonValue &v : mon.toArray()) {
            double d = 0.0;
            if (numeric(v, &d))
                n.monitoredTgs.append(int(d));
        }
    }

    n.hasPos = positionFrom(o, "lat", "lon", &n.latitude, &n.longitude);
    return n;
}

ReflectorFeed::Session ReflectorFeed::parseSession(const QJsonObject &o, bool *ok)
{
    Session s;
    s.callsign = stringOf(o, "callsign").toUpper();
    *ok = !s.callsign.isEmpty();
    if (!*ok)
        return s;

    s.startMs = longOf(o, "start_ms");

    const bool haveEnd = hasValue(o, "end_ms");
    const qint64 endMs = haveEnd ? longOf(o, "end_ms") : 0;

    /* An explicit "active" wins; without one, no end time means still talking. */
    s.active = hasValue(o, "active") ? boolOf(o, "active") : !haveEnd;
    s.endMs  = s.active ? 0 : endMs;

    const QJsonObject node = objectOf(o, "node");
    if (!node.isEmpty()) {
        s.location = stringOf(node, "nodeLocation");
        s.tg       = intOf(node, "tg");
    }
    if (s.location.isEmpty())
        s.location = stringOf(o, "location");
    if (s.tg == 0)
        s.tg = intOf(o, "tg");

    return s;
}

bool ReflectorFeed::syntheticNode(const QJsonObject &s, Node *out)
{
    const QString cs = stringOf(s, "callsign").toUpper();
    /* Only the suffixed callsigns: everything else is in the nodes list, with
     * better data than a session row carries. */
    if (cs.isEmpty() || !cs.contains(QLatin1Char('/')))
        return false;

    const QJsonObject node = objectOf(s, "node");
    const QJsonObject qth  = objectOf(node, "qth");

    Node n;
    n.callsign = cs;
    n.hasPos = positionFrom(qth,  "lat", "long", &n.latitude, &n.longitude)
            || positionFrom(qth,  "lat", "lon",  &n.latitude, &n.longitude)
            || positionFrom(node, "lat", "lon",  &n.latitude, &n.longitude)
            || positionFrom(s,    "lat", "lon",  &n.latitude, &n.longitude);
    if (!n.hasPos)
        return false;

    n.online   = false;
    n.isTalker = boolOf(s, "active");
    n.tg       = node.contains(QLatin1String("tg")) ? intOf(node, "tg") : intOf(s, "tg");
    n.location = stringOf(node, "nodeLocation");
    if (n.location.isEmpty())
        n.location = stringOf(s, "location");

    *out = n;
    return true;
}

ReflectorFeed::Node ReflectorFeed::mergeNode(const Node &existing, const Node &incoming,
                                             const QJsonObject &raw)
{
    /* A node_upsert carries only what changed. Anything the message does not
     * mention keeps its old value — and a coordinate is never blanked, which
     * is what stops the markers flickering. */
    Node n = incoming;
    n.online       = hasValue(raw, "online")       ? incoming.online       : existing.online;
    n.isTalker     = hasValue(raw, "isTalker")     ? incoming.isTalker     : existing.isTalker;
    n.tg           = hasValue(raw, "tg")           ? incoming.tg           : existing.tg;
    n.monitoredTgs = hasValue(raw, "monitoredTGs") ? incoming.monitoredTgs : existing.monitoredTgs;
    n.location     = hasValue(raw, "location")     ? incoming.location     : existing.location;

    if (!incoming.hasPos && existing.hasPos) {
        n.hasPos    = true;
        n.latitude  = existing.latitude;
        n.longitude = existing.longitude;
    }
    return n;
}

void ReflectorFeed::upsertSynthetic(const QJsonObject &sessionObj)
{
    Node synth;
    if (!syntheticNode(sessionObj, &synth))
        return;

    const auto it = m_nodes.constFind(synth.callsign);
    if (it != m_nodes.constEnd()) {
        Node merged = synth;
        merged.online       = it->online;
        merged.monitoredTgs = it->monitoredTgs;
        if (merged.tg == 0)          merged.tg = it->tg;
        if (merged.location.isEmpty()) merged.location = it->location;
        m_nodes.insert(synth.callsign, merged);
    } else {
        m_nodes.insert(synth.callsign, synth);
    }
}

void ReflectorFeed::sortAndCapSessions()
{
    std::stable_sort(m_sessions.begin(), m_sessions.end(),
                     [](const Session &a, const Session &b) {
                         if (a.active != b.active)
                             return a.active;                          /* live first */
                         return a.lastActivityMs() > b.lastActivityMs();
                     });
    if (m_sessions.size() > kSessionLimit)
        m_sessions.resize(kSessionLimit);
}

void ReflectorFeed::applySnapshot(const QJsonObject &root)
{
    m_nodes.clear();
    m_sessions.clear();

    const QJsonValue nodesVal = root.value(QLatin1String("nodes"));
    if (nodesVal.isArray()) {
        for (const QJsonValue &v : nodesVal.toArray()) {
            bool ok = false;
            const Node n = parseNode(v.toObject(), &ok);
            if (ok)
                m_nodes.insert(n.callsign, n);
        }
    }

    /* The portal sends both lists: "sessions" is the rolling 24 hours and
     * already contains the live ones, "active" is only the live ones. Taking
     * both and keeping the most recent per callsign is what the other clients
     * do, and it survives a portal that fills in only one of them. */
    QHash<QString, Session> byCall;
    for (const char *key : {"sessions", "active"}) {
        const QJsonValue arr = root.value(QLatin1String(key));
        if (!arr.isArray())
            continue;
        for (const QJsonValue &v : arr.toArray()) {
            const QJsonObject o = v.toObject();

            /* Portables and mobiles exist only here, never in the nodes list. */
            if (!m_nodes.contains(stringOf(o, "callsign").toUpper()))
                upsertSynthetic(o);

            bool ok = false;
            const Session s = parseSession(o, &ok);
            if (!ok)
                continue;
            const auto prev = byCall.constFind(s.callsign);
            if (prev == byCall.constEnd() || s.lastActivityMs() > prev->lastActivityMs())
                byCall.insert(s.callsign, s);
        }
    }

    m_sessions = QVector<Session>(byCall.cbegin(), byCall.cend());
    sortAndCapSessions();

    const bool was = m_available;
    m_available = true;
    m_gotSnapshot = true;

    if (!was) {
        log_info("reflector feed: enhanced reflector — %d nodes, %d sessions in the last 24h",
                 int(m_nodes.size()), int(m_sessions.size()));
        emit availabilityChanged(true);
    }
    emit changed();
}

bool ReflectorFeed::handleJson(const QByteArray &json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject())
        return false;

    const QJsonObject root = doc.object();
    const QString type = stringOf(root, "type");

    if (type == QLatin1String("snapshot")) {
        applySnapshot(root);
        return true;
    }

    if (type == QLatin1String("node_upsert")) {
        const QJsonObject raw = objectOf(root, "node");
        bool ok = false;
        const Node incoming = parseNode(raw, &ok);
        if (!ok)
            return false;

        const auto it = m_nodes.constFind(incoming.callsign);
        m_nodes.insert(incoming.callsign,
                       it == m_nodes.constEnd() ? incoming : mergeNode(*it, incoming, raw));
        emit changed();
        return false;
    }

    if (type == QLatin1String("node_remove")) {
        const QString cs = stringOf(root, "callsign").toUpper();
        /* The node is gone, so its marker must go; its past overs are history
         * and stay in the session list. */
        if (!cs.isEmpty() && m_nodes.remove(cs) > 0)
            emit changed();
        return false;
    }

    if (type == QLatin1String("talk_start") || type == QLatin1String("talk_stop")) {
        const QJsonObject sessionObj = objectOf(root, "session");
        bool ok = false;
        const Session s = parseSession(sessionObj, &ok);
        if (!ok)
            return false;

        /* One row per station: the start is replaced by the stop, rather than
         * the same over appearing twice. */
        for (int i = m_sessions.size() - 1; i >= 0; --i)
            if (m_sessions[i].callsign == s.callsign)
                m_sessions.remove(i);
        m_sessions.append(s);
        sortAndCapSessions();

        upsertSynthetic(sessionObj);

        emit changed();
        return false;
    }

    /* Anything else — keepalives, types added by a newer portal — is ignored
     * on purpose, and must not count as a snapshot. */
    return false;
}
