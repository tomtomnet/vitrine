// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QString>

/*
 * Whether the running VMs fit in the memory of the host.  A guest takes its
 * RAM as it touches it, so a VM that holds little so far can still grow to
 * all of it: when the host runs out, the kernel kills a process to free
 * memory, often QEMU, and the guest loses its unsaved work.
 */
namespace HostMemory {

struct Info {
    qint64 totalMiB = 0;
    qint64 availableMiB = 0;        // MemAvailable
    qint64 swapFreeMiB = 0;
};
Info read(const QString &meminfo = "/proc/meminfo");
/* The resident memory of process @pid, 0 if unknown */
qint64 residentMiB(qint64 pid, const QString &proc = "/proc");

struct Vm {
    qint64 guestMiB;                // its RAM
    qint64 residentMiB;             // what QEMU holds so far
};
/* What the VMs can still take, besides what they hold: their RAM, and
   some for QEMU itself */
qint64 growthMiB(const QList<Vm> &vms);
/* What must stay free for the host: 5% of its RAM, 1 GiB at least */
qint64 reserveMiB(const Info &host);
/* Whether the VMs, grown to all their RAM, would leave the host short */
bool tight(const Info &host, const QList<Vm> &vms);

}
