// SPDX-License-Identifier: GPL-2.0-or-later
#include "pcipassthrough.h"

#include <QFileInfo>
#include <QPointer>
#include <QTimer>

#include <cstring>

#include "core/helpersession.h"
#include "core/hostsettings.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"

#ifndef VITRINE_HELPER_PATH
#define VITRINE_HELPER_PATH "/usr/libexec/vitrine-helper"
#endif

/* pkexec's line when it does not run the program: "...: Not authorized" */
static const char kPkexecError[] = "Error executing command as another user: ";

static QPointer<PciPassthrough> s_instance;

struct PciPassthrough::Run {
    quint64 serial = 0;
    QString vmId;
    QString vmName;
    QStringList addresses;
    qint64 lockMiB = 0;
    QString verb;                               // devices, devices-bind
    HelperSession *helper = nullptr;
    QTimer *timeout = nullptr;                  // for the answer
    std::function<void(const QString &)> done;  // until answered
    QHash<QString, QString> taken;              // address -> the driver it had
    bool watched = false;                       // its QEMU handed to the helper
    bool noPassword = false;                    // polkit wants none: no dialog to wait for
    bool retried = false;                       // devices, then devices-bind once
};

/* A message as part of a sentence: without its period */
static QString clause(QString text)
{
    text = text.trimmed();
    if (text.endsWith('.') && !text.endsWith("..")) {
        text.chop(1);
    }
    return text;
}

void PciPassthrough::forget(Run *run)
{
    if (!run) {
        return;
    }
    if (run->helper) {
        run->helper->deleteLater();
    }
    if (run->timeout) {
        run->timeout->deleteLater();
    }
    delete run;
}

void PciPassthrough::retire(Run *run)
{
    if (!run) {
        return;
    }
    if (HelperSession *helper = run->helper) {
        run->helper = nullptr;
        if (helper->running()) {
            /* its end reaped, then it goes; what it says no longer counts */
            QObject::disconnect(helper, &HelperSession::line, nullptr, nullptr);
            QObject::connect(helper, &HelperSession::finished, helper, &QObject::deleteLater);
        } else {
            helper->deleteLater();
        }
    }
    forget(run);
}

void PciPassthrough::startTimeout(Run *run)
{
    const QString vmId = run->vmId;
    const quint64 serial = run->serial;

    if (!run->done || run->timeout) {
        return;
    }
    run->timeout = new QTimer(this);
    run->timeout->setSingleShot(true);
    connect(run->timeout, &QTimer::timeout, this, [this, vmId, serial]() {
        if (Run *r = current(vmId, serial); r && r->done) {
            if (r->helper) {
                r->helper->cancel();
            }
            answer(r, tr("PCI passthrough: vitrine-helper did not answer in time."));
        }
    });
    run->timeout->start(m_answerMs);
}

PciPassthrough::Plan PciPassthrough::plan(const QStringList &addresses, qint64 lockMiB,
                                          const QString &sysRoot, const QString &procRoot)
{
    const HostDevices::OpenNodes open = HostDevices::openNodes();
    bool bind = false, node = false;
    Plan plan;

    for (const QString &address : addresses) {
        if (!QFileInfo::exists(sysRoot + "/bus/pci/devices/" + address)) {
            plan.problem = tr("PCI passthrough: %1 is not in this computer.").arg(address);
            return plan;
        }
        const PciDevice dev = HostDevices::pciDevice(address, sysRoot);
        const QStringList why = HostDevices::pciBlockers(dev, addresses, open, sysRoot, procRoot);
        if (!why.isEmpty()) {
            plan.problem = tr("PCI passthrough: %1").arg(why.first());
            return plan;
        }
        /* pci-stub's, or no driver: vfio-pci binds them at start too */
        bind |= dev.driver != "vfio-pci";
        node |= !HostDevices::pciNodeOpen(dev, sysRoot);
    }
    const qint64 limit = HostDevices::memlockLimit();
    const bool memlock = limit >= 0 && limit < (lockMiB << 20);
    plan.verb = bind ? QString("devices-bind") : node || memlock ? QString("devices") : QString();
    return plan;
}

