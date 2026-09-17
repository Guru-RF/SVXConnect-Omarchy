/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 */
#include "ui/omarchytheme.h"

#include <QApplication>
#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPalette>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace {

QString home()            { return QDir::homePath(); }
QString stateDir()        { return home() + QStringLiteral("/.local/state/omarchy/current"); }
/* SVX_OMARCHY_THEME_DIR points at any theme directory — a stock one under
 * /usr/share/omarchy/themes, say — to preview it without switching the desktop. */
QString themeDir()
{
    const QString forced = qEnvironmentVariable("SVX_OMARCHY_THEME_DIR");
    return forced.isEmpty() ? stateDir() + QStringLiteral("/theme") : forced;
}
QString userOmarchyDir()  { return home() + QStringLiteral("/.config/omarchy"); }
QString userShellToml()   { return userOmarchyDir() + QStringLiteral("/shell.toml"); }
QString fontconfigDir()   { return home() + QStringLiteral("/.config/fontconfig"); }
QString hyprDir()         { return home() + QStringLiteral("/.config/hypr"); }

QString readText(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromUtf8(f.readAll());
}

/* Strip a TOML comment that is not inside quotes. */
QString stripComment(const QString &line)
{
    QChar quote;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (!quote.isNull()) {
            if (c == quote) quote = QChar();
        } else if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            quote = c;
        } else if (c == QLatin1Char('#')) {
            return line.left(i);
        }
    }
    return line;
}

QString unquote(QString v)
{
    v = v.trimmed();
    if (v.size() >= 2
        && (v.front() == QLatin1Char('"') || v.front() == QLatin1Char('\''))
        && v.back() == v.front())
        v = v.mid(1, v.size() - 2);
    return v;
}

/* The same deliberately small TOML walker the shell uses (Color.qml's
 * parseShell): sections, and flat `key = value` pairs, keyed "section.key".
 * colors.toml has no sections, so its keys come back bare. */
QHash<QString, QString> parseToml(const QString &text)
{
    QHash<QString, QString> out;
    QString section;
    static const QRegularExpression sectionRe(QStringLiteral(R"(^\[([A-Za-z0-9_.-]+)\]$)"));
    static const QRegularExpression kvRe(QStringLiteral(R"(^([A-Za-z0-9_-]+)\s*=\s*(.+)$)"));

    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = stripComment(raw).trimmed();
        if (line.isEmpty())
            continue;

        const auto sm = sectionRe.match(line);
        if (sm.hasMatch()) {
            section = sm.captured(1);
            continue;
        }

        const auto kv = kvRe.match(line);
        if (!kv.hasMatch())
            continue;

        const QString key = section.isEmpty() ? kv.captured(1)
                                              : section + QLatin1Char('.') + kv.captured(1);
        out.insert(key, unquote(kv.captured(2)));
    }
    return out;
}

int hexByte(const QString &s, int at)
{
    bool ok = false;
    const int v = s.mid(at, 2).toInt(&ok, 16);
    return ok ? v : -1;
}

/* One colour literal, in any of the spellings an Omarchy theme uses:
 *   #rgb  #rrggbb  #rrggbbaa          colors.toml, shell.toml
 *   rgb(rrggbb)  rgba(rrggbbaa)       Hyprland border colours
 *   rgba(r, g, b, a)                  CSS, which a hand-written theme may use */
QColor parseColorLiteral(QString s)
{
    s = s.trimmed();
    if (s.compare(QLatin1String("transparent"), Qt::CaseInsensitive) == 0)
        return QColor(0, 0, 0, 0);

    if (s.startsWith(QLatin1Char('#'))) {
        const QString h = s.mid(1);
        if (h.size() == 3) {
            const QString e = QString(h.at(0)) + h.at(0) + h.at(1) + h.at(1) + h.at(2) + h.at(2);
            return QColor(hexByte(e, 0), hexByte(e, 2), hexByte(e, 4));
        }
        if (h.size() == 6 || h.size() == 8) {
            const int r = hexByte(h, 0), g = hexByte(h, 2), b = hexByte(h, 4);
            const int a = h.size() == 8 ? hexByte(h, 6) : 255;
            if (r < 0 || g < 0 || b < 0 || a < 0)
                return QColor();
            return QColor(r, g, b, a);
        }
        return QColor();
    }

    static const QRegularExpression hyprHex(
        QStringLiteral(R"(^rgba?\(\s*([0-9A-Fa-f]{6})([0-9A-Fa-f]{2})?\s*\)$)"));
    auto hm = hyprHex.match(s);
    if (hm.hasMatch()) {
        const QString h = hm.captured(1) + (hm.captured(2).isEmpty() ? QStringLiteral("ff")
                                                                    : hm.captured(2));
        return QColor(hexByte(h, 0), hexByte(h, 2), hexByte(h, 4), hexByte(h, 6));
    }

    static const QRegularExpression cssRgb(
        QStringLiteral(R"(^rgba?\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*(?:,\s*([0-9.]+)\s*)?\)$)"));
    auto cm = cssRgb.match(s);
    if (cm.hasMatch()) {
        QColor c(cm.captured(1).toInt(), cm.captured(2).toInt(), cm.captured(3).toInt());
        if (!cm.captured(4).isEmpty()) {
            const double a = cm.captured(4).toDouble();
            c.setAlphaF(static_cast<float>(a > 1.0 ? a / 255.0 : a));
        }
        return c;
    }

    return QColor();
}

