/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 */
#include "ui/preferencesdialog.h"
#include "ui/theme.h"
#include "core/devlist.h"
#include "ptt/portalbackend.h"
#include "ptt/hyprlandbinding.h"
#include "ui/notifier.h"
#include "ui/locationdialog.h"
#include "net/reflectorfeed.h"
#include "net/portalinfo.h"
#include "ui/talkgroupsdialog.h"

#include <QSettings>
#include <QLocale>
#include <QDateTime>
#include <QJsonObject>
#include <QJsonDocument>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QScrollArea>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QRegularExpressionValidator>
#include <QDoubleValidator>
#include <QGuiApplication>
#include <QSet>
#include <QSignalBlocker>
#include <QApplication>

namespace {

/* A caption under a control. These carry the explanations that would otherwise
 * only exist in example.conf, which a GUI user never opens. */
QLabel *hint(const QString &text, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    Theme::setRole(l, "hint");
    l->setWordWrap(true);
    /* Expanding, or QFormLayout gives a word-wrapped label its size hint —
     * about half the field — and computes the wrapped height for that width. */
    l->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    return l;
}

/* A block of text to copy: Lua for bindings.lua, shell for a script. */
QLabel *code(QWidget *parent)
{
    auto *l = new QLabel(parent);
    Theme::setRole(l, "code");
    l->setTextFormat(Qt::PlainText);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->setWordWrap(false);
    return l;
}

QFormLayout *form(QWidget *page)
{
    auto *f = new QFormLayout(page);
    f->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    f->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    f->setContentsMargins(Theme::space(14), Theme::space(14), Theme::space(14), Theme::space(14));
    f->setHorizontalSpacing(Theme::space(12));
    f->setVerticalSpacing(Theme::space(8));
    return f;
}

QWidget *scrolled(QWidget *inner)
{
    auto *area = new QScrollArea;
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setWidget(inner);
    return area;
}

constexpr char kPttMode[]    = "ptt/mode";
constexpr char kPttTrigger[] = "ptt/trigger";

} // namespace

PreferencesDialog::PreferencesDialog(svx_app *app, const QString &configPath, QWidget *parent)
    : QDialog(parent), m_app(app), m_store(app ? app_config(app) : nullptr, configPath)
{
    setWindowTitle(tr("SVXConnect Preferences"));
    resize(Theme::space(780), Theme::space(640));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(Theme::space(12), Theme::space(12), Theme::space(12), Theme::space(12));
    root->setSpacing(Theme::space(10));

    auto *tabs = new QTabWidget(this);
    m_tabs = tabs;
    tabs->setDocumentMode(true);
    tabs->addTab(scrolled(buildConnectionTab()), tr("Connection"));
    tabs->addTab(scrolled(buildAudioTab()),      tr("Audio"));
    tabs->addTab(scrolled(buildTalkgroupsTab()), tr("Talkgroups"));
    m_namesPage = scrolled(buildNamesTab());
    tabs->addTab(m_namesPage, tr("Names"));
    tabs->addTab(scrolled(buildPttTab()),        tr("Push-to-talk"));
    tabs->addTab(scrolled(buildGeneralTab()),    tr("General"));
    root->addWidget(tabs, 1);

    m_buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply, this);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &PreferencesDialog::onAccept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked,
            this, &PreferencesDialog::onApply);
    root->addWidget(m_buttons);

    load();
}

QWidget *PreferencesDialog::buildConnectionTab()
{
    auto *page = new QWidget;
    auto *f = form(page);

    m_callsign = new QLineEdit(page);
    /* Uppercase, and only the characters a callsign can contain. '/' is
     * deliberately NOT allowed: a '/'-suffixed callsign only ever arrives FROM
     * the reflector's decoder, it is never something you enrol as. */
    m_callsign->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[A-Za-z0-9-]{0,31}")), m_callsign));
    connect(m_callsign, &QLineEdit::textEdited, this, [this](const QString &t) {
        const int pos = m_callsign->cursorPosition();
        const QSignalBlocker b(m_callsign);
        m_callsign->setText(t.toUpper());
        m_callsign->setCursorPosition(pos);
    });
    f->addRow(tr("Callsign"), m_callsign);

    m_email = new QLineEdit(page);
    f->addRow(tr("Email"), m_email);
    f->addRow(QString(), hint(
        tr("Becomes the certificate request's contact address, so the reflector "
           "sysop can reach you. Required to enrol."), page));

    m_reflector = new QLineEdit(page);
    /* Strip whitespace as it is typed: a hostname pasted out of an email
     * routinely arrives with a trailing space, and the SRV lookup then fails
     * in a way that looks like the reflector is down. */
    connect(m_reflector, &QLineEdit::textEdited, this, [this](const QString &t) {
        if (!t.contains(QLatin1Char(' ')) && !t.contains(QLatin1Char('\t')))
            return;
        const int pos = m_reflector->cursorPosition();
        const QSignalBlocker b(m_reflector);
        m_reflector->setText(t.simplified().remove(QLatin1Char(' ')));
        m_reflector->setCursorPosition(std::max(0, pos - 1));
    });
    f->addRow(tr("Reflector"), m_reflector);

    m_port = new QSpinBox(page);
    m_port->setRange(1, 65535);
    f->addRow(tr("Port"), m_port);
    f->addRow(QString(), hint(
        tr("The host is looked up by SRV record first, so the port is usually "
           "discovered automatically and this is only the fallback."), page));

    /* The enhanced reflector. Not a host to type: it is derived from the one
     * above, probed for, and either there or not. */
    m_enhanced = new QCheckBox(tr("Use the reflector's portal feed when it has one"), page);
    connect(m_enhanced, &QCheckBox::toggled, this, [this]() { refreshJsonEditors(); });
    m_enhanced->setToolTip(tr("Looks for a portal at wss://reflector.%1/")
                               .arg(tr("<your reflector>")));
    f->addRow(QString(), m_enhanced);
    f->addRow(QString(), hint(
        tr("Some reflectors publish their traffic and their nodes' positions over a "
           "WebSocket. When one does, Recent is replaced by the reflector's own 24-hour "
           "history — which covers every talkgroup, not only the ones you monitor — and "
           "the map has something to show. Turn this off to see exactly what a plain "
           "reflector gives you."), page));

    auto *qth = new QGroupBox(tr("Station position"), page);
    auto *qthForm = new QFormLayout(qth);
    qthForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    /* Two ways to have a position, and they are exclusive: either Omarchy owns
     * it — one place to change it, for the weather panel and the reflector
     * both — or this dialog does. Anything in between is a pair of numbers
     * nobody can say the origin of. */
    m_posMode = new QComboBox(qth);
    m_posMode->addItem(tr("Manual"), QStringLiteral("manual"));
    m_posMode->addItem(tr("Automatic — follow Omarchy"), QStringLiteral("auto"));
    qthForm->addRow(tr("Position"), m_posMode);
    connect(m_posMode, &QComboBox::currentIndexChanged, this, [this]() { applyPositionMode(); });

    m_location = new QLineEdit(qth);
    qthForm->addRow(tr("Location"), m_location);

    /* QLocale::c() is not optional. Under a comma locale a QDoubleValidator
     * built from the system locale accepts "51,05", which is then written to
     * the config verbatim and read back by the core as 51. */
    auto *latVal = new QDoubleValidator(-90.0, 90.0, 7, this);
    latVal->setNotation(QDoubleValidator::StandardNotation);
    latVal->setLocale(QLocale::c());
    m_latitude = new QLineEdit(qth);
    m_latitude->setValidator(latVal);
    qthForm->addRow(tr("Latitude"), m_latitude);

    auto *lonVal = new QDoubleValidator(-180.0, 180.0, 7, this);
    lonVal->setNotation(QDoubleValidator::StandardNotation);
    lonVal->setLocale(QLocale::c());
    m_longitude = new QLineEdit(qth);
    m_longitude->setValidator(lonVal);
    qthForm->addRow(tr("Longitude"), m_longitude);

    m_grid = new QLabel(qth);
    Theme::setRole(m_grid, "value");
    qthForm->addRow(tr("Grid square"), m_grid);

    auto recomputeGrid = [this]() {
        bool okLat = false, okLon = false;
        const double la = QLocale::c().toDouble(m_latitude->text(), &okLat);
        const double lo = QLocale::c().toDouble(m_longitude->text(), &okLon);
        if (!okLat || !okLon || (la == 0.0 && lo == 0.0)) {
            m_grid->setText(tr("—"));
            return;
        }
        char g[16] = {0};
        maidenhead(g, sizeof g, la, lo);
        m_grid->setText(QString::fromUtf8(g));
    };
    connect(m_latitude,  &QLineEdit::textChanged, this, recomputeGrid);
    connect(m_longitude, &QLineEdit::textChanged, this, recomputeGrid);

    /* Coordinates precise enough to plot an antenna are not something anyone
     * knows by heart, and reading them off a map is where a digit goes missing.
     * The lookup fills all three fields at once; the grid square then follows
     * from the textChanged connections above. */
    m_lookup = new QPushButton(tr("Find my position…"), qth);
    m_lookup->setCursor(Qt::PointingHandCursor);
    m_lookup->setToolTip(tr("Look the coordinates up from an address, or take them "
                            "from your Omarchy weather location."));
    connect(m_lookup, &QPushButton::clicked, this, [this]() {
        LocationDialog dlg(this);
        if (dlg.exec() != QDialog::Accepted)
            return;
        const LocationDialog::Place p = dlg.chosen();
        if (!p.name.isEmpty())
            m_location->setText(p.name);
        /* QLocale::c(), for the same reason the validators above use it. */
        m_latitude->setText(QLocale::c().toString(p.latitude, 'f', 6));
        m_longitude->setText(QLocale::c().toString(p.longitude, 'f', 6));
    });
    qthForm->addRow(QString(), m_lookup);

    m_posHint = hint(QString(), qth);
    qthForm->addRow(QString(), m_posHint);

    f->addRow(qth);
    return page;
}

