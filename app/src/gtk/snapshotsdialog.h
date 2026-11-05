// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/dialog.h"

#include <QList>
#include <QPointer>

#include <functional>

class Vm;
class VmSnapshots;

/*
 * The snapshots of a VM: take one, go back to one, start from one, delete
 * one; the Qt app's SnapshotView, on the core's VmSnapshots.  Each
 * snapshot is a row, the latest first, with what it holds (the disks, or
 * the running state too) and its actions.  Start From It asks the window
 * to start the VM from it (@startFrom), the snapshot's running state, or
 * its disks taken back first.
 */
class SnapshotsDialog : public Gtk4::Dialog
{
public:
    /* Start the VM, from the running state of @snapshot; empty: as its disks are now */
    using StartFrom = std::function<void(const QString &snapshot)>;

    SnapshotsDialog(Vm *vm, Presenter presenter, StartFrom startFrom);

    /* For tests: the names listed, the latest first */
    QStringList names() const;
    bool isBusy() const;
    void take(const QString &name);

private:
    void fill();
    void updateDrives();
    void updateButtons();
    void busy(const QString &what);
    /* The question for Take: the name, then the snapshot */
    void askName();
    void restore(const QString &name);
    void startFrom(const QString &name);
    void remove(const QString &name);
    bool hasState(const QString &name) const;
    void closed() override;

    QPointer<Vm> m_vm;
    VmSnapshots *m_snapshots;
    StartFrom m_startFrom;
    /* no qcow2 file to keep a snapshot in, now */
    bool m_cannotTake = false;
    /* Start From It of a snapshot of the disks only: the start once they are back */
    bool m_startAfterRestore = false;

    AdwBanner *m_drives;
    AdwBanner *m_error;
    AdwBanner *m_notice;
    GtkWidget *m_take;
    GtkWidget *m_stack;
    AdwPreferencesGroup *m_list;
    QList<GtkWidget *> m_rows;
    /* each row's actions, to follow the VM's state */
    struct Actions {
        QString name;
        GtkWidget *restore;
        GtkWidget *start;
        GtkWidget *remove;
    };
    QList<Actions> m_actions;
    GtkWidget *m_progress;
    GtkWidget *m_progressText;
};
