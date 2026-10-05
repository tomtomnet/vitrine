// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QElapsedTimer>
#include <QFlags>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QString>

#include "core/perfstats.h"

class GuestToolsMonitor;
class QmpClient;
class QTimer;
class Vm;

/*
 * What the selected VM takes of the host, for the status bar, VirtualBox
 * style; each part read only while the status bar shows it:
 * - CPU: QEMU's threads, from /proc/PID/task (PerfStats), its vCPUs by
 *   their names ("CPU 0/KVM", QEMU's -name debug-threads=on) or else by
 *   the thread ids QMP gives;
 * - memory: QEMU's resident memory, /proc/PID/status, its proportional
 *   share (PSS, smaps_rollup) on demand; the guest's own view from the
 *   guest tools' agent;
 * - disk: QMP's query-blockstats, the guest's reads and writes;
 * - network: the guest's own counters, from the guest tools' agent.  QEMU
 *   counts nothing: passt over vhost-user moves the packets itself, and
 *   QEMU has no counters for its other backends either;
 * - GPU: QEMU's clients of the host's GPU, from the DRM fdinfo of its fds
 *   on /dev/dri (drm-usage-stats: amdgpu, i915, xe and others);
 * - and PerfStats' display, main loop and KVM statistics.
 * The parsers and rates are functions of their text, for the tests.
 */