QColor mix(const QColor &a, const QColor &b, double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return QColor::fromRgbF(
        static_cast<float>(a.redF()   + (b.redF()   - a.redF())   * t),
        static_cast<float>(a.greenF() + (b.greenF() - a.greenF()) * t),
        static_cast<float>(a.blueF()  + (b.blueF()  - a.blueF())  * t));
}

double clampAlpha(double v, double fallback)
{
    return std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : fallback;
}

} // namespace

/* ---------------------------------------------------------------------------
 * Lifetime
 * ------------------------------------------------------------------------- */

OmarchyTheme &OmarchyTheme::get()
{
    static OmarchyTheme instance;
    return instance;
}

OmarchyTheme::OmarchyTheme()
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(300);
    connect(&m_debounce, &QTimer::timeout, this, &OmarchyTheme::reload);
}

void OmarchyTheme::install()
{
    if (m_installed)
        return;
    m_installed = true;

    /* Fusion is the one Qt style that honours a stylesheet completely and
     * draws nothing of its own that would clash — the platform theme on this
     * desktop is gtk3, whose palette would otherwise leak into every control
     * the stylesheet does not mention. */
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QApplication::setAttribute(Qt::AA_DontShowIconsInMenus, true);

    m_watch = new QFileSystemWatcher(this);
    connect(m_watch, &QFileSystemWatcher::fileChanged,      this, [this]() { m_debounce.start(); });
    connect(m_watch, &QFileSystemWatcher::directoryChanged, this, [this]() { m_debounce.start(); });

    reload();
}

/* ---------------------------------------------------------------------------
 * Reading the theme
 * ------------------------------------------------------------------------- */

