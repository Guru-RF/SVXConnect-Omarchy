/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * The header row: link state, identity, packet rates, grid square, the
 * connect button — and, at the trailing edge, whatever the window adds. There
 * is no menu bar; like the Omarchy shell's panels, the window's actions live in
 * a row of icon buttons at the top right and behind keyboard shortcuts.
 */
#ifndef SVXCONNECT_OMARCHY_STATUSBAR_H
#define SVXCONNECT_OMARCHY_STATUSBAR_H

#include <QWidget>

#include "core/svxcore.h"

class QHBoxLayout;
class QLabel;
class QPushButton;

class ConnectionBar : public QWidget {
    Q_OBJECT

public:
    explicit ConnectionBar(svx_app *app, QWidget *parent = nullptr);

    void tickModel(quint64 nowMs);

    /* Append a widget after the connect button. */
    void addTrailing(QWidget *w);

protected:
    QSize minimumSizeHint() const override;
    void resizeEvent(QResizeEvent *) override;

private:
    void buildUi();

    /* Drop what does not fit, least important first.
     *
     * A QLabel's minimum size is its whole text, so a row of them sets a floor
     * under the window's width: at 900 px the grid square and the node count
     * simply stopped the window being made narrower. The row therefore measures
     * itself against the width it actually has and hides the optional parts —
     * grid square, node count, packet rates, host — until the rest fits. */
    void relayout();

    svx_app *m_app = nullptr;

    QHBoxLayout *m_row      = nullptr;
    QLabel      *m_dot      = nullptr;
    QLabel      *m_state    = nullptr;
    QLabel      *m_detail   = nullptr;
    QLabel      *m_identity = nullptr;
    QLabel      *m_rx       = nullptr;
    QLabel      *m_tx       = nullptr;
    QLabel      *m_nodes    = nullptr;
    QLabel      *m_grid     = nullptr;
    QPushButton *m_connect  = nullptr;

    /* Both spellings of the identity, so relayout() can swap between them
     * without re-deriving either: "ON6URE · be.svx.link:5300" and "ON6URE". */
    QString m_identityFull;
    QString m_identityShort;

    rc_state m_lastState = RC_IDLE;
    bool     m_stateInit = false;
};

#endif
