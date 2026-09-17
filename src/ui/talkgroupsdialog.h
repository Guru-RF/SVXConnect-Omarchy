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
#include <QList>
#include <QString>

class QCheckBox;

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

private:
    struct Row {
        Entry      entry;
        QCheckBox *monitor  = nullptr;
        QCheckBox *switchTo = nullptr;
    };

    QList<Row> m_rows;
};

#endif
