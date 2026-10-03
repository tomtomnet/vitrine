// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QWidget>

class QLabel;
class QTimer;
class Vm;
class VmStore;

/*
 * A warning in the status bar, there only when the running VMs could take
 * more memory than the host has free: out of memory, the kernel kills a
 * process to free some, often QEMU.
 */
class MemoryMonitor : public QWidget
{
    Q_OBJECT

public:
    explicit MemoryMonitor(VmStore *store, QWidget *parent = nullptr);

    void refresh();

private:
    void watch(Vm *vm);

    VmStore *m_store;
    QLabel *m_icon;
    QLabel *m_text;
    QTimer *m_timer;
};
