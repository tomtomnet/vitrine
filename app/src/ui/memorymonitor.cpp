// SPDX-License-Identifier: GPL-2.0-or-later
#include "memorymonitor.h"

#include <QMessageBox>
#include <QStyle>
#include <QTimer>
#include <QToolButton>

#include "core/hostmemory.h"
#include "core/vmconfig.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "ui/icons.h"
#include "ui/memorytext.h"
#include "ui/widgets.h"

MemoryMonitor::MemoryMonitor(VmStore *store, QWidget *window)
    : QObject(window), m_store(store), m_window(window),
      m_button(Widgets::statusButton(
          "memoryMonitor", Icons::themed({"dialog-warning"}, QStyle::SP_MessageBoxWarning),
          MemoryText::label())),
      m_timer(new QTimer(this))
{
    connect(m_button, &QToolButton::clicked, this, [this]() {
        auto *box = Widgets::messageBox(QMessageBox::Warning, MemoryText::title(),
                                        m_button->toolTip(), QMessageBox::Close, m_window);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->open();
    });

    /* the guests take their memory as they go */
    m_timer->setInterval(3000);
    connect(m_timer, &QTimer::timeout, this, &MemoryMonitor::refresh);
    m_timer->start();
    for (Vm *vm : store->vms()) {
        watch(vm);
    }
    connect(store, &VmStore::added, this, &MemoryMonitor::watch);
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
        m_button->setToolTip(MemoryText::text(guest, held, host));
    }
    m_button->setVisible(tight);
}
