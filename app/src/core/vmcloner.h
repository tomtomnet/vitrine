// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QObject>
#include <QStringList>

#include "core/argsfile.h"

class QProcess;
class QTimer;
class Vm;
class VmStore;

/*
 * Clones a VM, as virt-manager does: a new VM with the same arguments and
 * copies of the disks and firmware, the files it writes, copied into its
 * folder.  cp --reflink=auto makes the copies instant on btrfs and XFS,
 * sharing the data until either VM changes it.  The files it only reads,
 * CD/DVD images, kernels and the like, stay shared, and the network cards
 * get new MAC addresses, the VM a new UUID.  The VM must be stopped: the
 * disks of a running one would be copied halfway through its writes.
 */
class VmCloner : public QObject
{
    Q_OBJECT
    /* for the metatype of Vm *, in finished() */
    Q_MOC_INCLUDE("core/vmstore.h")

public:
    struct Copy {
        QString from;
        QString to;             // in the folder of the clone
    };

    explicit VmCloner(VmStore *store, QObject *parent = nullptr);
    ~VmCloner() override;

    /*
     * The arguments of the clone of @args, from the folder @from to @to,
     * and the copies to make for them
     */
    static ArgsFile plan(const ArgsFile &args, const QString &name, const QString &from,
                         const QString &to, QList<Copy> *copies);
    /* The files a clone of @vm copies, with their sizes */
    static QList<std::pair<QString, qint64>> filesToCopy(const Vm *vm);

    bool isRunning() const { return m_clone != nullptr; }
    void start(Vm *source, const QString &name);
    /* The clone is removed */
    void cancel();

signals:
    /* In bytes */
    void progress(qint64 done, qint64 total);
    /* @clone is null on failure */
    void finished(Vm *clone, const QString &error);

private:
    void copyNext();
    void fail(const QString &error);

    VmStore *m_store;
    Vm *m_clone = nullptr;
    ArgsFile m_args;
    QList<Copy> m_copies;
    QProcess *m_process = nullptr;
    QTimer *m_timer;
    qint64 m_done = 0;
    qint64 m_total = 0;
};
