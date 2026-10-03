// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QLabel>
#include <QPointer>

class Vm;
class VmConsole;

namespace PerfStats {
class Sampler;
struct Snapshot;
}

/*
 * How smoothly the selected VM runs, in the status bar while it runs: the
 * frames its display shows, their latency and the input latency, and how
 * long QEMU's main loop waits for a CPU.  The tooltip has the details.
 *
 * The frames come from QEMU's SDL display (x-query-display-stats), or for
 * a VM shown in vitrine's window from the screen itself: frames on screen
 * and the time from QEMU's update to the screen.
 */
class PerfMonitor : public QLabel
{
    Q_OBJECT

public:
    explicit PerfMonitor(QWidget *parent = nullptr);

    /* The VM to follow, or none, and its console, whose screen measures
       its frames while it shows */
    void setVm(Vm *vm, VmConsole *console = nullptr);

protected:
    bool event(QEvent *event) override;

private:
    void refresh();
    /* The sampler's, with the display from the screen when it shows */
    bool snapshot(PerfStats::Snapshot *s) const;
    QString tooltip() const;

    QPointer<Vm> m_vm;
    QPointer<VmConsole> m_console;
    PerfStats::Sampler *m_sampler;
};