QWidget *PreferencesDialog::buildAudioTab()
{
    auto *page = new QWidget;
    auto *f = form(page);

    m_inputDev = new QComboBox(page);
    m_outputDev = new QComboBox(page);
    f->addRow(tr("Microphone"), m_inputDev);
    f->addRow(tr("Speaker"), m_outputDev);

    m_devWarning = hint(QString(), page);
    Theme::setTone(m_devWarning, "warn");
    m_devWarning->hide();
    f->addRow(QString(), m_devWarning);

    auto *toneRow = new QHBoxLayout;
    auto *tone = new QPushButton(tr("Play test tone"), page);
    tone->setCursor(Qt::PointingHandCursor);
    connect(tone, &QPushButton::clicked, this, &PreferencesDialog::onTestTone);
    toneRow->addWidget(tone);
    toneRow->addStretch(1);
    f->addRow(QString(), toneRow);
    f->addRow(QString(), hint(
        tr("Two beeps through the speaker SVXConnect is using right now. If you just "
           "changed the speaker above, press Apply first — the tone follows the device "
           "in use, not the one selected here. The volume is forced to at least 50%, "
           "and a muted output is unmuted for the test."), page));

    m_volume = new QSpinBox(page);
    m_volume->setRange(0, 100);
    m_volume->setSuffix(tr(" %"));
    f->addRow(tr("Output volume"), m_volume);

    auto *mic = new QGroupBox(tr("Microphone processing"), page);
    auto *micForm = new QFormLayout(mic);
    micForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    m_micGain = new QSpinBox(mic);
    m_micGain->setRange(-20, 40);
    m_micGain->setSuffix(tr(" dB"));
    micForm->addRow(tr("Input gain"), m_micGain);

    m_micAgc = new QCheckBox(tr("Automatic gain control"), mic);
    micForm->addRow(QString(), m_micAgc);

    m_micAgcTgt = new QSpinBox(mic);
    m_micAgcTgt->setRange(5, 95);
    m_micAgcTgt->setSuffix(tr(" %"));
    micForm->addRow(tr("AGC target level"), m_micAgcTgt);
    connect(m_micAgc, &QCheckBox::toggled, m_micAgcTgt, &QWidget::setEnabled);

    micForm->addRow(QString(), hint(
        tr("Gain is applied before AGC. If your audio is quiet at the far end, "
           "raise the gain rather than the AGC target."), mic));
    f->addRow(mic);

    auto *net = new QGroupBox(tr("Network audio"), page);
    auto *netForm = new QFormLayout(net);
    netForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    m_jitter = new QSpinBox(net);
    m_jitter->setRange(40, 300);
    m_jitter->setSuffix(tr(" ms"));
    netForm->addRow(tr("Jitter buffer"), m_jitter);
    netForm->addRow(QString(), hint(
        tr("How much audio to hold back before playing, to absorb network jitter. "
           "Raise it if speech breaks up on a poor link; lower it to reduce delay."),
        net));

    m_tailTrim = new QSpinBox(net);
    m_tailTrim->setRange(0, 1000);
    m_tailTrim->setSingleStep(50);
    m_tailTrim->setSuffix(tr(" ms"));
    netForm->addRow(tr("Tail trim"), m_tailTrim);
    f->addRow(net);

    auto *beep = new QGroupBox(tr("Roger beep"), page);
    auto *beepForm = new QFormLayout(beep);
    beepForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    m_rogerBeep = new QCheckBox(tr("Beep when an over ends"), beep);
    beepForm->addRow(QString(), m_rogerBeep);
    m_rogerMin = new QSpinBox(beep);
    m_rogerMin->setRange(0, 60);
    m_rogerMin->setSuffix(tr(" s"));
    beepForm->addRow(tr("Minimum over length"), m_rogerMin);
    connect(m_rogerBeep, &QCheckBox::toggled, m_rogerMin, &QWidget::setEnabled);
    beepForm->addRow(QString(), hint(
        tr("Overs shorter than this do not produce a beep. Your own transmissions "
           "never do."), beep));
    f->addRow(beep);

    return page;
}

