/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * The reflector portal's two metadata files.
 *
 * An enhanced reflector serves, beside its WebSocket feed, a pair of flat JSON
 * objects over HTTPS:
 *
 *   talkgroups.json   {"8": "70cm Repeaters", "1745": "145.450 ON0ORA-S"}
 *   callsigns.json    {"ON0ORA": "TX:438.8000 RX:431.2000\nCTCSS: 131.8\nSysop: …"}
 *
 * Neither is traffic, so neither belongs in reflectorfeed.h: this is the
 * reflector's paperwork. It answers the two questions the feed cannot — what a
 * talkgroup is called, and what a repeater's frequencies are — which is what
 * makes "load the talkgroups from the reflector" and a station card worth
 * having.
 *
 * WHAT IT IS NOT
 * --------------
 * It is not a directory. On be.svx.link, callsigns.json covers 23 repeaters out
 * of 146 nodes, and talkgroups.json lists 18 talkgroups while the nodes between
 * them monitor several more. Both are a decoration on top of the feed, never a
 * substitute for it, and every consumer has to read well with them absent.
 *
 * CACHING
 * -------
 * The raw bodies are kept in QSettings and refreshed once a day, the way the
 * macOS app does it. Keeping the text rather than a parsed model is deliberate:
 * it survives a portal that adds a field, and it means a cold start shows the
 * names it showed yesterday instead of nothing.
 */
#ifndef SVXCONNECT_OMARCHY_PORTALINFO_H
#define SVXCONNECT_OMARCHY_PORTALINFO_H

#include <QHash>
#include <QObject>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;

class PortalInfo : public QObject {
    Q_OBJECT

public:
    explicit PortalInfo(QObject *parent = nullptr);

    /* The reflector host from the configuration. Setting it loads whatever was
     * cached for it and, if that is a day old or missing, fetches. */
    void setReflector(const QString &host);

    /* Fetch now, whatever the cache says. */
    void refresh();

    bool hasTalkgroups() const { return !m_talkgroups.isEmpty(); }

    /* "70cm Repeaters" for 8, or an empty string. */
    QString talkgroupName(quint32 tg) const { return m_talkgroups.value(tg); }
    QHash<quint32, QString> talkgroups() const { return m_talkgroups; }

    /* The portal's free text for a station — several lines, or empty. The key
     * is matched upper-cased. */
    QString callsignInfo(const QString &callsign) const;

    /* ---- pure, and unit-tested ---- */

    /* https://portal.<bare host>/ — the comma list, the port and a leading
     * "reflector." are all stripped, so be.svx.link and reflector.be.svx.link
     * both give portal.be.svx.link. */
    static QUrl portalBaseUrl(const QString &host);

    static QHash<quint32, QString> parseTalkgroups(const QByteArray &json);
    static QHash<QString, QString> parseCallsigns(const QByteArray &json);

signals:
    /* New metadata is in hand — or the cache was loaded for a new reflector. */
    void changed();

private:
    void load();
    void fetch(const QString &file);

    QNetworkAccessManager *m_net = nullptr;

    QString m_host;
    QUrl    m_base;

    QHash<quint32, QString> m_talkgroups;
    QHash<QString, QString> m_callsigns;
};

#endif
