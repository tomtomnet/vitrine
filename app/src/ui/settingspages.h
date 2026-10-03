// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDialog>
#include <QIcon>
#include <QList>
#include <QMap>
#include <QTemporaryDir>
#include <QWidget>

#include "core/argsfile.h"
#include "core/vmconfig.h"
#include "core/vmhardware.h"

class ArgsEditorPane;
class Banner;
class QemuDocs;
class QRadioButton;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class QTableWidget;
class QTreeWidget;
class QTreeWidgetItem;
class Vm;

/*
 * A page of the settings of a VM.  All pages edit the same arguments: a
 * page shows them when it is entered and writes back what the user changed
 * when it is left, so that the other pages, the Arguments page above all,
 * see the changes.
 */
class SettingsPage : public QWidget
{
    Q_OBJECT

public:
    using QWidget::QWidget;

    virtual QString title() const = 0;
    virtual QIcon icon() const = 0;
    virtual void load(const ArgsFile &args) = 0;
    /* Only the settings the user changed, to keep the rest as written */
    virtual void save(ArgsFile &args) = 0;
    /* The user changed something since load() */
    virtual bool isModified() const = 0;
    /*
     * What the arguments to save need in the VM folder, e.g. the new disks
     * they name: done on Apply, not when a page is left, so that Discard
     * leaves nothing behind
     */
    virtual bool commit(const ArgsFile &args, const QString &vmDir, QString *error)
    {
        Q_UNUSED(args);
        Q_UNUSED(vmDir);
        Q_UNUSED(error);
        return true;
    }
};

class GeneralPage : public SettingsPage
{
    Q_OBJECT

public:
    explicit GeneralPage(Vm *vm, QWidget *parent = nullptr);

    QString title() const override { return tr("General"); }
    QIcon icon() const override;
    void load(const ArgsFile &args) override;
    void save(ArgsFile &args) override;
    bool isModified() const override;

private:
    QLineEdit *m_name;
    QString m_loaded;
};

class SystemPage : public SettingsPage
{
    Q_OBJECT

public:
    explicit SystemPage(Vm *vm, QWidget *parent = nullptr);

    QString title() const override { return tr("System"); }
    QIcon icon() const override;
    void load(const ArgsFile &args) override;
    void save(ArgsFile &args) override;
    bool isModified() const override;
    bool commit(const ArgsFile &args, const QString &vmDir, QString *error) override;

private:
    void loadBoot(const ArgsFile &args);
    void saveBoot(ArgsFile &args);
    bool bootModified() const;
    void describeFirmware();
    /* Reset UEFI Variables: for the variable store the saved arguments name */
    void updateResetVars();
    void resetVars();

    /* The QEMU chosen, empty for the default one */
    QString chosenQemu() const;
    void updateQemu();
    void fillLists();
    void updateTopology();
    void describe();

    QemuDocs *m_docs = nullptr;
    QSlider *m_memorySlider;
    QSpinBox *m_memory;
    QSlider *m_cpuSlider;
    QSpinBox *m_cpus;
    QCheckBox *m_topology;
    QSpinBox *m_sockets;
    QSpinBox *m_cores;
    QSpinBox *m_threads;
    QComboBox *m_model;
    QLabel *m_modelInfo;
    QComboBox *m_machine;
    QLabel *m_machineInfo;
    QComboBox *m_accel;
    QRadioButton *m_defaultQemu;
    QRadioButton *m_ownQemu;
    QLineEdit *m_qemuPath;
    QLabel *m_qemuInfo;
    Vm *m_vm;
    QComboBox *m_firmware;
    QLabel *m_firmwareInfo;
    QPushButton *m_resetVars;
    QCheckBox *m_bootMenu;
    QComboBox *m_bootDevice;
    /* The firmware files apply() copies, until the dialog applies */
    QTemporaryDir m_staging;

    qint64 m_loadedMemory = 0;
    VmConfig::Cpus m_loadedCpus;
    QString m_loadedMachine;
    QString m_loadedAccel;
    QString m_loadedQemu;
    VmConfig::FirmwareKind m_loadedFirmware = VmConfig::FirmwareKind::Bios;
    bool m_loadedBootMenu = false;
    VmConfig::BootDevice m_loadedBootDevice = VmConfig::BootDevice::Default;
};

class DisplayPage : public SettingsPage
{
    Q_OBJECT

public:
    explicit DisplayPage(QWidget *parent = nullptr);

    QString title() const override { return tr("Display"); }
    QIcon icon() const override;
    void load(const ArgsFile &args) override;
    void save(ArgsFile &args) override;
    bool isModified() const override;

private:
    /* The graphics the page shows */
    VmConfig::Graphics shown() const;
    void fillDevices();
    void update();