QWidget *PreferencesDialog::buildTalkgroupsTab()
{
    auto *page = new QWidget;
    auto *f = form(page);

    m_switchable = new QLineEdit(page);
    m_switchable->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[0-9,+ ]*")), m_switchable));
    f->addRow(tr("Switchable"), m_switchable);
    f->addRow(QString(), hint(
        tr("The talkgroups the sidebar cycles through, in this order."), page));

    m_monitored = new QLineEdit(page);
    m_monitored->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[0-9,+ ]*")), m_monitored));
    f->addRow(tr("Monitored"), m_monitored);

    m_tgPreview = hint(QString(), page);
    f->addRow(QString(), m_tgPreview);

    f->addRow(QString(), hint(
        tr("Everything you want to hear. A trailing '+' raises priority: "
           "8 is normal, 8+ is higher, 8++ is highest. A talker on a "
           "higher-priority talkgroup moves you to it automatically."), page));

    /* The reflector knows what its own talkgroups are called and which ones
     * anyone is listening to. Typing those numbers from memory is how a
     * configuration ends up monitoring a talkgroup that was renumbered last
     * year. */
    m_loadTgs = new QPushButton(tr("Load from reflector…"), page);
    m_loadTgs->setObjectName(QStringLiteral("loadFromReflector"));
    m_loadTgs->setCursor(Qt::PointingHandCursor);
    connect(m_loadTgs, &QPushButton::clicked, this, &PreferencesDialog::onLoadFromReflector);
    f->addRow(QString(), m_loadTgs);

    auto updatePreview = [this]() {
        svx_tg_entry v[SVX_MAX_TG];
        const int n = tglist_parse(qPrintable(m_monitored->text()), v, SVX_MAX_TG);
        if (n < 0) {
            m_tgPreview->setText(tr("Not a valid list."));
            Theme::setTone(m_tgPreview, "error");
            return;
        }
        QStringList parts;
        for (int i = 0; i < n; ++i)
            parts << (v[i].priority > 0
                          ? tr("TG %1 (priority %2)").arg(v[i].id).arg(v[i].priority)
                          : tr("TG %1").arg(v[i].id));
        Theme::setTone(m_tgPreview, "");
        m_tgPreview->setText(parts.join(QStringLiteral(" · ")));
    };
    connect(m_monitored, &QLineEdit::textChanged, this, updatePreview);

    m_defaultTg = new QSpinBox(page);
    m_defaultTg->setRange(0, 999999);
    m_defaultTg->setSpecialValueText(tr("monitor only"));
    f->addRow(tr("Talkgroup at start"), m_defaultTg);

    m_lockOnStart = new QCheckBox(tr("Start locked"), page);
    f->addRow(QString(), m_lockOnStart);

    m_linger = new QSpinBox(page);
    m_linger->setRange(10, 300);
    m_linger->setSuffix(tr(" s"));
    f->addRow(tr("Linger"), m_linger);
    f->addRow(QString(), hint(
        tr("How long a talkgroup stays protected after an over, so a higher-priority "
           "talkgroup cannot pull you out of the gap between two overs of the QSO "
           "you are actually having."), page));

    m_idle = new QSpinBox(page);
    m_idle->setRange(0, 3600);
    m_idle->setSuffix(tr(" s"));
    m_idle->setSpecialValueText(tr("never"));
    f->addRow(tr("Drop to monitor after"), m_idle);
    f->addRow(QString(), hint(
        tr("Silence everywhere for this long releases your talkgroup and returns "
           "you to monitoring."), page));

    m_tgOrder = new QComboBox(page);
    m_tgOrder->addItem(tr("By number"), QStringLiteral("numeric"));
    m_tgOrder->addItem(tr("By last heard"), QStringLiteral("lastheard"));
    f->addRow(tr("Sidebar order"), m_tgOrder);

    return page;
}

void PreferencesDialog::setReflectorInfo(ReflectorFeed *feed, PortalInfo *portal)
{
    m_feed   = feed;
    m_portal = portal;
    refreshReflectorButton();

    loadJsonEditors();
    refreshJsonEditors();
    refreshPortalStatus();

    if (!m_portal)
        return;
    connect(m_portal, &PortalInfo::statusChanged, this, &PreferencesDialog::refreshPortalStatus);
    connect(m_portal, &PortalInfo::changed, this, [this]() {
        refreshReflectorButton();
        /* A fetch landed while the dialog is open. Show it — unless the
         * operator is in the middle of editing, whose text is theirs. */
        const bool untouched = m_tgJson->toPlainText().trimmed() == m_tgJsonLoaded
                            && m_callJson->toPlainText().trimmed() == m_callJsonLoaded;
        if (untouched)
            loadJsonEditors();
    });
}

void PreferencesDialog::refreshReflectorButton()
{
    if (!m_loadTgs)
        return;

    const bool live   = m_feed && m_feed->isAvailable();
    const bool named  = m_portal && m_portal->hasTalkgroups();
    m_loadTgs->setEnabled(live || named);
    m_loadTgs->setToolTip(live || named
        ? tr("Pick from the talkgroups this reflector names, and the ones its nodes "
             "are listening to right now.")
        : tr("Only an enhanced reflector can answer this — a plain one does not "
             "publish its talkgroups."));
}

