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
 * THE JSON IS ALSO THE OPERATOR'S
 * ------------------------------
 * Every SVXConnect client treats the talkgroup info as a JSON document the
 * operator can see and edit, not as an opaque cache: on a plain reflector with
 * no portal it is the ONLY way to get names at all, and on an enhanced one it
 * is how a name the sysop got wrong is corrected. So the raw text is exposed,
 * setManualJson() accepts a replacement, and "update automatically" can be
 * switched off so a hand-edited document is not overwritten the next morning.
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
class QTimer;

class PortalInfo : public QObject {
    Q_OBJECT

public:
    explicit PortalInfo(QObject *parent = nullptr);

    /* The reflector host from the configuration. Setting it loads whatever was
     * cached for it and, if that is a day old or missing, fetches. */
    void setReflector(const QString &host);

    /* Fetch from this base URL instead of the one derived from a host, and
     * load what is cached for it; nothing is fetched until refresh(). For the
     * tests, which serve the two files from 127.0.0.1. */
    void setPortalUrl(const QUrl &base);

    /* Fetch now, whatever the cache says. */
    void refresh();

    /* No portal, no fetching: the enhanced-reflector switch in Preferences
     * turns this off too. Cached and hand-pasted names still load — that is
     * exactly the plain-reflector case they exist for. */
    void setNetworkEnabled(bool on);

    /* The documents as stored, for the editors in Preferences. */
    QByteArray rawTalkgroups() const { return m_rawTalkgroups; }
    QByteArray rawCallsigns()  const { return m_rawCallsigns; }

    /* Replace the documents with the operator's own. Empty text clears one.
     * Returns false, with the reason, when either is not a JSON object —
     * checked BEFORE anything is stored, so a typo cannot blank the names that
     * were working. */
    bool setManualJson(const QByteArray &talkgroups, const QByteArray &callsigns,
                       QString *error);

    /* Whether the portal may overwrite the documents by itself. On by default;
     * off is how a hand-edited document survives the daily refresh. */
    static bool autoUpdateSetting();
    static void setAutoUpdateSetting(bool on);

    bool    isFetching()  const { return m_pending > 0; }
    qint64  lastFetched() const;                 /* epoch seconds, 0 = never */
    QString lastError()   const { return m_lastError; }

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

    /* A fetch started, finished or failed: what Preferences shows beside its
     * "Update now" button. */
    void statusChanged();

private:
    void load();
    void fetch(const QString &file);
    void fileDone(quint64 round, bool isTalkgroups, bool ok);

    QNetworkAccessManager *m_net = nullptr;

    QString m_host;
    QUrl    m_base;

    QHash<quint32, QString> m_talkgroups;
    QHash<QString, QString> m_callsigns;
    QByteArray m_rawTalkgroups;
    QByteArray m_rawCallsigns;

    bool    m_network = true;
    int     m_pending = 0;
    quint64 m_round = 0;         /* bumped by refresh(): which replies count */
    int     m_roundLeft = 0;
    bool    m_roundTgOk = false;
    QString m_lastError;
};

#endif
