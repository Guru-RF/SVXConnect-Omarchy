/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * Semantic colours and small styling helpers, over OmarchyTheme.
 *
 * SVXConnect-Qt hard-coded six colours here, because a desktop palette has no
 * notion of "transmitting" or "linked" and a Highlight role means nothing in
 * particular. An Omarchy theme is different: every one ships a red, a green and
 * a yellow chosen to read against its own background, and the shell already
 * uses them for exactly these meanings (bar.active is red, polkit errors are
 * red). So the semantics stay fixed — transmit is red, linked is green,
 * reconnecting is yellow — and the hues come from the theme.
 *
 * Everything that is NOT custom-painted is styled by the application
 * stylesheet through a `role` property and, where the look depends on state, a
 * second property such as `tone`. Changing one of those at runtime needs a
 * re-polish; setProp() does that and skips the work when nothing changed, so it
 * is safe on the 100 ms model tick.
 */
#ifndef SVXCONNECT_OMARCHY_THEME_H
#define SVXCONNECT_OMARCHY_THEME_H

#include <QColor>
#include <QFont>
#include <QStyle>
#include <QVariant>
#include <QWidget>

#include "ui/omarchytheme.h"

namespace Theme {

using Font = OmarchyTheme::Font;

inline const OmarchyTheme::Palette &palette() { return OmarchyTheme::get().palette(); }
inline const OmarchyTheme::Tokens  &tokens()  { return OmarchyTheme::get().tokens(); }

inline QColor tx()        { return palette().red; }             /* transmitting   */
inline QColor connected() { return palette().green; }           /* linked         */
inline QColor busy()      { return palette().yellow; }          /* (re)connecting */
inline QColor down()      { return palette().darkForeground; }  /* idle           */
inline QColor star()      { return palette().yellow; }          /* priority star  */

/* Meter scale. The stops are positions along the FULL track, not along the
 * filled part, so a given colour always means the same level. */
inline QColor meterLow()  { return palette().green; }
inline QColor meterMid()  { return palette().yellow; }
inline QColor meterHigh() { return palette().red; }

inline QColor wash(const QColor &c, double alpha) { return OmarchyTheme::alpha(c, alpha); }

inline QFont font(Font role, bool bold = false) { return OmarchyTheme::get().font(role, bold); }
inline int   space(int px)                      { return OmarchyTheme::get().space(px); }

/* Set a styling property and re-polish only if it actually changed. */
inline void setProp(QWidget *w, const char *name, const QVariant &value)
{
    if (!w || w->property(name) == value)
        return;
    w->setProperty(name, value);
    w->style()->unpolish(w);
    w->style()->polish(w);
    w->update();
}

inline void setRole(QWidget *w, const char *role) { setProp(w, "role", QString::fromLatin1(role)); }
inline void setTone(QWidget *w, const char *tone) { setProp(w, "tone", QString::fromLatin1(tone)); }

/* Nerd Font glyphs. Omarchy's default font is a Nerd Font, and fontconfig
 * falls back to one for any glyph the chosen family lacks — which is how the
 * shell's own bar icons render. */
namespace Glyph {
inline QString of(char32_t cp) { return QString::fromUcs4(&cp, 1); }

constexpr char32_t Menu       = 0xf0c9;   /* bars          */
constexpr char32_t Settings   = 0xf013;   /* cog           */
constexpr char32_t Log        = 0xf120;   /* terminal      */
constexpr char32_t Microphone = 0xf130;
constexpr char32_t Speaker    = 0xf028;   /* volume up     */
constexpr char32_t SpeakerOff = 0xf026;   /* volume off    */
constexpr char32_t Lock       = 0xf023;
constexpr char32_t Unlock     = 0xf09c;
constexpr char32_t Plus       = 0xf067;
constexpr char32_t Minus      = 0xf068;
constexpr char32_t Crosshairs = 0xf05b;   /* recentre the map */
constexpr char32_t Close      = 0xf00d;
constexpr char32_t User       = 0xf007;
constexpr char32_t Link       = 0xf08e;   /* external link    */
} // namespace Glyph

} // namespace Theme

#endif
