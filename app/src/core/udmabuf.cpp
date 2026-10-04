// SPDX-License-Identifier: GPL-2.0-or-later
#include "udmabuf.h"

#include <QFile>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

#include "core/hostsettings.h"
#include "core/vmhardware.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"

namespace Udmabuf {

static QString tr(const char *text)
{
    return QObject::tr(text);
}

bool Limits::low() const
{
    return deviceErrno != 0 || !known() || listLimit < kMinListLimit ||
           sizeLimitMb < kMinSizeLimitMb;
}

Limits read(const QString &sysRoot, const QString &device)
{
    Limits limits;
    const int fd = ::open(QFile::encodeName(device).constData(), O_RDWR | O_CLOEXEC | O_NOCTTY);

    if (fd < 0) {
        limits.deviceErrno = errno;
        limits.deviceError = QString::fromLocal8Bit(strerror(errno));
    } else {
        ::close(fd);
    }
    auto value = [&sysRoot](const char *name) {
        QFile f(sysRoot + "/module/udmabuf/parameters/" + name);
        bool ok = false;
        const qint64 v = f.open(QIODevice::ReadOnly) ? f.readAll().trimmed().toLongLong(&ok) : -1;
        return ok && v >= 0 ? v : qint64(-1);
    };
    limits.listLimit = value("list_limit");
    limits.sizeLimitMb = value("size_limit_mb");
    return limits;
}

QString problem(const Limits &limits)
{
    if (limits.deviceErrno == ENOENT) {
        return tr("/dev/udmabuf does not exist: the kernel has no udmabuf driver, or its module "
                  "is not loaded");
    }
    if (limits.deviceErrno == EACCES || limits.deviceErrno == EPERM) {
        return tr("/dev/udmabuf is not open to you: systemd gives it to the user of the local "
                  "desktop session");
    }
    if (limits.deviceErrno) {
        return tr("/dev/udmabuf cannot be opened: %1").arg(limits.deviceError);
    }
    if (!limits.known()) {
        return tr("the host's udmabuf limits cannot be read");
    }
    return tr("the host's udmabuf limits are %1 entries and %2 MB, below what native context "
              "needs (%3 and %4 MB)")
        .arg(limits.listLimit)
        .arg(limits.sizeLimitMb)
        .arg(kMinListLimit)
        .arg(kMinSizeLimitMb);
}

QString grubbyCommand()
{
    return QString("sudo grubby --update-kernel=ALL --args='udmabuf.list_limit=%1 "
                   "udmabuf.size_limit_mb=%2'")
        .arg(kListLimit)
        .arg(kSizeLimitMb);
}

QString tmpfilesPath()
{
    return QStringLiteral("/etc/tmpfiles.d/udmabuf.conf");
}

QString tmpfilesContent()
{
    return QString("w /sys/module/udmabuf/parameters/list_limit - - - - %1\n"
                   "w /sys/module/udmabuf/parameters/size_limit_mb - - - - %2\n")
        .arg(kListLimit)
        .arg(kSizeLimitMb);
}

} // namespace Udmabuf

using namespace Udmabuf;

QString UdmabufWatch::Issue::text() const
{
    return tr("Guest windows drawn by the CPU will be copied: %1").arg(problem(limits));
}

UdmabufWatch::UdmabufWatch(VmStore *store, HostSettings *host, QObject *parent)
    : QObject(parent), m_store(store), m_host(host)
{
    if (host) {
        connect(host, &HostSettings::udmabufAnswered, this, &UdmabufWatch::answered);
    }
    if (store) {
        connect(store, &VmStore::added, this, &UdmabufWatch::watchVm);
        connect(store, &VmStore::removed, this, [this](const QString &id) {
            if (m_runs.take(id).issue.active()) {
                emit changed();
            }
        });
        for (Vm *vm : store->vms()) {
            watchVm(vm);
        }
    }
}

QList<UdmabufWatch::Issue> UdmabufWatch::issues() const
{
    QList<Issue> list;

    for (const Run &run : m_runs) {
        if (run.issue.active()) {
            list << run.issue;
            if (Vm *vm = m_store ? m_store->find(run.issue.vmId) : nullptr) {
                list.last().vmName = vm->name();
            }
        }
    }
    std::sort(list.begin(), list.end(), [](const Issue &a, const Issue &b) {
        return a.vmName.localeAwareCompare(b.vmName) < 0;
    });
    return list;
}

