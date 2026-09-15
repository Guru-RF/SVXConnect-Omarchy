/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * The Omarchy theme, read live.
 *
 * Omarchy keeps the active theme in ~/.local/state/omarchy/current/theme/ and
 * swaps the whole directory atomically on `omarchy theme set` (rm -rf, then a
 * rename from next-theme). Two files in it describe everything this window
 * needs:
 *
 *   colors.toml   the foundational palette — background, foreground, accent,
 *                 the ANSI hues and a handful of derived surfaces
 *   shell.toml    the Omarchy shell's surface and control tokens — fill and
 *                 border alphas per interaction state, the font base size, the
 *                 spacing scale, menu and tooltip borders
 *
 * plus ~/.config/omarchy/shell.toml, the machine-level override the shell
 * layers on top (`omarchy display text size` writes it). Corner rounding is
 * Hyprland's decoration:rounding, exactly as the shell's Style.qml reads it,
 * and the font is whatever the fontconfig `monospace` alias resolves to, which
 * is what `omarchy font set` rewrites.
 *
 * So this class mirrors qs.Commons.Color and qs.Commons.Style from
 * /usr/share/omarchy/shell, for Qt Widgets: same inputs, same token names, same
 * defaults. A window styled from it looks like a shell panel because it is
 * drawn from the same numbers.
 *
 * WHY WATCH A DIRECTORY AND NOT THE FILES
 * ---------------------------------------
 * omarchy-theme-set removes the theme directory and renames a new one into its
 * place. A QFileSystemWatcher on colors.toml loses the inode on the first
 * switch and never fires again. Watching the PARENT (…/omarchy/current) sees
 * the rename, and every reload re-adds the file paths for good measure. Changes
 * are debounced, because a theme switch touches a dozen files in a burst.
 */
#ifndef SVXCONNECT_OMARCHY_OMARCHYTHEME_H
#define SVXCONNECT_OMARCHY_OMARCHYTHEME_H

#include <QObject>
#include <QColor>
#include <QFont>
#include <QHash>
#include <QString>
#include <QTimer>

class QFileSystemWatcher;
class QProcess;

class OmarchyTheme : public QObject {
    Q_OBJECT

public:
    /* The shell's type scale, as multipliers of [font] base-size. */
    enum class Font {
        Caption,     /* 0.833  — 10 px at base 12 */
        BodySmall,   /* 0.917  — 11 px */
        Body,        /* 1.000  — 12 px */
        Subtitle,    /* 1.083  — 13 px */
        Title,       /* 1.167  — 14 px */
        Heading,     /* 1.333  — 16 px */
        Headline,    /* 1.667  — 20 px, the sidebar's talkgroup number */
        Display      /* 2.000  — 24 px */
    };

    /* colors.toml, with fallbacks for themes that predate the named keys. */
    struct Palette {
        bool    dark = true;

        QColor  background        {0x1a, 0x1b, 0x26};
        QColor  foreground        {0xa9, 0xb1, 0xd6};
        QColor  accent            {0x7a, 0xa2, 0xf7};
        QColor  muted             {0x41, 0x48, 0x68};
        QColor  selection         {0x29, 0x2e, 0x42};

        QColor  darkBackground    {0x13, 0x14, 0x1c};
        QColor  darkerBackground  {0x0e, 0x0e, 0x14};
        QColor  lighterBackground {0x24, 0x28, 0x3b};

        QColor  darkForeground    {0x56, 0x5f, 0x89};
        QColor  lightForeground   {0xb4, 0xbe, 0xe6};
        QColor  brightForeground  {0xc0, 0xca, 0xf5};

        QColor  red               {0xf7, 0x76, 0x8e};
        QColor  green             {0x9e, 0xce, 0x6a};
        QColor  yellow            {0xe0, 0xaf, 0x68};
        QColor  orange            {0xeb, 0x92, 0x7b};
        QColor  blue              {0x7a, 0xa2, 0xf7};
        QColor  cyan              {0x44, 0x9d, 0xab};
        QColor  magenta           {0xad, 0x8e, 0xe6};
    };

    /* shell.toml — the subset a Widgets window can honour. */
    struct Tokens {
        /* [controls] */
        double normalFill        = 0.04;
        double hoverFill         = 0.08;
        double selectedFill      = 0.18;
        double pressedFill       = 0.22;
        double selectionFill     = 0.35;
        double normalBorderAlpha = 0.40;
        double hoverBorderAlpha  = 0.25;
        double focusBorderAlpha  = 0.25;
        int    normalBorderWidth = 1;

        /* [hyprland] — solid colours; a gradient contributes its first stop. */
        QColor activeBorder;
        QColor activeBorderForeground;

        /* [menu] / [tooltip] / [popups] */
        QColor menuBorder;
        QColor menuSelectedText;
        double menuSelectedBackgroundAlpha = 0.08;
        QColor tooltipBorder;
        QColor popupBorder;

        /* [font] and [spacing] */
        int    fontBaseSize        = 12;
        double spacingScale        = 1.0;
        bool   spacingScaleWithFont = true;

        /* Hyprland decoration:rounding. */
        int    radius = 0;
    };

    static OmarchyTheme &get();

    /* Load, apply to the QApplication, and start watching. Call once, right
     * after the QApplication exists and before any window is created. */
    void install();

    const Palette &palette() const { return m_palette; }
    const Tokens  &tokens()  const { return m_tokens; }

    /* The theme's slug, e.g. "tokyo-night". Empty when not on Omarchy. */
    QString name() const { return m_name; }

    /* True when an Omarchy theme was actually found. Off Omarchy the defaults
     * above (Tokyo Night) are used, so the window still looks deliberate. */
    bool isOmarchy() const { return m_found; }

    /* The concrete family `monospace` resolves to, e.g. "JetBrainsMono Nerd Font". */
    QString fontFamily() const { return m_fontFamily; }

    int   fontPx(Font role) const;
    QFont font(Font role, bool bold = false) const;

    /* Style.space(): a pixel value through the shell's spacing scale. */
    int space(int px) const;

    /* A colour at an alpha, and its QSS spelling. */
    static QColor alpha(QColor c, double a);
    static QString css(const QColor &c);

signals:
    /* Emitted after the new palette, font and stylesheet are in place.
     * Custom-painted widgets repaint on it; everything else is restyled by the
     * application stylesheet on its own. */
    void changed();

private:
    OmarchyTheme();

    void reload();
    void apply();
    void rewatch();
    void refreshRounding();
    void resolveFont();

    QString buildStyleSheet() const;
    QString writeGlyphs() const;   /* returns the directory holding the SVGs */

    Palette m_palette;
    Tokens  m_tokens;
    QHash<QString, int> m_fontOverrides;   /* [font] per-token pins, in px */
    QString m_name;
    QString m_fontFamily = QStringLiteral("monospace");
    bool    m_found      = false;
    bool    m_installed  = false;
    QString m_appliedSignature;

    QFileSystemWatcher *m_watch = nullptr;
    QTimer              m_debounce;
};

#endif