void PreferencesDialog::onLoadFromReflector()
{
    /* How busy each talkgroup is, from the feed. It annotates the list; it
     * does not add to it — see ReflectorTalkgroupsDialog::entriesFor(). */
    QHash<quint32, int> nodeCount;
    if (m_feed) {
        const QHash<QString, ReflectorFeed::Node> &nodes = m_feed->nodes();
        for (auto it = nodes.cbegin(); it != nodes.cend(); ++it)
            for (int tg : it.value().monitoredTgs)
                if (tg > 0)
                    nodeCount[quint32(tg)] += 1;
    }

    const QHash<quint32, QString> names = m_portal ? m_portal->talkgroups()
                                                   : QHash<quint32, QString>();

    QList<quint32> kept;
    const QList<ReflectorTalkgroupsDialog::Entry> entries =
        ReflectorTalkgroupsDialog::entriesFor(names, nodeCount, m_monitored->text(),
                                              m_switchable->text(), &kept);

    if (entries.isEmpty()) {
        QMessageBox::information(this, tr("Nothing to load"),
            tr("The reflector published no talkgroups. Either it is not an enhanced "
               "reflector, or its portal is not answering. You can write the talkgroup "
               "info yourself on the Names tab."));
        return;
    }

    ReflectorTalkgroupsDialog dlg(entries, this);
    if (!kept.isEmpty()) {
        QStringList text;
        for (quint32 id : std::as_const(kept)) text << QString::number(id);
        dlg.setKeptNote(tr("Also in your configuration, and kept as they are: TG %1 — "
                           "the reflector does not name them, so they are not listed.")
                            .arg(text.join(QStringLiteral(", "))));
    }
    if (dlg.exec() != QDialog::Accepted)
        return;

    const ReflectorTalkgroupsDialog::Fields f =
        ReflectorTalkgroupsDialog::compose(dlg.chosen(), m_monitored->text(), m_switchable->text());
    m_monitored->setText(f.monitored);
    m_switchable->setText(f.switchable);
}

QWidget *PreferencesDialog::buildNamesTab()
{
    auto *page = new QWidget;
    auto *f = form(page);

    f->addRow(QString(), hint(
        tr("What talkgroups and stations are called. It is a pair of plain JSON "
           "documents: an enhanced reflector publishes them and they are fetched "
           "once a day, and on a plain reflector — which publishes nothing — you "
           "can write them yourself. The names appear under the active talkgroup, "
           "in tooltips, in the talkgroup picker and on the map."), page));

    m_portalAuto = new QCheckBox(tr("Update from the reflector's portal automatically"), page);
    connect(m_portalAuto, &QCheckBox::toggled, this, [this]() { refreshJsonEditors(); });
    f->addRow(QString(), m_portalAuto);

    auto *row = new QHBoxLayout;
    row->setSpacing(Theme::space(10));
    m_portalUpdate = new QPushButton(tr("Update now"), page);
    m_portalUpdate->setCursor(Qt::PointingHandCursor);
    connect(m_portalUpdate, &QPushButton::clicked, this, [this]() {
        if (m_portal) m_portal->refresh();
    });
    row->addWidget(m_portalUpdate);
    m_portalStatus = hint(QString(), page);
    row->addWidget(m_portalStatus, 1);
    f->addRow(QString(), row);

    m_tgJson = new QPlainTextEdit(page);
    m_tgJson->setMinimumHeight(Theme::space(120));
    m_tgJson->setPlaceholderText(
        QStringLiteral("{\"4\": \"4m Repeaters\", \"8\": \"70cm Repeaters\", \"9990\": \"Parrot\"}"));
    f->addRow(tr("Talkgroup info"), m_tgJson);
    f->addRow(QString(), hint(
        tr("The talkgroup number, as text, and what to call it."), page));

    m_callJson = new QPlainTextEdit(page);
    m_callJson->setMinimumHeight(Theme::space(140));
    m_callJson->setPlaceholderText(
        QStringLiteral("{\"ON0ORA\": \"TX:438.8000 RX:431.2000\\nCTCSS: 131.8\"}"));
    f->addRow(tr("Callsign info"), m_callJson);
    f->addRow(QString(), hint(
        tr("A callsign and a free description, shown on that station's card on the "
           "map. Use \\n for a line break."), page));

    m_jsonError = hint(QString(), page);
    Theme::setTone(m_jsonError, "error");
    m_jsonError->hide();
    f->addRow(QString(), m_jsonError);

    return page;
}

void PreferencesDialog::loadJsonEditors()
{
    if (!m_tgJson)
        return;

    /* Indented, because the portal serves one long line and nobody can find a
     * talkgroup in that. The text put in is remembered, so at commit time an
     * edit can be told from merely having looked. */
    auto pretty = [](const QByteArray &raw) {
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        return doc.isObject() && !doc.object().isEmpty()
             ? QString::fromUtf8(doc.toJson(QJsonDocument::Indented)).trimmed()
             : QString::fromUtf8(raw).trimmed();
    };

    m_tgJsonLoaded   = m_portal ? pretty(m_portal->rawTalkgroups()) : QString();
    m_callJsonLoaded = m_portal ? pretty(m_portal->rawCallsigns())  : QString();
    m_tgJson->setPlainText(m_tgJsonLoaded);
    m_callJson->setPlainText(m_callJsonLoaded);
}

void PreferencesDialog::refreshJsonEditors()
{
    if (!m_tgJson)
        return;

    /* Editable when nothing else is going to overwrite them: on a plain
     * reflector, or with automatic updates off. The same rule as the Android
     * and macOS apps — an editor whose contents vanish overnight is a trap. */
    const bool editable = !m_enhanced->isChecked() || !m_portalAuto->isChecked();
    m_tgJson->setReadOnly(!editable);
    m_callJson->setReadOnly(!editable);

    const QString why = editable
        ? QString()
        : tr("Kept up to date from the portal. Turn automatic updates off to edit it.");
    m_tgJson->setToolTip(why);
    m_callJson->setToolTip(why);
}

void PreferencesDialog::refreshPortalStatus()
{
    if (!m_portalStatus)
        return;

    const bool fetching = m_portal && m_portal->isFetching();
    m_portalUpdate->setEnabled(m_portal && !fetching);
    m_portalUpdate->setText(fetching ? tr("Updating…") : tr("Update now"));

    if (!m_portal) {
        m_portalStatus->clear();
        return;
    }
    if (!m_portal->lastError().isEmpty() && !fetching) {
        Theme::setTone(m_portalStatus, "error");
        m_portalStatus->setText(m_portal->lastError());
        return;
    }

    Theme::setTone(m_portalStatus, "");
    const qint64 at = m_portal->lastFetched();
    m_portalStatus->setText(at > 0
        ? tr("Last updated %1").arg(QLocale().toString(QDateTime::fromSecsSinceEpoch(at),
                                                       QLocale::ShortFormat))
        : tr("Never updated"));
}

bool PreferencesDialog::commitJson()
{
    PortalInfo::setAutoUpdateSetting(m_portalAuto->isChecked());

    if (!m_portal || m_tgJson->isReadOnly())
        return true;

    const QString tg   = m_tgJson->toPlainText().trimmed();
    const QString call = m_callJson->toPlainText().trimmed();
    if (tg == m_tgJsonLoaded && call == m_callJsonLoaded)
        return true;                       /* looked, did not touch */

    QString error;
    if (!m_portal->setManualJson(tg.toUtf8(), call.toUtf8(), &error)) {
        /* Nothing was stored. Say what is wrong where it is wrong, rather than
         * closing the dialog on a document that silently shows no names. */
        m_jsonError->setText(error);
        m_jsonError->show();
        if (m_tabs && m_namesPage)
            m_tabs->setCurrentWidget(m_namesPage);
        return false;
    }

    m_jsonError->hide();
    m_tgJsonLoaded   = tg;
    m_callJsonLoaded = call;
    return true;
}