namespace VmStats {

/* --- CPU --- */

/* The vCPU of a thread QEMU named (debug-threads=on: "CPU 3/KVM"), else -1 */
int vcpuIndex(const QString &threadName);
/*
 * The names of the threads of process @pid by thread id, from their comm:
 * those of @known taken as they are, so that a thread's name is read once
 */
QHash<qint64, QString> readThreadNames(qint64 pid, const QHash<qint64, QString> &known = {},
                                       const QString &proc = "/proc");

/* A thread's share of a CPU between two readings */
struct ThreadUse {
    qint64 tid = 0;
    QString name;
    double cpu = 0;             // % of one CPU
};
/* The threads of @after that ran since @before, the busiest first, at most @count */
QList<ThreadUse> busiestThreads(const QHash<qint64, PerfStats::Thread> &before,
                                const QHash<qint64, PerfStats::Thread> &after,
                                const QHash<qint64, QString> &names, qint64 elapsedNs,
                                int count);

/* --- memory --- */

/* QEMU's memory on the host, in bytes */
struct Memory {
    qint64 resident = -1;       // VmRSS
    qint64 anon = 0;            // RssAnon: QEMU's own
    qint64 file = 0;            // RssFile: its program, libraries, files
    qint64 shmem = 0;           // RssShmem: the guest's RAM it touched (memfd)
    qint64 swap = 0;            // VmSwap
    qint64 proportional = -1;   // Pss of smaps_rollup, when read
};
/* /proc/PID/status: its Vm and Rss lines */
Memory parseStatus(const QByteArray &text);
/* /proc/PID/smaps_rollup: its Pss line into @memory */
bool parseSmapsRollup(const QByteArray &text, Memory *memory);

/* --- disk --- */

/* A block device of the guest, what it read and wrote since QEMU started */
struct BlockDevice {
    QString name;
    qint64 readBytes = 0;
    qint64 writtenBytes = 0;
    qint64 reads = 0;           // operations
    qint64 writes = 0;
};
/* A query-blockstats reply: the guest's devices (their backends' top) */
QList<BlockDevice> parseBlockstats(const QJsonArray &reply);

struct DeviceRate {
    QString name;
    double read = 0;            // bytes per second
    double written = 0;
    double reads = 0;           // operations per second
    double writes = 0;
    qint64 readTotal = 0;       // since QEMU started
    qint64 writtenTotal = 0;
};
struct Disk {
    double read = 0;            // bytes per second, all devices
    double written = 0;
    QList<DeviceRate> devices;  // those of both readings
};
Disk diskRates(const QList<BlockDevice> &before, const QList<BlockDevice> &after,
               double seconds);

/* --- what the guest tools' agent tells: network, the guest's memory --- */

struct Interface {
    QString name;
    qint64 rxBytes = 0;
    qint64 txBytes = 0;
    qint64 rxPackets = 0;
    qint64 txPackets = 0;
};
struct GuestCounters {
    double time = -1;           // the guest's monotonic clock, in seconds
    QList<Interface> interfaces;    // its network cards: devices, not lo or bridges
    qint64 memoryTotal = -1;    // bytes, MemTotal
    qint64 memoryAvailable = -1;    // MemAvailable
};
/* The "stats" object of the agent's answer */
GuestCounters parseGuestCounters(const QJsonObject &stats);

struct InterfaceRate {
    QString name;
    double rx = 0;              // bytes per second, received by the guest
    double tx = 0;              // sent
    qint64 rxTotal = 0;         // since the guest started
    qint64 txTotal = 0;
};
struct Network {
    double rx = 0;
    double tx = 0;
    QList<InterfaceRate> interfaces;
};
/* Between two answers, by the guest's clock; counters that went back (a
   card reset, a reboot) count nothing */
Network networkRates(const GuestCounters &before, const GuestCounters &after);

/* --- GPU --- */

/* An engine of a DRM client: its busy time, or busy and total cycles (xe) */
struct DrmEngine {
    qint64 busyNs = -1;
    qint64 cycles = -1;
    qint64 totalCycles = -1;
    int capacity = 1;           // identical engines it stands for
};
/* What a DRM client holds of a memory region, in bytes; -1 if not said */
struct DrmRegion {
    qint64 total = -1;
    qint64 shared = -1;
    qint64 resident = -1;
    qint64 purgeable = -1;
    qint64 active = -1;
};
/* One open DRM file (struct drm_file), as its fdinfo describes it */
struct DrmClient {
    QString driver;
    QString pdev;               // PCI address
    qint64 id = -1;             // drm-client-id
    QMap<QString, DrmEngine> engines;   // by the driver's name: gfx, render, rcs...
    QMap<QString, DrmRegion> regions;   // vram, gtt, system0...
};
/* The drm-* keys of an fdinfo; false if it is not a DRM client's */
bool parseDrmFdinfo(const QByteArray &text, DrmClient *client);
/*
 * The DRM clients of process @pid: its fds on /dev/dri, each client once
 * (dup()ed fds share one).  @error: why they could not be read, such as a
 * process that is not ours.
 */
QList<DrmClient> readDrmClients(qint64 pid, const QString &proc = "/proc",
                                QString *error = nullptr);

struct EngineUse {
    QString name;
    double busy = 0;            // % of the time, of the engine's capacity
};
struct RegionUse {
    QString name;
    qint64 resident = 0;        // bytes
    qint64 total = 0;
};
/* What the clients of one GPU took */
struct GpuUse {
    QString driver;
    QString pdev;
    int clients = 0;
    double busy = 0;            // the busiest engine
    QList<EngineUse> engines;   // by name
    QList<RegionUse> regions;   // those with some memory
    qint64 memory = 0;          // resident, all regions
};
/*
 * Per GPU, from two readings @elapsedNs apart; a client in one reading
 * only counts nothing.  An engine's time that went back counts nothing
 * either: the drm-usage-stats specification lets drivers report a lower
 * value for a while, and the reader keep the higher one meanwhile, which
 * mergeDrmClients() does for the next reading.
 */
QList<GpuUse> gpuUse(const QList<DrmClient> &before, const QList<DrmClient> &after,
                     qint64 elapsedNs);
/* @after, with each engine's counters no lower than in @before */
QList<DrmClient> mergeDrmClients(const QList<DrmClient> &before, const QList<DrmClient> &after);

/* --- text --- */

/* "512 B", "12 KiB", "1.2 MiB", "0.98 GiB": three digits at most */
QString formatBytes(qint64 bytes);
/* "0 B/s", "45 KiB/s", "1.2 MiB/s" */
QString formatRate(double bytesPerSecond);
/* "0 %", "42 %": whole percents, for labels that do not flicker */
QString formatWholePercent(double percent);

/* --- the sampler --- */

/* What the status bar shows, each part valid when its flag is set */
struct Snapshot {
    PerfStats::Snapshot perf;   // display, QEMU's threads (perf.threads), KVM
    int hostCpus = 0;
    QList<ThreadUse> busiest;   // QEMU's busiest threads
    bool memory = false;
    Memory qemuMemory;
    bool guestMemory = false;   // from the agent
    qint64 guestTotal = 0;
    qint64 guestAvailable = 0;
    bool disk = false;
    Disk diskUse;
    bool network = false;
    Network net;
    bool gpu = false;
    QList<GpuUse> gpus;         // one per GPU QEMU uses, the busiest first
    int drmClients = -1;        // QEMU's DRM clients; -1 before the first reading
    QString gpuError;
};

/* Where the guest's own counters come from */
enum class GuestSource {
    None,           // the VM does not run
    NoAgent,        // no guest tools' agent answered
    OldAgent,       // one without the stats command
    Agent,
};

/*
 * Polls the VM once a second while it runs, for the parts asked
 * (setSources()), and tells when the snapshot changed.  The display's
 * statistics from QEMU (SDL) are asked only with Display, the KVM counters
 * with Kvm, PSS with Pss: the status bar asks for those while their
 * details show.  Nothing is polled while no VM runs or nothing is asked.
 */
class Sampler : public QObject
{
    Q_OBJECT

public:
    enum Source {
        Cpu = 0x1,          // QEMU's threads
        Memory = 0x2,
        Disk = 0x4,
        Network = 0x8,
        Gpu = 0x10,
        Display = 0x20,     // x-query-display-stats
        Kvm = 0x40,         // query-stats
        Pss = 0x80,         // with Memory
    };
    Q_DECLARE_FLAGS(Sources, Source)