void OmarchyTheme::reload()
{
    const QString colorsText = readText(themeDir() + QStringLiteral("/colors.toml"));
    const QHash<QString, QString> colors = parseToml(colorsText);
    m_found = !colors.isEmpty();

    m_name = qEnvironmentVariableIsSet("SVX_OMARCHY_THEME_DIR")
           ? QFileInfo(themeDir()).fileName()
           : readText(stateDir() + QStringLiteral("/theme.name")).trimmed();

    /* ---- palette ---- */
    Palette p;   /* Tokyo Night defaults, for a machine without Omarchy */

    auto pick = [&colors](std::initializer_list<const char *> keys, const QColor &fallback) {
        for (const char *k : keys) {
            const QColor c = parseColorLiteral(colors.value(QLatin1String(k)));
            if (c.isValid())
                return c;
        }
        return fallback;
    };

    if (m_found) {
        p.background = pick({"background", "color0"}, p.background);
        p.foreground = pick({"foreground", "color7"}, p.foreground);
        p.red        = pick({"red", "color1"},        p.red);
        p.green      = pick({"green", "color2"},      p.green);
        p.yellow     = pick({"yellow", "color3"},     p.yellow);
        p.blue       = pick({"blue", "color4"},       p.blue);
        p.magenta    = pick({"magenta", "color5"},    p.magenta);
        p.cyan       = pick({"cyan", "color6"},       p.cyan);
        p.orange     = pick({"orange", "bright_yellow", "color11"}, mix(p.red, p.yellow, 0.5));
        p.accent     = pick({"accent", "color4"},     p.blue);
        p.muted      = pick({"muted", "color8"},      mix(p.foreground, p.background, 0.6));
        p.selection  = pick({"selection", "selection_background"}, mix(p.background, p.foreground, 0.15));

        p.darkBackground    = pick({"dark_background"},    mix(p.background, Qt::black, 0.25));
        p.darkerBackground  = pick({"darker_background"},  mix(p.background, Qt::black, 0.45));
        p.lighterBackground = pick({"lighter_background"}, mix(p.background, p.foreground, 0.08));
        p.darkForeground    = pick({"dark_foreground"},    mix(p.foreground, p.background, 0.45));
        p.lightForeground   = pick({"light_foreground"},   mix(p.foreground, Qt::white, 0.08));
        p.brightForeground  = pick({"bright_foreground", "color15"}, mix(p.foreground, Qt::white, 0.15));

        const QString mode = colors.value(QStringLiteral("mode")).toLower();
        p.dark = mode.isEmpty() ? p.background.lightnessF() < 0.5 : mode != QLatin1String("light");
    }
    m_palette = p;

    /* ---- shell.toml: the theme's, then the user's override on top ---- */
    QHash<QString, QString> shell = parseToml(readText(themeDir() + QStringLiteral("/shell.toml")));
    const QHash<QString, QString> user = parseToml(readText(userShellToml()));
    for (auto it = user.cbegin(); it != user.cend(); ++it)
        shell.insert(it.key(), it.value());

    /* A value may be a literal, a palette role, or a reference to another
     * shell key ("hyprland.active-border"), and a gradient contributes its
     * first colour stop — exactly Color.qml's flatColor(). */
    std::function<QColor(const QString &, const QColor &, int)> resolve;
    resolve = [&](const QString &raw, const QColor &fallback, int depth) -> QColor {
        QString token;
        static const QRegularExpression ws(QStringLiteral(R"(\s+(?![^(]*\)))"));
        const QStringList parts = raw.trimmed().split(ws, Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            if (!part.endsWith(QLatin1String("deg"))) { token = part; break; }
        }
        if (token.isEmpty())
            return fallback;

        if (depth < 4 && shell.contains(token) && shell.value(token) != token)
            return resolve(shell.value(token), fallback, depth + 1);

        const QString role = token.toLower();
        if (role == QLatin1String("foreground") || role == QLatin1String("text")) return p.foreground;
        if (role == QLatin1String("background")) return p.background;
        if (role == QLatin1String("accent"))     return p.accent;
        if (role == QLatin1String("urgent"))     return p.red;
        if (role == QLatin1String("muted"))      return p.muted;

        const QColor c = parseColorLiteral(token);
        return c.isValid() ? c : fallback;
    };

    auto num = [&shell](const char *key, double fallback) {
        bool ok = false;
        const double v = shell.value(QLatin1String(key)).toDouble(&ok);
        return ok ? v : fallback;
    };
    auto flag = [&shell](const char *key, bool fallback) {
        const QString v = shell.value(QLatin1String(key)).toLower();
        if (v == QLatin1String("true")  || v == QLatin1String("1") || v == QLatin1String("yes")) return true;
        if (v == QLatin1String("false") || v == QLatin1String("0") || v == QLatin1String("no"))  return false;
        return fallback;
    };
    auto surface = [&](const char *colorKey, const char *alphaKey,
                       const QColor &fallback, double alphaFallback) {
        QColor c = resolve(shell.value(QLatin1String(colorKey)), fallback, 0);
        const double a = clampAlpha(num(alphaKey, alphaFallback), alphaFallback);
        c.setAlphaF(static_cast<float>(c.alphaF() * a));
        return c;
    };

    Tokens t;
    t.normalFill        = clampAlpha(num("controls.normal-fill-alpha",       t.normalFill),        t.normalFill);
    t.hoverFill         = clampAlpha(num("controls.hover-cursor-fill-alpha", t.hoverFill),         t.hoverFill);
    t.selectedFill      = clampAlpha(num("controls.selected-fill-alpha",     t.selectedFill),      t.selectedFill);
    t.pressedFill       = clampAlpha(num("controls.pressed-fill-alpha",      t.pressedFill),       t.pressedFill);
    t.selectionFill     = clampAlpha(num("controls.selection-fill-alpha",    t.selectionFill),     t.selectionFill);
    t.normalBorderAlpha = clampAlpha(num("controls.normal-border-alpha",     t.normalBorderAlpha), t.normalBorderAlpha);
    t.hoverBorderAlpha  = clampAlpha(num("controls.hover-cursor-border-alpha", t.hoverBorderAlpha), t.hoverBorderAlpha);
    t.focusBorderAlpha  = clampAlpha(num("controls.focus-border-alpha",      t.hoverBorderAlpha),  t.hoverBorderAlpha);
    t.normalBorderWidth = std::max(0, static_cast<int>(std::lround(num("controls.normal-border-width", 1))));

    t.activeBorder           = resolve(shell.value(QStringLiteral("hyprland.active-border")), p.accent, 0);
    t.activeBorderForeground = resolve(shell.value(QStringLiteral("hyprland.active-border-foreground")), p.foreground, 0);

    t.menuBorder       = surface("menu.border", "menu.border-alpha", t.activeBorderForeground, 1.0);
    t.menuSelectedText = resolve(shell.value(QStringLiteral("menu.selected-text")), p.accent, 0);
    t.menuSelectedBackgroundAlpha =
        clampAlpha(num("menu.selected-background-alpha", 0.08), 0.08);
    t.tooltipBorder    = surface("tooltip.border", "tooltip.border-alpha", t.activeBorderForeground, 1.0);
    t.popupBorder      = surface("popups.border", "popups.border-alpha", t.activeBorder, 1.0);

    t.fontBaseSize         = std::max(1, static_cast<int>(std::lround(num("font.base-size", 12))));
    t.spacingScale         = std::max(0.0, num("spacing.scale", 1.0));
    t.spacingScaleWithFont = flag("spacing.scale-with-font", true);
    t.radius               = m_tokens.radius;   /* owned by refreshRounding() */

    m_fontOverrides.clear();
    for (const char *k : {"caption", "body-small", "body", "subtitle", "title", "heading", "display"}) {
        bool ok = false;
        const int v = shell.value(QStringLiteral("font.") + QLatin1String(k)).toInt(&ok);
        if (ok && v > 0)
            m_fontOverrides.insert(QLatin1String(k), v);
    }

    m_tokens = t;

    resolveFont();
    apply();
    rewatch();
    refreshRounding();
}

void OmarchyTheme::resolveFont()
{
    /* The shell binds font.family to "monospace" and lets fontconfig resolve
     * it. Qt caches fontconfig's configuration for the life of the process, so
     * a later `omarchy font set` would never reach an alias lookup made here —
     * ask fc-match for the concrete family instead, every reload. */
    QProcess fc;
    fc.start(QStringLiteral("fc-match"),
             {QStringLiteral("-f"), QStringLiteral("%{family[0]}"), QStringLiteral("monospace")});
    QString family;
    if (fc.waitForFinished(1500) && fc.exitCode() == 0)
        family = QString::fromUtf8(fc.readAllStandardOutput()).trimmed();

    m_fontFamily = (!family.isEmpty() && QFontDatabase::hasFamily(family))
                       ? family
                       : QStringLiteral("monospace");
}

