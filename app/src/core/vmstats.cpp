// SPDX-License-Identifier: GPL-2.0-or-later
#include "vmstats.h"

#include <QDir>
#include <QFile>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <dirent.h>
#include <unistd.h>

#include "core/guesttools.h"
#include "core/qmpclient.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"

namespace VmStats {

/* Once a second, as VirtualBox and system monitors do: rates over a
   second, and labels that do not flicker */
static const int kPollMs = 1000;
/* PSS walks all of QEMU's page tables (its guest's RAM among them): every
   few polls, and only while asked */
static const int kPssEvery = 3;
/* The KVM counters worth a look */
static const QStringList kKvmStats{"exits", "halt_exits", "io_exits", "mmio_exits",
                                   "halt_attempted_poll", "halt_successful_poll",
                                   "halt_poll_success_ns", "halt_poll_fail_ns"};

static QByteArray readFile(const QString &path)
{
    QFile f(path);

    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

static qint64 grew(qint64 before, qint64 after)
{
    return after > before ? after - before : 0;
}

/* --- CPU --- */

int vcpuIndex(const QString &threadName)
{
    /* qemu_thread_create(.., "CPU %d/%s", .., the accelerator's name) */
    static const QRegularExpression vcpu("^CPU (\\d+)/\\w+$");
    const QRegularExpressionMatch m = vcpu.match(threadName);

    return m.hasMatch() ? m.captured(1).toInt() : -1;
}

QHash<qint64, QString> readThreadNames(qint64 pid, const QHash<qint64, QString> &known,
                                       const QString &proc)
{
    QHash<qint64, QString> names;
    const QDir tasks(QString("%1/%2/task").arg(proc).arg(pid));

    if (pid <= 0) {
        return names;
    }
    for (const QString &entry : tasks.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool ok = false;
        const qint64 tid = entry.toLongLong(&ok);

        if (!ok) {
            continue;
        }
        if (const auto it = known.constFind(tid); it != known.cend()) {
            names.insert(tid, *it);
            continue;
        }
        const QByteArray comm = readFile(tasks.filePath(entry + "/comm"));
        if (!comm.isEmpty()) {
            names.insert(tid, QString::fromUtf8(comm).trimmed());
        }
    }
    return names;
}

QList<ThreadUse> busiestThreads(const QHash<qint64, PerfStats::Thread> &before,
                                const QHash<qint64, PerfStats::Thread> &after,
                                const QHash<qint64, QString> &names, qint64 elapsedNs,
                                int count)
{
    QList<ThreadUse> list;

    if (elapsedNs <= 0) {
        return list;
    }
    for (auto a = after.cbegin(); a != after.cend(); ++a) {
        const auto b = before.constFind(a.key());
        if (b == before.cend()) {
            continue;
        }
        const qint64 ran = grew(b->runNs, a->runNs);
        if (ran > 0) {
            list.append({a.key(), names.value(a.key()), 100.0 * ran / elapsedNs});
        }
    }
    std::sort(list.begin(), list.end(), [](const ThreadUse &x, const ThreadUse &y) {
        return x.cpu != y.cpu ? x.cpu > y.cpu : x.tid < y.tid;
    });
    return list.mid(0, count);
}

/* --- memory --- */

/* "   123456 kB" in bytes; -1 if not a number */
static qint64 kilobytes(const QByteArray &value)
{
    const QList<QByteArray> parts = value.simplified().split(' ');
    bool ok = false;
    const qint64 n = parts.value(0).toLongLong(&ok);

    if (!ok) {
        return -1;
    }
    return parts.value(1) == "kB" ? n * 1024 : n;
}

/* The "Key:  value" lines of a /proc file */
static QHash<QByteArray, QByteArray> keyValues(const QByteArray &text)
{
    QHash<QByteArray, QByteArray> values;

    for (const QByteArray &line : text.split('\n')) {
        const qsizetype colon = line.indexOf(':');
        if (colon > 0) {
            values.insert(line.left(colon).trimmed(), line.mid(colon + 1).trimmed());
        }
    }
    return values;
}

Memory parseStatus(const QByteArray &text)
{
    const QHash<QByteArray, QByteArray> values = keyValues(text);
    Memory m;

    if (values.contains("VmRSS")) {
        m.resident = qMax<qint64>(0, kilobytes(values.value("VmRSS")));
    }
    m.anon = qMax<qint64>(0, kilobytes(values.value("RssAnon")));
    m.file = qMax<qint64>(0, kilobytes(values.value("RssFile")));
    m.shmem = qMax<qint64>(0, kilobytes(values.value("RssShmem")));
    m.swap = qMax<qint64>(0, kilobytes(values.value("VmSwap")));
    return m;
}

bool parseSmapsRollup(const QByteArray &text, Memory *memory)
{
    const QHash<QByteArray, QByteArray> values = keyValues(text);
    const qint64 pss = values.contains("Pss") ? kilobytes(values.value("Pss")) : -1;

    if (pss < 0) {
        return false;
    }
    memory->proportional = pss;
    return true;
}

/* --- disk --- */

/*
 * The guest's name for a device: the -drive's id ("device"), else the
 * -device's id from its QOM path (/machine/peripheral/ID/virtio-backend),
 * else the node's name or the last part of the path
 */
static QString blockName(const QJsonObject &entry)
{
    const QString device = entry["device"].toString();
    const QStringList path = entry["qdev"].toString().split('/', Qt::SkipEmptyParts);

    if (!device.isEmpty()) {
        return device;
    }
    if (path.size() >= 3 && path[0] == "machine" && path[1] == "peripheral") {
        return path[2];
    }
    if (!entry["node-name"].toString().isEmpty()) {
        return entry["node-name"].toString();
    }
    for (qsizetype i = path.size() - 1; i >= 0; i--) {
        if (path[i] != "virtio-backend") {
            return path[i];
        }
    }
    return QObject::tr("disk");
}

QList<BlockDevice> parseBlockstats(const QJsonArray &reply)
{
    QList<BlockDevice> devices;

    for (const QJsonValue &v : reply) {
        const QJsonObject entry = v.toObject();
        const QJsonObject stats = entry["stats"].toObject();

        if (stats.isEmpty()) {
            continue;
        }
        BlockDevice d;
        d.name = blockName(entry);
        d.readBytes = stats["rd_bytes"].toInteger();
        d.writtenBytes = stats["wr_bytes"].toInteger();
        d.reads = stats["rd_operations"].toInteger();
        d.writes = stats["wr_operations"].toInteger();
        devices << d;
    }
    return devices;
}

Disk diskRates(const QList<BlockDevice> &before, const QList<BlockDevice> &after, double seconds)
{
    QHash<QString, BlockDevice> earlier;
    Disk disk;

    if (seconds <= 0) {
        return disk;
    }
    for (const BlockDevice &d : before) {
        earlier.insert(d.name, d);
    }
    for (const BlockDevice &a : after) {
        const auto b = earlier.constFind(a.name);
        if (b == earlier.cend()) {
            continue;
        }
        DeviceRate r;
        r.name = a.name;
        r.read = grew(b->readBytes, a.readBytes) / seconds;
        r.written = grew(b->writtenBytes, a.writtenBytes) / seconds;
        r.reads = grew(b->reads, a.reads) / seconds;
        r.writes = grew(b->writes, a.writes) / seconds;
        r.readTotal = a.readBytes;
        r.writtenTotal = a.writtenBytes;
        disk.read += r.read;
        disk.written += r.written;
        disk.devices << r;
    }
    return disk;
}

/* --- the guest's counters --- */

GuestCounters parseGuestCounters(const QJsonObject &stats)
{
    GuestCounters c;
    const QJsonObject memory = stats["memory"].toObject();

    c.time = stats["time"].isDouble() ? stats["time"].toDouble() : -1;
    for (const QJsonValue &v : stats["net"].toArray()) {
        const QJsonObject o = v.toObject();
        if (o["name"].toString().isEmpty()) {
            continue;
        }
        c.interfaces.append({o["name"].toString(), o["rxBytes"].toInteger(),
                             o["txBytes"].toInteger(), o["rxPackets"].toInteger(),
                             o["txPackets"].toInteger()});
    }
    if (memory["total"].isDouble()) {
        c.memoryTotal = memory["total"].toInteger();
        c.memoryAvailable = memory["available"].toInteger(-1);
    }
    return c;
}

Network networkRates(const GuestCounters &before, const GuestCounters &after)
{
    QHash<QString, Interface> earlier;
    const double seconds = after.time - before.time;
    Network net;

    if (before.time < 0 || seconds <= 0) {
        return net;
    }
    for (const Interface &i : before.interfaces) {
        earlier.insert(i.name, i);
    }
    for (const Interface &a : after.interfaces) {
        InterfaceRate r;
        r.name = a.name;
        r.rxTotal = a.rxBytes;
        r.txTotal = a.txBytes;
        if (const auto b = earlier.constFind(a.name); b != earlier.cend()) {
            r.rx = grew(b->rxBytes, a.rxBytes) / seconds;
            r.tx = grew(b->txBytes, a.txBytes) / seconds;
        }
        net.rx += r.rx;
        net.tx += r.tx;
        net.interfaces << r;
    }
    return net;
}

/* --- GPU --- */

/* "34880 KiB", "16 MiB", "0": bytes, as drm-usage-stats writes them; -1 if not a number */
static qint64 drmBytes(const QByteArray &value)
{
    const QList<QByteArray> parts = value.simplified().split(' ');
    bool ok = false;
    const qint64 n = parts.value(0).toLongLong(&ok);
    const QByteArray unit = parts.value(1);

    if (!ok) {
        return -1;
    }
    if (unit == "KiB") {
        return n * 1024;
    }
    if (unit == "MiB") {
        return n * 1024 * 1024;
    }
    if (unit == "GiB") {
        return n * 1024 * 1024 * 1024;
    }
    return n;
}

static qint64 drmNumber(const QByteArray &value)
{
    bool ok = false;
    const qint64 n = value.simplified().split(' ').value(0).toLongLong(&ok);

    return ok ? n : -1;
}

bool parseDrmFdinfo(const QByteArray &text, DrmClient *client)
{
    /* the memory keys: drm-<kind>-<region> */
    enum Kind { Total, Shared, Resident, Purgeable, Active, Memory };
    static const QList<std::pair<QByteArray, Kind>> kinds{
        {"drm-total-", Total},         {"drm-shared-", Shared}, {"drm-resident-", Resident},
        {"drm-purgeable-", Purgeable}, {"drm-active-", Active}, {"drm-memory-", Memory}};
    DrmClient c;
    bool found = false;

    for (const QByteArray &line : text.split('\n')) {
        const qsizetype colon = line.indexOf(':');
        if (colon <= 0) {
            continue;
        }
        const QByteArray key = line.left(colon).trimmed();
        const QByteArray value = line.mid(colon + 1).trimmed();

        if (!key.startsWith("drm-")) {
            continue;
        }
        if (key == "drm-driver") {
            c.driver = QString::fromUtf8(value);
            found = true;
        } else if (key == "drm-pdev") {
            c.pdev = QString::fromUtf8(value);
        } else if (key == "drm-client-id") {
            c.id = drmNumber(value);
        } else if (key.startsWith("drm-engine-capacity-")) {
            /* before drm-engine-, which it starts with */
            const qint64 n = drmNumber(value);
            c.engines[QString::fromUtf8(key.mid(20))].capacity = n > 0 ? int(n) : 1;
        } else if (key.startsWith("drm-engine-")) {
            c.engines[QString::fromUtf8(key.mid(11))].busyNs = drmNumber(value);
        } else if (key.startsWith("drm-total-cycles-")) {
            /* before drm-total-, a memory key */
            c.engines[QString::fromUtf8(key.mid(17))].totalCycles = drmNumber(value);
        } else if (key.startsWith("drm-cycles-")) {
            c.engines[QString::fromUtf8(key.mid(11))].cycles = drmNumber(value);
        } else if (!key.startsWith("drm-maxfreq-")) {
            for (const auto &[prefix, kind] : kinds) {
                if (!key.startsWith(prefix) || key.size() == prefix.size()) {
                    continue;
                }
                DrmRegion &r = c.regions[QString::fromUtf8(key.mid(prefix.size()))];
                const qint64 bytes = drmBytes(value);
                switch (kind) {
                case Total: r.total = bytes; break;
                case Shared: r.shared = bytes; break;
                case Resident: r.resident = bytes; break;
                case Purgeable: r.purgeable = bytes; break;
                case Active: r.active = bytes; break;
                /* the deprecated alias of resident (amdgpu prints both) */
                case Memory: r.resident = r.resident < 0 ? bytes : r.resident; break;
                }
                break;
            }
        }
    }
    if (found) {
        *client = c;
    }
    return found;
}

/* One client per drm_file: the id is unique on its device, or everywhere */
static QString clientKey(const DrmClient &c)
{
    return c.pdev + '/' + c.driver + '/' + QString::number(c.id);
}

QList<DrmClient> readDrmClients(qint64 pid, const QString &proc, QString *error)
{
    QList<DrmClient> clients;
    QSet<QString> seen;
    const QByteArray fdDir = QString("%1/%2/fd").arg(proc).arg(pid).toLocal8Bit();
    const QString infoDir = QString("%1/%2/fdinfo/").arg(proc).arg(pid);
    DIR *dir = pid > 0 ? opendir(fdDir.constData()) : nullptr;

    if (!dir) {
        if (error) {
            *error = pid > 0 ? QObject::tr("cannot read the files QEMU has open (%1)")
                                   .arg(QString::fromLocal8Bit(strerror(errno)))
                             : QString();
        }
        return clients;
    }
    while (const dirent *entry = readdir(dir)) {
        const QByteArray name = entry->d_name;
        char target[256];

        if (name.startsWith('.')) {
            continue;
        }
        const ssize_t n = readlinkat(dirfd(dir), name.constData(), target, sizeof(target) - 1);
        if (n <= 0) {
            continue;
        }
        target[n] = 0;
        if (qstrncmp(target, "/dev/dri/", 9) != 0) {
            continue;
        }
        DrmClient c;
        if (!parseDrmFdinfo(readFile(infoDir + QString::fromLatin1(name)), &c)) {
            continue;
        }
        /* a dup()ed fd, or one passed on: the same client */
        const QString key = c.id >= 0 ? clientKey(c) : "fd" + QString::fromLatin1(name);
        if (!seen.contains(key)) {
            seen.insert(key);
            clients << c;
        }
    }
    closedir(dir);
    if (error) {
        error->clear();
    }
    return clients;
}

QList<GpuUse> gpuUse(const QList<DrmClient> &before, const QList<DrmClient> &after,
                     qint64 elapsedNs)
{
    struct Sums {
        GpuUse use;
        QMap<QString, double> busyNs;       // engines with time
        QMap<QString, double> cycleShare;   // engines with cycles
        QMap<QString, int> capacity;
        QMap<QString, RegionUse> regions;
    };
    QHash<QString, DrmClient> earlier;
    QMap<QString, Sums> gpus;               // by device

    for (const DrmClient &c : before) {
        earlier.insert(clientKey(c), c);
    }
    for (const DrmClient &a : after) {
        Sums &s = gpus[a.pdev + '/' + a.driver];
        const auto b = a.id >= 0 ? earlier.constFind(clientKey(a)) : earlier.cend();

        s.use.driver = a.driver;
        s.use.pdev = a.pdev;
        s.use.clients++;
        for (auto e = a.engines.cbegin(); e != a.engines.cend(); ++e) {
            s.capacity[e.key()] = qMax(s.capacity.value(e.key(), 1), e->capacity);
            if (e->busyNs < 0 && e->cycles < 0) {
                continue;
            }
            /* listed even when idle or new */
            if (e->busyNs >= 0) {
                s.busyNs[e.key()] += 0;
            } else {
                s.cycleShare[e.key()] += 0;
            }
            if (b == earlier.cend() || !b->engines.contains(e.key())) {
                continue;
            }
            const DrmEngine &was = b->engines[e.key()];
            if (e->busyNs >= 0) {
                if (was.busyNs >= 0) {
                    s.busyNs[e.key()] += grew(was.busyNs, e->busyNs);
                }
            } else if (e->cycles >= 0 && was.cycles >= 0 && e->totalCycles > was.totalCycles &&
                       was.totalCycles >= 0) {
                s.cycleShare[e.key()] +=
                    double(grew(was.cycles, e->cycles)) / (e->totalCycles - was.totalCycles);
            }
        }
        for (auto r = a.regions.cbegin(); r != a.regions.cend(); ++r) {
            RegionUse &u = s.regions[r.key()];
            const qint64 resident = r->resident >= 0 ? r->resident : qMax<qint64>(0, r->total);
            u.name = r.key();
            u.resident += resident;
            u.total += qMax<qint64>(0, r->total);
        }
    }

    QList<GpuUse> list;
    for (Sums &s : gpus) {
        auto add = [&s](const QString &name, double fraction) {
            const double busy = qBound(0.0, 100.0 * fraction / s.capacity.value(name, 1), 100.0);
            s.use.engines.append({name, busy});
            s.use.busy = qMax(s.use.busy, busy);
        };
        for (auto e = s.busyNs.cbegin(); e != s.busyNs.cend(); ++e) {
            add(e.key(), elapsedNs > 0 ? e.value() / elapsedNs : 0);
        }
        for (auto e = s.cycleShare.cbegin(); e != s.cycleShare.cend(); ++e) {
            add(e.key(), e.value());
        }
        std::sort(s.use.engines.begin(), s.use.engines.end(),
                  [](const EngineUse &x, const EngineUse &y) { return x.name < y.name; });
        for (const RegionUse &r : std::as_const(s.regions)) {
            if (r.resident > 0 || r.total > 0) {
                s.use.regions << r;
                s.use.memory += r.resident;
            }
        }
        list << s.use;
    }
    std::stable_sort(list.begin(), list.end(),
                     [](const GpuUse &x, const GpuUse &y) { return x.busy > y.busy; });
    return list;
}

QList<DrmClient> mergeDrmClients(const QList<DrmClient> &before, const QList<DrmClient> &after)
{
    QHash<QString, DrmClient> earlier;
    QList<DrmClient> merged = after;

    for (const DrmClient &c : before) {
        earlier.insert(clientKey(c), c);
    }
    for (DrmClient &c : merged) {
        const auto b = c.id >= 0 ? earlier.constFind(clientKey(c)) : earlier.cend();
        if (b == earlier.cend()) {
            continue;
        }
        for (auto e = c.engines.begin(); e != c.engines.end(); ++e) {
            const DrmEngine was = b->engines.value(e.key());
            e->busyNs = qMax(e->busyNs, was.busyNs);
            e->cycles = qMax(e->cycles, was.cycles);
            e->totalCycles = qMax(e->totalCycles, was.totalCycles);
        }
    }
    return merged;
}

/* --- text --- */

QString formatBytes(qint64 bytes)
{
    static const char *const units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = qMax<qint64>(0, bytes);
    int unit = 0;

    /* 1000 KiB is 0.98 MiB: three digits at most */
    while (value >= 999.5 && unit < 4) {
        value /= 1024;
        unit++;
    }
    if (unit == 0) {
        return QObject::tr("%1 %2").arg(qRound64(value)).arg(units[unit]);
    }
    const int decimals = value < 0.995 ? 2 : value < 9.95 ? 1 : 0;
    return QObject::tr("%1 %2").arg(value, 0, 'f', decimals).arg(units[unit]);
}

QString formatRate(double bytesPerSecond)
{
    return QObject::tr("%1/s").arg(formatBytes(qRound64(qMax(0.0, bytesPerSecond))));
}

QString formatWholePercent(double percent)
{
    return QObject::tr("%1 %").arg(qRound(qMax(0.0, percent)));
}

/* --- the sampler --- */

Sampler::Sampler(QObject *parent) : QObject(parent), m_timer(new QTimer(this))
{
    m_timer->setInterval(kPollMs);
    connect(m_timer, &QTimer::timeout, this, &Sampler::poll);
    m_clock.start();
}

void Sampler::setVm(Vm *vm)
{
    if (vm == m_vm) {
        return;
    }
    if (m_vm) {
        disconnect(m_vm->runner(), nullptr, this, nullptr);
    }
    if (m_agent) {
        disconnect(m_agent, nullptr, this, nullptr);
    }
    reset();
    m_vm = vm;
    m_agent = vm ? GuestToolsMonitor::of(vm) : nullptr;
    if (vm) {
        connect(vm->runner(), &VmRunner::stateChanged, this, &Sampler::restart);
        connect(m_agent, &GuestToolsMonitor::statsReceived, this, &Sampler::guestCounters);
        /* an agent came or went: guestSource() */
        connect(m_agent, &GuestToolsMonitor::changed, this, &Sampler::changed);
    }
    restart();
    emit changed();
}

Vm *Sampler::vm() const
{
    return m_vm;
}

void Sampler::setSources(Sources sources)
{
    const Sources dropped = m_sources & ~sources;

    if (sources == m_sources) {
        return;
    }
    m_sources = sources;
    /* a reading taken long ago is no start for a rate */
    if (dropped & Cpu) {
        m_threads.clear();
        m_snapshot.perf.threads = false;
        m_snapshot.busiest.clear();
    }
    if (dropped & Memory) {
        m_snapshot.memory = false;
    }
    if (dropped & Pss) {
        m_snapshot.qemuMemory.proportional = -1;
        m_pssCountdown = 0;
    }
    if (dropped & Disk) {
        m_blocks.clear();
        m_snapshot.disk = false;
    }
    if (dropped & (Network | Memory)) {
        m_guest = {};
        m_snapshot.network = false;
        m_snapshot.guestMemory = false;
    }
    if (dropped & Gpu) {
        m_drm.clear();
        m_drmAt = 0;
        m_snapshot.gpu = false;
        m_snapshot.gpus.clear();
        m_snapshot.drmClients = -1;
    }
    if (dropped & Display) {
        m_snapshot.perf.display = false;
    }
    if (dropped & Kvm) {
        m_kvm.clear();
        m_snapshot.perf.kvm = false;
    }
    restart();
}

GuestSource Sampler::guestSource() const
{
    if (!m_vm || !m_vm->runner()->isActive()) {
        return GuestSource::None;
    }
    if (!m_agent || !m_agent->hasAgent()) {
        return GuestSource::NoAgent;
    }
    return m_agent->reportsStats() ? GuestSource::Agent : GuestSource::OldAgent;
}

void Sampler::setInterval(int ms)
{
    m_timer->setInterval(ms);
}

void Sampler::reset()
{
    m_generation++;
    m_qmp = nullptr;
    m_pid = 0;
    m_hasData = false;
    m_asked = false;
    m_hasDisplayStats = false;
    m_hasKvmStats = false;
    m_displayBusy = false;
    m_kvmBusy = false;
    m_diskBusy = false;
    m_pssCountdown = 0;
    m_vcpusAsked = false;
    m_qmpVcpus.clear();
    m_threads.clear();
    m_names.clear();
    m_threadsAt = 0;
    m_kvm.clear();
    m_kvmAt = 0;
    m_blocks.clear();
    m_blocksAt = 0;
    m_drm.clear();
    m_drmAt = 0;
    m_guest = {};
    m_snapshot = {};
}

/* Polls while the VM runs and something is asked, from now on */
void Sampler::restart()
{
    const bool active = m_vm && m_vm->runner()->isActive() && m_sources;

    if (!active) {
        m_timer->stop();
        if (m_pid) {
            reset();
            emit changed();
        }
        return;
    }
    if (!m_timer->isActive()) {
        m_timer->start();
        poll();
    }
}

void Sampler::poll()
{
    VmRunner *runner = m_vm ? m_vm->runner() : nullptr;
    const qint64 pid = runner && runner->isActive() ? runner->pid() : 0;
    QmpClient *qmp = runner ? runner->qmp() : nullptr;

    if (pid != m_pid || qmp != m_qmp) {
        /* stopped, or another run */
        const bool had = m_hasData;
        reset();
        m_pid = pid;
        m_qmp = qmp;
        if (had) {
            emit changed();
        }
    }
    if (!pid) {
        return;
    }

    const qint64 now = m_clock.nsecsElapsed();
    if (m_sources & Cpu) {
        readThreads(now);
    }
    if (m_sources & Memory) {
        readMemory();
    }
    if (m_sources & Gpu) {
        readGpu(now);
    }
    /* the guest's counters: the answer comes back as statsReceived() */
    if ((m_sources & (Network | Memory)) && m_agent && m_agent->reportsStats()) {
        m_agent->requestStats();
    }
    if (!qmp || !qmp->isReady()) {
        return;
    }
    if (m_sources & Disk) {
        askDisk();
    }
    if (m_sources & (Display | Kvm)) {
        if (!m_asked) {
            askCommands();
            return;
        }
        if ((m_sources & Display) && m_hasDisplayStats) {
            askDisplay();
        }
        if ((m_sources & Kvm) && m_hasKvmStats) {
            askKvm();
        }
    }
}

/* Which of the commands this QEMU has */
void Sampler::askCommands()
{
    const int generation = m_generation;
    const QPointer<Sampler> self(this);

    m_asked = true;
    m_qmp->execute("query-commands", {},
                   [self, generation](const QJsonValue &result, const QString &error) {
        if (!self || self->m_generation != generation || !error.isEmpty()) {
            return;
        }
        for (const QJsonValue &command : result.toArray()) {
            const QString name = command["name"].toString();
            self->m_hasDisplayStats |= name == "x-query-display-stats";
            self->m_hasKvmStats |= name == "query-stats";
        }
    });
}

/* The vCPUs' threads from QMP, for a QEMU that does not name its threads */
void Sampler::askVcpus()
{
    const int generation = m_generation;
    const QPointer<Sampler> self(this);

    m_qmp->execute("query-cpus-fast", {},
                   [self, generation](const QJsonValue &result, const QString &error) {
        if (!self || self->m_generation != generation || !error.isEmpty()) {
            return;
        }
        self->m_qmpVcpus.clear();
        for (const QJsonValue &cpu : result.toArray()) {
            self->m_qmpVcpus << cpu["thread-id"].toInteger();
        }
    });
}

void Sampler::readThreads(qint64 now)
{
    const QHash<qint64, PerfStats::Thread> threads = PerfStats::readThreads(m_pid);
    QList<qint64> vcpus, others;

    if (threads.isEmpty()) {
        return;             // not ours to read
    }
    m_names = readThreadNames(m_pid, m_names);
    for (auto it = threads.cbegin(); it != threads.cend(); ++it) {
        if (vcpuIndex(m_names.value(it.key())) >= 0) {
            vcpus << it.key();
        }
    }
    if (vcpus.isEmpty()) {
        /* no debug-threads=on: QMP knows them */
        for (const qint64 tid : std::as_const(m_qmpVcpus)) {
            if (threads.contains(tid)) {
                vcpus << tid;
            }
        }
        /* asked once per run, and again when one went (unplugged) */
        if ((!m_vcpusAsked || vcpus.size() != m_qmpVcpus.size()) && m_qmp && m_qmp->isReady()) {
            m_vcpusAsked = true;
            m_qmpVcpus = vcpus;
            askVcpus();
        }
    }
    for (auto it = threads.cbegin(); it != threads.cend(); ++it) {
        if (it.key() != m_pid && !vcpus.contains(it.key())) {
            others << it.key();
        }
    }
    if (!m_threads.isEmpty() && now > m_threadsAt) {
        const qint64 elapsed = now - m_threadsAt;
        m_snapshot.perf.threads = true;
        m_snapshot.perf.mainLoop = PerfStats::load(m_threads, threads, {m_pid}, elapsed);
        m_snapshot.perf.vcpus = PerfStats::load(m_threads, threads, vcpus, elapsed);
        m_snapshot.perf.others = PerfStats::load(m_threads, threads, others, elapsed);
        m_snapshot.busiest = busiestThreads(m_threads, threads, m_names, elapsed, 6);
        m_snapshot.hostCpus = QThread::idealThreadCount();
        m_hasData = true;
        emit changed();
    }
    m_threads = threads;
    m_threadsAt = now;
}

void Sampler::readMemory()
{
    const QString proc = QString("/proc/%1/").arg(m_pid);
    VmStats::Memory m = parseStatus(readFile(proc + "status"));

    if (m.resident < 0) {
        return;             // gone
    }
    m.proportional = m_snapshot.qemuMemory.proportional;
    if (m_sources & Pss) {
        if (m_pssCountdown-- <= 0) {
            parseSmapsRollup(readFile(proc + "smaps_rollup"), &m);
            m_pssCountdown = kPssEvery - 1;
        }
    } else {
        m.proportional = -1;
    }
    m_snapshot.memory = true;
    m_snapshot.qemuMemory = m;
    m_hasData = true;
    emit changed();
}

void Sampler::readGpu(qint64 now)
{
    QString error;
    const QList<DrmClient> clients = readDrmClients(m_pid, "/proc", &error);

    m_snapshot.gpuError = error;
    if (!error.isEmpty()) {
        m_snapshot.gpu = false;
        m_snapshot.drmClients = -1;
        emit changed();
        return;
    }
    if (m_drmAt > 0 && now > m_drmAt) {
        m_snapshot.gpus = gpuUse(m_drm, clients, now - m_drmAt);
        m_snapshot.gpu = !clients.isEmpty();
        m_hasData = true;
    }
    m_snapshot.drmClients = int(clients.size());
    m_drm = mergeDrmClients(m_drm, clients);
    m_drmAt = now;
    emit changed();
}

void Sampler::askDisk()
{
    const int generation = m_generation;
    const QPointer<Sampler> self(this);

    if (m_diskBusy) {
        return;
    }
    m_diskBusy = true;
    m_qmp->execute("query-blockstats", {},
                   [self, generation](const QJsonValue &result, const QString &error) {
        if (!self || self->m_generation != generation) {
            return;
        }
        self->m_diskBusy = false;
        if (!error.isEmpty() || !(self->m_sources & Disk)) {
            return;
        }
        const qint64 now = self->m_clock.nsecsElapsed();
        const QList<BlockDevice> devices = parseBlockstats(result.toArray());
        if (self->m_blocksAt > 0 && now > self->m_blocksAt) {
            self->m_snapshot.diskUse =
                diskRates(self->m_blocks, devices, (now - self->m_blocksAt) / 1e9);
            self->m_snapshot.disk = true;
            self->m_hasData = true;
            emit self->changed();
        }
        self->m_blocks = devices;
        self->m_blocksAt = now;
    });
}

void Sampler::askDisplay()
{
    const int generation = m_generation;
    const QPointer<Sampler> self(this);

    if (m_displayBusy) {
        return;
    }
    m_displayBusy = true;
    m_qmp->execute("x-query-display-stats", {},
                   [self, generation](const QJsonValue &result, const QString &error) {
        if (!self || self->m_generation != generation) {
            return;
        }
        self->m_displayBusy = false;
        if (error.isEmpty() && (self->m_sources & Display)) {
            self->m_snapshot.perf.display =
                PerfStats::parseDisplay(result.toObject(), &self->m_snapshot.perf.screen,
                                        &self->m_snapshot.perf.displayType);
            self->m_hasData = true;
            emit self->changed();
        }
    });
}

void Sampler::askKvm()
{
    const int generation = m_generation;
    const QPointer<Sampler> self(this);
    QJsonArray names;

    if (m_kvmBusy) {
        return;
    }
    for (const QString &name : kKvmStats) {
        names.append(name);
    }
    m_kvmBusy = true;
    m_qmp->execute("query-stats",
                   {{"target", "vcpu"},
                    {"providers", QJsonArray{QJsonObject{{"provider", "kvm"}, {"names", names}}}}},
                   [self, generation](const QJsonValue &result, const QString &error) {
        if (!self || self->m_generation != generation) {
            return;
        }
        self->m_kvmBusy = false;
        if (!error.isEmpty()) {
            self->m_hasKvmStats = false;
        } else if (self->m_sources & Kvm) {
            self->readKvm(result.toArray(), self->m_clock.nsecsElapsed());
        }
    });
}

void Sampler::readKvm(const QJsonArray &reply, qint64 now)
{
    const QHash<QString, double> kvm = PerfStats::sumKvmStats(reply);

    if (kvm.isEmpty()) {
        return;             // TCG
    }
    if (!m_kvm.isEmpty() && now > m_kvmAt) {
        const double seconds = (now - m_kvmAt) / 1e9;
        auto rate = [&](const char *name) {
            return qMax(0.0, kvm.value(name) - m_kvm.value(name)) / seconds;
        };
        const double attempted = rate("halt_attempted_poll");
        PerfStats::Snapshot &s = m_snapshot.perf;

        s.kvm = true;
        s.exits = rate("exits");
        s.haltExits = rate("halt_exits");
        s.deviceExits = rate("io_exits") + rate("mmio_exits");
        s.haltPollSuccess = attempted > 0 ? rate("halt_successful_poll") / attempted : -1;
        s.haltPollCpu = (rate("halt_poll_success_ns") + rate("halt_poll_fail_ns")) / 1e7;
        m_hasData = true;
        emit changed();
    }
    m_kvm = kvm;
    m_kvmAt = now;
}

void Sampler::guestCounters(const QJsonObject &stats)
{
    const GuestCounters c = parseGuestCounters(stats);

    if (!m_pid || !(m_sources & (Network | Memory))) {
        return;
    }
    if (c.time < m_guest.time) {
        /* the guest restarted: its counters with it */
        m_guest = {};
    }
    if (m_sources & Network) {
        if (m_guest.time >= 0) {
            m_snapshot.net = networkRates(m_guest, c);
            m_snapshot.network = true;
            m_hasData = true;
        }
    }
    if ((m_sources & Memory) && c.memoryTotal > 0) {
        m_snapshot.guestMemory = true;
        m_snapshot.guestTotal = c.memoryTotal;
        m_snapshot.guestAvailable = qMax<qint64>(0, c.memoryAvailable);
    }
    m_guest = c;
    emit changed();
}

}
