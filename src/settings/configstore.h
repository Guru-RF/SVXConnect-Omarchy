/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * Writing svxconnect.conf back out.
 *
 * The core can read a configuration and set individual keys in memory, but it
 * has NO config_save — nothing in SVXConnect-CLI ever writes the file. The
 * terminal client tells you to edit it by hand, so it never needed one. A
 * Preferences dialog does.
 *
 * WHY THIS IS NOT "SERIALISE THE STRUCT"
 * --------------------------------------
 * The obvious implementation is config_dump() to a temporary file and rename
 * it over the original. That is what ConfigFile::createDefault() does, and for
 * a file that does not exist yet it is exactly right.
 *
 * For a file the user already owns it is destructive. This file is shared
 * verbatim with the command-line client and is meant to be hand-edited — the
 * comments in it are the documentation. Dumping the struct would silently
 * delete every comment, every blank line, every bit of grouping and ordering
 * the user put there, and reorder the keys into the core's internal table
 * order. Changing the transmit timeout should not rewrite the file.
 *
 * So this is a read-modify-write over the original text: only the value of a
 * changed key is replaced, in place, and everything else — including the
 * trailing comment on that very line — survives byte for byte. A save that
 * changes nothing produces a file identical to the input, which is the
 * property tests/test_configstore.cpp checks.
 *
 * THE PARSER IT HAS TO MATCH
 * --------------------------
 * config_load() (config.c:395-422) does, per line: strip a comment starting at
 * an unescaped '#' or ';'; trim; split on the FIRST '='; trim both halves;
 * remove one layer of matched quotes from the value. Keys are
 * case-insensitive. When a key appears more than once the LAST occurrence
 * wins, because config_set() is simply called again — which is why save()
 * rewrites every occurrence of a changed key rather than just the first.
 */
#ifndef SVXCONNECT_OMARCHY_CONFIGSTORE_H
#define SVXCONNECT_OMARCHY_CONFIGSTORE_H

#include <QString>
#include <QStringList>
#include <QHash>

#include "core/svxcore.h"

class ConfigStore {
public:
    /* `cfg` is the live configuration, read for current values. It is NOT
     * modified by this class — see the note on save(). */
    ConfigStore(const svx_config *cfg, QString path);

    /* Current value of a key, as the file would spell it. */
    QString value(const QString &key) const;
    int     valueInt(const QString &key) const;
    bool    valueBool(const QString &key) const;

    /* Stage a change. Setting a key back to its current value un-stages it, so
     * touching a control and putting it back does not dirty the file. */
    void set(const QString &key, const QString &value);
    void setInt(const QString &key, int value);
    void setBool(const QString &key, bool value);

    bool        isDirty() const { return !m_changes.isEmpty(); }
    QStringList changedKeys() const;
    void        revert() { m_changes.clear(); }

    /* Write the staged changes to the file, preserving everything else.
     *
     * Does NOT touch the live svx_config. Two reasons: the talkgroup manager
     * holds indices into cfg's lists and rewriting them under a live
     * connection is a use-after-free waiting for the next talker; and the
     * keys that CAN be changed safely at runtime (volume, the audio devices)
     * have proper app_* entry points that do the necessary teardown, which a
     * config_set() behind the core's back would skip.
     *
     * So the flow is: save() writes the file, the caller applies whatever is
     * safely applicable through app_*, and the file watcher raises the
     * "restart to apply" bar for the rest — the same path a hand-edit takes.
     *
     * Returns false and leaves the original file untouched on failure. */
    bool save(QString *errorOut);

    QString path() const { return m_path; }

private:
    const svx_config *m_cfg;
    QString           m_path;
    QHash<QString, QString> m_changes;   /* lower-cased key -> new value */
};

#endif
