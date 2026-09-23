/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * When is the portal cache "fresh", and whose is it?
 *
 * 0.1.13 stamped the cache as fetched — and as belonging to the current host
 * — on the FIRST file that came back. So a talkgroups.json that failed next
 * to a callsigns.json that worked was not retried for a day; and pointed at a
 * new reflector, the new host's callsigns were filed next to the OLD host's
 * talkgroup names, which load() then served as the new reflector's.
 *
 * Both files are served here from a local HTTP server, under two host names
 * for the same machine (127.0.0.1 and localhost), with a status per file.
 */
#include <cstdio>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

#include "net/portalinfo.h"

namespace {

int g_fail;
int g_run;

void check(bool ok, const char *what)
{
    ++g_run;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

/* path -> (status, body) */
QHash<QByteArray, QPair<int, QByteArray>> g_routes;

class Http : public QObject {
public:
    Http()
    {
        m_tcp.listen(QHostAddress::Any);
        QObject::connect(&m_tcp, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket *s = m_tcp.nextPendingConnection()) {
                QObject::connect(s, &QTcpSocket::readyRead, s, [s]() {
                    const QByteArray req = s->readAll();
                    const QByteArray path = req.split(' ').value(1);
                    const auto route = g_routes.value(path, qMakePair(404, QByteArray()));
                    s->write("HTTP/1.1 " + QByteArray::number(route.first) + " X\r\n"
                             "Content-Type: application/json\r\n"
                             "Content-Length: " + QByteArray::number(route.second.size()) + "\r\n"
                             "Connection: close\r\n\r\n" + route.second);
                    s->disconnectFromHost();
                });
            }
        });
    }
    QUrl base(const char *host) const
    {
        return QUrl(QStringLiteral("http://%1:%2/").arg(QLatin1String(host)).arg(m_tcp.serverPort()));
    }

private:
    QTcpServer m_tcp;
};

void fetchAll(PortalInfo &p)
{
    p.refresh();
    QElapsedTimer t;
    t.start();
    while (p.isFetching() && t.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

const QByteArray kTalkgroups = R"json({"8":"70cm Repeaters","2061":"Brussels"})json";
const QByteArray kCallsignsA = R"json({"ON0ABC":"Repeater A"})json";
const QByteArray kCallsignsB = R"json({"ON0XYZ":"Repeater B"})json";

} // namespace

int main(int argc, char **argv)
{
    QTemporaryDir sandbox;
    qputenv("XDG_CONFIG_HOME", sandbox.path().toUtf8());
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SVXConnect-test"));
    QCoreApplication::setApplicationName(QStringLiteral("portalfetch"));

    Http http;

    /* ---- reflector A: both files served ---- */
    {
        g_routes = {{"/talkgroups.json", {200, kTalkgroups}},
                    {"/callsigns.json",  {200, kCallsignsA}}};
        PortalInfo p;
        p.setPortalUrl(http.base("127.0.0.1"));
        fetchAll(p);
        check(p.lastFetched() > 0, "both files in: the cache is stamped fresh");
        check(p.talkgroupName(8) == QLatin1String("70cm Repeaters"), "talkgroup names loaded");
    }
    /* Back-date A's stamp, so "stamped again" cannot hide inside the same
     * second as the first. */
    QSettings().setValue(QStringLiteral("portal/fetched"), 1000);
    const qint64 stampA = PortalInfo().lastFetched();

    /* ---- reflector B: talkgroups.json fails, callsigns.json works ---- */
    {
        g_routes = {{"/talkgroups.json", {500, QByteArray()}},
                    {"/callsigns.json",  {200, kCallsignsB}}};
        PortalInfo p;
        p.setPortalUrl(http.base("localhost"));
        fetchAll(p);
        check(p.lastFetched() == stampA,
              "talkgroups failed: the cache is NOT stamped fresh (it will be retried)");
        check(p.callsignInfo(QStringLiteral("ON0XYZ")) == QLatin1String("Repeater B"),
              "the callsigns that did arrive are used");
    }

    /* ---- next start, still on B: whose talkgroups? ---- */
    {
        PortalInfo p;
        p.setPortalUrl(http.base("localhost"));
        check(!p.hasTalkgroups(),
              "B does not inherit A's talkgroup names from a half-finished fetch");
        check(p.callsignInfo(QStringLiteral("ON0XYZ")) == QLatin1String("Repeater B"),
              "B's own callsigns are loaded");
    }

    /* ---- and A still has its own ---- */
    {
        PortalInfo p;
        p.setPortalUrl(http.base("127.0.0.1"));
        check(p.talkgroupName(2061) == QLatin1String("Brussels"), "A's talkgroup names survive");
        check(p.callsignInfo(QStringLiteral("ON0XYZ")).isEmpty(), "A does not get B's callsigns");
    }

    std::printf("\n%d/%d passed\n", g_run - g_fail, g_run);
    return g_fail ? 1 : 0;
}
