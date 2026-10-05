// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QPointer>
#include <QWidget>

class Banner;
class QLabel;
class QPushButton;
class QTableWidget;
class Vm;
class VmSnapshots;

/* The snapshots of a VM: take one, go back to one, start from one, delete one */
class SnapshotView : public QWidget
{
    Q_OBJECT

public:
    explicit SnapshotView(QWidget *parent = nullptr);

    void setVm(Vm *vm);

signals:
    /* Start From It: the VM, stopped, starts from snapshot @name (its
       running state); an empty @name: as it is, its disks taken back already */
    void startRequested(const QString &name);

protected:
    void showEvent(QShowEvent *event) override;

private:
    void fill();
    void updateButtons();
    void updateDrives();
    QString selected() const;
    void take();
    void restore();
    void remove();
    void startFrom();
    /* Whether snapshot @name holds the VM's running state, not the disks only */
    bool hasState(const QString &name) const;
    void busy(const QString &what);

    QPointer<Vm> m_vm;
    VmSnapshots *m_snapshots = nullptr;
    Banner *m_drives;
    Banner *m_error;
    Banner *m_notice;
    QTableWidget *m_table;
    QLabel *m_progress;
    QPushButton *m_take;
    QPushButton *m_restore;
    QPushButton *m_start;
    QPushButton *m_delete;
    /* no qcow2 file to keep a snapshot in, now */
    bool m_cannotTake = false;
    /* Start From It of a snapshot of the disks only: the start once they are back */
    bool m_startAfterRestore = false;
};