PciPassthrough::Status PciPassthrough::status(const PciDevice &dev, const QStringList &named,
                                              qint64 guestMiB, const HostDevices::OpenNodes &open,
                                              const QString &sysRoot)
{
    const QStringList blockers = HostDevices::pciBlockers(dev, named, open, sysRoot);
    const QString node = QString("/dev/vfio/%1").arg(dev.iommuGroup);
    Status s;

    if (!blockers.isEmpty()) {
        s.text = clause(blockers.first());
        s.details = blockers.join('\n');
        return s;
    }
    s.ok = true;
    if (!HostSettings::helperInstalled()) {
        /* by hand, then: what it still lacks */
        const QStringList problems = HostDevices::pciProblems(dev, guestMiB, sysRoot);
        s.ok = problems.isEmpty();
        s.text = problems.isEmpty() ? tr("Ready") : problems.first();
        s.details = problems.isEmpty() ? tr("Ready to be passed through")
                                       : problems.join('\n') + '\n' +
                                             tr("vitrine-helper, which is not installed, would "
                                                "do this when the VM starts.");
        return s;
    }
    if (dev.driver != "vfio-pci") {
        s.text = dev.driver.isEmpty() ? tr("Bound to vfio-pci at start")
                                      : tr("Taken from %1 at start").arg(dev.driver);
        s.details = (dev.driver.isEmpty()
                         ? tr("When the VM starts, Vitrine binds it to vfio-pci")
                         : tr("When the VM starts, Vitrine takes it from %1 for vfio-pci")
                               .arg(dev.driver)) +
                    tr(", which asks for an administrator's password (kept a few minutes), and "
                       "lets you open %1 with enough locked memory for the VM. When the VM "
                       "stops, all goes back as it was.")
                        .arg(node);
        return s;
    }
    const qint64 limit = HostDevices::memlockLimit();
    if (!HostDevices::pciNodeOpen(dev, sysRoot) ||
        (limit >= 0 && limit < ((qMax<qint64>(guestMiB, 0) + 1024) << 20))) {
        s.text = tr("Ready at start");
        s.details = tr("Set aside for VMs (vfio-pci). When the VM starts, Vitrine lets you open "
                       "%1 and raises the locked memory limit, without a password for members "
                       "of the vitrine group; both go back when the VM stops.")
                        .arg(node);
        return s;
    }
    s.text = tr("Ready");
    s.details = tr("Ready to be passed through");
    return s;
}

QString PciPassthrough::explain(const QString &helperLine)
{
    /* "error vfio 0000:6e:00.0: why", "error vfio: why" */
    const qsizetype at = helperLine.indexOf(": ");
    const QString why = clause(at >= 0 ? helperLine.mid(at + 2) : helperLine);
    QString more;

    if (why.contains("gnome-shell (pid")) {
        more = tr("GNOME uses every GPU it finds: a udev rule tagging this one "
                  "TAG+=\"mutter-device-ignore\" keeps GNOME away from it from the next login, "
                  "or bind it to vfio-pci at boot (README.md, PCI passthrough).");
    } else if (why.contains("kwin_wayland (pid")) {
        more = tr("KDE Plasma uses every GPU it finds: KWIN_DRM_DEVICES naming only the others "
                  "keeps it away from this one, or bind it to vfio-pci at boot (README.md, PCI "
                  "passthrough).");
    } else if (why.contains("(boot_vga)")) {
        more = tr("Choose another GPU as the primary one in the firmware settings, or pass "
                  "another GPU through.");
    } else if (why.startsWith("a display is connected")) {
        more = tr("Unplug the display, or bind the GPU to vfio-pci at boot to give it to VMs "
                  "with its display (README.md, PCI passthrough).");
    } else if (why.endsWith(": pass it through too")) {
        more = tr("The devices of an IOMMU group go to a VM together: check it too in the PCI "
                  "Devices settings, or put the card in a slot whose group it has alone.");
    } else if (why.contains("the IOMMU is off")) {
        more = tr("Turn it on in the firmware settings of this computer (AMD-Vi or IOMMU on "
                  "AMD, VT-d on Intel, where the kernel may also want intel_iommu=on).");
    } else if (why.contains(" is mounted on ") || why.endsWith(" is in use as swap")) {
        more = tr("Unmount it first.");
    }
    return more.isEmpty() ? tr("PCI passthrough: %1.").arg(why)
                          : tr("PCI passthrough: %1. %2").arg(why, more);
}

