/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 */
#include "ui/locationdialog.h"
#include "ui/theme.h"

#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QPushButton>
#include <QStandardPaths>
#include <QUrlQuery>
#include <QVBoxLayout>

namespace {

constexpr char kAutoMode[] = "location/auto";

/* Nominatim's terms ask for an identifying User-Agent naming the application,
 * and at most one request a second. A dialog where a human types an address and
 * presses a button cannot exceed that. */
QByteArray userAgent()
{
    return QByteArrayLiteral("SVXConnect-Omarchy/") + SVXCONNECT_VERSION
         + QByteArrayLiteral(" (+https://svxconnect.app)");
}

double asDouble(const QJsonValue &v)
{
    if (v.isDouble())
        return v.toDouble();
    bool ok = false;
    const double d = v.toString().toDouble(&ok);   /* Nominatim sends strings */
    return ok ? d : 0.0;
}

} // namespace

/* ---------------------------------------------------------------- pure bits */

QUrl LocationDialog::nominatimUrl(const QString &street, const QString &postcode,
                                  const QString &city, const QString &country)
{
    /* The structured query, not a free-text one: the fields are already
     * separate on screen, and structured lookups do not guess. */
    QUrlQuery q;
    if (!street.trimmed().isEmpty())   q.addQueryItem(QStringLiteral("street"),     street.trimmed());
    if (!postcode.trimmed().isEmpty()) q.addQueryItem(QStringLiteral("postalcode"), postcode.trimmed());
    if (!city.trimmed().isEmpty())     q.addQueryItem(QStringLiteral("city"),       city.trimmed());
    if (!country.trimmed().isEmpty())  q.addQueryItem(QStringLiteral("country"),    country.trimmed());
    q.addQueryItem(QStringLiteral("format"), QStringLiteral("jsonv2"));
    q.addQueryItem(QStringLiteral("addressdetails"), QStringLiteral("1"));
    q.addQueryItem(QStringLiteral("limit"), QStringLiteral("8"));

    QUrl url(QStringLiteral("https://nominatim.openstreetmap.org/search"));
    url.setQuery(q);
    return url;
}

QList<LocationDialog::Place> LocationDialog::parseNominatim(const QByteArray &json)
{
    QList<Place> out;
    for (const QJsonValue &v : QJsonDocument::fromJson(json).array()) {
        const QJsonObject o = v.toObject();
        Place p;
        p.latitude  = asDouble(o.value(QStringLiteral("lat")));
        p.longitude = asDouble(o.value(QStringLiteral("lon")));
        p.detail    = o.value(QStringLiteral("display_name")).toString();

        /* `location` in the configuration is a place name, not a postal
         * address: prefer the town, and fall back to whatever the result is
         * named. */
        const QJsonObject addr = o.value(QStringLiteral("address")).toObject();
        for (const char *key : {"city", "town", "village", "municipality", "county"}) {
            const QString v2 = addr.value(QLatin1String(key)).toString();
            if (!v2.isEmpty()) { p.name = v2; break; }
        }
        if (p.name.isEmpty())
            p.name = o.value(QStringLiteral("name")).toString();
        if (p.name.isEmpty() && !p.detail.isEmpty())
            p.name = p.detail.section(QLatin1Char(','), 0, 0).trimmed();

        if (p.isValid())
            out.append(p);
    }
    return out;
}

QString LocationDialog::omarchyLocationPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation)
         + QStringLiteral("/omarchy/settings/weather.json");
}

LocationDialog::Place LocationDialog::parseOmarchyLocation(const QByteArray &json)
{
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    Place p;
    p.name      = o.value(QStringLiteral("name")).toString();
    p.latitude  = asDouble(o.value(QStringLiteral("latitude")));
    p.longitude = asDouble(o.value(QStringLiteral("longitude")));
    p.detail    = QCoreApplication::translate("LocationDialog", "from your Omarchy weather location");
    return p;
}

LocationDialog::Place LocationDialog::omarchyPlace()
{
    QFile f(omarchyLocationPath());
    if (!f.open(QIODevice::ReadOnly))
        return Place{};
    return parseOmarchyLocation(f.readAll());
}