void OmarchyTheme::refreshRounding()
{
    /* Asynchronous, like the shell's own hyprctl probe: a missing or wedged
     * Hyprland must never stall the window. */
    auto *proc = new QProcess(this);
    connect(proc, &QProcess::finished, this, [this, proc](int code, QProcess::ExitStatus st) {
        proc->deleteLater();
        if (st != QProcess::NormalExit || code != 0)
            return;
        const QJsonObject o = QJsonDocument::fromJson(proc->readAllStandardOutput()).object();
        if (!o.contains(QStringLiteral("int")))
            return;
        const int r = std::max(0, o.value(QStringLiteral("int")).toInt());
        if (r != m_tokens.radius) {
            m_tokens.radius = r;
            apply();
        }
    });
    connect(proc, &QProcess::errorOccurred, proc, &QObject::deleteLater);
    proc->start(QStringLiteral("hyprctl"),
                {QStringLiteral("-j"), QStringLiteral("getoption"), QStringLiteral("decoration:rounding")});
}

void OmarchyTheme::rewatch()
{
    if (!m_watch)
        return;

    if (!m_watch->files().isEmpty())
        m_watch->removePaths(m_watch->files());
    if (!m_watch->directories().isEmpty())
        m_watch->removePaths(m_watch->directories());

    /* Directories catch the theme swap (a rename) and files created later;
     * files catch in-place edits. See the header for why both. */
    QStringList wanted {
        stateDir(),
        themeDir(),
        themeDir() + QStringLiteral("/colors.toml"),
        themeDir() + QStringLiteral("/shell.toml"),
        stateDir() + QStringLiteral("/theme.name"),
        userOmarchyDir(),
        userShellToml(),
        fontconfigDir(),
        fontconfigDir() + QStringLiteral("/fonts.conf"),
        hyprDir(),
        hyprDir() + QStringLiteral("/looknfeel.lua"),
    };
    wanted.erase(std::remove_if(wanted.begin(), wanted.end(),
                                [](const QString &p) { return !QFileInfo::exists(p); }),
                 wanted.end());
    if (!wanted.isEmpty())
        m_watch->addPaths(wanted);
}

/* ---------------------------------------------------------------------------
 * Tokens
 * ------------------------------------------------------------------------- */

int OmarchyTheme::fontPx(Font role) const
{
    const char *key = nullptr;
    double mult = 1.0;
    switch (role) {
    case Font::Caption:   key = "caption";    mult = 0.833; break;
    case Font::BodySmall: key = "body-small"; mult = 0.917; break;
    case Font::Body:      key = "body";       mult = 1.000; break;
    case Font::Subtitle:  key = "subtitle";   mult = 1.083; break;
    case Font::Title:     key = "title";      mult = 1.167; break;
    case Font::Heading:   key = "heading";    mult = 1.333; break;
    case Font::Headline:  key = nullptr;      mult = 1.667; break;
    case Font::Display:   key = "display";    mult = 2.000; break;
    }
    if (key && m_fontOverrides.contains(QLatin1String(key)))
        return m_fontOverrides.value(QLatin1String(key));
    return std::max(1, static_cast<int>(std::lround(m_tokens.fontBaseSize * mult)));
}

QFont OmarchyTheme::font(Font role, bool bold) const
{
    QFont f(m_fontFamily);
    f.setPixelSize(fontPx(role));
    f.setBold(bold);
    return f;
}

int OmarchyTheme::space(int px) const
{
    if (px <= 0)
        return 0;
    const double fontScale = std::max(1.0 / 12.0, m_tokens.fontBaseSize / 12.0);
    const double scale = m_tokens.spacingScale * (m_tokens.spacingScaleWithFont ? fontScale : 1.0);
    return std::max(1, static_cast<int>(std::lround(px * scale)));
}

QColor OmarchyTheme::alpha(QColor c, double a)
{
    c.setAlphaF(static_cast<float>(std::clamp(a, 0.0, 1.0) * c.alphaF()));
    return c;
}

QString OmarchyTheme::css(const QColor &c)
{
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(c.red()).arg(c.green()).arg(c.blue())
        .arg(QString::number(c.alphaF(), 'f', 3));
}

/* ---------------------------------------------------------------------------
 * Applying
 * ------------------------------------------------------------------------- */

