/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * The enhanced reflector's portal feed, over a WebSocket.
 *
 * WHAT "ENHANCED" MEANS
 * --------------------
 * A plain SvxLink reflector speaks only the v3 TCP/UDP protocol the core
 * implements: it tells you who is talking on the talkgroups you monitor, from
 * the moment you connect. It does not know where anybody is, and it has no
 * memory — a station that talked two minutes before you connected never
 * happened.
 *
 * Some reflectors run a portal alongside (SVXReflectorFeed/svx_talker_ws.py),
 * which publishes the same traffic as JSON over a WebSocket, with node
 * coordinates and a rolling 24 hours of sessions. That is the whole difference,
 * and it is what makes both the map and a real history possible.
 *
 * DISCOVERY IS A PROBE, NOT A RECORD
 * ----------------------------------
 * There is no SRV record and no configuration for this. The URL is derived
 * from the reflector host — be.svx.link becomes wss://reflector.be.svx.link/ —
 * and the client simply connects. A reflector that offers the feed sends a
 * "snapshot" message immediately; if none arrives within five seconds, this is
 * a plain reflector, the feed is marked unavailable and the window falls back
 * to what the core alone can tell it. Retry every fifteen seconds after that,
 * without backoff: the point is to notice when a portal comes back, and one
 * connection attempt per fifteen seconds is not a load on anything.
 *
 * SCHEMA DRIFT IS THE NORM
 * ------------------------
 * Field types vary between portal versions — a talkgroup is a number here and
 * a string there — so every accessor is lenient. Two traps are load-bearing
 * and both have tests:
 *
 *   - node_upsert is PARTIAL. A message carrying only isTalker must not blank
 *     the coordinates, or every marker on the map flickers twice a second. A
 *     JSON null counts as absent, not as a value.
 *   - portable and mobile callsigns (anything with a '/') are never in the
 *     nodes list. Their coordinates hide in session.node.qth, where the key is
 *     "long" and the values are strings — while a listed node spells the same
 *     thing "lon" as a number.
 *
 * The parsing is separated from the socket so tests/test_reflectorfeed.cpp can
 * drive handleJson() with recorded payloads and no network at all.
 */
#ifndef SVXCONNECT_OMARCHY_REFLECTORFEED_H
#define SVXCONNECT_OMARCHY_REFLECTORFEED_H

#include <QHash>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVector>

class QTimer;
class QWebSocket;
class QJsonObject;

class ReflectorFeed : public QObject {
    Q_OBJECT

public:
    /* A station the portal knows about. `hasPos` rather than a sentinel:
     * 0,0 is a real place in the Gulf of Guinea and the map would draw it. */
    struct Node {
        QString      callsign;
        bool         online    = false;
        bool         isTalker  = false;
        int          tg        = 0;
        QVector<int> monitoredTgs;
        QString      location;
        bool         hasPos    = false;
        double       latitude  = 0.0;
        double       longitude = 0.0;
    };

    /* One over. `active` means it is still going; `endMs` is 0 then. */
    struct Session {
        QString callsign;
        qint64  startMs = 0;
        qint64  endMs   = 0;
        bool    active  = false;
        QString location;
        int     tg      = 0;

        qint64 lastActivityMs() const { return (!active && endMs > 0) ? endMs : startMs; }
    };

    explicit ReflectorFeed(QObject *parent = nullptr);
    ~ReflectorFeed() override;

    /* The reflector host from the configuration — "be.svx.link",
     * "be.svx.link:5300", or a comma-separated fallback list. Connecting is
     * automatic; passing an empty string stops the feed. */
    void setReflector(const QString &host);

    /* Switch the whole feature off: no socket, no probe, and isAvailable()
     * stays false so the window uses the core's own history. */
    void setEnabled(bool on);
    bool isEnabled() const { return m_enabled; }

    bool                      isAvailable() const { return m_available; }
    const QHash<QString, Node> &nodes()    const { return m_nodes; }
    const QVector<Session>     &sessions() const { return m_sessions; }

    /* ---- pure, and unit-tested ---- */

    /* wss://reflector.<bare host>/ — the comma list and the port are stripped,
     * and a host that already starts with "reflector." is left alone. An empty
     * or unusable host yields an invalid QUrl. */
    static QUrl feedUrl(const QString &host);

    /* Apply one message. Returns true when it was a snapshot, which is what
     * makes the feed "available". */
    bool handleJson(const QByteArray &json);

    /* Whether the feature is wanted at all, in the desktop settings. On by
     * default: a reflector that does not offer a feed costs one refused
     * connection every fifteen seconds and nothing else. */
    static bool enabledSetting();
    static void setEnabledSetting(bool on);

signals:
    /* The feed came up or went away. The window switches its history section
     * on this, so it fires only on a real change. */
    void availabilityChanged(bool available);

    /* Nodes or sessions changed. Fired per message: a portal under load sends
     * dozens a second, so consumers repaint on their own tick rather than on
     * this. */
    void changed();

private:
    void connectNow();
    void teardown();
    void markUnavailable();
    void scheduleReconnect();

    void applySnapshot(const QJsonObject &root);
    void sortAndCapSessions();
    void upsertSynthetic(const QJsonObject &sessionObj);

    static bool    hasValue(const QJsonObject &o, const char *key);
    static Node    parseNode(const QJsonObject &o, bool *ok);
    static Session parseSession(const QJsonObject &o, bool *ok);
    static Node    mergeNode(const Node &existing, const Node &incoming, const QJsonObject &raw);
    static bool    syntheticNode(const QJsonObject &sessionObj, Node *out);

    QWebSocket *m_socket    = nullptr;
    QTimer     *m_snapshot  = nullptr;   /* 5 s: is this an enhanced reflector? */
    QTimer     *m_reconnect = nullptr;   /* 15 s, flat                          */

    QString m_host;
    QUrl    m_url;
    bool    m_enabled   = true;
    bool    m_available = false;
    bool    m_gotSnapshot = false;

    QHash<QString, Node> m_nodes;
    QVector<Session>     m_sessions;
};

#endif