PciPassthrough::PciPassthrough(VmStore *store, QObject *parent) : QObject(parent), m_store(store)
{
    if (!s_instance) {
        s_instance = this;
    }
    if (store) {
        for (Vm *vm : store->vms()) {
            watchVm(vm);
        }
        connect(store, &VmStore::added, this, &PciPassthrough::watchVm);
    }
}

PciPassthrough::~PciPassthrough()
{
    /* the helpers go on alone with the QEMUs they watch (their sessions
       close with this object, whose children they are) */
    for (Run *run : std::as_const(m_runs)) {
        forget(run);
    }
}

PciPassthrough *PciPassthrough::instance()
{
    return s_instance;
}

void PciPassthrough::watchVm(Vm *vm)
{
    const QPointer<PciPassthrough> self(this);
    const QString id = vm->id();

    vm->runner()->setDevicePreparer([self, id, vm](const QStringList &addresses, qint64 lockMiB,
                                                    const std::function<void(const QString &)> &done) {
        if (!self) {
            done(tr("PCI passthrough: Vitrine is closing."));
            return;
        }
        self->prepare(id, vm->name(), addresses, lockMiB, done);
    });
    connect(vm->runner(), &VmRunner::qemuLaunched, this,
            [this, id](qint64 pid) { launched(id, pid); });
    connect(vm->runner(), &VmRunner::stateChanged, this, [this, id](VmRunner::State state) {
        if (state == VmRunner::State::Stopped) {
            stopped(id);
        }
    });
}

PciPassthrough::Run *PciPassthrough::current(const QString &vmId, quint64 serial) const
{
    Run *run = m_runs.value(vmId);
    return run && run->serial == serial ? run : nullptr;
}

bool PciPassthrough::helperRunning(const QString &vmId) const
{
    const Run *run = m_runs.value(vmId);
    return run && run->helper && run->helper->running();
}

QString PciPassthrough::takenFrom(const QString &vmId, const QString &address) const
{
    const Run *run = m_runs.value(vmId);
    return run ? run->taken.value(address) : QString();
}

void PciPassthrough::prepare(const QString &vmId, const QString &vmName,
                             const QStringList &addresses, qint64 lockMiB,
                             const std::function<void(const QString &)> &done)
{
    /*
     * The helper of its run before still gives its devices back: after it.
     * Each waiting start has a guard of its own, its timer's context: a
     * start that ended meanwhile (stopped()) takes it away, timer included,
     * and a later one gets another
     */
    if (helperRunning(vmId)) {
        Waiting waiting;

        delete m_waiting.take(vmId).guard;
        waiting.guard = new QObject(this);
        waiting.go = [this, vmId, vmName, addresses, lockMiB, done]() {
            prepare(vmId, vmName, addresses, lockMiB, done);
        };
        QTimer::singleShot(m_waitMs, waiting.guard, [this, vmId, guard = waiting.guard]() {
            if (m_waiting.value(vmId).guard != guard) {
                return;
            }
            const Waiting late = m_waiting.take(vmId);
            late.guard->deleteLater();
            /* it did not end: on all the same, its helper says what holds them */
            retire(m_runs.take(vmId));
            late.go();
        });
        m_waiting.insert(vmId, waiting);
        return;
    }
    const Plan p = plan(addresses, lockMiB, m_sysRoot, m_procRoot);
    if (!p.problem.isEmpty()) {
        done(p.problem);
        return;
    }
    /* another VM of this vitrine's has it, or another device of its group */
    for (const Run *other : std::as_const(m_runs)) {
        if (other->vmId == vmId || !other->helper || !other->helper->running()) {
            continue;
        }
        for (const QString &a : addresses) {
            const int group = HostDevices::pciDevice(a, m_sysRoot).iommuGroup;
            for (const QString &b : other->addresses) {
                const int theirs = HostDevices::pciDevice(b, m_sysRoot).iommuGroup;
                if (a == b || (group >= 0 && theirs == group)) {
                    done(tr("PCI passthrough: %1 goes to %2, which runs.").arg(a, other->vmName));
                    return;
                }
            }
        }
    }
    forget(m_runs.take(vmId));
    if (p.verb.isEmpty()) {
        /* set up by hand: nothing to ask for */
        note(vmId, tr("PCI devices ready already: %1").arg(addresses.join(", ")));
        done(QString());
        return;
    }
    auto *run = new Run;
    run->serial = ++m_serial;
    run->vmId = vmId;
    run->vmName = vmName;
    run->addresses = addresses;
    run->lockMiB = lockMiB;
    run->verb = p.verb;
    run->done = done;
    m_runs.insert(vmId, run);
    ask(run);
}

