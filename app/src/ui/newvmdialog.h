// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDialog>

#include "core/vmtemplate.h"

class OsChooser;
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QRadioButton;
class QSlider;
class QSpinBox;
class Vm;
class VmStore;

/*
 * Creates a VM: a folder, a disk, the firmware variables and vm.args.  It
 * asks only what vitrine cannot choose: the disc to install from, the
 * system, which the disc tells unless the user chooses it (GuestOs), the
 * memory and processors, the disk; the rest comes from the template of the
 * system, the settings change it afterwards.
 */
class NewVmDialog : public QDialog
{
    Q_OBJECT

public:
    explicit NewVmDialog(VmStore *store, QWidget *parent = nullptr);

    void accept() override;
    /* After accept() */
    Vm *vm() const { return m_vm; }
    bool openSettings() const;
    /* Warnings about what could not be done as asked, e.g. no UEFI firmware */
    QStringList warnings() const { return m_warnings; }

private:
    /* The template's memory, processors and disk, when its kind changed */
    void applyDefaults();
    void updateDisk();
    /* The system of the disc chosen, while "Detect" is on */
    void detect();
    /* The system shown changed: the template, the desktop */
    void systemChanged();
    /* The VM named after its system, unless the user named it */
    void nameAfterSystem();
    VmTemplate::Os templateOs() const;
    bool create(Vm *vm, QString *error);

    VmStore *m_store;
    QLineEdit *m_name;
    OsChooser *m_os;
    QCheckBox *m_detect;
    QLabel *m_detected;
    QComboBox *m_desktop;
    /* The name given after the system, until the user writes another */
    QString m_autoName;
    /* The kind of the template the defaults are of */
    VmTemplate::Os m_kind = VmTemplate::Os::Other;
    bool m_defaulted = false;
    QSlider *m_memorySlider;
    QSpinBox *m_memory;
    QSlider *m_cpuSlider;
    QSpinBox *m_cpus;
    QRadioButton *m_newDisk;
    QSpinBox *m_diskSize;
    QRadioButton *m_existingDisk;
    QLineEdit *m_diskPath;
    QLineEdit *m_iso;
    QLabel *m_note;
    QCheckBox *m_settings;
    Vm *m_vm = nullptr;
    QStringList m_warnings;
};
