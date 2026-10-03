// SPDX-License-Identifier: GPL-2.0-or-later
#include "memorymonitor.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QStyle>
#include <QTimer>

#include "core/hostmemory.h"
#include "core/vmconfig.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "ui/icons.h"

static QString gib(qint64 mib)
{
    return mib < 1024 ? QObject::tr("%1 MiB").arg(mib)
                      : QObject::tr("%1 GiB").arg(double(mib) / 1024, 0, 'f', 1);
}

MemoryMonitor::MemoryMonitor(VmStore *store, QWidget *parent)
    : QWidget(parent), m_store(store), m_icon(new QLabel), m_text(new QLabel(tr("Memory is short"))),
      m_timer(new QTimer(this))
{
    auto *layout = new QHBoxLayout(this);
    const int size = style()->pixelMetric(QStyle::PM_SmallIconSize);

    setObjectName("memoryMonitor");
    layout->setContentsMargins(0, 0, 0, 0);
    m_icon->setPixmap(
        Icons::themed({"dialog-warning"}, QStyle::SP_MessageBoxWarning).pixmap(size));
    layout->addWidget(m_icon);
    layout->addWidget(m_text);

    /* the guests take their memory as they go */
    m_timer->setInterval(3000);
    connect(m_timer, &QTimer::timeout, this, &MemoryMonitor::refresh);
    m_timer->start();
    for (Vm *vm : store->vms()) {
        watch(vm);
    }
    connect(store, &VmStore::added, this, &MemoryMonitor::watch);
    hide();
    refresh();
}

void MemoryMonitor::watch(Vm *vm)
{
    connect(vm->runner(), &VmRunner::stateChanged, this, &MemoryMonitor::refresh);
}

void MemoryMonitor::refresh()
{
    const HostMemory::Info host = HostMemory::read();
    QList<HostMemory::Vm> vms;
    qint64 guest = 0, held = 0;

    for (Vm *vm : m_store->vms()) {
        if (vm->runner()->isActive()) {
            const qint64 ram = VmConfig::memoryMiB(vm->args());
            vms << HostMemory::Vm{ram > 0 ? ram : 128,
                                  HostMemory::residentMiB(vm->runner()->pid())};
            guest += vms.last().guestMiB;
            held += vms.last().residentMiB;
        }
    }

    const bool tight = !vms.isEmpty() && HostMemory::tight(host, vms);
    if (tight) {
        setToolTip(tr("<p>The running VMs have %1 of RAM, and hold %2 of it so far; the host "
                      "has %3 free out of %4. As the guests take all of theirs, the host can "
                      "run out, and the kernel then stops a program to free memory, often "
                      "QEMU: its guest loses its unsaved work.</p>"
                      "<p>Close programs or VMs, or give the VMs less memory.</p>")
                       .arg(gib(guest), gib(held), gib(host.availableMiB), gib(host.totalMiB)));
    }
    setVisible(tight);
}