QWidget *PreferencesDialog::buildPttTab()
{
    const bool hyprland = HyprlandBinding::isHyprland();

    auto *page = new QWidget;
    auto *f = form(page);

    m_pttMode = new QComboBox(page);
    m_pttMode->addItem(tr("Hold to talk"), QStringLiteral("hold"));
    m_pttMode->addItem(tr("Toggle"),       QStringLiteral("toggle"));
    f->addRow(tr("Mode"), m_pttMode);

    /* ---- the global key ---- */
    auto *kb = new QGroupBox(hyprland ? tr("Global key (Hyprland)") : tr("Global key"), page);
    auto *kbForm = new QFormLayout(kb);
    kbForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    kbForm->setVerticalSpacing(Theme::space(8));

    m_pttPortalStatus = hint(QString(), kb);
    kbForm->addRow(tr("Portal"), m_pttPortalStatus);

    m_pttShortcut = new QLabel(kb);
    Theme::setRole(m_pttShortcut, "value");
    m_pttShortcut->setTextInteractionFlags(Qt::TextSelectableByMouse);
    kbForm->addRow(tr("Bound to"), m_pttShortcut);

    if (hyprland) {
        m_pttKeys = new QLineEdit(kb);
        m_pttKeys->setPlaceholderText(QLatin1String(HyprlandBinding::kDefaultKeys));
        m_pttKeys->setToolTip(tr("Written the way Hyprland writes chords, for example "
                                 "SUPER + GRAVE or SUPER + CTRL + F12."));
        connect(m_pttKeys, &QLineEdit::textChanged, this, &PreferencesDialog::updatePttPreview);
        kbForm->addRow(tr("Key"), m_pttKeys);

        m_pttKeysWarning = hint(QString(), kb);
        m_pttKeysWarning->hide();
        kbForm->addRow(QString(), m_pttKeysWarning);

        m_pttPreview = code(kb);
        m_pttPreview->setToolTip(HyprlandBinding::bindingsFile());
        kbForm->addRow(tr("Adds"), m_pttPreview);

        auto *btnRow = new QHBoxLayout;
        btnRow->setSpacing(Theme::space(8));

        m_pttBind = new QPushButton(tr("Bind in Hyprland"), kb);
        m_pttBind->setCursor(Qt::PointingHandCursor);
        connect(m_pttBind, &QPushButton::clicked, this, &PreferencesDialog::onBindHyprland);
        btnRow->addWidget(m_pttBind);

        m_pttUnbind = new QPushButton(tr("Remove"), kb);
        m_pttUnbind->setCursor(Qt::PointingHandCursor);
        m_pttUnbind->setToolTip(tr("Take SVXConnect's block back out of bindings.lua"));
        connect(m_pttUnbind, &QPushButton::clicked, this, &PreferencesDialog::onUnbindHyprland);
        btnRow->addWidget(m_pttUnbind);

        auto *edit = new QPushButton(tr("Edit bindings.lua"), kb);
        edit->setFlat(true);
        edit->setCursor(Qt::PointingHandCursor);
        connect(edit, &QPushButton::clicked, this, []() {
            HyprlandBinding::openBindingsFile();
        });
        btnRow->addWidget(edit);
        btnRow->addStretch(1);
        kbForm->addRow(QString(), btnRow);

        kbForm->addRow(QString(), hint(
            tr("Hyprland owns the key: SVXConnect registers a global shortcut named %1 "
               "and a bind in bindings.lua points a key at it. Hyprland passes both the "
               "press and the release through, so holding the key transmits and letting "
               "go stops.\n\n"
               "SVXConnect must be running for the key to do anything — closing the "
               "window keeps it running in the tray.")
                .arg(QLatin1String(HyprlandBinding::kShortcutId)), kb));

        kbForm->addRow(QString(), hint(
            tr("To use CapsLock, make it a Super key in ~/.config/hypr/input.lua:\n"
               "    kb_options = \"caps:super\"\n"
               "and CapsLock + ` is then SUPER + GRAVE."), kb));
    } else {
        kbForm->addRow(QString(), hint(
            tr("Your desktop owns this key: it is registered through the global-shortcuts "
               "portal and changed in your desktop's keyboard shortcut settings, where "
               "SVXConnect is listed by name. SVXConnect must be running for it to work."),
            kb));
    }

    f->addRow(kb);

    /* ---- the fallback that always works ---- */
    auto *fifo = new QGroupBox(tr("Control FIFO"), page);
    auto *fifoLay = new QVBoxLayout(fifo);
    fifoLay->setSpacing(Theme::space(8));
    fifoLay->addWidget(hint(
        tr("The control FIFO needs no portal at all and always sees both edges, which "
           "makes it the right choice for a foot switch or a script. As a Hyprland bind:"),
        fifo));
    m_fifoPreview = code(fifo);
    fifoLay->addWidget(m_fifoPreview);
    fifoLay->addWidget(hint(
        tr("Other commands: tg <n>|next|prev, lock on|off|toggle, mute <n>, "
           "volume 0-100, status, quit"), fifo));
    f->addRow(fifo);

    return page;
}

QWidget *PreferencesDialog::buildGeneralTab()
{
    auto *page = new QWidget;
    auto *f = form(page);

    m_notifyTalkers = new QCheckBox(tr("Notify when someone starts talking"), page);
    f->addRow(QString(), m_notifyTalkers);
    f->addRow(QString(), hint(
        tr("A desktop notification naming the station and its talkgroup, while the "
           "window is closed to the tray. Click it to bring SVXConnect back. Your own "
           "transmissions never notify."), page));

    m_txTimeout = new QSpinBox(page);
    m_txTimeout->setRange(0, 3600);
    m_txTimeout->setSuffix(tr(" s"));
    m_txTimeout->setSpecialValueText(tr("no limit"));
    f->addRow(tr("Transmit timeout"), m_txTimeout);
    f->addRow(QString(), hint(
        tr("Hard un-key after this long, so a stuck push-to-talk cannot leave you "
           "transmitting. Setting it to \"no limit\" is strongly discouraged."), page));

    m_logLevel = new QComboBox(page);
    /* The core's enum is "err|warn|info|debug" — the last is spelled debug,
     * not dbg. config_set() rejects anything else. */
    m_logLevel->addItem(tr("Errors only"), QStringLiteral("err"));
    m_logLevel->addItem(tr("Warnings"),    QStringLiteral("warn"));
    m_logLevel->addItem(tr("Normal"),      QStringLiteral("info"));
    m_logLevel->addItem(tr("Debug"),       QStringLiteral("debug"));
    f->addRow(tr("Log detail"), m_logLevel);

    m_ctlFifo = new QLineEdit(page);
    connect(m_ctlFifo, &QLineEdit::textChanged, this, &PreferencesDialog::updatePttPreview);
    f->addRow(tr("Control FIFO"), m_ctlFifo);
    f->addRow(QString(), hint(
        tr("A named pipe for scripts, a foot switch, or a key binding. "
           "Clearing this disables it."), page));

    /* How wide "home" is on the map. A desktop setting, not a config key: the
     * terminal client has no map and would warn about a key it does not
     * know. */
    m_mapRadius = new QSpinBox(page);
    m_mapRadius->setRange(10, 2000);
    m_mapRadius->setSingleStep(25);
    m_mapRadius->setSuffix(tr(" km"));
    f->addRow(tr("Map home view"), m_mapRadius);
    f->addRow(QString(), hint(
        tr("How much of the map to show around your own station when nobody is "
           "transmitting. A talker always pulls the view to itself, at 20 km."), page));

    return page;
}

