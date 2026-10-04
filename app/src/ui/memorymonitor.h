// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QObject>

class QTimer;
class QToolButton;
class Vm;
class VmStore;

/*
 * A warning in the status bar, there only when the running VMs could take
 * more memory than the host has free: out of memory, the kernel kills a
 * process to free some, often QEMU.  A notice as the others there, which
 * a click explains as its tooltip does.
 */
class MemoryMonitor : public QObject
{
    Q_OBJECT

public:
    explicit MemoryMonitor(VmStore *store, QWidget *window);

    /* For the status bar */
    QToolButton *button() const { return m_button; }
    void refresh();

private:
    void watch(Vm *vm);

    VmStore *m_store;
    QWidget *m_window;
    QToolButton *m_button;
    QTimer *m_timer;
};
