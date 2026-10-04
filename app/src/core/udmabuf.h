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
    /* Native context's windows would be refused: limits below the minimum,
       not known, or no device */
    bool low() const;
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
 * The lines of qemu.log that say a guest buffer was refused: QEMU's
 * "UDMABUF_CREATE_LIST" warning and "ctrl 0x10c, error 0x1201" (the blob
 * refused), "refusing it" (with -d guest_errors), and virglrenderer's
 * "Couldn't find res_id" / "invalid res_id" for such a buffer used later
 */
struct LogCount {
    int createList = 0;
    int outOfMemory = 0;
    int refusing = 0;
    int unknownResource = 0;

    /* One line of the log, without its newline */
    void scan(QByteArrayView line);
    bool any() const { return createList || outOfMemory || refusing || unknownResource; }
    /* Buffers refused: one refusal shows in several of these lines */
    int refusals() const;
    bool operator==(const LogCount &) const = default;
};

} // namespace Udmabuf

/*
 * For each VM of @store while it runs: at its start, when its GPU has
 * native context, the udmabuf limits are checked, and when they are too
 * low and host tuning will not raise them, the log gets a note saying so
 * and the VM an issue (limitsWhy); and its qemu.log is read as it grows
 * for buffers refused (refusals).  One issue per VM per run, gone when it
 * stops.
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
        /* For the status: plain, one sentence without its period */
        QString text() const;
    };

    /* @host: whether tuning raises them (null: it does not) */
    UdmabufWatch(VmStore *store, HostSettings *host, QObject *parent = nullptr);

    /* The VMs running with an issue, by name */
    QList<Issue> issues() const;

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
        QByteArray partial;         // a line not ended yet
        bool checked = false;       // the start check done (native context only)
        bool noted = false;         // the note on the limits written
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
    void read(Vm *vm, Run &run);
    void updateTimer();

    VmStore *m_store;
    QPointer<HostSettings> m_host;
    QString m_sysRoot = QStringLiteral("/sys");
    QString m_device = QStringLiteral("/dev/udmabuf");
    QTimer *m_timer;
    QHash<QString, Run> m_runs;     // VM id -> its run
    QHash<qint64, Answer> m_answers;  // host tuning's, for QEMUs not checked yet
};
