// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QList>
#include <QObject>
#include <QStringList>

#include <utility>

#include "core/argsfile.h"

class VmRunner;

/*
 * The internal snapshots of a VM, kept in its qcow2 files.  While the VM
 * runs, QEMU takes them with the running state (savevm), goes back to them
 * (loadvm) and deletes them (delvm); while it is stopped, qemu-img does, on
 * the disks alone.  A snapshot of the VM is the one of that name on each
 * of the files it writes to; QEMU keeps the running state in one of them.
 */
class VmSnapshots : public QObject
{
    Q_OBJECT

public:
    struct Snapshot {
        QString name;
        QDateTime date;
        qint64 vmClockMs = 0;       // how long the VM had run
        qint64 stateBytes = 0;      // the running state; 0 for the disks only
        QStringList files;          // those that have it
    };

    /* A file the VM writes to, which snapshots are about */
    struct Drive {
        QString file;               // absolute
        QString format;             // as given, else as its name tells; empty if unknown
        bool pflash = false;        // the UEFI variables
        bool canSnapshot() const { return format == "qcow2"; }
    };

    explicit VmSnapshots(VmRunner *runner, QObject *parent = nullptr);
    ~VmSnapshots() override;

    /* The arguments and folder of the VM, for qemu-img while it is stopped */
    void setVm(const ArgsFile &args, const QString &dir);

    /* As last listed, oldest first */
    QList<Snapshot> snapshots() const;
    /* Taking, restoring or deleting one */
    bool isBusy() const;

    /* Lists them again, then listed() */
    void refresh();
    /* Then finished(), with an error or not, and listed() */
    void take(const QString &name);
    void restore(const QString &name);
    void remove(const QString &name);

    /* The files @args writes to: disks and UEFI variables, not CD/DVD images */
    static QList<Drive> drives(const ArgsFile &args, const QString &dir);
    /* The snapshots of images, from `qemu-img info` or query-block: by name, oldest first */
    static QList<Snapshot> merge(const QList<std::pair<QString, QJsonArray>> &images);
    /* The images a running VM writes to, with their snapshots, from query-block */
    static QList<std::pair<QString, QJsonArray>> images(const QJsonArray &queryBlock);
    /* The error an HMP command printed, if any */
    static QString hmpError(const QString &output);
    /* A double-quoted HMP argument */
    static QString hmpQuote(const QString &text);
    /* A name QEMU takes for a name, not for the ID of another snapshot */
    static bool isValidName(const QString &name);
    /* What QEMU said, in words for the user */
    static QString explain(const QString &error);
    /* Whether savevm failed only for the running state, which a snapshot of the
       qcow2 disks can do without; and why, in words for the user */
    static bool needsDisksOnly(const QString &error);
    static QString disksOnlyReason(const QString &error);
    /* The blockdev-snapshot-internal-sync actions of a transaction snapshotting
       the qcow2 images a running VM writes to, from query-block */
    static QJsonArray diskActions(const QJsonArray &queryBlock, const QString &name);

signals:
    /* @error is empty when the list is up to date */
    void listed(const QString &error);
    /* An action ended; @error is empty on success */
    void finished(const QString &error);
    /* How it went, when not as asked: e.g. the disks only */
    void notice(const QString &text);
    void busyChanged(bool busy);

private:
    struct Private;
    Private *d;
};