    Banner *m_custom;
    QComboBox *m_kind;
    QComboBox *m_device;
    QCheckBox *m_nativeContext;
    QCheckBox *m_venus;
    Banner *m_venusUnused;
    QSpinBox *m_hostmem;
    QComboBox *m_window;

    VmConfig::Graphics m_loaded;
    int m_loadedHostmemGiB = 0;
    /* ARM's virt: no VGA */
    bool m_virt = false;
};

class StoragePage : public SettingsPage
{
    Q_OBJECT

public:
    explicit StoragePage(const QString &vmDir, QWidget *parent = nullptr);

    QString title() const override { return tr("Storage"); }
    QIcon icon() const override;
    void load(const ArgsFile &args) override;
    void save(ArgsFile &args) override;
    bool isModified() const override;
    bool commit(const ArgsFile &args, const QString &vmDir, QString *error) override;

private:
    struct Entry {
        VmConfig::Disk disk;        // as loaded; line -1 for one added since
        QString file;               // the disc now in a CD/DVD drive
        int newGiB = 0;             // a disk to create, of this size
        bool removed = false;
    };

    void fill();
    void updateButtons();
    int current() const;
    void addDisk();
    void addCdrom();
    void chooseDisc();
    void resize();
    QString newDiskName() const;

    QString m_vmDir;
    QTableWidget *m_table;
    QPushButton *m_disc;
    QPushButton *m_eject;
    QPushButton *m_resize;
    QPushButton *m_remove;
    QList<Entry> m_entries;
    /* New disks, by file name, until the dialog applies and creates them */
    QMap<QString, int> m_pending;
    bool m_virt = false;
};

class SharesPage : public SettingsPage
{
    Q_OBJECT

public:
    explicit SharesPage(QWidget *parent = nullptr);

    QString title() const override { return tr("Shared Folders"); }
    QIcon icon() const override;
    void load(const ArgsFile &args) override;
    void save(ArgsFile &args) override;
    bool isModified() const override;

private:
    void fill();
    void edit(int row);
    void updateHints();

    Banner *m_virtiofsd;
    Banner *m_memory;
    QTableWidget *m_table;
    QPushButton *m_edit;
    QPushButton *m_remove;
    QLabel *m_mount;

    QList<VmConfig::Share> m_shares;
    QList<VmConfig::Share> m_loaded;
    bool m_sharedMemory = false;
    bool m_fixMemory = false;
};

/* Adds or edits a shared folder */
class ShareDialog : public QDialog
{
    Q_OBJECT

public:
    ShareDialog(const VmConfig::Share &share, const QStringList &otherTags,
                QWidget *parent = nullptr);

    VmConfig::Share share() const;

private:
    void validate();

    QLineEdit *m_path;
    QLineEdit *m_tag;
    QComboBox *m_cache;
    QLabel *m_cacheInfo;
    QCheckBox *m_readonly;
    QCheckBox *m_mount;
    QLineEdit *m_mountDir;
    QLabel *m_error;
    QPushButton *m_ok;
    QStringList m_otherTags;
    bool m_tagEdited = false;
    bool m_mountEdited = false;
};

class PciPage : public SettingsPage
{
    Q_OBJECT

public:
    explicit PciPage(QWidget *parent = nullptr);

    QString title() const override { return tr("PCI Devices"); }
    QIcon icon() const override;
    void load(const ArgsFile &args) override;
    void save(ArgsFile &args) override;
    bool isModified() const override;

private:
    QStringList checked() const;

    Banner *m_banner;
    QTreeWidget *m_tree;
    QStringList m_loaded;
};

class UsbPage : public SettingsPage
{
    Q_OBJECT

public:
    explicit UsbPage(QWidget *parent = nullptr);

    QString title() const override { return tr("USB Devices"); }
    QIcon icon() const override;
    void load(const ArgsFile &args) override;
    void save(ArgsFile &args) override;
    bool isModified() const override;

private:
    QList<VmConfig::UsbId> checked() const;

    Banner *m_controller;
    QTreeWidget *m_tree;
    QList<VmConfig::UsbId> m_loaded;
    bool m_addController = false;
};

class ArgumentsPage : public SettingsPage
{
    Q_OBJECT

public:
    explicit ArgumentsPage(const QString &vmDir, QWidget *parent = nullptr);

    QString title() const override { return tr("Arguments"); }
    QIcon icon() const override;
    void load(const ArgsFile &args) override;
    void save(ArgsFile &args) override;
    bool isModified() const override;

private:
    ArgsEditorPane *m_pane;
    QString m_loaded;
};