bool PreferencesDialog::holdMode()
{
    return QSettings().value(QLatin1String(kPttMode), QStringLiteral("hold")).toString()
           != QLatin1String("toggle");
}

PttBinding PreferencesDialog::keyboardBinding()
{
    /* The portal's preferred_trigger, in the freedesktop Shortcuts syntax.
     * KDE honours it; Hyprland ignores it and takes the key from bindings.lua.
     * The default matches HyprlandBinding::kDefaultKeys, so both desktops
     * suggest the same chord. */
    PttBinding b;
    b.trigger = QSettings().value(QLatin1String(kPttTrigger), QStringLiteral("LOGO+grave")).toString();
    return b;
}

void PreferencesDialog::applyPositionMode()
{
    const bool automatic = m_posMode->currentData().toString() == QLatin1String("auto");

    /* Read-only rather than disabled: the numbers still have to be legible,
     * and a greyed-out field reads as "broken" more than as "not yours". */
    m_location->setReadOnly(automatic);
    m_latitude->setReadOnly(automatic);
    m_longitude->setReadOnly(automatic);
    m_lookup->setEnabled(!automatic);

    if (!automatic) {
        Theme::setTone(m_posHint, "");
        m_posHint->setText(tr("Leave both at 0 to publish no position at all. The reflector "
                              "portal then shows no marker, rather than plotting you off the "
                              "coast of Ghana."));
        return;
    }

    const LocationDialog::Place p = LocationDialog::omarchyPlace();
    if (!p.isValid()) {
        Theme::setTone(m_posHint, "warn");
        m_posHint->setText(tr("Omarchy has no position stored yet — run "
                              "omarchy-weather-location, or switch to Manual and look your "
                              "address up here. Until then no position is published."));
        return;
    }

    if (!p.name.isEmpty())
        m_location->setText(p.name);
    m_latitude->setText(QLocale::c().toString(p.latitude, 'f', 6));
    m_longitude->setText(QLocale::c().toString(p.longitude, 'f', 6));

    Theme::setTone(m_posHint, "");
    m_posHint->setText(tr("Following your Omarchy weather location. Change it with "
                          "omarchy-weather-location; SVXConnect picks the new position up "
                          "at its next start."));
}

void PreferencesDialog::load()
{
    m_callsign->setText(m_store.value(QStringLiteral("callsign")));
    m_email->setText(m_store.value(QStringLiteral("email")));
    m_reflector->setText(m_store.value(QStringLiteral("reflector")));
    m_port->setValue(m_store.valueInt(QStringLiteral("port")));
    m_location->setText(m_store.value(QStringLiteral("location")));
    m_latitude->setText(m_store.value(QStringLiteral("latitude")));
    m_longitude->setText(m_store.value(QStringLiteral("longitude")));

    m_posMode->setCurrentIndex(LocationDialog::autoModeSetting() ? 1 : 0);
    applyPositionMode();   /* in automatic mode this overwrites the three above */

    m_enhanced->setChecked(ReflectorFeed::enabledSetting());
    refreshReflectorButton();
    m_portalAuto->setChecked(PortalInfo::autoUpdateSetting());
    refreshJsonEditors();
    m_mapRadius->setValue(QSettings().value(QStringLiteral("map/homeRadiusKm"), 100).toInt());

    m_volume->setValue(m_store.valueInt(QStringLiteral("output_volume_pct")));
    m_micGain->setValue(m_store.valueInt(QStringLiteral("mic_gain")));
    m_micAgc->setChecked(m_store.valueBool(QStringLiteral("mic_agc")));
    m_micAgcTgt->setValue(m_store.valueInt(QStringLiteral("mic_agc_target_pct")));
    m_micAgcTgt->setEnabled(m_micAgc->isChecked());
    m_jitter->setValue(m_store.valueInt(QStringLiteral("jitter_ms")));
    m_tailTrim->setValue(m_store.valueInt(QStringLiteral("tail_trim_ms")));
    m_rogerBeep->setChecked(m_store.valueBool(QStringLiteral("roger_beep")));
    m_rogerMin->setValue(m_store.valueInt(QStringLiteral("roger_beep_min_sec")));
    m_rogerMin->setEnabled(m_rogerBeep->isChecked());

    m_switchable->setText(m_store.value(QStringLiteral("switchable")));
    m_monitored->setText(m_store.value(QStringLiteral("monitored")));
    m_defaultTg->setValue(m_store.valueInt(QStringLiteral("default_tg")));
    m_lockOnStart->setChecked(m_store.valueBool(QStringLiteral("lock_on_start")));
    m_linger->setValue(m_store.valueInt(QStringLiteral("linger_seconds")));
    m_idle->setValue(m_store.valueInt(QStringLiteral("idle_seconds")));
    m_tgOrder->setCurrentIndex(
        m_tgOrder->findData(m_store.value(QStringLiteral("tg_order"))));

    m_txTimeout->setValue(m_store.valueInt(QStringLiteral("tx_timeout_sec")));
    m_logLevel->setCurrentIndex(
        m_logLevel->findData(m_store.value(QStringLiteral("log_level"))));
    m_ctlFifo->setText(m_store.value(QStringLiteral("ctl_fifo")));

    m_notifyTalkers->setChecked(Notifier::enabledSetting());

    m_pttMode->setCurrentIndex(m_pttMode->findData(
        QSettings().value(QLatin1String(kPttMode), QStringLiteral("hold")).toString()));

    refreshPttStatus();
    refreshDeviceLists();
    updatePttPreview();
}

