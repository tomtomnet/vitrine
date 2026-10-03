// SPDX-License-Identifier: GPL-2.0-or-later
#include "hostmemory.h"

#include <QFile>
#include <QRegularExpression>

namespace HostMemory {

/* "Name:   1234 kB" in /proc files */
static qint64 fieldMiB(const QString &text, const QString &name)
{
    const QRegularExpression re("^" + QRegularExpression::escape(name) + ":\\s+(\\d+) kB",
                                QRegularExpression::MultilineOption);
    return re.match(text).captured(1).toLongLong() / 1024;
}

Info read(const QString &meminfo)
{
    QFile f(meminfo);
    Info info;

    if (f.open(QIODevice::ReadOnly)) {
        const QString text = QString::fromLatin1(f.readAll());
        info.totalMiB = fieldMiB(text, "MemTotal");
        info.availableMiB = fieldMiB(text, "MemAvailable");
        info.swapFreeMiB = fieldMiB(text, "SwapFree");
    }
    return info;
}

qint64 residentMiB(qint64 pid, const QString &proc)
{
    QFile f(QString("%1/%2/status").arg(proc).arg(pid));

    if (pid <= 0 || !f.open(QIODevice::ReadOnly)) {
        return 0;
    }
    return fieldMiB(QString::fromLatin1(f.readAll()), "VmRSS");
}

qint64 growthMiB(const QList<Vm> &vms)
{
    qint64 growth = 0;

    for (const Vm &vm : vms) {
        /* the devices, the display, the caches of QEMU */
        const qint64 whole = vm.guestMiB + vm.guestMiB / 20 + 128;
        growth += qMax<qint64>(0, whole - vm.residentMiB);
    }
    return growth;
}

qint64 reserveMiB(const Info &host)
{
    return qMax<qint64>(1024, host.totalMiB / 20);
}

bool tight(const Info &host, const QList<Vm> &vms)
{
    return host.totalMiB > 0 && host.availableMiB - growthMiB(vms) < reserveMiB(host);
}

}
