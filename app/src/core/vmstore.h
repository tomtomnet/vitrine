// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QObject>

#include "core/argsfile.h"

class QFileSystemWatcher;
class VmRunner;

/*
 * A VM is a folder holding vm.args, its QEMU command line one option per
 * line, and whatever the VM owns: disks, the UEFI variable store, logs.
 */
class Vm : public QObject
{
    Q_OBJECT

public:
    explicit Vm(const QString &dir, QObject *parent = nullptr);

    /* The folder name */
    QString id() const;
    QString dir() const;
    /* <dir>/vm.args */
    QString argsPath() const;
    /* -name, else the id */
    QString name() const;
    const ArgsFile &args() const { return m_args; }
    /* Writes vm.args atomically */
    bool save(const ArgsFile &args, QString *error = nullptr);
    VmRunner *runner() const { return m_runner; }

signals:
    /* vm.args saved, or edited outside the manager */
    void changed();

private:
    friend class VmStore;
    void reload();

    QString m_dir;
    ArgsFile m_args;
    VmRunner *m_runner;
};

class VmStore : public QObject
{
    Q_OBJECT

public:
    /* @dir is Paths::vmsDir() but for tests */
    explicit VmStore(const QString &dir, QObject *parent = nullptr);

    QString dir() const { return m_dir; }
    /* By name */
    QList<Vm *> vms() const;
    Vm *find(const QString &id) const;
    /* A new folder named after @name, holding a vm.args with just -name */
    Vm *create(const QString &name, QString *error = nullptr);
    /* Moves the folder of @vm, disks included, to the trash */
    bool remove(Vm *vm, QString *error = nullptr);
    /* Picks up folders added or removed outside the manager */
    void reload();

signals:
    void added(Vm *vm);
    /* The Vm object is deleted later */
    void removed(const QString &id);

private:
    QString m_dir;
    QList<Vm *> m_vms;
    QFileSystemWatcher *m_watcher;
};

/* qemu-img create -f qcow2 @path @bytes */
bool createDiskImage(const QString &path, qint64 bytes, QString *error = nullptr);