void PciPassthrough::ask(Run *run)
{
    const QString vmId = run->vmId;
    const quint64 serial = run->serial;

    if (!m_command.isEmpty()) {
        /* tests: no polkit, no dialog */
        run->noPassword = true;
        spawn(run);
        return;
    }
    /* polkit first, for what keeps its action from it; a password it wants
       is pkexec's dialog's to ask for */
    HostSettings::checkAction("org.vitrine.helper." + run->verb, this,
                              [this, vmId, serial](const HostSettings::Status &status) {
        using Problem = HostSettings::Problem;
        Run *run = current(vmId, serial);

        if (!run) {
            return;
        }
        switch (status.problem) {
        case Problem::NotInstalled:
            answer(run, tr("PCI passthrough needs vitrine-helper, which is not installed (README.md, "
                           "Install), or the devices set up by hand (README.md, PCI passthrough)."));
            return;
        case Problem::NoPolicy:
            answer(run, tr("PCI passthrough: the installed vitrine-helper is older than Vitrine and "
                           "cannot make PCI devices ready: install it again."));
            return;
        case Problem::Failed:
            answer(run, tr("PCI passthrough: %1.").arg(clause(status.why())));
            return;
        default:
            break;
        }
        run->noPassword = status.active();
        spawn(run);
    });
}

void PciPassthrough::spawn(Run *run)
{
    const QString vmId = run->vmId;
    const quint64 serial = run->serial;
    const QStringList command =
        m_command.isEmpty()
            ? QStringList{"pkexec", "--disable-internal-agent", VITRINE_HELPER_PATH, run->verb}
            : m_command + QStringList{run->verb};
    QString error;

    run->helper = new HelperSession(this);
    connect(run->helper, &HelperSession::line, this,
            [this, vmId, serial](const QString &line) { handleLine(vmId, serial, line); });
    connect(run->helper, &HelperSession::finished, this,
            [this, vmId, serial]() { finished(vmId, serial); });
    if (!run->helper->start(command, &error)) {
        run->helper->deleteLater();
        run->helper = nullptr;
        answer(run, tr("PCI passthrough: %1.").arg(error));
        return;
    }
    /* sent once it is ready (pkexec's password dialog may come first) */
    run->helper->send(
        QString("vfio %1 %2").arg(run->addresses.join(',')).arg(run->lockMiB).toLatin1());
    /* without a dialog, it answers in seconds from now */
    if (run->noPassword) {
        startTimeout(run);
    }
}

void PciPassthrough::answer(Run *run, const QString &error)
{
    std::function<void(const QString &)> done = std::move(run->done);

    run->done = nullptr;
    if (run->timeout) {
        run->timeout->stop();
    }
    /* last: VmRunner may end the start in it, and the run with it */
    if (done) {
        done(error);
    }
}

