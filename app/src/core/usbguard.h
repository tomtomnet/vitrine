// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>

#include "core/argsfile.h"
#include "core/hostdevices.h"
#include "core/usbhotplug.h"
#include "core/vmstore.h"

template <typename T>
class QFutureWatcher;
class QSocketNotifier;
class QTimer;

/*
 * Two guards for the USB devices VMs pass through (the user's choice of
 * 2026-11-10, after a phone that adb's server, GNOME's MTP mounts and two
 * VMs fought over), on this side, without root:
 *
 *  - One device, one VM.  A VM does not start while another running VM has
 *    a device its arguments would take too, or would take the same devices,
 *    and a device is not given to a running VM that another running VM has
 *    or would take (VmRunner's start check and UsbHotplug's attach check,
 *    set on each VM of the store): two QEMUs would race for it at each of
 *    its reconnects.  A rule by vendor and product ID takes any device with
 *    those IDs, so it conflicts with every rule and every device of those
 *    IDs in another VM, however many such devices are plugged in.  A device
 *    given while the VM runs goes by its bus and address: two identical
 *    devices can go to two VMs that way.
 *  - What else holds a device a running VM passes through: adb's server,
 *    GNOME's MTP or camera mounts (gvfs), another QEMU, which take some of
 *    its interfaces or reset it under the guest.  The processes of this
 *    user with its node open (/proc/PID/fd: other users' cannot be read
 *    without root), but the VM's own QEMU; else, with none of those, an
 *    interface claimed through usbfs while the VM's QEMU does not hold the
 *    device, by a program of another user or of the system.  Looked for as
 *    the VMs' devices are read (a start, a VM found running, a device given
 *    or taken back), after the kernel's USB events while running VMs have
 *    USB devices (a device plugged in again, an interface claimed or let
 *    go: a netlink socket, open only then), and every few seconds while
 *    something holds one, in a thread of its own: issues() and changed(),
 *    and notice() once per new holder, also written to that VM's log.
 */
class UsbGuard : public QObject
{
    Q_OBJECT

public:
    /* A program holding a device's node */
    struct Holder {
        enum class Kind { Adb, Mtp, Gphoto2, Qemu, Other };
        qint64 pid = 0;
        QString program;        // argv[0]'s file name, else its comm
        Kind kind = Kind::Other;
        /* a QEMU's VM: the store's name for one of its VMs, else its -name */
        QString vmName;
        bool vitrine = false;   // that VM is one of the store's
        /* "adb (pid 1234)", "fedora-kde6e's QEMU (pid 2345)" */
        QString text() const;
        bool operator==(const Holder &other) const = default;
    };
    /* A device a running VM passes through that something else holds too */
    struct Issue {
        QString vmId;
        QString vmName;
        UsbDevice device;
        /* this user's processes with its node open, the VM's QEMU aside */
        QList<Holder> holders;
        /* none of those, but an interface claimed through usbfs (lsusb -t:
           Driver=usbfs) while the VM's QEMU does not hold it */
        bool claimedElsewhere = false;
        /* "adb (pid 1234) holds vivo X200 Ultra, which fedora-gnome1 passes
           through": plain, one sentence without its period */
        QString text() const;
    };
    /* A running VM, as a scan is read for it */
    struct Run {
        QString id;
        QString name;
        qint64 pid = 0;         // its QEMU
        QList<UsbHotplug::Device> devices;
    };
    /* What a scan finds: the devices plugged in (but hubs), who holds their nodes */
    struct Scan {
        QList<UsbDevice> hosts;
        QHash<QString, QList<Holder>> held;     // by node, under the /dev scanned
    };

    explicit UsbGuard(VmStore *store, QObject *parent = nullptr);
    ~UsbGuard() override;
    /* The app's, null before */
    static UsbGuard *instance();

    /* The devices something else holds now, by VM */
    QList<Issue> issues() const { return m_issues; }

    /* The usb-host devices of @args (QemuOpts lines), as QEMU's filters */
    static QList<UsbHotplug::Device> filtersOf(const ArgsFile &args);
    /*
     * Whether a host device could go to both @a and @b: one of them takes a
     * device in particular (its node, or its bus and address) that is
     * plugged in (@hosts) and the other takes it too; else none of their
     * filters is apart.  An empty filter takes no device in particular.
     */
    static bool overlap(const UsbHotplug::Device &a, const UsbHotplug::Device &b,
                        const QList<UsbDevice> &hosts);
    /*
     * Why @vm may not start with @args: another running VM of @store has a
     * device they take, or would take the same; "" when it may.  Nothing is
     * read when @args pass no USB device through, nor when no other VM does.
     */
    static QString startConflict(VmStore *store, const Vm *vm, const ArgsFile &args);
    /*
     * The running VM of @store, not @vm, that has @host or would take it,
     * and @why, as a sentence; null when none.  @hosts: the devices plugged
     * in (read when not given), which tell whether a rule by IDs that takes
     * @host has another device of those IDs.
     */
    static Vm *attachConflict(VmStore *store, const Vm *vm, const UsbDevice &host,
                              QString *why = nullptr, const QList<UsbDevice> *hosts = nullptr);

    /* The devices of @sysfs and the nodes under @dev this user's processes hold (@proc) */
    static Scan scan(const QString &sysfs, const QString &dev, const QString &proc);
    /* What @scan says of @runs; @qemus: the store's QEMUs by pid, which name their VMs */
    static QList<Issue> issuesOf(const QList<Run> &runs, const Scan &scan, const QString &dev,
                                 const QHash<qint64, QString> &qemus = {});
    static Holder::Kind kindOf(const QString &program);

    /* Tests: where /proc is; how long after a change it looks, how often while one holds */
    void setProcRoot(const QString &root) { m_procRoot = root; }
    void setDelays(int settleMs, int recheckMs);
    /* A USB device changed (the kernel said so, or a test): it looks after the settle delay */
    void hostChanged();
    /* It looks now, in this thread (tests) */
    void scanNow();
    /* The kernel's events are read: a running VM has USB devices */
    bool listening() const { return m_uevents >= 0; }

signals:
    /* issues() changed */
    void changed();
    /* A new holder, as Issue::text() says it, with its period: once while it holds */
    void notice(const QString &text);

private:
    void watchVm(Vm *vm);
    /* The devices of the VMs changed: a VM read, given a device, stopped */
    void vmsChanged();
    QList<Run> runs() const;
    QHash<qint64, QString> qemus() const;
    void listen(bool on);
    void readUevents();
    /* A scan after the settle delay, unless one is coming */
    void settle();
    void startScan();
    void scanned(const Scan &scan);
    /* issues() from the last scan, for the VMs as they are now */
    void update();

    QPointer<VmStore> m_store;
    QString m_procRoot = QStringLiteral("/proc");
    int m_settleMs = 1000;
    int m_recheckMs = 5000;
    QTimer *m_settle;
    QTimer *m_recheck;
    QFutureWatcher<Scan> *m_watcher;
    bool m_again = false;           // asked while a scan ran
    Scan m_last;
    QList<Issue> m_issues;
    /* the holders told of (notice(), the log), "VM node pid", while they hold */
    QSet<QString> m_told;
    /* "VM node" claimed elsewhere at the last scan, and at the one before too:
       only those count (udev's probes claim a device for a moment) */
    QSet<QString> m_unsure;
    QSet<QString> m_confirmed;
    int m_uevents = -1;
    QSocketNotifier *m_notifier = nullptr;
};
