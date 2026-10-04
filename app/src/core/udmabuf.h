// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QByteArrayView>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>

class HostSettings;
class QTimer;
class Vm;
class VmStore;

/*
 * The host's udmabuf, which a VM whose GPU has native context needs for
 * every guest buffer in guest memory: QEMU (vitrine's patches) makes each
 * one a udmabuf, one entry per contiguous piece of guest RAM, and refuses
 * the buffer when the kernel refuses that udmabuf.  KWin in the guest
 * makes such a buffer of every window drawn by the CPU (Qt Widgets and GTK
 * apps, cursors): a maximized 4K window is ~32 MB in 1,200 to 8,000 pieces,
 * and the kernel's defaults, list_limit 1024 and size_limit_mb 64, refuse
 * it.  The guest then copies that window at each change.
 */
namespace Udmabuf {

/* What host tuning sets, and the commands below */
constexpr qint64 kListLimit = 65536;
constexpr qint64 kSizeLimitMb = 2048;
/* Enough for a maximized 4K window and then some */
constexpr qint64 kMinListLimit = 16384;
constexpr qint64 kMinSizeLimitMb = 128;

struct Limits {
    qint64 listLimit = -1;      // -1: not known (no udmabuf parameters)
    qint64 sizeLimitMb = -1;
    int deviceErrno = 0;        // why /dev/udmabuf does not open read-write, 0 if it does
    QString deviceError;        // its text

    bool known() const { return listLimit >= 0 && sizeLimitMb >= 0; }
    /* Below what native context needs, or not known */
    bool tooLow() const;
    /* Native context's windows would be refused: tooLow(), or no device */
    bool low() const { return deviceErrno != 0 || tooLow(); }
    /* Host tuning could fix it: the device opens, its limits are there */
    bool raisable() const { return deviceError.isEmpty() && known(); }
};

/*
 * The limits under @sysRoot (/sys: world-readable) and whether @device
 * opens read-write for this user, as QEMU opens it.  The device first: a
 * module the kernel loads on the open has its parameters then.
 */
Limits read(const QString &sysRoot = QStringLiteral("/sys"),
            const QString &device = QStringLiteral("/dev/udmabuf"));

/* Why native context's windows would be copied, plain, without a period:
   "the host's udmabuf limits are 1024 entries and 64 MB (...)" */
QString problem(const Limits &limits);

/* The persistent ways to raise them, at each boot */
QString grubbyCommand();
QString tmpfilesPath();
QString tmpfilesContent();

/*
 * The lines of qemu.log that say a guest buffer was refused: "ctrl 0x10c,
 * error 0x1201" (RESOURCE_CREATE_BLOB refused) and "refusing it" (the same,
 * with -d guest_errors).  QEMU refuses one whenever it cannot hand it to
 * the host GPU; the udmabuf limits are one reason, told by QEMU's
 * "UDMABUF_CREATE_LIST: Invalid argument (N entries, M bytes)" warning
 * with more entries or bytes than the limits allow.
 */
struct LogCount {
    int blobRefused = 0;    // "ctrl 0x10c, error 0x1201"
    int refusing = 0;       // "refusing it"
    int createList = 0;     // UDMABUF_CREATE_LIST warnings, whatever the error
    int overLimits = 0;     // those of them beyond the limits

    /*
     * One line of the log, without its newline.  @limits: the limits as
     * the line is read, for a udmabuf refused: beyond them, or (QEMU
     * without the sizes in its warning) them too low for native context.
     */
    void scan(QByteArrayView line, const Limits &limits);
    /* Buffers refused: one refusal shows in several of these lines */
    int refusals() const;
    bool any() const { return refusals() > 0; }
    /* Of those, the ones the limits refused */
    int limitRefusals() const;
    bool operator==(const LogCount &) const = default;
};

} // namespace Udmabuf

/*
 * For each VM of @store while it runs: at its start, when its GPU has
 * native context, the udmabuf limits are checked, and when they are too
 * low and host tuning will not raise them, the log gets a note saying so
 * and the VM an issue (limitsLow); limits high enough then, but not held
 * by host tuning for it, are read again while it runs.  Its qemu.log is
 * read as it grows for buffers refused (log), the limits blamed only for
 * udmabufs beyond them.  One issue per VM per run, gone when it stops.
 */
class UdmabufWatch : public QObject
{
    Q_OBJECT

public:
    struct Issue {
        QString vmId;
        QString vmName;
        /* At the start of the run, its GPU with native context: the limits
           too low, and host tuning does not raise them */
        bool limitsLow = false;
        Udmabuf::Limits limits;     // read then
        QString notRaised;          // why host tuning does not ("" if it cannot help)
        Udmabuf::LogCount log;      // what qemu.log said so far
        bool active() const { return limitsLow || log.any(); }
        /* /dev/udmabuf did not open at the start check */
        bool device() const { return limitsLow && limits.deviceErrno != 0; }
        /* The udmabuf limits are to blame: too low at the start check, or
           buffers refused for them */
        bool limitsBlamed() const
        {
            return (limitsLow && !limits.deviceErrno) || log.limitRefusals() > 0;
        }
        /* For the status: plain, one sentence without its period */
        QString text() const;
    };

    /* @host: whether tuning raises them (null: it does not) */
    UdmabufWatch(VmStore *store, HostSettings *host, QObject *parent = nullptr);

    /* The VMs running with an issue, by name */
    QList<Issue> issues() const;
    /* The limits now, read where the start check reads them */
    Udmabuf::Limits limitsNow() const { return Udmabuf::read(m_sysRoot, m_device); }

    /* Tests: where /sys and the device are, how often the logs are read */
    void setSysRoot(const QString &root) { m_sysRoot = root; }
    void setDevice(const QString &device) { m_device = device; }
    void setPollInterval(int ms);
    /* The logs read now */
    void poll();

signals:
    /* issues() changed */
    void changed();

private:
    struct Run {
        qint64 pid = 0;
        qint64 offset = 0;          // read up to there
        QByteArray head;            // the log's first bytes: another log after a Clear
        QByteArray partial;         // a line not ended yet
        bool checked = false;       // the start check done (native context only)
        bool noted = false;         // the note on the limits written (in this run of QEMU)
        /* Host tuning does not hold the limits for it, why: they were high
           enough at its start check (another VM's hold, say), and are read
           again at each poll() until they are not */
        QString unheld;
        Issue issue;
    };
    struct Answer {
        bool raised = false;
        QString why;
    };

    void watchVm(Vm *vm);
    void stateChanged(Vm *vm);
    /* The start check of a native-context run */
    void check(Vm *vm, Run &run);
    /* Host tuning's answer for the QEMU @pid */
    void answered(qint64 pid, bool raised, const QString &why);
    /* The limits too low and not raised, for @why: the note, the issue */
    void notRaised(Vm *vm, Run &run, const QString &why);
    void raised(Vm *vm, Run &run);
    /* qemu.log read on from where it was */
    void read(Vm *vm, Run &run);
    /* The limits read again for a run they are not held for */
    void recheck(Vm *vm, Run &run);
    void updateTimer();

    VmStore *m_store;
    QPointer<HostSettings> m_host;
    QString m_sysRoot = QStringLiteral("/sys");
    QString m_device = QStringLiteral("/dev/udmabuf");
    QTimer *m_timer;
    QHash<QString, Run> m_runs;     // VM id -> its run
    QHash<qint64, Answer> m_answers;  // host tuning's, for QEMUs not checked yet
};
