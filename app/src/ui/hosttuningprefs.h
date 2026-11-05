// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QObject>
#include <QStringList>

#include "core/gpuinputawake.h"
#include "core/hostsettings.h"

class Form;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QWidget;

/*
 * The Preferences entries of host tuning while VMs run (HostSettings,
 * vitrine-helper): on or off, the GPU clock floor, and the CPU kept ready
 * to wake up (HostSettings::cpuLatency()).  On, a line under it
 * says whether tuning is active, and if not why and what to do (Set Up for
 * the vitrine group); off, nothing to warn about.  On a host with a
 * dedicated GPU, which a clock floor does nothing for, the floor's row has
 * gpu-input-awake instead (GpuInputAwake): whether it is installed, and
 * Install… or Remove…, which act at once, with the administrator's password.
 */
class HostTuningPrefs : public QObject
{
    Q_OBJECT

public:
    /* Adds its rows to @form, in @parent */
    HostTuningPrefs(Form *form, QWidget *parent);

    void save() const;

private:
    void update();
    /* The state, asked again (polkit, the user database) */
    void refresh();
    void showState(const HostSettings::Status &status);
    /* gpu-input-awake's state, read again (its files, systemctl) */
    void refreshAwake();
    void showAwake(const QString &active);
    /* Install (true) or Remove, once confirmed */
    void runAwake(bool install);

    QWidget *m_parent;
    QCheckBox *m_tune;
    QLabel *m_state;
    QPushButton *m_setUp;
    QComboBox *m_floor;
    QSpinBox *m_mhz;
    QCheckBox *m_cpuLatency;
    bool m_checked = false;
    bool m_needsGroup = false;
    /* gpu-input-awake: none of these without a dedicated GPU */
    QStringList m_dedicated;        // the GPUs it serves, by name
    QLabel *m_awake = nullptr;
    QPushButton *m_awakeInstall = nullptr;
    QPushButton *m_awakeRemove = nullptr;
    GpuInputAwake::Installed m_installed = GpuInputAwake::Installed::No;
};
