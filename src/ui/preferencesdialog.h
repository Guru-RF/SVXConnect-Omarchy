/* SPDX-License-Identifier: MIT
 * SVXConnect-Omarchy — Copyright (c) 2026 Diëlectricum BV
 *
 * Preferences.
 *
 * Tabs over the keys the core actually parses, written back to svxconnect.conf
 * through ConfigStore so the terminal client sees the same settings. There is
 * no separate GUI settings file for anything the core understands — one
 * identity, one configuration.
 *
 * WHAT APPLIES IMMEDIATELY, AND WHAT DOES NOT
 * -------------------------------------------
 * Three settings have proper runtime entry points and are applied at once: the
 * input device, the output device and the output volume. Everything else is
 * written to the file and takes effect on restart, and the window's
 * file-watcher bar says so — the same path a hand-edit takes.
 *
 * app_new() keeps the svx_config POINTER, and the talkgroup manager holds
 * indices into its lists; rewriting those fields underneath a live connection
 * is a use-after-free waiting for the next talker. Restarting is cheap and
 * provably correct.
 *
 * PUSH-TO-TALK ON HYPRLAND
 * ------------------------
 * The global key is a line in ~/.config/hypr/bindings.lua, not a portal
 * setting (see ptt/hyprlandbinding.h). The tab shows which key is bound,
 * previews the exact Lua it would write, and writes it only when asked.
 */
#ifndef SVXCONNECT_OMARCHY_PREFERENCESDIALOG_H
#define SVXCONNECT_OMARCHY_PREFERENCESDIALOG_H

#include <QDialog>

#include "core/svxcore.h"
#include "settings/configstore.h"
#include "ptt/pttbackend.h"

class QLineEdit;
class QSpinBox;
class QCheckBox;
class QComboBox;
class QLabel;
class QDialogButtonBox;
class QPushButton;

class PreferencesDialog : public QDialog {
    Q_OBJECT

public:
    PreferencesDialog(svx_app *app, const QString &configPath, QWidget *parent = nullptr);

    /* What the desktop reports as bound, for display. On Hyprland the dialog
     * reads the binding itself and this only triggers a refresh. */
    void setCurrentShortcut(const QString &human);

    /* PTT settings live in QSettings, not svxconnect.conf: the core has no
     * keys for them and the terminal client cannot use them. */
    static PttBinding keyboardBinding();
    static bool       holdMode();

signals:
    /* The PTT binding or mode changed; the window re-applies it. */
    void pttBindingChanged();

    /* Emitted after a successful save that changed at least one key which
     * needs a restart. The window turns this into its "restart to apply" bar. */
    void restartNeeded();

    /* The talkgroup lists changed, so the sidebar must rebuild its buttons. */
    void talkgroupsChanged();

private slots:
    void onApply();
    void onAccept();
    void onTestTone();
    /* Automatic position: the three fields follow Omarchy's weather location
     * and stop being editable. Called whenever the mode changes. */
    void applyPositionMode();

    void refreshDeviceLists();
    void onBindHyprland();
    void onUnbindHyprland();
    void updatePttPreview();

private:
    QWidget *buildConnectionTab();
    QWidget *buildAudioTab();
    QWidget *buildTalkgroupsTab();
    QWidget *buildPttTab();
    QWidget *buildGeneralTab();
    void     refreshPttStatus();
    void     savePtt();

    void load();
    bool commit();          /* stage every control into the store, then save */
    void applyLiveChanges(const QStringList &changed);

    svx_app     *m_app = nullptr;
    ConfigStore  m_store;

    /* Connection */
    QLineEdit *m_callsign  = nullptr;
    QLineEdit *m_email     = nullptr;
    QLineEdit *m_reflector = nullptr;
    QSpinBox  *m_port      = nullptr;
    QCheckBox   *m_enhanced  = nullptr;
    QComboBox   *m_posMode   = nullptr;
    QLineEdit   *m_location  = nullptr;
    QLineEdit   *m_latitude  = nullptr;
    QLineEdit   *m_longitude = nullptr;
    QLabel      *m_grid      = nullptr;
    QPushButton *m_lookup    = nullptr;
    QLabel      *m_posHint   = nullptr;

    /* Audio */
    QComboBox *m_inputDev   = nullptr;
    QComboBox *m_outputDev  = nullptr;
    QLabel    *m_devWarning = nullptr;
    QSpinBox  *m_volume     = nullptr;
    QCheckBox *m_micAgc     = nullptr;
    QSpinBox  *m_micAgcTgt  = nullptr;
    QSpinBox  *m_micGain    = nullptr;
    QSpinBox  *m_jitter     = nullptr;
    QSpinBox  *m_tailTrim   = nullptr;
    QCheckBox *m_rogerBeep  = nullptr;
    QSpinBox  *m_rogerMin   = nullptr;

    /* Talkgroups */
    QLineEdit *m_switchable = nullptr;
    QLineEdit *m_monitored  = nullptr;
    QSpinBox  *m_defaultTg  = nullptr;
    QCheckBox *m_lockOnStart= nullptr;
    QSpinBox  *m_linger     = nullptr;
    QSpinBox  *m_idle       = nullptr;
    QComboBox *m_tgOrder    = nullptr;
    QLabel    *m_tgPreview  = nullptr;

    /* PTT */
    QComboBox   *m_pttMode         = nullptr;
    QLabel      *m_pttPortalStatus = nullptr;
    QLabel      *m_pttShortcut     = nullptr;
    QLineEdit   *m_pttKeys         = nullptr;
    QLabel      *m_pttKeysWarning  = nullptr;
    QLabel      *m_pttPreview      = nullptr;
    QPushButton *m_pttBind         = nullptr;
    QPushButton *m_pttUnbind       = nullptr;
    QLabel      *m_fifoPreview     = nullptr;

    /* General */
    QCheckBox *m_notifyTalkers = nullptr;
    QSpinBox  *m_txTimeout  = nullptr;
    QComboBox *m_logLevel   = nullptr;
    QLineEdit *m_ctlFifo    = nullptr;

    QDialogButtonBox *m_buttons = nullptr;
};

#endif
