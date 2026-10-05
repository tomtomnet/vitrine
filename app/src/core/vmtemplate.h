// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>
#include <QStringList>

#include <functional>
#include <optional>
#include <utility>

#include "core/argsfile.h"

struct QemuInfo;

/*
 * The arguments of a new VM: a modern machine with KVM, virtio devices for
 * Linux and devices Windows has drivers for, shared memory for virtiofs,
 * and the clipboard channel of spice-vdagent.  On x86-64 a q35 PC, on
 * aarch64 (Asahi Linux on Apple silicon) the virt machine, which has no
 * VGA, IDE or PS/2: PCI graphics, virtio and SCSI disks, USB input.
 *
 * Linux on a PC gets the research launcher's machine (host/run-vm.sh):
 * the 3D card with DRM native context and the guest's vblank locked to
 * the host's, shown in vitrine's window; the disk on an iothread; virtio
 * input, sound and random numbers; NAT through passt with the guest's SSH
 * on a port of this computer; UEFI without Secure Boot, as the guest
 * tools' kernel module is not signed.
 */
namespace VmTemplate {

enum class Os { Linux, Windows11, Windows, Other };
enum class Firmware { Uefi, UefiSecureBoot, Bios };
enum class Graphics { Accelerated, Standard, Compatible };

struct Options {
    QString name;
    Os os = Os::Linux;
    /* Of a Linux guest, e.g. kde or gnome: the #guest directive */
    QString desktop;
    /* What it runs, e.g. fedora44 (GuestOs): the #guest directive's id */
    QString system;
    qint64 memoryMiB = 4096;
    int cpus = 4;
    /* Of the topology on a PC; 0: 2 when this computer's cores have two */
    int threadsPerCore = 0;
    /* Relative to the VM folder or absolute; empty for none */
    QString disk;
    QString iso;
    /* But for Linux on a PC, which gets the 3D card */
    Graphics graphics = Graphics::Accelerated;
    /* DRM native context, for Accelerated */
    bool nativeContext = false;
    /* QEMU's name of the guest architecture; empty for the host's */
    QString arch;
    /* Linux on a PC: the port of this computer forwarded to the guest's
       SSH, 0 for none; NAT through passt rather than QEMU's own */
    int sshPort = 0;
    bool passt = false;
    /*
     * The properties of virtio-gpu-gl-pci in the QEMU the VM runs with, to
     * leave out those of vitrine's QEMU it lacks; not set for vitrine's
     * QEMU, built or not, which has them all
     */
    std::optional<QStringList> gpuProperties;
};

struct Defaults {
    qint64 memoryMiB;
    int cpus;
    int diskGiB;
    Firmware firmware;
    Graphics graphics;
};
Defaults defaults(Os os);

/* What the machine of @arch (empty for the host's) offers */
bool hasBios(const QString &arch = {});
bool hasVga(const QString &arch = {});

/*
 * What the QEMU a VM runs with offers.  @chosen is the QEMU the user chose
 * for it, its #qemu line else the other QEMU of the preferences: empty
 * for vitrine's, which has all the template uses and is not asked, built
 * or not; until it is, VmRunner::start() refuses such a VM where the
 * system's QEMU lacks what it uses.  A chosen QEMU is asked: from @info,
 * its QemuInfo, else (null: not loaded yet, or it could not be) from the
 * binary itself.  One that does not answer offers none of it, unless it
 * is vitrine's.
 *
 * NAT through passt: installed on this computer, and in that QEMU, which
 * has it if built with it, as vitrine's is.
 */
bool hasPasst(const QemuInfo *info, const QString &chosen);
/* The properties of virtio-gpu-gl-pci; not set for vitrine's QEMU */
std::optional<QStringList> gpuProperties(const QemuInfo *info, const QString &chosen);

/*
 * The properties the 3D card of Linux on a PC gets, KEY and VALUE in the
 * order the template writes them, when the QEMU has them all: what
 * CardUpdate offers VMs made before
 */
const QList<std::pair<QString, QString>> &cardProperties();

/*
 * The arguments, in sections.  @addFirmware is called where the firmware
 * section goes, to append its lines (FirmwareDb::apply does); a section
 * left empty is dropped.
 */
ArgsFile build(const Options &options,
               const std::function<void(ArgsFile &)> &addFirmware = {});

}
