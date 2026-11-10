// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QHash>
#include <QObject>
#include <QStringList>

#include <functional>

#include "core/hostdevices.h"

class HelperSession;
class QTimer;
class Vm;
class VmStore;

/*
 * PCI devices passed through (-device vfio-pci,host=...), made ready as a
 * VM starts - after virtiofsd, before QEMU (VmRunner::setDevicePreparer):
 * vitrine-helper puts their IOMMU groups on vfio-pci, makes
 * /dev/vfio/<group> the user's and raises vitrine's locked memory limit,
 * which QEMU inherits, to what the guest's memory needs; it gives all of it
 * back when that QEMU ends, crash included, or when the start fails before
 * it.  One helper per such start, host tuning on or off: "vitrine-helper
 * devices" when the devices are on vfio-pci already (bound at boot): no
 * password for the vitrine group; "devices-bind" when one is to be taken
 * from its driver: an administrator's password, which polkit keeps a few
 * minutes.  Polkit's dialog asks whoever needs a password.  No helper at
 * all when everything is ready already (bound at boot, a udev rule for the
 * node, a limit of the session's).  README.md, PCI passthrough.
 */
class PciPassthrough : public QObject
{
    Q_OBJECT

public:
    /* What a VM start needs, as far as this user can tell (the helper checks again as root) */
    struct Plan {
        QString verb;       // "devices-bind", "devices", or "": all ready already
        QString problem;    // why the start cannot go on, a sentence; "" when it can
    };
    static Plan plan(const QStringList &addresses, qint64 lockMiB,
                     const QString &sysRoot = QStringLiteral("/sys"),
                     const QString &procRoot = QStringLiteral("/proc"));
    /*
     * What the PCI settings say of @dev for a VM that passes @named through
     * with @guestMiB of memory: a few words ("Ready", "Taken from amdgpu at
     * start", what keeps it) and their explanation.  @open:
     * HostDevices::openNodes(), taken once for the page.
     */
    struct Status {
        QString text;
        QString details;
        bool ok = false;    // nothing keeps it from being passed through
    };
    static Status status(const PciDevice &dev, const QStringList &named, qint64 guestMiB,
                         const HostDevices::OpenNodes &open,
                         const QString &sysRoot = QStringLiteral("/sys"));
    /* The helper's refusal "error vfio ...: why", as the start's error says it */
    static QString explain(const QString &helperLine);

    explicit PciPassthrough(VmStore *store, QObject *parent = nullptr);
    ~PciPassthrough() override;
    /* The app's, for the Details; null before */
    static PciPassthrough *instance();

    /*
     * The VM @vmId's start (VmRunner's preparer): @done gets "" once its
     * devices are ready, else why the start cannot go on
     */
    void prepare(const QString &vmId, const QString &vmName, const QStringList &addresses,
                 qint64 lockMiB, const std::function<void(const QString &error)> &done);
    /* Its QEMU started as @pid: the helper watches it, then it is on its own */
    void launched(const QString &vmId, qint64 pid);
    /* Its run is over, or its start before QEMU: what QEMU did not take goes back */
    void stopped(const QString &vmId);
    /* The driver @address was taken from for @vmId's run, "" if none (the Details) */
    QString takenFrom(const QString &vmId, const QString &address) const;
    /* A helper runs for @vmId */
    bool helperRunning(const QString &vmId) const;

    /* Tests: run @command VERB instead of pkexec <helper> VERB, without asking polkit */
    void setHelperCommand(const QStringList &command) { m_command = command; }
    void setSysRoot(const QString &root) { m_sysRoot = root; }
    void setProcRoot(const QString &root) { m_procRoot = root; }
    /* Tests: how long a helper has to answer, and a start waits for the
       helper of the run before */
    void setAnswerMs(int ms) { m_answerMs = ms; }
    void setWaitMs(int ms) { m_waitMs = ms; }

signals:
    /* For the status bar: a device that did not go back to its driver */
    void notice(const QString &text);
    /* Each line of the helper of @vmId's run */
    void helperLine(const QString &vmId, const QString &line);
    /* That helper ended */
    void helperFinished(const QString &vmId);

private:
    struct Run;

    /* @run let go of, its helper's session and timer with it (later: one of
       them may be sending the signal that got here) */
    static void forget(Run *run);
    /* @run let go of, its helper left to end on its own (then reaped) */
    static void retire(Run *run);
    /* The answer's time starts: from the helper's start when polkit wants no
       password, from its "ready" (the dialog's end) otherwise */
    void startTimeout(Run *run);
    void watchVm(Vm *vm);
    /* polkit asked, then the helper runs */
    void ask(Run *run);
    void spawn(Run *run);
    void answer(Run *run, const QString &error);
    void handleLine(const QString &vmId, quint64 serial, const QString &line);
    void finished(const QString &vmId, quint64 serial);
    /* @text in the VM's log, as a "vitrine: " note */
    void note(const QString &vmId, const QString &text) const;
    Run *current(const QString &vmId, quint64 serial) const;

    VmStore *m_store;
    QStringList m_command;
    QString m_sysRoot = QStringLiteral("/sys");
    QString m_procRoot = QStringLiteral("/proc");
    QHash<QString, Run *> m_runs;       // VM id -> its run, one at a time
    /* a start waiting for the helper of the VM's run before: its own guard,
       whose end ends its timer, the start itself */
    struct Waiting {
        QObject *guard = nullptr;
        std::function<void()> go;
    };
    QHash<QString, Waiting> m_waiting;
    quint64 m_serial = 0;
    int m_answerMs = 60000;
    int m_waitMs = 30000;
};