void PreferencesDialog::refreshPttStatus()
{
    PortalBackend portal;
    const PttAvailability a = portal.probe();

    const char *tone = "ok";
    switch (a.state) {
    case PttAvailability::Available:   tone = "ok";    break;
    case PttAvailability::NeedsSetup:  tone = "warn";  break;
    case PttAvailability::Unavailable: tone = "error"; break;
    }
    QString text = a.reason;
    if (!a.instructions.isEmpty())
        text += QLatin1Char('\n') + a.instructions;
    Theme::setTone(m_pttPortalStatus, tone);
    m_pttPortalStatus->setText(text);

    if (HyprlandBinding::isHyprland()) {
        const QString keys = HyprlandBinding::boundKeys();
        setCurrentShortcut(keys);
        if (m_pttKeys && m_pttKeys->text().isEmpty())
            m_pttKeys->setText(keys.isEmpty() ? QLatin1String(HyprlandBinding::kDefaultKeys) : keys);
        if (m_pttUnbind)
            m_pttUnbind->setEnabled(!keys.isEmpty());
    }
}

void PreferencesDialog::setCurrentShortcut(const QString &human)
{
    if (!m_pttShortcut)
        return;

    /* On Hyprland the portal has nothing to say about the key; trust the
     * binding over whatever the window passed in. */
    const QString shown = HyprlandBinding::isHyprland() ? HyprlandBinding::boundKeys() : human;

    if (shown.isEmpty()) {
        m_pttShortcut->setText(tr("not bound"));
        Theme::setTone(m_pttShortcut, "warn");
    } else {
        m_pttShortcut->setText(shown);
        Theme::setTone(m_pttShortcut, "");
    }
}

void PreferencesDialog::updatePttPreview()
{
    const QString fifoPath = m_ctlFifo && !m_ctlFifo->text().trimmed().isEmpty()
                           ? m_ctlFifo->text().trimmed()
                           : m_store.value(QStringLiteral("ctl_fifo"));
    const QString keys = (m_pttKeys && !m_pttKeys->text().trimmed().isEmpty())
                       ? m_pttKeys->text()
                       : QLatin1String(HyprlandBinding::kDefaultKeys);

    if (m_fifoPreview) {
        m_fifoPreview->setText(fifoPath.isEmpty()
            ? tr("The control FIFO is disabled (General › Control FIFO).")
            : HyprlandBinding::fifoExample(fifoPath, QStringLiteral("F12")));
    }

    if (!m_pttPreview)
        return;

    QString why;
    const bool safe = HyprlandBinding::isSafeKeys(keys, &why);
    m_pttKeysWarning->setVisible(!safe);
    m_pttKeysWarning->setText(why);
    Theme::setTone(m_pttKeysWarning, "error");
    m_pttBind->setEnabled(safe);

    m_pttPreview->setText(safe
        ? HyprlandBinding::managedBlock(keys, QString()).trimmed()
        : QString());
    m_pttPreview->setVisible(safe);
}

void PreferencesDialog::onBindHyprland()
{
    const QString keys = HyprlandBinding::normalizeKeys(m_pttKeys->text().trimmed().isEmpty()
                                                        ? QLatin1String(HyprlandBinding::kDefaultKeys)
                                                        : m_pttKeys->text());
    QString why;
    if (!HyprlandBinding::isSafeKeys(keys, &why)) {
        QMessageBox::warning(this, tr("Not a safe push-to-talk key"), why);
        return;
    }

    /* Omarchy's rule for rebinding: say what the key did before, and unbind it
     * explicitly rather than stacking a second bind on top. */
    const QString conflict = HyprlandBinding::conflictFor(keys);
    QString question = tr("Bind %1 to push-to-talk in\n%2?")
                           .arg(keys, HyprlandBinding::bindingsFile());
    if (!conflict.isEmpty())
        question += tr("\n\n%1 is currently bound to “%2”. That binding will be "
                       "unbound first, and the file will say so.").arg(keys, conflict);
    question += tr("\n\nThe file is backed up first, and put back if Hyprland reports "
                   "an error.");

    if (QMessageBox::question(this, tr("Bind in Hyprland"), question,
                              QMessageBox::Yes | QMessageBox::Cancel,
                              QMessageBox::Yes) != QMessageBox::Yes)
        return;

    QString error;
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = HyprlandBinding::install(keys, conflict, &error);
    QGuiApplication::restoreOverrideCursor();

    if (!ok) {
        QMessageBox::critical(this, tr("Could not bind the key"), error);
        return;
    }

    m_pttKeys->setText(keys);
    refreshPttStatus();
    emit pttBindingChanged();
}