QString OmarchyTheme::writeGlyphs() const
{
    /* QSS can only draw an arrow or a check mark from an image. Render the
     * three it needs as SVG in the theme's own colours, into a directory keyed
     * by those colours so a theme switch never shows a stale glyph. */
    const Palette &p = m_palette;
    const QString fg = p.foreground.name(QColor::HexRgb);
    const QString bg = p.background.name(QColor::HexRgb);
    const QString ac = m_tokens.menuSelectedText.name(QColor::HexRgb);

    const QByteArray key = QCryptographicHash::hash((fg + bg + ac).toUtf8(),
                                                    QCryptographicHash::Sha1).toHex().left(12);
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                      + QStringLiteral("/glyphs/") + QString::fromLatin1(key);
    QDir().mkpath(dir);

    auto write = [&dir](const char *name, const QString &body) {
        const QString path = dir + QLatin1Char('/') + QLatin1String(name);
        if (QFileInfo::exists(path))
            return;
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(body.toUtf8());
    };

    const QString head = QStringLiteral(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 16 16\" width=\"16\" height=\"16\">");
    auto stroke = [&head](const QString &colour, const char *path) {
        return head + QStringLiteral("<path d=\"%1\" fill=\"none\" stroke=\"%2\" stroke-width=\"2\" "
                                     "stroke-linecap=\"square\" stroke-linejoin=\"miter\"/></svg>")
                          .arg(QLatin1String(path), colour);
    };

    write("chevron-down.svg", stroke(fg, "M3 6 L8 11 L13 6"));
    write("chevron-up.svg",   stroke(fg, "M3 10 L8 5 L13 10"));
    write("check-bg.svg",     stroke(bg, "M3 8.5 L6.5 12 L13 4.5"));
    write("check-accent.svg", stroke(ac, "M3 8.5 L6.5 12 L13 4.5"));
    return dir;
}

void OmarchyTheme::apply()
{
    const QString sheet = buildStyleSheet();
    const QString signature = sheet + m_fontFamily
                            + m_palette.background.name() + m_palette.foreground.name();
    if (signature == m_appliedSignature)
        return;
    m_appliedSignature = signature;

    const Palette &p = m_palette;
    const Tokens  &t = m_tokens;

    QPalette pal;
    auto set = [&pal](QPalette::ColorRole role, const QColor &c) {
        pal.setColor(QPalette::All, role, c);
    };
    set(QPalette::Window,          p.background);
    set(QPalette::WindowText,      p.foreground);
    set(QPalette::Base,            p.background);
    set(QPalette::AlternateBase,   p.lighterBackground);
    set(QPalette::Text,            p.foreground);
    set(QPalette::Button,          p.background);
    set(QPalette::ButtonText,      p.foreground);
    set(QPalette::BrightText,      p.brightForeground);
    set(QPalette::Highlight,       mix(p.background, p.foreground, t.selectionFill));
    set(QPalette::HighlightedText, p.brightForeground);
    set(QPalette::ToolTipBase,     p.background);
    set(QPalette::ToolTipText,     p.foreground);
    set(QPalette::PlaceholderText, mix(p.foreground, p.background, 0.5));
    set(QPalette::Link,            p.accent);
    set(QPalette::LinkVisited,     p.magenta);
    set(QPalette::Light,           mix(p.background, p.foreground, 0.20));
    set(QPalette::Midlight,        mix(p.background, p.foreground, 0.12));
    set(QPalette::Mid,             mix(p.background, p.foreground, 0.30));
    set(QPalette::Dark,            p.darkBackground);
    set(QPalette::Shadow,          p.darkerBackground);
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    set(QPalette::Accent,          p.accent);
#endif
    const QColor disabled = mix(p.foreground, p.background, 0.55);
    pal.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    pal.setColor(QPalette::Disabled, QPalette::Text,       disabled);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    QGuiApplication::styleHints()->setColorScheme(p.dark ? Qt::ColorScheme::Dark
                                                         : Qt::ColorScheme::Light);
#endif
    QApplication::setPalette(pal);
    /* The application font covers QPainter text and anything unstyled. It is
     * NOT enough for widgets: the platform theme (gtk3 on Omarchy) keeps a
     * per-class font table for QLabel, QPushButton, QCheckBox and friends, and
     * rebuilds it from GTK on every theme-change event — which setColorScheme()
     * above posts. Fonts set per class are wiped a moment later, so the family
     * and size are carried by the stylesheet's `*` rule instead. */
    QApplication::setFont(font(Font::Body));
    qApp->setStyleSheet(sheet);

    /* Custom-painted widgets read the theme in paintEvent; make sure they get
     * one even where the stylesheet change did not cause a repolish. */
    const QWidgetList all = QApplication::allWidgets();
    for (QWidget *w : all)
        w->update();

    emit changed();
}

