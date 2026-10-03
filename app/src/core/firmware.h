// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

#include "core/argsfile.h"

/*
 * The firmware images the distribution describes in the standard QEMU
 * firmware descriptors, the JSON files in /usr/share/qemu/firmware and
 * friends (docs/interop/firmware.json in QEMU).
 */
struct Firmware
{
    QString descriptorPath;
    QString description;
    QStringList interfaceTypes;     // uefi, bios
    QString code;                   // mapping.executable.filename
    QString varsTemplate;           // mapping.nvram-template.filename, if any
    QString format;                 // raw or qcow2
    QString mode;                   // split, combined or stateless
    QStringList machines;           // for the architecture, e.g. pc-q35-*
    QStringList features;           // secure-boot, enrolled-keys, requires-smm...

    bool isUefi() const;
    /* secure-boot with the Microsoft keys enrolled */
    bool hasSecureBoot() const;
    bool requiresSmm() const;
};

namespace FirmwareDb {

/*
 * The flash firmware for @arch (by default Paths::hostArch()) in @dirs, by
 * default /usr/share/qemu/firmware, /etc/qemu/firmware and
 * ~/.config/qemu/firmware: later directories override files of the same
 * name, and the result is in file name order, which is the priority order.
 */
QList<Firmware> list(const QStringList &dirs = {}, const QString &arch = {});
/* The usual machine of @arch: q35 for x86_64, virt for aarch64 */
QString defaultMachine(const QString &arch = {});
/* The first UEFI firmware for @machine (by default the usual one) with or
   without secure boot */
std::optional<Firmware> find(bool secureBoot, const QString &machine = {},
                             const QStringList &dirs = {}, const QString &arch = {});
/*
 * Makes @fw the firmware of @args: replaces its pflash drives (and -bios)
 * with those of @fw, and for firmware that requires SMM (x86 secure boot)
 * sets smm=on and the secure flash.  The firmware and its variable store template are
 * copied into @vmDir, unless copies are there already, and referenced
 * relatively (QEMU runs in the VM folder): the VM keeps its firmware
 * wherever its folder goes, to another distribution even, and the
 * variables stay with the firmware they were made for.
 */
bool apply(ArgsFile &args, const Firmware &fw, const QString &vmDir,
           QString *error = nullptr);
/*
 * Copies the firmware files @args names outside @vmDir (its pflash drives,
 * -pflash and -bios) into it, and references the copies relatively: for
 * an imported VM, like apply() does for a new one.  @copied gets the
 * files copied.
 */
bool copyIntoVm(ArgsFile &args, const QString &vmDir, QStringList *copied = nullptr,
                QString *error = nullptr);

}
