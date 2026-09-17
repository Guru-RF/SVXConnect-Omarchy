/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * Finding your station position without reading coordinates off a map.
 *
 * The reflector plots you from latitude and longitude, and the grid square the
 * status bar shows is derived from them — so they have to be right, and typing
 * them by hand is both tedious and the easiest thing in the configuration to
 * get subtly wrong. This dialog offers the two ways of not doing that:
 *
 *   - type an address and look it up. OpenStreetMap's Nominatim is the only
 *     free geocoder that resolves a house number, which is the level of detail
 *     an antenna position deserves;
 *   - detect it. Omarchy already stores a location for its weather panel, and
 *     when that has coordinates they are the best answer available locally and
 *     cost no request at all. Failing that, the IP address gives a city-centre
 *     guess, which is honestly labelled as such.
 *
 * Nothing is sent anywhere until the operator presses a button, and each
 * button says where it is about to look.
 *
 * The URL building and the JSON parsing are static and pure, so
 * tests/test_locationlookup.cpp can check them against recorded payloads
 * without a network.
 */
#ifndef SVXCONNECT_OMARCHY_LOCATIONDIALOG_H
#define SVXCONNECT_OMARCHY_LOCATIONDIALOG_H

#include <QDialog>
#include <QList>
#include <QString>
#include <QUrl>

class QLabel;
class QLineEdit;
class QListWidget;
class QNetworkAccessManager;
class QNetworkReply;
class QPushButton;

class LocationDialog : public QDialog {
    Q_OBJECT

public:
    /* One candidate position. `name` is what goes in the `location` setting —
     * a town, the way the rest of the configuration spells it — and `detail`
     * is the full address, for choosing between two streets of the same name. */
    struct Place {
        QString name;
        QString detail;
        double  latitude  = 0.0;
        double  longitude = 0.0;
        bool    isValid() const { return latitude != 0.0 || longitude != 0.0; }
    };

    explicit LocationDialog(QWidget *parent = nullptr);

    /* Valid once the dialog was accepted. */
    Place chosen() const { return m_chosen; }

    /* ---- pure, and unit-tested ---- */

    static QUrl  nominatimUrl(const QString &street, const QString &postcode,
                              const QString &city, const QString &country);
    static QList<Place> parseNominatim(const QByteArray &json);

    /* ~/.local/state/omarchy/settings/weather.json, which omarchy-weather-location
     * writes. Coordinates are optional there: a name alone is not a position. */
    static QString omarchyLocationPath();
    static Place   parseOmarchyLocation(const QByteArray &json);

    static QUrl  ipLookupUrl();
    static Place parseIpLookup(const QByteArray &json);

private slots:
    void search();
    void detect();
    void onSelectionChanged();

private:
    void setBusy(bool busy, const QString &what = QString());
    void showPlaces(const QList<Place> &places);
    void setPlaceholder(const QString &text);
    void fail(const QString &why);
    QNetworkReply *get(const QUrl &url);

    QLineEdit   *m_street   = nullptr;
    QLineEdit   *m_postcode = nullptr;
    QLineEdit   *m_city     = nullptr;
    QLineEdit   *m_country  = nullptr;
    QPushButton *m_search   = nullptr;
    QPushButton *m_detect   = nullptr;
    QListWidget *m_results  = nullptr;
    QLabel      *m_status   = nullptr;
    QPushButton *m_ok       = nullptr;

    QNetworkAccessManager *m_net = nullptr;
    QList<Place>           m_places;
    Place                  m_chosen;
};

#endif