QString OmarchyTheme::buildStyleSheet() const
{
    const Palette &p = m_palette;
    const Tokens  &t = m_tokens;
    const QString glyphs = writeGlyphs();

    auto fgA = [&p](double a) { return css(alpha(p.foreground, a)); };

    const int knob   = space(14);
    const int track  = std::max(4, space(4));
    const int border = std::max(1, space(2));

    /* Placeholders are replaced longest-first-safe: every name is wrapped in
     * braces, so {s1} can never match inside {s12}. */
    QList<QPair<QString, QString>> vars {
        {QStringLiteral("bg"),          css(p.background)},
        {QStringLiteral("bgDark"),      css(p.darkBackground)},
        {QStringLiteral("bgLighter"),   css(p.lighterBackground)},
        {QStringLiteral("fg"),          css(p.foreground)},
        {QStringLiteral("fgBright"),    css(p.brightForeground)},
        {QStringLiteral("fgDark"),      css(p.darkForeground)},
        {QStringLiteral("fgDim"),       fgA(0.62)},
        {QStringLiteral("fgFaint"),     fgA(0.45)},
        {QStringLiteral("fgDisabled"),  fgA(0.40)},
        {QStringLiteral("section"),     css(p.foreground.darker(140))},
        {QStringLiteral("rule"),        fgA(0.12)},
        {QStringLiteral("accent"),      css(p.accent)},
        {QStringLiteral("red"),         css(p.red)},
        {QStringLiteral("green"),       css(p.green)},
        {QStringLiteral("yellow"),      css(p.yellow)},
        {QStringLiteral("redWash"),     css(alpha(p.red, 0.14))},
        {QStringLiteral("greenWash"),   css(alpha(p.green, 0.14))},
        {QStringLiteral("yellowWash"),  css(alpha(p.yellow, 0.14))},
        {QStringLiteral("fillNormal"),  fgA(t.normalFill)},
        {QStringLiteral("fillHover"),   fgA(t.hoverFill)},
        {QStringLiteral("fillSelected"), fgA(t.selectedFill)},
        {QStringLiteral("fillPressed"), fgA(t.pressedFill)},
        {QStringLiteral("fillSelection"), fgA(t.selectionFill)},
        {QStringLiteral("borderNormal"), fgA(t.normalBorderAlpha)},
        {QStringLiteral("borderHover"), fgA(t.hoverBorderAlpha)},
        {QStringLiteral("activeBorder"), css(t.activeBorder)},
        {QStringLiteral("menuBorder"),  css(t.menuBorder)},
        {QStringLiteral("menuSelText"), css(t.menuSelectedText)},
        {QStringLiteral("menuSelFill"), fgA(t.menuSelectedBackgroundAlpha)},
        {QStringLiteral("tooltipBorder"), css(t.tooltipBorder)},
        {QStringLiteral("r"),           QString::number(t.radius)},
        {QStringLiteral("bw"),          QString::number(t.normalBorderWidth)},
        {QStringLiteral("fCaption"),    QString::number(fontPx(Font::Caption))},
        {QStringLiteral("fSmall"),      QString::number(fontPx(Font::BodySmall))},
        {QStringLiteral("fBody"),       QString::number(fontPx(Font::Body))},
        {QStringLiteral("fontFamily"),  m_fontFamily},
        {QStringLiteral("fTitle"),      QString::number(fontPx(Font::Title))},
        {QStringLiteral("fHeading"),    QString::number(fontPx(Font::Heading))},
        {QStringLiteral("fHeadline"),   QString::number(fontPx(Font::Headline))},
        {QStringLiteral("padX"),        QString::number(space(10))},
        {QStringLiteral("padY"),        QString::number(space(6))},
        {QStringLiteral("inputPadY"),   QString::number(space(5))},
        {QStringLiteral("pttH"),        QString::number(space(34))},
        {QStringLiteral("knob"),        QString::number(knob - 2 * border)},
        {QStringLiteral("knobB"),       QString::number(border)},
        {QStringLiteral("knobM"),       QString::number((knob - track) / 2)},
        {QStringLiteral("knobR"),       QString::number(knob / 2)},
        {QStringLiteral("track"),       QString::number(track)},
        {QStringLiteral("trackR"),      QString::number(track / 2)},
        {QStringLiteral("dotR"),        QString::number(space(8) / 2)},
        {QStringLiteral("glyphDown"),   glyphs + QStringLiteral("/chevron-down.svg")},
        {QStringLiteral("glyphUp"),     glyphs + QStringLiteral("/chevron-up.svg")},
        {QStringLiteral("glyphCheck"),  glyphs + QStringLiteral("/check-bg.svg")},
        {QStringLiteral("glyphCheckAccent"), glyphs + QStringLiteral("/check-accent.svg")},
    };
    for (int n : {2, 4, 6, 8, 10, 12, 14, 16, 20, 24, 72})
        vars.append({QStringLiteral("s%1").arg(n), QString::number(space(n))});

    QString s = QStringLiteral(R"QSS(
* { font-family: "{fontFamily}"; font-size: {fBody}px; }

QToolTip {
    background: {bg}; color: {fg};
    border: 1px solid {tooltipBorder};
    padding: {padY}px {padX}px;
    font-size: {fSmall}px;
}

QMainWindow, QDialog, QMessageBox { background: {bg}; }
QLabel { color: {fg}; background: transparent; }

QFrame[role="rule"]  { background: {rule}; border: none; min-height: 1px; max-height: 1px; }
QFrame[role="vrule"] { background: {rule}; border: none; min-width: 1px;  max-width: 1px; }

/* ---- buttons: the shell's Button.qml state ladder ---- */
QPushButton, QToolButton {
    background: {fillNormal}; color: {fg};
    border: {bw}px solid {borderNormal}; border-radius: {r}px;
    padding: {padY}px {padX}px;
}
QPushButton:hover, QToolButton:hover     { background: {fillHover}; border-color: {borderHover}; }
QPushButton:pressed, QToolButton:pressed { background: {fillPressed}; }
QPushButton:checked, QToolButton:checked { background: {fillSelected}; color: {fgBright}; }
QPushButton:focus, QToolButton:focus     { border-color: {activeBorder}; }
QPushButton:default                      { border-color: {activeBorder}; }
QPushButton:disabled, QToolButton:disabled {
    color: {fgDisabled}; background: transparent; border-color: {rule};
}
QPushButton[flat="true"] { background: transparent; border-color: transparent; }
QPushButton[flat="true"]:hover { background: {fillHover}; border-color: {borderHover}; }
QToolButton::menu-indicator { image: none; width: 0px; }

QToolButton[role="icon"] {
    background: transparent; border-color: transparent;
    padding: {s4}px {s8}px; font-size: {fTitle}px;
}
QToolButton[role="icon"]:hover   { background: {fillHover}; border-color: {borderHover}; }
QToolButton[role="icon"]:checked { background: {fillSelected}; color: {accent}; }

/* Controls that sit ON the map, over raster tiles rather than over the
 * window's own background: they need a solid surface and a border of their
 * own, or they are illegible over a city. */
QToolButton[role="map"] {
    background: {bg}; color: {fgBright};
    border: {bw}px solid {borderNormal}; border-radius: {r}px;
    padding: {s2}px {s6}px; font-size: {fBody}px;
    min-width: {s16}px;
}
QToolButton[role="map"]:hover    { background: {fillSelection}; border-color: {borderHover}; }
QToolButton[role="map"]:disabled { color: {fgDisabled}; border-color: {rule}; }

*[role="mapcard"] {
    background: {bg}; color: {fg};
    border: {bw}px solid {menuBorder}; border-radius: {r}px;
}

QToolButton[role="lock"] { padding: {s2}px {padX}px; }
QToolButton[role="lock"][locked="true"] {
    background: {yellow}; color: {bg}; border-color: {yellow}; font-weight: bold;
}

QPushButton[role="ptt"] {
    font-size: {fTitle}px; font-weight: bold;
    min-height: {pttH}px;
}
QPushButton[role="ptt"][tx="true"],
QPushButton[role="ptt"][tx="true"]:hover,
QPushButton[role="ptt"][tx="true"]:pressed {
    background: {red}; color: {bg}; border-color: {red};
}

/* ---- inputs ---- */
QLineEdit, QSpinBox, QComboBox, QPlainTextEdit {
    background: {fillNormal}; color: {fg};
    border: {bw}px solid {borderNormal}; border-radius: {r}px;
    padding: {inputPadY}px {padX}px;
    selection-background-color: {fillSelection}; selection-color: {fgBright};
}
QPlainTextEdit { padding: {s6}px; }
QLineEdit:hover, QSpinBox:hover, QComboBox:hover { background: {fillHover}; }
QLineEdit:focus, QSpinBox:focus, QComboBox:focus, QPlainTextEdit:focus { border-color: {activeBorder}; }
QLineEdit:disabled, QSpinBox:disabled, QComboBox:disabled {
    color: {fgDisabled}; border-color: {rule}; background: transparent;
}

QSpinBox { padding-right: {s20}px; }
QSpinBox::up-button, QSpinBox::down-button {
    subcontrol-origin: border; width: {s20}px; border: none; background: transparent;
}
QSpinBox::up-button   { subcontrol-position: top right; }
QSpinBox::down-button { subcontrol-position: bottom right; }
QSpinBox::up-button:hover, QSpinBox::down-button:hover { background: {fillHover}; }
QSpinBox::up-arrow   { image: url("{glyphUp}");   width: {s8}px; height: {s8}px; }
QSpinBox::down-arrow { image: url("{glyphDown}"); width: {s8}px; height: {s8}px; }

QComboBox { padding-right: {s24}px; }
QComboBox::drop-down {
    subcontrol-origin: padding; subcontrol-position: center right;
    width: {s24}px; border: none;
}
QComboBox::down-arrow { image: url("{glyphDown}"); width: {s10}px; height: {s10}px; }
QComboBox QAbstractItemView {
    background: {bg}; color: {fg};
    border: 1px solid {menuBorder}; outline: 0; padding: {s4}px;
    selection-background-color: {menuSelFill}; selection-color: {menuSelText};
}

QCheckBox { spacing: {s8}px; color: {fg}; background: transparent; }
QCheckBox:disabled { color: {fgDisabled}; }
QCheckBox::indicator {
    width: {s14}px; height: {s14}px;
    border: {bw}px solid {borderNormal}; border-radius: {r}px; background: {fillNormal};
}
QCheckBox::indicator:hover   { background: {fillHover}; }
QCheckBox::indicator:checked { background: {accent}; border-color: {accent}; image: url("{glyphCheck}"); }

/* ---- menus: the shell's [menu] surface ---- */
QMenu {
    background: {bg}; color: {fg};
    border: 1px solid {menuBorder}; padding: {s4}px;
}
QMenu::item {
    padding: {padY}px {s24}px {padY}px {padX}px;
    border: 1px solid transparent; border-radius: {r}px;
}
QMenu::item:selected { background: {menuSelFill}; color: {menuSelText}; border-color: {borderHover}; }
QMenu::item:disabled { color: {fgDisabled}; }
QMenu::separator { height: 1px; background: {rule}; margin: {s4}px {s6}px; }
QMenu::indicator { width: {s12}px; height: {s12}px; padding-left: {s4}px; }
QMenu::indicator:checked { image: url("{glyphCheckAccent}"); }

/* ---- tabs ---- */
QTabWidget::pane { border: none; border-top: 1px solid {rule}; top: -1px; }
QTabBar::tab {
    background: transparent; color: {fgDim};
    padding: {padY}px {s12}px; margin-right: {s4}px;
    border: 1px solid transparent; border-radius: {r}px;
}
QTabBar::tab:hover    { background: {fillHover}; color: {fg}; }
QTabBar::tab:selected { background: {fillSelected}; color: {fgBright}; font-weight: bold; }

QGroupBox {
    border: 1px solid {rule}; border-radius: {r}px;
    margin-top: {s20}px; padding: {s12}px {s8}px {s8}px {s8}px;
}
QGroupBox::title {
    subcontrol-origin: margin; subcontrol-position: top left;
    left: {s8}px; padding: 0px {s4}px; color: {section};
}

/* ---- scrolling ---- */
QScrollBar:vertical   { background: transparent; width: {s8}px;  margin: 0px; }
QScrollBar:horizontal { background: transparent; height: {s8}px; margin: 0px; }
QScrollBar::handle:vertical   { background: {fillSelected}; min-height: {s24}px; }
QScrollBar::handle:horizontal { background: {fillSelected}; min-width: {s24}px; }
QScrollBar::handle:hover { background: {fillSelection}; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0px; height: 0px; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

/* ---- the shell's PanelSlider ---- */
QSlider::groove:horizontal { height: {track}px; background: {fillSelected}; border-radius: {trackR}px; }
QSlider::sub-page:horizontal { background: {fg}; border-radius: {trackR}px; }
QSlider::handle:horizontal {
    background: {fg}; width: {knob}px; height: {knob}px;
    margin: -{knobM}px 0px; border-radius: {knobR}px; border: {knobB}px solid {bg};
}
QSlider::handle:horizontal:hover { background: {fgBright}; }

QSplitter::handle { background: {rule}; }
QSplitter::handle:vertical { height: 1px; }

/* ---- label roles ---- */
QLabel[role="section"]  { color: {section}; font-size: {fCaption}px; font-weight: bold; }
QLabel[role="hint"]     { color: {fgDim};   font-size: {fSmall}px; }
QLabel[role="dim"]      { color: {fgDim};   font-size: {fSmall}px; }
QLabel[role="state"]    { color: {fgBright}; font-weight: bold; }
QLabel[role="value"]    { color: {fgBright}; font-weight: bold; }
QLabel[role="headline"] { color: {fgBright}; font-size: {fHeadline}px; font-weight: bold; }
QLabel[role="empty"]    { color: {fgFaint}; font-size: {fSmall}px; padding: {s10}px 0px; }
QLabel[role="time"]     { color: {fgDim};   font-size: {fCaption}px; }
QLabel[role="call"]     { color: {fg}; }
QLabel[role="call"][live="true"] { color: {fgBright}; font-weight: bold; }
QLabel[role="caption"]  { color: {section}; font-size: {fCaption}px; font-weight: bold; }
QLabel[role="glyph"]    { color: {fgDim}; font-size: {fTitle}px; }
QLabel[role="badge"] {
    color: {green}; background: {greenWash}; border-radius: {r}px;
    padding: 0px {s6}px; font-size: {fCaption}px; font-weight: bold;
}
QLabel[role="chip"] {
    color: {fgDim}; background: {fillHover}; border-radius: {r}px;
    padding: 0px {s6}px; font-size: {fCaption}px;
}
QLabel[role="code"] {
    color: {fg}; background: {bgDark};
    border: 1px solid {rule}; border-radius: {r}px;
    padding: {s8}px {s10}px; font-size: {fSmall}px;
}

QLabel[tone="ok"]    { color: {green}; }
QLabel[tone="warn"]  { color: {yellow}; }
QLabel[tone="error"] { color: {red}; }

QLabel[role="dot"]                 { border-radius: {dotR}px; background: {fgFaint}; }
QLabel[role="dot"][tone="ok"]      { background: {green}; }
QLabel[role="dot"][tone="warn"]    { background: {yellow}; }
QLabel[role="dot"][tone="error"]   { background: {red}; }
QLabel[role="dot"][tone="down"]    { background: {fgDark}; }

/* ---- notice bars ---- */
*[role="banner"] {
    background: {yellowWash}; color: {yellow};
    border: none; border-left: 2px solid {yellow};
}
*[role="banner"][tone="ok"]    { background: {greenWash}; color: {green}; border-left-color: {green}; }
*[role="banner"][tone="error"] { background: {redWash};   color: {red};   border-left-color: {red}; }
*[role="banner"] QLabel              { color: {yellow}; }
*[role="banner"][tone="ok"] QLabel    { color: {green}; }
*[role="banner"][tone="error"] QLabel { color: {red}; }

/* ---- rows ---- */
*[role="row"] { border: 1px solid transparent; border-radius: {r}px; }
*[role="row"]:hover { background: {fillHover}; border-color: {borderHover}; }

QPlainTextEdit[role="log"] {
    background: {bgDark}; border: none; border-radius: 0px;
    font-size: {fSmall}px; padding: {s6}px {s12}px;
}

/* The gtk3 platform theme puts stock icons on OK / Cancel / Apply. */
QDialogButtonBox { dialogbuttonbox-buttons-have-icons: 0; }
QDialogButtonBox QPushButton { min-width: {s72}px; }
)QSS");

    for (const auto &v : std::as_const(vars))
        s.replace(QLatin1Char('{') + v.first + QLatin1Char('}'), v.second);

    return s;
}
