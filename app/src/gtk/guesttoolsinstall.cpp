// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/guesttoolsinstall.h"

#include <QDateTime>
#include <QTimer>

#include <utility>

#include "core/buildmanager.h"
#include "core/guesttools.h"
#include "core/guesttoolsbuilder.h"
#include "core/snapshots.h"
#include "core/tasks.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "gtk/markup.h"
#include "ui/guesttoolstext.h"

using GuestTools::Pending;

/*
 * How long it waits for the guest to shut down before it gives up
 * restarting the VM (the install stays for its next start): a guest that
 * asks on its screen has the user's answer by then, or the user went
 * elsewhere, and should not see the VM start again whenever it stops later
 */
static int shutdownWaitMs = 5 * 60 * 1000;

void GuestToolsInstall::setShutdownWait(int ms)
{
    shutdownWaitMs = ms;
}

GuestToolsInstall::GuestToolsInstall(Vm *vm, Starter starter)
    : m_vm(vm), m_vmName(vm->name()), m_starter(std::move(starter)), m_wait(new QTimer(&m_qt))
{
    m_wait->setSingleShot(true);
    QObject::connect(m_wait, &QTimer::timeout, &m_qt, [this]() {
        if (m_step == Step::ShuttingDown) {
            stopWaiting("The guest did not shut down: the guest tools install at the VM's next "
                        "start.");
        }
    });
    QObject::connect(vm->runner(), &VmRunner::stateChanged, &m_qt, [this](VmRunner::State state) {
        if (m_step == Step::ShuttingDown && state == VmRunner::State::Stopped) {
            m_wait->stop();
            /* QEMU stopped by itself (a crash) is no shutdown to restart after */
            if (!m_vm->runner()->errorString().isEmpty()) {
                stopWaiting("The VM stopped with an error: the guest tools install at its next "
                            "start.");
            } else {
                next();
            }
            return;
        }
        /* what the dialog offers follows the VM */
        notify();
    });
    /* the VM gone: nothing more to do for it */
    QObject::connect(vm, &QObject::destroyed, &m_qt, [this]() {
        if (m_step != Step::Done) {
            m_cancelled = true;
            m_progress = "The VM was removed.";
            finish(Step::Done);
        }
    });
    /* a build starting or ending: Install waits for it, or goes on once it is done */
    if (BuildManager *builds = BuildManager::instance()) {
        QObject::connect(builds, &BuildManager::runStarted, &m_qt, [this]() { notify(); });
        QObject::connect(builds, &BuildManager::runFinished, &m_qt,
                         [this](const QString &error) { buildDone(error); });
    }
    if (GuestToolsBuilder *builder = GuestToolsBuilder::instance()) {
        QObject::connect(builder, &GuestToolsBuilder::finished, &m_qt,
                         [this](const QString &error) { buildDone(error); });
    }
}

GuestToolsInstall::~GuestToolsInstall()
{
    /* nothing said any more as it goes (m_qt, last, takes its connections); its task with it */
    changed = nullptr;
    if (m_task && m_task->isRunning()) {
        m_task->dismiss();
    }
}

Vm *GuestToolsInstall::vm() const
{
    return m_vm;
}

Task *GuestToolsInstall::task() const
{
    return m_task;
}

bool GuestToolsInstall::isBusy() const
{
    return m_step != Step::Idle && m_step != Step::Done;
}

void GuestToolsInstall::setDetails(std::function<void()> show)
{
    m_details = std::move(show);
    if (m_task) {
        m_task->setDetails(m_details);
    }
}

void GuestToolsInstall::notify()
{
    if (changed) {
        changed();
    }
}

void GuestToolsInstall::setStep(Step step, const QString &progress)
{
    m_step = step;
    m_progress = progress;
    if (m_task && m_task->isRunning()) {
        m_task->setText(step == Step::WaitingForBuild
                            ? QString("Waiting for the build of the medium")
                        : step == Step::ShuttingDown ? QString("Waiting for the guest to shut down")
                        : step == Step::Snapshot     ? QString("Taking a snapshot of the disks")
                                                     : QString("Starting the VM"));
    }
    notify();
}

void GuestToolsInstall::startTask()
{
    if (m_task && m_task->isRunning()) {
        return;
    }
    if (m_task) {
        m_task->dismiss();
    }
    m_task = TaskList::instance()->add(Task::Kind::Other,
                                       QString("Guest tools for %1").arg(m_vmName));
    if (m_details) {
        m_task->setDetails(m_details);
    }
}

void GuestToolsInstall::finishTask()
{
    if (!m_task || !m_task->isRunning()) {
        return;
    }
    if (m_cancelled) {
        /* the user's own doing: nothing to see */
        m_task->dismiss();
    } else if (m_error) {
        m_task->finish(Task::State::Failed, Markup::plain(m_progress));
    } else if (m_started) {
        m_task->finish(Task::State::Done, m_mediumOnly
                                              ? QString("Started with the tools medium")
                                              : QString("Started: the guest installs the tools"));
    } else {
        /* given up waiting: what happens next */
        m_task->finish(Task::State::Done, Markup::plain(m_progress));
    }
}

