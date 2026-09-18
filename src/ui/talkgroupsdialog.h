/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * Choosing talkgroups from what the reflector actually has.
 *
 * Typing talkgroup numbers into a text field means knowing them, and the only
 * places they are written down are the reflector's portal and other people's
 * configurations. An enhanced reflector publishes both halves of the answer:
 * talkgroups.json names them, and the WebSocket feed says which ones its nodes
 * are listening to right now. This puts the two together and lets the operator
 * tick the ones they want.
 *
 * WHY A PICKER AND NOT A "FILL" BUTTON
 * ------------------------------------
 * The macOS app has a button that overwrites both fields with every talkgroup
 * the portal lists. On be.svx.link that is eighteen, which makes the sidebar's
 * cycle eighteen long and throws away any priorities already set. The two
 * fields also mean different things — Monitored is everything you want to
 * hear, Switchable is the short list you cycle through — so filling them with
 * the same eighteen entries is not a useful answer to either question.
 *
 * So: a list, ticked, with the current configuration already ticked in it, and
 * priorities preserved on the way back out.
 */
#ifndef SVXCONNECT_OMARCHY_TALKGROUPSDIALOG_H
#define SVXCONNECT_OMARCHY_TALKGROUPSDIALOG_H

#include <QDialog>
#include <QHash>
#include <QList>
#include <QString>

class QCheckBox;
class QLabel;

class ReflectorTalkgroupsDialog : public QDialog {
    Q_OBJECT

public:
    struct Entry {
        quint32 id    = 0;
        QString name;         /* from the portal, may be empty        */
        int     nodes = 0;    /* how many nodes monitor it right now  */
        bool    monitored  = false;
        bool    switchable = false;
    };

    /* `entries` is everything the reflector knows about, with `monitored` and
     * `switchable` pre-set from the configuration as it stands. */
    ReflectorTalkgroupsDialog(const QList<Entry> &entries, QWidget *parent = nullptr);

    QList<Entry> chosen() const;

    /* ---- pure, and unit-tested ----
     *
     * This dialog edits somebody's configuration, so the two steps that decide
     * what ends up in it are functions of their inputs and nothing else. */

    /* What to list: the talkgroups the JSON names — and only those; the feed's
     * node counts annotate them but do not add to them — ticked according to
     * the two configuration fields as they stand. With no names at all, the
     * feed's talkgroups are the fallback. Busiest first. `keptOut`, if given,
     * receives the configured talkgroups that are NOT listed. */
    static QList<Entry> entriesFor(const QHash<quint32, QString> &names,
                                   const QHash<quint32, int> &nodeCount,
                                   const QString &monitoredText,
                                   const QString &switchableText,
                                   QList<quint32> *keptOut = nullptr);

    /* The two fields after the operator's choice. Priorities already set
     * survive; the switch order already set survives, new entries joining the
     * end; and configured talkgroups that were never listed are carried
     * through untouched — filtering a list must not be a way to delete from a
     * configuration. */
    struct Fields { QString monitored; QString switchable; };
    static Fields compose(const QList<Entry> &chosen,
                          const QString &monitoredText,
                          const QString &switchableText);

    /* A line under the list, for what the caller is carrying through without
     * showing — talkgroups in the configuration that the reflector does not
     * name. Hidden when empty. */
    void setKeptNote(const QString &text);

private:
    struct Row {
        Entry      entry;
        QCheckBox *monitor  = nullptr;
        QCheckBox *switchTo = nullptr;
    };

    QList<Row> m_rows;
    QLabel    *m_keptNote = nullptr;
};

#endif
