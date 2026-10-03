// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include "core/argsfile.h"

/*
 * The hardware of a VM as the pages show it: machine and accelerator,
 * graphics, disks and boot.  Like the rest of VmConfig, reading takes the
 * arguments as they are, and writing changes only what a setting owns,
 * keeping the other keys and lines as written.  What is set up by hand in
 * ways these cannot follow comes back as Custom, for the Arguments page.
 */
namespace VmConfig {

/* The type of the last -machine that has one, e.g. q35 */
QString machineType(const ArgsFile &args);
void setMachineType(ArgsFile &args, const QString &type);

/* -accel, -machine accel= or -enable-kvm; empty when unset (TCG) */
QString accel(const ArgsFile &args);
void setAccel(ArgsFile &args, const QString &accel);
/*
 * A property of the accelerator, e.g. honor-guest-pat.  Only -accel takes
 * them, so an accelerator given with -machine accel= or -enable-kvm moves
 * there.  An empty @value removes the property.
 */
QString accelProperty(const ArgsFile &args, const QString &key);
void setAccelProperty(ArgsFile &args, const QString &key, const QString &value);

/* The graphics card and the window it shows in */
struct Graphics {
    enum Kind {
        Accelerated,    // virtio-gpu with OpenGL (virgl, native context, Venus)
        Virtio,         // virtio-gpu, 2D
        Standard,       // VGA, QEMU's default
        None,
        Custom,         // e.g. qxl-vga, several cards, -nographic
    };
    Kind kind = Standard;
    /* e.g. virtio-vga-gl, virtio-gpu-gl-pci; empty for -vga or the default */
    QString device;
    /* DRM native context: the guest uses the host GPU's own driver */
    bool nativeContext = false;
    /* Vulkan through Venus */
    bool venus = false;
    /* hostmem=, the memory window of blob resources; 0 when not given */
    qint64 hostmemMiB = 0;
    /* -display: sdl, gtk, none...; empty for QEMU's default (-display
       vnc= is a VNC server, not the window) */
    QString display;
    /* Why it is Custom */
    QString custom;
};
Graphics graphics(const ArgsFile &args);
/*
 * For Accelerated, OpenGL goes on in the window; native context and Venus
 * add blob=on, hostmem= (4G when not given) and, with KVM, honor-guest-pat.
 * A Custom card is left as it is; only the window changes.
 */
void setGraphics(ArgsFile &args, const Graphics &graphics);
/*
 * Where the VM's screen shows: in vitrine's window, through QEMU's D-Bus
 * display (-display dbus,p2p=yes); in a window of QEMU's own (sdl, gtk, or
 * QEMU's default, which it opens only without -vnc and -spice); or nowhere
 * (none, egl-headless, -nographic, VNC or SPICE alone, or a D-Bus
 * display on the session bus or the bus of addr=, which vitrine cannot
 * attach to)
 */
enum class Screen { Embedded, OwnWindow, None };
Screen screen(const ArgsFile &args);
/*
 * Shows the screen in vitrine's window (-display dbus,p2p=yes) or in
 * QEMU's SDL window, with OpenGL for a 3D card; the card stays as it is
 */
void setScreen(ArgsFile &args, Screen screen);
/* With a VGA mode, which shows the firmware and boot screens */
bool isVgaDevice(const QString &device);
/* The same virtio card with OpenGL, or without: virtio-vga for virtio-vga-gl */
QString glCounterpart(const QString &device);

/* Hard disks and CD/DVD drives */
struct Disk {
    enum Bus { Virtio, Sata, Scsi, Nvme, Usb, Other };
    int line = -1;          // the -drive, -hdX or -cdrom line
    int deviceLine = -1;    // the -device of an if=none drive
    QString file;           // empty for an empty CD/DVD drive
    bool cdrom = false;
    Bus bus = Virtio;
    QString format;         // as given, e.g. qcow2
    int bootindex = -1;     // of its device, -1 when not given
    /* false for -blockdev and other setups to edit by hand */
    bool editable = true;
};
QList<Disk> disks(const ArgsFile &args);
/* After the other disks: -drive file=,format=,if=virtio|ide (SATA on q35) */
void addDisk(ArgsFile &args, const QString &file, Disk::Bus bus);
/* An empty CD/DVD drive if @iso is empty */
void addCdrom(ArgsFile &args, const QString &iso);
/* Takes the disk out of the VM; the file stays */
void removeDisk(ArgsFile &args, const Disk &disk);
/* The disc in a CD/DVD drive; an empty @iso ejects it, keeping the drive */
void setDisc(ArgsFile &args, const Disk &drive, const QString &iso);
/* The disk format of a file name, e.g. qcow2; empty when unknown */
QString diskFormat(const QString &path);

/*
 * The network as the Network page shows it: one card behind NAT, through
 * passt or QEMU's user-mode network, with a port of this computer
 * forwarded to the guest's SSH server.  Other setups (a bridge, a tap,
 * several cards, a -netdev in JSON) are Custom, left as written.
 */
struct Network {
    enum Kind {
        Nat,
        Off,            // no card: -nic none, or -nodefaults
        Custom,
    };
    Kind kind = Nat;
    /* passt or user; empty for QEMU's default card, when nothing is set */
    QString backend;
    /* The card, e.g. virtio-net-pci; empty for QEMU's default */
    QString card;
    /* The host port forwarded to the guest's port 22, 0 for none */
    int sshPort = 0;
    /* Why it is Custom */
    QString custom;
};
Network network(const ArgsFile &args);
/*
 * Nat or Off; a Custom network is left alone.  Off puts -nic none where
 * the card was.  Turning NAT on adds a -netdev of @network.backend (user
 * when empty) and a card (@network.card, else virtio-net-pci, or e1000e
 * for Windows on a PC); passt goes over vhost-user when guest RAM is
 * shared and the card is a virtio-net one, which vhost-user needs.
 * Otherwise only the forward changes, on 127.0.0.1, keeping the other
 * keys of the -netdev.
 */
void setNetwork(ArgsFile &args, const Network &network);

/* Boot */
enum class FirmwareKind { Bios, Uefi, UefiSecureBoot, Custom };
/* By the firmware descriptors in @dirs (the standard ones by default) */
FirmwareKind firmwareKind(const ArgsFile &args, const QStringList &dirs = {});
/* Back to SeaBIOS: without the pflash drives and the secure flash */
void useBios(ArgsFile &args);

bool bootMenu(const ArgsFile &args);
void setBootMenu(ArgsFile &args, bool on);

enum class BootDevice { Default, Disk, Cdrom, Network };
/* The device with the lowest bootindex, else -boot order= */
BootDevice firstBootDevice(const ArgsFile &args);
/*
 * bootindex=1 on the first disk, CD/DVD drive or network card, which both
 * SeaBIOS and OVMF follow, and no bootindex on the others; -boot order=
 * goes.  A disk given by -drive if=virtio|ide, -hdX or -cdrom becomes an
 * if=none drive and its -device, where bootindex goes.  False when there
 * is no such device to boot from.
 */
bool setFirstBootDevice(ArgsFile &args, BootDevice device);

}