bool LocationDialog::autoModeSetting()
{
    return QSettings().value(QLatin1String(kAutoMode), false).toBool();
}

void LocationDialog::setAutoModeSetting(bool on)
{
    QSettings().setValue(QLatin1String(kAutoMode), on);
}

QUrl LocationDialog::ipLookupUrl()
{
    return QUrl(QStringLiteral("https://ipapi.co/json/"));
}

LocationDialog::Place LocationDialog::parseIpLookup(const QByteArray &json)
{
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    Place p;
    p.latitude  = asDouble(o.value(QStringLiteral("latitude")));
    p.longitude = asDouble(o.value(QStringLiteral("longitude")));
    p.name      = o.value(QStringLiteral("city")).toString();

    QStringList where;
    if (!p.name.isEmpty())                                         where << p.name;
    const QString region = o.value(QStringLiteral("region")).toString();
    if (!region.isEmpty())                                         where << region;
    const QString country = o.value(QStringLiteral("country_name")).toString();
    if (!country.isEmpty())                                        where << country;
    p.detail = QCoreApplication::translate("LocationDialog",
                   "%1 — from your IP address, so it points at the area, not your antenna")
                   .arg(where.join(QStringLiteral(", ")));
    return p;
}

/* -------------------------------------------------------------------- UI */

LocationDialog::LocationDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Find my position"));
    resize(Theme::space(560), Theme::space(520));

    m_net = new QNetworkAccessManager(this);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(Theme::space(14), Theme::space(14), Theme::space(14), Theme::space(14));
    root->setSpacing(Theme::space(10));

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setHorizontalSpacing(Theme::space(12));
    form->setVerticalSpacing(Theme::space(8));

    m_street   = new QLineEdit(this);
    m_postcode = new QLineEdit(this);
    m_city     = new QLineEdit(this);
    m_country  = new QLineEdit(this);
    m_street->setPlaceholderText(tr("Kerkstraat 12"));
    m_postcode->setPlaceholderText(tr("9000"));
    m_city->setPlaceholderText(tr("Gent"));
    m_country->setPlaceholderText(tr("Belgium"));

    form->addRow(tr("Street and number"), m_street);
    form->addRow(tr("Postcode"), m_postcode);
    form->addRow(tr("City"), m_city);
    form->addRow(tr("Country"), m_country);
    root->addLayout(form);

    for (QLineEdit *e : {m_street, m_postcode, m_city, m_country})
        connect(e, &QLineEdit::returnPressed, this, &LocationDialog::search);

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(Theme::space(8));
    m_search = new QPushButton(tr("Look up address"), this);
    m_search->setCursor(Qt::PointingHandCursor);
    connect(m_search, &QPushButton::clicked, this, &LocationDialog::search);
    buttons->addWidget(m_search);

    m_detect = new QPushButton(tr("Detect automatically"), this);
    m_detect->setFlat(true);
    m_detect->setCursor(Qt::PointingHandCursor);
    m_detect->setToolTip(tr("Your Omarchy weather location when it has coordinates, "
                            "otherwise a guess from your IP address"));
    connect(m_detect, &QPushButton::clicked, this, &LocationDialog::detect);
    buttons->addWidget(m_detect);
    buttons->addStretch(1);
    root->addLayout(buttons);

    m_results = new QListWidget(this);
    connect(m_results, &QListWidget::itemSelectionChanged, this, &LocationDialog::onSelectionChanged);
    connect(m_results, &QListWidget::itemDoubleClicked, this, [this]() {
        if (m_ok->isEnabled()) accept();
    });
    root->addWidget(m_results, 1);

    m_status = new QLabel(tr("Addresses are looked up with OpenStreetMap's Nominatim; "
                             "nothing is sent until you press a button."), this);
    Theme::setRole(m_status, "hint");
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_ok = box->button(QDialogButtonBox::Ok);
    m_ok->setText(tr("Use this position"));
    m_ok->setEnabled(false);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(box);

    setPlaceholder(tr("Matching addresses appear here."));   /* needs m_ok */
}

