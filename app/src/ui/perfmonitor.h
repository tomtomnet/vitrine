// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QLabel>
#include <QPointer>

class Vm;

namespace PerfStats {
class Sampler;
}

/*
 * How smoothly the selected VM runs, in the status bar while it runs: the
 * frames its display shows, their latency and the input latency, and how
 * long QEMU's main loop waits for a CPU.  The tooltip has the details.
 */
class PerfMonitor : public QLabel
{
    Q_OBJECT

public:
    explicit PerfMonitor(QWidget *parent = nullptr);

    /* The VM to follow, or none */
    void setVm(Vm *vm);

protected:
    bool event(QEvent *event) override;

private:
    void refresh();
    QString tooltip() const;

    QPointer<Vm> m_vm;
    PerfStats::Sampler *m_sampler;
};
