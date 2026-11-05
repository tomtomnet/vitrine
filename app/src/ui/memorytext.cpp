// SPDX-License-Identifier: GPL-2.0-or-later
#include "memorytext.h"

static QString gib(qint64 mib)
{
    return mib < 1024 ? QObject::tr("%1 MiB").arg(mib)
                      : QObject::tr("%1 GiB").arg(double(mib) / 1024, 0, 'f', 1);
}

QString MemoryText::label()
{
    return tr("Memory is short");
}

QString MemoryText::title()
{
    return tr("Memory Is Short");
}

QString MemoryText::text(qint64 guestMiB, qint64 heldMiB, const HostMemory::Info &host)
{
    return tr("<p>The running VMs have %1 of RAM, and hold %2 of it so far; the host "
              "has %3 free out of %4. As the guests take all of theirs, the host can "
              "run out, and the kernel then stops a program to free memory, often "
              "QEMU: its guest loses its unsaved work.</p>"
              "<p>Close programs or VMs, or give the VMs less memory.</p>")
        .arg(gib(guestMiB), gib(heldMiB), gib(host.availableMiB), gib(host.totalMiB));
}