QNetworkReply *LocationDialog::get(const QUrl &url)
{
    QNetworkRequest req(url);
    req.setRawHeader(QByteArrayLiteral("User-Agent"), userAgent());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QVariant::fromValue(QNetworkRequest::NoLessSafeRedirectPolicy));
    req.setTransferTimeout(10000);
    return m_net->get(req);
}

void LocationDialog::setBusy(bool busy, const QString &what)
{
    m_search->setEnabled(!busy);
    m_detect->setEnabled(!busy);
    if (busy) {
        Theme::setTone(m_status, "");
        m_status->setText(what);
    }
}

void LocationDialog::fail(const QString &why)
{
    Theme::setTone(m_status, "error");
    m_status->setText(why);
}

/* An empty list widget is a grey void that says nothing about whether the
 * lookup ran. A single unselectable line says it. */
void LocationDialog::setPlaceholder(const QString &text)
{
    m_results->clear();
    m_places.clear();
    m_chosen = Place{};
    m_ok->setEnabled(false);

    auto *item = new QListWidgetItem(text, m_results);
    item->setFlags(Qt::NoItemFlags);
    item->setForeground(Theme::wash(Theme::palette().foreground, 0.5));
}

void LocationDialog::showPlaces(const QList<Place> &places)
{
    if (places.isEmpty()) {
        setPlaceholder(tr("Nothing matched."));
        return;
    }

    m_places = places;
    m_results->clear();
    m_chosen = Place{};
    m_ok->setEnabled(false);

    for (const Place &p : places) {
        auto *item = new QListWidgetItem(
            QStringLiteral("%1\n%2").arg(p.detail.isEmpty() ? p.name : p.detail,
                                         tr("%1, %2")
                                             .arg(p.latitude,  0, 'f', 5)
                                             .arg(p.longitude, 0, 'f', 5)),
            m_results);
        item->setToolTip(p.detail);
    }
    m_results->setCurrentRow(0);
}

void LocationDialog::onSelectionChanged()
{
    const int row = m_results->currentRow();
    const bool ok = row >= 0 && row < m_places.size();
    m_chosen = ok ? m_places.at(row) : Place{};
    m_ok->setEnabled(ok);
}

void LocationDialog::search()
{
    if (m_street->text().trimmed().isEmpty() && m_city->text().trimmed().isEmpty()
        && m_postcode->text().trimmed().isEmpty()) {
        fail(tr("Give at least a city or a postcode."));
        return;
    }

    setBusy(true, tr("Looking up the address…"));
    QNetworkReply *reply = get(nominatimUrl(m_street->text(), m_postcode->text(),
                                            m_city->text(), m_country->text()));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        setBusy(false);

        if (reply->error() != QNetworkReply::NoError) {
            fail(tr("The lookup failed: %1").arg(reply->errorString()));
            return;
        }

        const QList<Place> places = parseNominatim(reply->readAll());
        showPlaces(places);
        if (places.isEmpty())
            fail(tr("Nothing found for that address. Try just the city and postcode."));
        else
            m_status->setText(tr("Pick the right one — the coordinates are on the second line."));
    });
}

void LocationDialog::detect()
{
    /* The local answer first: Omarchy's own weather location, when the user
     * gave it coordinates. It costs no request and no third party. */
    const Place local = omarchyPlace();
    if (local.isValid()) {
        showPlaces({local});
        m_status->setText(tr("From your Omarchy weather location."));
        return;
    }

    setBusy(true, tr("Asking ipapi.co where this computer is…"));
    QNetworkReply *reply = get(ipLookupUrl());
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        setBusy(false);

        if (reply->error() != QNetworkReply::NoError) {
            fail(tr("The lookup failed: %1").arg(reply->errorString()));
            return;
        }

        const Place p = parseIpLookup(reply->readAll());
        if (!p.isValid()) {
            fail(tr("That answered with no position. Type the address instead."));
            return;
        }
        showPlaces({p});
        Theme::setTone(m_status, "warn");
        m_status->setText(tr("An IP address places you in the right town at best. "
                             "Check it, or look up your address above."));
    });
}
