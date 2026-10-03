// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QString>

#include "argsfile.h"

/*
 * The settings the pages show, read from the command line and written back
 * into it.  Writing only changes the keys a setting owns: whatever else is
 * on those lines, hand-written or unknown to the manager, stays.
 */
namespace VmConfig {

QString name(const ArgsFile &args);
void setName(ArgsFile &args, const QString &name);

/*
 * The QEMU binary of this VM, the #qemu directive, e.g. a build with a
 * patch the VM needs; empty for the one in the preferences
 */
QString qemuBinary(const ArgsFile &args);
void setQemuBinary(ArgsFile &args, const QString &path);

/* QEMU sizes: a number with an optional K/M/G/T suffix */
qint64 parseSize(const QString &text, qint64 unit);
QString formatMiB(qint64 mib);

/*
 * Guest RAM: the size of the memory backend that -machine memory-backend=
 * names, else -m (MiB without suffix).  0 if unset (QEMU's default).
 */
qint64 memoryMiB(const ArgsFile &args);
void setMemoryMiB(ArgsFile &args, qint64 mib);

struct Cpus {
    int count = 1;
    int sockets = 0;        // 0: not given, QEMU computes it
    int cores = 0;
    int threads = 0;
    QString model;          // -cpu model, e.g. host; empty for the default
};
Cpus cpus(const ArgsFile &args);
void setCpus(ArgsFile &args, const Cpus &cpus);
/* FEATURE=on on the -cpu line, unless the line sets it already, either way */
void enableCpuFeature(ArgsFile &args, const QString &feature);

/*
 * Folders shared with virtiofs, the #share directives.  At start the
 * manager runs virtiofsd for each and adds the vhost-user-fs device.
 */
struct Share {
    QString tag;            // what the guest mounts: mount -t virtiofs TAG DIR
    QString path;           // host folder
    QString cache = "auto"; // virtiofsd --cache: auto, always or never
    bool readonly = false;
    /* Where the guest mounts it at start, if anywhere: systemd or its agent */
    QString mount = {};
};
QList<Share> shares(const ArgsFile &args);
void setShares(ArgsFile &args, const QList<Share> &shares);

/*
 * virtiofsd (vhost-user) maps guest RAM, which must come from a memory
 * backend it can share: memfd, or a file with share=on.
 */
bool hasSharedMemory(const ArgsFile &args);
/* Switch guest RAM to a shared memfd backend, keeping its size */
void useSharedMemory(ArgsFile &args);

/*
 * The files the arguments read: the values of -hda, -cdrom, -bios,
 * -kernel..., and of the file=, filename=, path=, mem-path= and script=
 * keys, but for the options that write files (-trace, -audiodev...) and
 * the sockets of -chardev.  QEMU runs in the VM folder, so relative paths
 * are relative to it.
 */
struct FileRef {
    int line;               // in args.lines
    QString key;            // empty when the whole value is the path
    QString path;
};
QList<FileRef> files(const ArgsFile &args);
void setFile(ArgsFile &args, const FileRef &file, const QString &path);

/* Host PCI devices passed through: -device vfio-pci,host=ADDRESS */
QStringList pciPassthrough(const ArgsFile &args);
void setPciPassthrough(ArgsFile &args, const QStringList &addresses);

/* Host USB devices passed through by ID: -device usb-host,vendorid=,productid= */
struct UsbId {
    quint16 vendor = 0;
    quint16 product = 0;
    bool operator==(const UsbId &o) const
    {
        return vendor == o.vendor && product == o.product;
    }
};
QList<UsbId> usbPassthrough(const ArgsFile &args);
void setUsbPassthrough(ArgsFile &args, const QList<UsbId> &ids);
/* A USB controller for the devices above, e.g. qemu-xhci */
bool hasUsbController(const ArgsFile &args);

}
