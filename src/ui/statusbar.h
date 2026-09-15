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

private:
    void buildUi();

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

    rc_state m_lastState = RC_IDLE;
    bool     m_stateInit = false;
};

#endif
