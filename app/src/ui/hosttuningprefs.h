// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QObject>

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
 * vitrine-helper): on or off, and the GPU clock floor.  On, a line under it
 * says whether tuning is active, and if not why and what to do (Set Up for
 * the vitrine group); off, nothing to warn about.
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

    QWidget *m_parent;
    QCheckBox *m_tune;
    QLabel *m_state;
    QPushButton *m_setUp;
    QComboBox *m_floor;
    QSpinBox *m_mhz;
    bool m_checked = false;
    bool m_needsGroup = false;
};
