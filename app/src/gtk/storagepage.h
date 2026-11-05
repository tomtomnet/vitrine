// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/settingspage.h"

#include <QList>
#include <QMap>

#include "core/vmhardware.h"

/*
 * The disks and CD/DVD drives of a VM, and the order it starts from them:
 * the Qt app's StoragePage, through the same core calls.  Each disk or
 * drive is a row with its own actions (Choose Disc, Eject, Size, Remove);
 * new disks are created in the VM folder on Apply (commit()); removing one
 * takes it out of the VM, its file stays.  The boot order, as virt-manager
 * has it, checked and in order.
 */
class StoragePage : public SettingsPage
{
public:
    explicit StoragePage(const QString &vmDir);
    ~StoragePage() override;

    QString title() const override { return "Storage"; }
    const char *icon() const override { return "drive-harddisk-symbolic"; }
    void load(const ArgsFile &args) override;
    void save(ArgsFile &args) override;
    bool isModified() const override;
    bool commit(const ArgsFile &args, const QString &vmDir, QString *error) override;

    /* What the dialogs ask, for tests: a disk or a drive added, as if chosen */
    void addDisk(const QString &file, int newGiB, VmConfig::Disk::Bus bus);
    void addCdrom(const QString &iso);
    /* The rows, as they read: "Hard disk: disk.qcow2" */
    QStringList bootOrder() const;
    void moveBoot(int index, int by);

private:
    struct Entry {
        VmConfig::Disk disk;        // as loaded; line -1 for one added since
        QString file;               // the disc now in a CD/DVD drive
        int newGiB = 0;             // a disk to create, of this size
        bool removed = false;
        int id = 0;                 // the boot order's name for it, while the page edits it
    };
    /* A device of the boot order: a disk of the page, or a network card */
    struct BootItem {
        VmConfig::BootEntry::Kind kind = VmConfig::BootEntry::HardDisk;
        int entry = -1;             // the id of a disk's Entry
        int nic = -1;               // the place of a network card among them
        QString card;               // its driver, e.g. virtio-net-pci
        bool on = false;
        bool editable = true;
    };

    void fill();
    void fillBoot();
    void addBootItem(const Entry &entry);
    void setBootOn(int index, bool on);
    QString newDiskName() const;
    QString bootText(const BootItem &item) const;
    /* The dialogs of the rows' actions */
    void askNewDisk();
    void chooseDisc(int entry);
    void askSize(int entry);
    void remove(int entry);

    QString m_vmDir;
    AdwPreferencesGroup *m_disks;
    AdwPreferencesGroup *m_boot;
    GtkWidget *m_addDisk;
    GtkWidget *m_addCdrom;
    GtkWidget *m_bootHint;
    QList<GtkWidget *> m_diskRows;
    QList<GtkWidget *> m_bootRows;

    QList<Entry> m_entries;
    int m_nextId = 0;
    QList<BootItem> m_bootItems;
    /* The arguments have an order of their own; the page changed it */
    bool m_bootSet = false;
    bool m_bootChanged = false;
    /* New disks, by file name, until Apply creates them */
    QMap<QString, int> m_pending;
    bool m_virt = false;
};