void UdmabufWatch::watchVm(Vm *vm)
{
    connect(vm->runner(), &VmRunner::stateChanged, this, [this, vm]() { stateChanged(vm); });
    stateChanged(vm);
}

void UdmabufWatch::stateChanged(Vm *vm)
{
    const VmRunner::State state = vm->runner()->state();
    auto it = m_runs.find(vm->id());

    if (state == VmRunner::State::Stopped) {
        /* the run's issue goes with it */
        if (it != m_runs.end()) {
            const bool active = it->issue.active();
            m_runs.erase(it);
            if (active) {
                emit changed();
            }
        }
        return;
    }
    if (it == m_runs.end()) {
        Run run;
        run.issue.vmId = vm->id();
        run.issue.vmName = vm->name();
        it = m_runs.insert(vm->id(), run);
    }
    const qint64 pid = vm->runner()->pid();
    if (pid > 0 && !it->checked &&
        (state == VmRunner::State::Running || state == VmRunner::State::Paused)) {
        it->pid = pid;
        it->checked = true;
        if (VmConfig::graphics(vm->runner()->runArgs()).nativeContext) {
            check(vm, *it);
        }
    }
}

void UdmabufWatch::check(Vm *vm, Run &run)
{
    const Answer answer = m_answers.take(run.pid);

    run.issue.limits = Udmabuf::read(m_sysRoot, m_device);
    if (!run.issue.limits.low()) {
        return;
    }
    if (!run.issue.limits.raisable()) {
        /* no device, no limits to raise: host tuning cannot help */
        notRaised(vm, run, QString());
    } else if (!m_host || !HostSettings::enabled()) {
        notRaised(vm, run, tr("host tuning is off"));
    } else if (!answer.raised && !answer.why.isEmpty()) {
        /* host tuning answered already (it is connected first) */
        notRaised(vm, run, answer.why);
    }
    /* else its answer comes: answered() */
}

void UdmabufWatch::answered(qint64 pid, bool raised, const QString &why)
{
    for (auto it = m_runs.begin(); it != m_runs.end(); ++it) {
        if (it->pid != pid || !it->checked) {
            continue;
        }
        Vm *vm = m_store ? m_store->find(it.key()) : nullptr;
        if (!vm || !it->issue.limits.low() || !it->issue.limits.raisable()) {
            return;
        }
        if (raised) {
            this->raised(vm, *it);
        } else {
            notRaised(vm, *it, why);
        }
        return;
    }
    /* before the VM's own start check: kept for it */
    if (m_answers.size() > 64) {
        m_answers.clear();
    }
    m_answers[pid] = {raised, why};
}

void UdmabufWatch::notRaised(Vm *vm, Run &run, const QString &why)
{
    if (run.issue.limitsLow && run.issue.notRaised == why) {
        return;
    }
    run.issue.limitsLow = true;
    run.issue.notRaised = why;
    if (!run.noted) {
        /* once per run, in the log, where the refusals will show */
        QStringList note{problem(run.issue.limits) +
                         tr(": QEMU will refuse guest windows drawn by the CPU (Qt Widgets and "
                            "GTK apps, cursors), which the guest then copies")};
        if (run.issue.limits.raisable()) {
            note << tr("host tuning would raise them while VMs run: %1").arg(why)
                 << tr("to raise them at each boot: %1 (see docs/host-tuning.md)")
                        .arg(grubbyCommand());
        }
        run.noted = true;
        vm->runner()->appendNote(note.join('\n'));
    }
    emit changed();
}

void UdmabufWatch::raised(Vm *vm, Run &run)
{
    if (!run.issue.limitsLow) {
        return;
    }
    run.issue.limitsLow = false;
    run.issue.notRaised.clear();
    if (run.noted) {
        vm->runner()->appendNote(tr("host tuning raised the udmabuf limits: guest windows made "
                                    "from now on are not copied"));
    }
    emit changed();
}
