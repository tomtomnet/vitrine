// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include "core/argsfile.h"

/*
 * The firmware files a VM keeps in its folder: the copies of the UEFI code
 * and variable store that FirmwareDb::apply() and the importer make.  A
 * power cut or a slip of the hand can damage or delete them, and they can
 * be made again from the templates the firmware descriptors name.
 */
namespace FirmwareFiles {

struct File
{
    enum class Role { Code, Vars };

    Role role = Role::Vars;
    int line = -1;              // the option that names it
    QString path;               // absolute
    QString format;             // raw or qcow2
    QString templatePath;       // the system file it is a copy of; empty if unknown
    QString templateDescription;

    QString name() const;
};

/*
 * The flash files of @args (-drive if=pflash and -pflash) that are in
 * @vmDir, where relative paths start.  Their templates come from the
 * descriptors in @dirs for @arch: by default the standard directories and
 * the architecture of the VM's QEMU.
 */
QList<File> list(const ArgsFile &args, const QString &vmDir, const QStringList &dirs = {},
                 const QString &arch = {});

/*
 * What is wrong with @file, from its size, its qcow2 tables and the headers
 * at its start, next to its template: empty if nothing seems to be.  Quick,
 * no qemu-img.  A blank variable store is fine: UEFI formats it.
 */
QString problem(const File &file);

/* The snapshots a qcow2 file holds, from its header */
int snapshotCount(const QString &path);

/* A new copy of the template of @file, which is missing */
bool recreate(const File &file, QString *error = nullptr);

/*
 * Puts a new copy of the template of @file in its place, while the VM is
 * stopped, and keeps the old file next to it as @backup.  A readable qcow2
 * file with snapshots gets the template written into it instead, with
 * @qemuImg, so that its snapshots keep the variables they were taken with
 * (@keptSnapshots).
 */
bool reset(const File &file, const QString &qemuImg, QString *backup = nullptr,
           bool *keptSnapshots = nullptr, QString *error = nullptr);

/* The qemu-img of the VM's own QEMU, else the one of the preferences */
QString qemuImg(const ArgsFile &args);

/*
 * Whether a QEMU error says a file is in use: another QEMU holds its lock,
 * e.g. the VM runs outside the manager.  Such a file is not damaged.
 */
bool inUse(const QString &error);

}
