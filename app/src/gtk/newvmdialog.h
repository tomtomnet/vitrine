// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/dialog.h"

#include <QStringList>

#include <functional>

class Vm;
class VmStore;

/*
 * New VM: a folder, a disk, the firmware variables and vm.args, as the Qt
 * app's NewVmDialog makes them, through the same core calls.  It asks
 * only what vitrine cannot choose: the system, the memory and processors,
 * the disk, the disc to install from; the rest comes from the template,
 * and the settings change it afterwards.
 *
 * Create is there once the VM has a name, and a disk image if it uses
 * one.  Once made, @created gets the VM, what could not be done as asked
 * (no UEFI firmware, say), and whether to open its settings.
 */
class NewVmDialog : public Gtk4::Dialog
{
public:
    using Created = std::function<void(Vm *vm, const QStringList &warnings, bool openSettings)>;

    NewVmDialog(VmStore *store, Presenter presenter, Created created);

    /* What the Create button does: the VM, or an alert saying why not */
    Vm *create();

    /* The fields, for tests: the file choosers' answers too */
    void setName(const QString &name);
    /* In the order of the list: Linux with KDE Plasma, with GNOME, another; Windows 11... */
    void setSystem(int index);
    void setMemoryMiB(int mib);
    void setCpus(int cpus);
    void setNewDisk(int gib);
    void setDiskImage(const QString &path);
    void setInstallDisc(const QString &path);
    void setOpenSettings(bool open);
    int memoryMiB() const;
    int cpus() const;
    int diskGiB() const;
    bool canCreate() const;

private:
    void applyDefaults();
    void update();
    bool make(Vm *vm, QString *error);

    VmStore *m_store;
    Created m_created;
    QString m_diskImage;
    QString m_installDisc;
    QStringList m_warnings;

    GtkWidget *m_name;
    GtkWidget *m_system;
    GtkWidget *m_note;
    GtkWidget *m_memory;
    GtkWidget *m_cpus;
    GtkWidget *m_newDisk;
    GtkWidget *m_diskSize;
    GtkWidget *m_existingDisk;
    GtkWidget *m_imageRow;
    GtkWidget *m_chooseImage;
    GtkWidget *m_discRow;
    GtkWidget *m_clearDisc;
    GtkWidget *m_settings;
    GtkWidget *m_create;
};
