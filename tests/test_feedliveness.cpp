/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * The reflector feed must notice a connection that has silently died.
 *
 * A NAT or middlebox that drops a TCP flow without a word leaves a WebSocket
 * "connected" for as long as the kernel's keepalive allows — hours, if it is
 * on at all — and 0.1.13 had no check of its own, so the map and activity
 * list could show a frozen world indefinitely. Two local servers here: one
 * sends a snapshot and then goes completely deaf, which is what a half-open
 * connection looks like from this end; the other sends a snapshot and then
 * nothing, but is alive and answers pings. The first must be given up on;
 * the second must not.
 *
 * And the reconnect race: an explicit connect while a reconnect was pending
 * used to be torn down again when the stale timer fired.
 */
#include <cstdio>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QWebSocket>
#include <QWebSocketServer>

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

void spin(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

const QByteArray kSnapshot =
    R"json({"type":"snapshot","nodes":[{"callsign":"ON0ABC","online":true,"lat":50.85,"lon":4.35}],"sessions":[],"active":[]})json";

/* A WebSocket server that completes the handshake, sends one snapshot, and
 * then never reads or writes again: no pong, no close. */
class DeafServer : public QObject {
public:
    DeafServer()
    {
        m_tcp.listen(QHostAddress::LocalHost);
        QObject::connect(&m_tcp, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket *s = m_tcp.nextPendingConnection()) {
                ++connections;
                QObject::connect(s, &QTcpSocket::readyRead, this, [s]() {
                    if (s->property("open").toBool())
                        return;                     /* deaf from here on */
                    QByteArray req = s->property("req").toByteArray() + s->readAll();
                    s->setProperty("req", req);
                    if (!req.contains("\r\n\r\n"))
                        return;
                    QByteArray key;
                    for (const QByteArray &line : req.split('\n'))
                        if (line.toLower().startsWith("sec-websocket-key:"))
                            key = line.mid(line.indexOf(':') + 1).trimmed();
                    const QByteArray accept = QCryptographicHash::hash(
                        key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
                        QCryptographicHash::Sha1).toBase64();
                    s->write("HTTP/1.1 101 Switching Protocols\r\n"
                             "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                             "Sec-WebSocket-Accept: " + accept + "\r\n\r\n");
                    QByteArray frame;
                    frame.append(char(0x81));       /* FIN, text */
                    if (kSnapshot.size() < 126) {   /* the shortest length form, as required */
                        frame.append(char(kSnapshot.size()));
                    } else {
                        frame.append(char(126));
                        frame.append(char((kSnapshot.size() >> 8) & 0xff));
                        frame.append(char(kSnapshot.size() & 0xff));
                    }
                    frame.append(kSnapshot);
                    s->write(frame);
                    s->setProperty("open", true);
                });
            }
        });
    }
    QUrl url() const { return QUrl(QStringLiteral("ws://127.0.0.1:%1/").arg(m_tcp.serverPort())); }

    int connections = 0;

private:
    QTcpServer m_tcp;
};

/* A real WebSocket server: sends a snapshot, then is quiet — but alive, so
 * Qt answers the feed's pings. */
class QuietServer : public QObject {
public:
    QuietServer() : m_ws(QStringLiteral("quiet"), QWebSocketServer::NonSecureMode)
    {
        m_ws.listen(QHostAddress::LocalHost);
        QObject::connect(&m_ws, &QWebSocketServer::newConnection, this, [this]() {
            while (QWebSocket *s = m_ws.nextPendingConnection()) {
                ++connections;
                s->sendTextMessage(QString::fromUtf8(kSnapshot));
            }
        });
    }
    QUrl url() const { return QUrl(QStringLiteral("ws://127.0.0.1:%1/").arg(m_ws.serverPort())); }

    int connections = 0;

private:
    QWebSocketServer m_ws;
};

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    /* ---- a half-open connection ---- */
    {
        DeafServer server;
        ReflectorFeed feed;
        feed.setIntervals(/*reconnect*/ 60000, /*ping*/ 200, /*silence*/ 700);

        int downs = 0;
        QObject::connect(&feed, &ReflectorFeed::availabilityChanged,
                         [&](bool up) { if (!up) ++downs; });

        feed.openUrl(server.url());
        spin(300);
        check(feed.isAvailable(), "deaf: the snapshot arrived and the feed is up");

        spin(1200);
        check(downs >= 1, "deaf: a connection that answers nothing is given up on");
        check(server.connections >= 2, "deaf: ...and a new one is opened at once");
    }

    /* ---- quiet but alive ---- */
    {
        QuietServer server;
        ReflectorFeed feed;
        feed.setIntervals(60000, 200, 700);

        int downs = 0;
        QObject::connect(&feed, &ReflectorFeed::availabilityChanged,
                         [&](bool up) { if (!up) ++downs; });

        feed.openUrl(server.url());
        spin(2500);                          /* over three silence periods */
        check(feed.isAvailable() && downs == 0,
              "quiet: a live connection with nothing to say is kept (pongs count)");
        check(server.connections == 1, "quiet: ...and not reopened");
    }

    /* ---- the reconnect race ---- */
    {
        QuietServer server;
        QTcpServer closed;
        closed.listen(QHostAddress::LocalHost);
        const QUrl nowhere(QStringLiteral("ws://127.0.0.1:%1/").arg(closed.serverPort()));
        closed.close();                      /* a port nothing listens on */

        ReflectorFeed feed;
        feed.setIntervals(/*reconnect*/ 500, 60000, 60000);

        feed.openUrl(nowhere);               /* refused: a reconnect is scheduled */
        spin(200);
        feed.openUrl(server.url());          /* an explicit connect, meanwhile */
        spin(1500);                          /* well past the stale reconnect */
        check(feed.isAvailable(), "race: the explicit connection is up");
        check(server.connections == 1,
              "race: a stale reconnect timer did not tear it down and reconnect");
    }

    std::printf("\n%d/%d passed\n", g_run - g_fail, g_run);
    return g_fail ? 1 : 0;
}
