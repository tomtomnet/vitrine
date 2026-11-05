// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

class QTimer;
class Task;
class Vm;
class VmSnapshots;

/*
 * Install Guest Tools for one VM, as the Qt app's GuestToolsDialog goes
 * about it, apart from the dialog that shows it (GuestToolsDialog): the
 * window keeps it, and the dialog closes while the guest shuts down, out
 * of the way of the VM's screen, where the guest may ask what to do
 * (Plasma's logout screen).
 *
 * Install or Attach Medium Only: the next start of the VM brings the
 * medium and the bootstrap, or the medium only (GuestToolsMonitor::
 * setPending()).  A running VM is asked to shut down, the gentlest way
 * its guest takes; once it is off, a snapshot of its disks is taken if
 * asked, and the VM starts again.  Asked while a build makes the medium
 * again, it waits for the build first.  It gives up after a while when the
 * guest does not shut down, and when QEMU stops with an error: the install
 * stays for the VM's next start.
 *
 * Under way, it is one of the app's tasks (TaskList), which the close
 * question lists; its details are the dialog.  Qt Core only.
 */
class GuestToolsInstall
{
public:
    enum class Step {
        Idle,
        WaitingForBuild,    // a build makes the medium again: then the install
        ShuttingDown,       // the guest was asked to shut down
        Snapshot,           // the disks' snapshot being taken
        Starting,           // the VM asked to start with the medium
        Done,               // over: started, failed, called off, or given up
    };
    /* How the window starts a VM (its checks, USB access...) */
    using Starter = std::function<void(Vm *vm)>;

    GuestToolsInstall(Vm *vm, Starter starter);
    ~GuestToolsInstall();
    GuestToolsInstall(const GuestToolsInstall &) = delete;
    GuestToolsInstall &operator=(const GuestToolsInstall &) = delete;

    Vm *vm() const;
    Step step() const { return m_step; }
    /* Waiting for a build or the guest, writing the snapshot, starting the VM */
    bool isBusy() const;
    /* Over, the VM started with the medium */
    bool succeeded() const { return m_step == Step::Done && m_started; }
    /* Over with an error: what it says is one */
    bool failed() const { return m_step == Step::Done && m_error; }
    /* Over, called off */
    bool cancelled() const { return m_step == Step::Done && m_cancelled; }
    /* What it does now, or why it ended: rich text */
    QString progress() const { return m_progress; }
    bool mediumOnly() const { return m_mediumOnly; }
    Task *task() const;

    /* Install (@mediumOnly: Attach Medium Only), with a snapshot of the disks first if @snapshot */
    void start(bool mediumOnly, bool snapshot);
    /*
     * Called off: no install, nor restart (a guest asked to shut down was
     * asked already).  Not while the snapshot is written: the VM then stays
     * off once it is.
     */
    void cancel();
    /* The VM stays off once its guest shut down (the window closes once its VMs are off) */
    void stayOff();
    /* Its task's details: the dialog */
    void setDetails(std::function<void()> show);

    /* Its step or what it says, after each change */
    std::function<void()> changed;

    /* How long it waits for the guest to shut down (tests) */
    static void setShutdownWait(int ms);

private:
    void setStep(Step step, const QString &progress);
    /* A build ended (@error empty when all was built): what waited for it goes on */
    void buildDone(const QString &error);
    void next();
    /* Over: @why said, the install pending for the next start */
    void stopWaiting(const QString &why);
    void fail(const QString &error);
    void finish(Step step);
    void notify();
    /* The task, made as it starts */
    void startTask();
    void finishTask();

    QObject m_qt;
    QPointer<Vm> m_vm;
    QString m_vmName;
    Starter m_starter;
    Step m_step = Step::Idle;
    bool m_mediumOnly = false;
    bool m_snapshot = false;
    /* during the snapshot: no start after it */
    bool m_cancelled = false;
    bool m_started = false;
    bool m_error = false;
    QString m_progress;
    QTimer *m_wait;
    VmSnapshots *m_snapshots = nullptr;
    QPointer<Task> m_task;
    std::function<void()> m_details;
};