void PciPassthrough::handleLine(const QString &vmId, quint64 serial, const QString &line)
{
    Run *run = current(vmId, serial);

    emit helperLine(vmId, line);
    if (!run) {
        return;
    }
    if (line.startsWith("ready")) {
        /* the dialog's end, a password typed: it answers in seconds */
        startTimeout(run);
    } else if (line.startsWith("error: ") && run->done) {
        /* its own, before any answer ("error: cannot use /run/vitrine-helper") */
        note(vmId, line);
        if (run->helper) {
            run->helper->cancel();
        }
        answer(run, tr("PCI passthrough: vitrine-helper: %1.").arg(clause(line.mid(7))));
    } else if (line.startsWith("error vfio") && run->done && run->verb == "devices" &&
               !run->retried && line.contains("needs vitrine-helper devices-bind")) {
        /*
         * On vfio-pci when planned, on their drivers again now: a dead
         * helper's record given back at this one's start.  Once more, with
         * the verb that binds - and the password it wants
         */
        auto *again = new Run;

        note(vmId, line);
        again->serial = ++m_serial;
        again->vmId = run->vmId;
        again->vmName = run->vmName;
        again->addresses = run->addresses;
        again->lockMiB = run->lockMiB;
        again->verb = "devices-bind";
        again->done = std::move(run->done);
        again->retried = true;
        run->done = nullptr;
        if (run->helper) {
            run->helper->cancel();
        }
        m_runs.insert(vmId, again);
        retire(run);
        ask(again);
    } else if (line.startsWith("ok vfio ")) {
        /* "ok vfio 0000:6e:00.0=amdgpu,0000:6e:00.1=snd_hda_intel: ..." */
        QString pairs = line.section(' ', 2, 2);
        if (pairs.endsWith(':')) {
            pairs.chop(1);
        }
        for (const QString &pair : pairs.split(',')) {
            const QString driver = pair.section('=', 1);
            if (!driver.isEmpty() && driver != "vfio-pci") {
                run->taken.insert(pair.section('=', 0, 0), driver);
            }
        }
        note(vmId, tr("PCI devices: %1").arg(line.mid(8)));
        answer(run, QString());
    } else if (line.startsWith("error vfio")) {
        note(vmId, line);
        /* nothing to watch: it ends */
        if (run->helper) {
            run->helper->cancel();
        }
        answer(run, explain(line));
    } else if (line.startsWith("restored vfio ") || line.startsWith("left vfio ") ||
               line.startsWith("error watch ")) {
        note(vmId, line);
        if (line.startsWith("left vfio ")) {
            /* "left vfio 21: why" */
            emit notice(tr("PCI devices of %1: %2.").arg(run->vmName, clause(line.section(": ", 1))));
        }
    }
}

void PciPassthrough::launched(const QString &vmId, qint64 pid)
{
    Run *run = m_runs.value(vmId);

    if (!run || !run->helper || run->watched || run->done) {
        return;
    }
    run->watched = true;
    run->helper->send("watch " + QByteArray::number(pid));
    /* on its own from now on: it ends after that QEMU, everything given back */
    run->helper->closeInput();
}

void PciPassthrough::stopped(const QString &vmId)
{
    Run *run = m_runs.value(vmId);

    /* a start that waits for the helper of the run before ends here: Force
       Off meanwhile; its guard takes its timer with it */
    delete m_waiting.take(vmId).guard;
    if (!run) {
        return;
    }
    /* the start ended before the answer (Force Off): it comes to nothing */
    run->done = nullptr;
    if (run->helper && run->helper->running()) {
        /* no QEMU took them: back now; else they go back with that QEMU,
           the helper's last lines to come */
        if (!run->watched) {
            run->helper->cancel();
        }
        return;
    }
    forget(m_runs.take(vmId));
}

void PciPassthrough::finished(const QString &vmId, quint64 serial)
{
    Run *run = current(vmId, serial);

    /* a helper retired meanwhile (its run replaced): nothing more of it */
    if (!run) {
        return;
    }
    {
        HelperSession *helper = run->helper;

        m_runs.remove(vmId);
        if (run->done && helper) {
            /* it ended without an answer: pkexec did not run it */
            QString why;
            for (const QString &l : helper->foreign()) {
                if (l.startsWith(kPkexecError)) {
                    why = clause(l.mid(int(strlen(kPkexecError))));
                    break;
                }
            }
            if (helper->exitCode() == 126 || why == "Request dismissed") {
                why = tr("the password dialog was dismissed");
            } else if (why == "Not authorized") {
                why = tr("not authorized (no administrator's password)");
            } else if (why.isEmpty()) {
                why = helper->foreign().isEmpty() ? tr("vitrine-helper ended without an answer")
                                                  : clause(helper->foreign().first());
            }
            answer(run, tr("PCI passthrough: %1.").arg(why));
        }
        forget(run);
    }
    emit helperFinished(vmId);
    /* the start that waited for it */
    const Waiting waiting = m_waiting.take(vmId);
    if (waiting.guard) {
        waiting.guard->deleteLater();
        waiting.go();
    }
}

void PciPassthrough::note(const QString &vmId, const QString &text) const
{
    if (Vm *vm = m_store ? m_store->find(vmId) : nullptr) {
        vm->runner()->appendNote(text);
    }
}
