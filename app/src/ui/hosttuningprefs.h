// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QObject>

class Form;
class QCheckBox;
class QComboBox;
class QLabel;
class QSpinBox;
class QWidget;

/*
 * The Preferences entries of host tuning while VMs run (HostSettings,
 * vitrine-helper): on or off, and the GPU clock floor
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

    QCheckBox *m_tune;
    QComboBox *m_floor;
    QSpinBox *m_mhz;
    QLabel *m_status;
};