    explicit Sampler(QObject *parent = nullptr);

    /* nullptr to stop */
    void setVm(Vm *vm);
    Vm *vm() const;
    void setSources(Sources sources);
    Sources sources() const { return m_sources; }
    const Snapshot &snapshot() const { return m_snapshot; }
    /* The VM runs, and at least one reading came in */
    bool hasData() const { return m_hasData; }
    GuestSource guestSource() const;
    /* Tests: the poll's interval, 1000 ms */
    void setInterval(int ms);

signals:
    void changed();

private:
    void reset();
    void restart();
    void poll();
    void askCommands();
    void askVcpus();
    void readThreads(qint64 now);
    void readMemory();
    void readGpu(qint64 now);
    void askDisk();
    void askDisplay();
    void askKvm();
    void readKvm(const QJsonArray &reply, qint64 now);
    void guestCounters(const QJsonObject &stats);

    QPointer<Vm> m_vm;
    QPointer<GuestToolsMonitor> m_agent;
    QPointer<QmpClient> m_qmp;
    Sources m_sources;
    qint64 m_pid = 0;               // of the run polled
    QTimer *m_timer;
    QElapsedTimer m_clock;
    int m_generation = 0;
    bool m_hasData = false;
    bool m_asked = false;           // for the commands QEMU has
    bool m_hasDisplayStats = false;
    bool m_hasKvmStats = false;
    bool m_displayBusy = false;     // a query is out
    bool m_kvmBusy = false;
    bool m_diskBusy = false;
    int m_pssCountdown = 0;         // polls until the next PSS reading
    bool m_vcpusAsked = false;
    QList<qint64> m_qmpVcpus;       // thread ids, from QMP
    QHash<qint64, PerfStats::Thread> m_threads;
    QHash<qint64, QString> m_names;
    qint64 m_threadsAt = 0;
    QHash<QString, double> m_kvm;
    qint64 m_kvmAt = 0;
    QList<BlockDevice> m_blocks;
    qint64 m_blocksAt = 0;
    QList<DrmClient> m_drm;
    qint64 m_drmAt = 0;
    GuestCounters m_guest;
    Snapshot m_snapshot;
};

}

Q_DECLARE_OPERATORS_FOR_FLAGS(VmStats::Sampler::Sources)
