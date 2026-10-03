// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDialog>

#include "core/vmtemplate.h"

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

/* Creates a VM: a folder, a disk, the firmware variables and vm.args */
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
    void applyDefaults();
    void updateDisk();
    bool create(Vm *vm, QString *error);

    VmStore *m_store;
    QLineEdit *m_name;
    QComboBox *m_os;
    QSlider *m_memorySlider;
    QSpinBox *m_memory;
    QSlider *m_cpuSlider;
    QSpinBox *m_cpus;
    QRadioButton *m_newDisk;
    QSpinBox *m_diskSize;
    QRadioButton *m_existingDisk;
    QLineEdit *m_diskPath;
    QRadioButton *m_noDisk;
    QLineEdit *m_iso;
    QComboBox *m_firmware;
    QComboBox *m_graphics;
    QCheckBox *m_nativeContext;
    QLabel *m_note;
    QCheckBox *m_settings;
    Vm *m_vm = nullptr;
    QStringList m_warnings;
};