void GuestToolsInstall::finish(Step step)
{
    m_step = step;
    m_wait->stop();
    finishTask();
    notify();
}

void GuestToolsInstall::start(bool mediumOnly, bool snapshot)
{
    if (!m_vm || isBusy()) {
        return;
    }
    m_mediumOnly = mediumOnly;
    m_snapshot = snapshot;
    m_cancelled = m_started = m_error = false;
    m_progress.clear();
    startTask();
    if (GuestToolsText::mediumBeingBuilt(GuestToolsText::toolsRelease(m_vm))) {
        setStep(Step::WaitingForBuild,
                "The guest tools install as soon as the build under way is done.");
        return;
    }
    GuestToolsMonitor::of(m_vm)->setPending(mediumOnly ? Pending::Medium : Pending::Bootstrap);
    if (m_vm->runner()->isActive()) {
        /*
         * The gentlest way the guest takes (VmRunner::powerdown()); the
         * window's notice says how it was asked, and the dialog is out of
         * the way of the VM's screen for as long as it takes
         */
        setStep(Step::ShuttingDown,
                "Waiting for the guest to shut down. If it asks what to do, answer it on its "
                "screen: the VM then starts again with the tools medium.");
        m_wait->start(shutdownWaitMs);
        m_vm->runner()->powerdown();
        return;
    }
    next();
}

void GuestToolsInstall::buildDone(const QString &error)
{
    if (!m_vm || m_step != Step::WaitingForBuild) {
        notify();
        return;
    }
    if (!error.isEmpty()) {
        stopWaiting(QString("The build did not end well (%1): nothing was installed.")
                        .arg(error.toHtmlEscaped()));
        return;
    }
    /* a run of the host only, or a part of the run: the medium is still to come */
    if (GuestToolsText::mediumBeingBuilt(GuestToolsText::toolsRelease(m_vm))) {
        return;
    }
    m_step = Step::Idle;
    if (const QString problem = GuestToolsText::obstacle(m_vm, GuestToolsText::mediumOf(m_vm));
        !problem.isEmpty()) {
        stopWaiting(problem.toHtmlEscaped());
        return;
    }
    start(m_mediumOnly, m_snapshot);
}

void GuestToolsInstall::next()
{
    if (!m_vm) {
        return;
    }
    if (m_step < Step::Snapshot && m_snapshot) {
        const QString name =
            QString("Before guest tools %1")
                .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm"));

        setStep(Step::Snapshot, QString("Taking the snapshot “%1”…").arg(name.toHtmlEscaped()));
        m_snapshots = new VmSnapshots(m_vm->runner(), &m_qt);
        m_snapshots->setVm(m_vm->args(), m_vm->dir());
        QObject::connect(m_snapshots, &VmSnapshots::finished, &m_qt, [this](const QString &error) {
            if (m_cancelled) {
                m_progress = "The snapshot is written; the VM stays off.";
                finish(Step::Done);
                return;
            }
            if (!error.isEmpty()) {
                fail(QString("The snapshot failed: %1").arg(VmSnapshots::explain(error)));
                return;
            }
            next();
        });
        m_snapshots->take(name);
        return;
    }
    setStep(Step::Starting, "Starting the VM…");
    m_started = true;
    if (m_starter) {
        m_starter(m_vm);
    } else {
        m_vm->runner()->start(m_vm->args());
    }
    finish(Step::Done);
}

void GuestToolsInstall::cancel()
{
    switch (m_step) {
    case Step::WaitingForBuild:
        /* nothing asked of the VM yet */
        m_cancelled = true;
        m_progress.clear();
        finish(Step::Done);
        break;
    case Step::ShuttingDown:
        /* no install after all, nor restart (the guest was asked already) */
        m_cancelled = true;
        m_progress.clear();
        if (m_vm) {
            GuestToolsMonitor::of(m_vm)->setPending(Pending::None);
        }
        finish(Step::Done);
        break;
    case Step::Snapshot:
        /* not while qemu-img writes it: the VM stays off once it is written */
        if (!m_cancelled) {
            m_cancelled = true;
            if (m_vm) {
                GuestToolsMonitor::of(m_vm)->setPending(Pending::None);
            }
            m_progress = "The snapshot is being written; then the VM stays off.";
            notify();
        }
        break;
    case Step::Starting:
    case Step::Idle:
    case Step::Done:
        break;
    }
}

void GuestToolsInstall::stayOff()
{
    if (m_step == Step::ShuttingDown) {
        stopWaiting("The VM stays off: the guest tools install at its next start.");
    }
}

void GuestToolsInstall::stopWaiting(const QString &why)
{
    m_progress = why;
    finish(Step::Done);
}

void GuestToolsInstall::fail(const QString &error)
{
    m_error = true;
    m_progress = QString("<b>%1</b><br>%2")
                     .arg(error.toHtmlEscaped(),
                          QString("The VM is off; the guest tools install at its next start.")
                              .toHtmlEscaped());
    finish(Step::Done);
}
