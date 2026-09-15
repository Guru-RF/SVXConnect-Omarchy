/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * Opening svxconnect.conf in the user's text editor.
 *
 * Until the Preferences dialog exists (M3), and afterwards for the keys it
 * does not surface, editing the file by hand is the way settings are changed.
 * The file is shared verbatim with SVXConnect-CLI — one identity, one config —
 * so this is also how the two clients stay in agreement.
 *
 * WHY QDesktopServices AND NOT $EDITOR
 * ------------------------------------
 * $EDITOR and $VISUAL name a TERMINAL editor. Launching vi from a GUI process
 * with no controlling terminal gets you nothing at all — and on this class of
 * desktop both variables are typically unset anyway. The desktop's registered
 * handler for text/plain is the right answer: it is what the user already
 * chose, it goes through the OpenURI portal when sandboxed, and it is how
 * every other GUI application opens a text file.
 *
 * xdg-open is kept as a fallback for the case where the Qt platform theme
 * cannot reach a portal or a .desktop database.
 */
#ifndef SVXCONNECT_OMARCHY_CONFIGFILE_H
#define SVXCONNECT_OMARCHY_CONFIGFILE_H

#include <QString>

/* svx_config cannot be forward-declared.
 *
 * Upstream it is an ANONYMOUS struct typedef — `typedef struct { ... }
 * svx_config;` at config.h:89 — so `struct svx_config;` here declares a
 * DIFFERENT, incomplete type and g++ rejects it with "conflicting declaration"
 * the moment the real header is also in scope. The same property is why
 * core/svxcore.h has to guard its copies with the upstream include guards.
 *
 * Pulling in the shim is the correct fix, and it is cheap: svxcore.h includes
 * only the ten C++-clean core headers. Tagging the typedefs upstream would
 * remove the need for both workarounds — see docs/PLAN.md, Q7. */
#include "core/svxcore.h"

class QWidget;

namespace ConfigFile {

/* Does the file exist and is it readable? */
bool exists(const QString &path);

/* Write a complete starter configuration to `path`, creating parent
 * directories as needed.
 *
 * The body comes from the core's own config_dump(), which walks the same key
 * table that drives parsing and validation. That makes the generated file
 * exhaustive and, by construction, exactly in step with what this binary
 * understands — which a checked-in example.conf drifts away from. Pass the
 * running configuration to capture the values actually in use, or nullptr for
 * pristine defaults.
 *
 * Returns false and leaves nothing behind on failure. */
bool createDefault(const QString &path, const svx_config *cfg, QString *errorOut);

/* Hand `path` to the desktop's text/plain handler. Tries QDesktopServices
 * first, then xdg-open. Returns false if neither worked; `parent` is used only
 * to anchor an error dialog and may be null. */
bool openInEditor(const QString &path, QWidget *parent);

/* The directory containing `path`, opened in the file manager. */
bool openContainingFolder(const QString &path, QWidget *parent);

} // namespace ConfigFile

#endif