void PreferencesDialog::onUnbindHyprland()
{
    if (QMessageBox::question(this, tr("Remove the binding"),
            tr("Remove SVXConnect's push-to-talk block from\n%1?\n\n"
               "A key bound to push-to-talk anywhere else in your Hyprland "
               "config is left alone.").arg(HyprlandBinding::bindingsFile()),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;

    QString error;
    if (!HyprlandBinding::remove(&error)) {
        QMessageBox::critical(this, tr("Could not remove the binding"), error);
        return;
    }
    refreshPttStatus();
    emit pttBindingChanged();
}

void PreferencesDialog::savePtt()
{
    Notifier::setEnabledSetting(m_notifyTalkers->isChecked());

    /* Only the mode is ours to store. The key belongs to the desktop. */
    QSettings qs;
    const QString mode = m_pttMode->currentData().toString();

    const bool changed = qs.value(QLatin1String(kPttMode)).toString() != mode;
    qs.setValue(QLatin1String(kPttMode), mode);

    if (changed)
        emit pttBindingChanged();
}

void PreferencesDialog::onApply()
{
    savePtt();
    commit();
}

void PreferencesDialog::onAccept()
{
    savePtt();
    if (commit())
        accept();
}

void PreferencesDialog::refreshDeviceLists()
{
    if (!m_app)
        return;

    QGuiApplication::setOverrideCursor(Qt::WaitCursor);

    auto fill = [this](QComboBox *box, bool capture, const QString &key) {
        const QString want = m_store.value(key);
        const QSignalBlocker b(box);
        box->clear();
        box->addItem(tr("System default"), QString());
        for (const DevList::Device &d : DevList::list(capture))
            box->addItem(d.name, d.id);

        int idx = box->findData(want);
        if (idx < 0 && !want.isEmpty()) {
            /* The saved device is not present. Show it anyway, marked, so the
             * user can see WHICH device went missing — and so the stale id is
             * not quietly rewritten into the config the CLI also reads. */
            box->addItem(tr("%1 (not connected)").arg(want), want);
            idx = box->count() - 1;
        }
        box->setCurrentIndex(idx < 0 ? 0 : idx);
        return want;
    };

    const QString wantIn  = fill(m_inputDev,  true,  QStringLiteral("input_device"));
    const QString wantOut = fill(m_outputDev, false, QStringLiteral("output_device"));

    QStringList missing;
    if (!wantIn.isEmpty()  && !DevList::resolve(true,  wantIn).matched)  missing << wantIn;
    if (!wantOut.isEmpty() && !DevList::resolve(false, wantOut).matched) missing << wantOut;

    QGuiApplication::restoreOverrideCursor();

    if (missing.isEmpty()) {
        m_devWarning->hide();
    } else {
        m_devWarning->setText(tr("Not currently connected: %1. "
                                 "The system default is being used instead.")
                                  .arg(missing.join(QStringLiteral(", "))));
        m_devWarning->show();
    }
}

bool PreferencesDialog::commit()
{
    /* First, because it is the one part that can refuse: a JSON document that
     * does not parse keeps the dialog open, and nothing else should have been
     * half-saved by then. */
    if (!commitJson())
        return false;

    LocationDialog::setAutoModeSetting(
        m_posMode->currentData().toString() == QLatin1String("auto"));
    ReflectorFeed::setEnabledSetting(m_enhanced->isChecked());
    QSettings().setValue(QStringLiteral("map/homeRadiusKm"), m_mapRadius->value());

    m_store.set(QStringLiteral("callsign"),   m_callsign->text().trimmed());
    m_store.set(QStringLiteral("email"),      m_email->text().trimmed());
    m_store.set(QStringLiteral("reflector"),  m_reflector->text().trimmed());
    m_store.setInt(QStringLiteral("port"),    m_port->value());
    m_store.set(QStringLiteral("location"),   m_location->text().trimmed());

    /* C-locale, 7 decimals, matching what the core emits. */
    bool okLat = false, okLon = false;
    const double la = QLocale::c().toDouble(m_latitude->text(), &okLat);
    const double lo = QLocale::c().toDouble(m_longitude->text(), &okLon);
    m_store.set(QStringLiteral("latitude"),  QString::number(okLat ? la : 0.0, 'f', 7));
    m_store.set(QStringLiteral("longitude"), QString::number(okLon ? lo : 0.0, 'f', 7));

    m_store.set(QStringLiteral("input_device"),  m_inputDev->currentData().toString());
    m_store.set(QStringLiteral("output_device"), m_outputDev->currentData().toString());
    m_store.setInt(QStringLiteral("output_volume_pct"), m_volume->value());
    m_store.setInt(QStringLiteral("mic_gain"),           m_micGain->value());
    m_store.setBool(QStringLiteral("mic_agc"),           m_micAgc->isChecked());
    m_store.setInt(QStringLiteral("mic_agc_target_pct"), m_micAgcTgt->value());
    m_store.setInt(QStringLiteral("jitter_ms"),          m_jitter->value());
    m_store.setInt(QStringLiteral("tail_trim_ms"),       m_tailTrim->value());
    m_store.setBool(QStringLiteral("roger_beep"),        m_rogerBeep->isChecked());
    m_store.setInt(QStringLiteral("roger_beep_min_sec"), m_rogerMin->value());

    m_store.set(QStringLiteral("switchable"), m_switchable->text().trimmed());
    m_store.set(QStringLiteral("monitored"),  m_monitored->text().trimmed());
    m_store.setInt(QStringLiteral("default_tg"),     m_defaultTg->value());
    m_store.setBool(QStringLiteral("lock_on_start"), m_lockOnStart->isChecked());
    m_store.setInt(QStringLiteral("linger_seconds"), m_linger->value());
    m_store.setInt(QStringLiteral("idle_seconds"),   m_idle->value());
    m_store.set(QStringLiteral("tg_order"), m_tgOrder->currentData().toString());

    m_store.setInt(QStringLiteral("tx_timeout_sec"), m_txTimeout->value());
    m_store.set(QStringLiteral("log_level"), m_logLevel->currentData().toString());
    m_store.set(QStringLiteral("ctl_fifo"),  m_ctlFifo->text().trimmed());

    if (!m_store.isDirty())
        return true;

    const QStringList changed = m_store.changedKeys();

    QString error;
    if (!m_store.save(&error)) {
        QMessageBox::critical(this, tr("Could not save the configuration"), error);
        return false;
    }

    applyLiveChanges(changed);

    if (changed.contains(QStringLiteral("switchable"))
        || changed.contains(QStringLiteral("monitored")))
        emit talkgroupsChanged();

    /* Everything except the three live-applicable keys needs a restart. */
    QStringList needsRestart = changed;
    needsRestart.removeAll(QStringLiteral("input_device"));
    needsRestart.removeAll(QStringLiteral("output_device"));
    needsRestart.removeAll(QStringLiteral("output_volume_pct"));
    if (!needsRestart.isEmpty())
        emit restartNeeded();

    return true;
}

void PreferencesDialog::applyLiveChanges(const QStringList &changed)
{
    if (!m_app)
        return;

    if (changed.contains(QStringLiteral("output_volume_pct")))
        app_set_volume(m_app, m_volume->value());

    /* Switching a device tears down the whole miniaudio context and takes one
     * to two seconds, on this thread, because the core is not thread-safe. A
     * wait cursor is the honest minimum. */
    const bool inChanged  = changed.contains(QStringLiteral("input_device"));
    const bool outChanged = changed.contains(QStringLiteral("output_device"));

    if (inChanged || outChanged) {
        QGuiApplication::setOverrideCursor(Qt::WaitCursor);
        m_inputDev->setEnabled(false);
        m_outputDev->setEnabled(false);
        QApplication::processEvents();

        if (inChanged)
            app_set_input_device(m_app, qPrintable(m_inputDev->currentData().toString()));
        if (outChanged)
            app_set_output_device(m_app, qPrintable(m_outputDev->currentData().toString()));

        m_inputDev->setEnabled(true);
        m_outputDev->setEnabled(true);
        QGuiApplication::restoreOverrideCursor();
    }
}

void PreferencesDialog::onTestTone()
{
    if (!m_app)
        return;

    /* app_test_tone() writes the beeps straight into the playback ring and
     * returns silently when the core has no device open, and it lifts the
     * volume but never the mute — so a muted output swallowed the tone with no
     * hint that the button had done anything at all. Both cases are now
     * answered, and the log says which speaker it went to. */
    if (!app_audio_ready(m_app)) {
        QMessageBox::warning(this, tr("No audio device"),
            tr("SVXConnect has no speaker open, so there is nothing to play the test "
               "tone through.\n\nPick a speaker above and press Apply, then try again. "
               "The log (Ctrl+L) shows what the audio backend reported."));
        return;
    }

    if (app_output_muted(m_app)) {
        app_toggle_output_mute(m_app);
        log_info("test tone: the output was muted — unmuting for the test");
    }

    const svx_config *cfg = app_config(m_app);
    log_info("test tone: %d%% into '%s'", app_volume(m_app),
             cfg->output_device[0] ? cfg->output_device : "(system default)");
    app_test_tone(m_app);
}
