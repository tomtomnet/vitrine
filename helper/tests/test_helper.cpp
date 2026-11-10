// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * vitrine-helper's logic against a fake sysfs/debugfs tree: the test build
 * (vitrine-helper-fake) plays the kernel's part there, so the order of the
 * writes matters as it does on a real kernel.  Its journal lists every
 * write.  QEMU is a stand-in process named qemu-system-x86_64.
 */
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMap>
#include <QProcess>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTest>
#include <QXmlStreamReader>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static const char kFair[] = "/sys/kernel/debug/sched/fair_server";
static const char kExt[] = "/sys/kernel/debug/sched/ext_server";
static const char kCard[] = "/sys/class/drm/card1/device";
static const char kUdmabuf[] = "/sys/module/udmabuf/parameters";
static const char kThp[] = "/sys/kernel/mm/transparent_hugepage/shmem_enabled";
static const char kCpu[] = "/sys/devices/system/cpu";
static const char kLatency[] = "/dev/cpu_dma_latency";
/* what the kernel applies without any request: 2000 s */
static const int kNoLatency = 2000000000;

static bool writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

static QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

static QByteArray od(int min, int max, int lo = 800, int hi = 2700)
{
    return QString("OD_SCLK:\n0:        %1Mhz\n1:       %2Mhz\nOD_RANGE:\n"
                   "SCLK:     %3Mhz       %4Mhz\n")
        .arg(min).arg(max).arg(lo).arg(hi).toLatin1();
}

/* A QEMU stand-in, stopped with the object */
class FakeQemu
{
public:
    FakeQemu(const QString &program = FAKE_QEMU, const QStringList &extraEnv = {})
    {
        if (!extraEnv.isEmpty()) {
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            for (const QString &e : extraEnv) {
                env.insert(e.section('=', 0, 0), e.section('=', 1));
            }
            m_process.setProcessEnvironment(env);
        }
        m_process.start(program, program.endsWith("sleep") ? QStringList{"300"} : QStringList{});
        m_process.waitForStarted();
        /* its threads */
        QTest::qWait(50);
    }
    ~FakeQemu() { stop(); }
    qint64 pid() const { return m_process.processId(); }
    QString pidText() const { return QString::number(pid()); }
    void stop()
    {
        if (m_process.state() != QProcess::NotRunning) {
            m_process.kill();
            m_process.waitForFinished();
        }
    }

private:
    QProcess m_process;
};

/*
 * Threads of the test's own process, idle until the object goes: the
 * helper's parent here, as vitrine is pkexec's caller, and so the threads
 * view-rt is for
 */
class Threads
{
public:
    explicit Threads(int n)
    {
        for (int i = 0; i < n; i++) {
            m_threads.emplace_back([this]() {
                std::unique_lock lk(m_lock);
                m_tids.push_back(gettid());
                m_changed.notify_all();
                m_changed.wait(lk, [this]() { return m_quit; });
            });
        }
        std::unique_lock lk(m_lock);
        m_changed.wait(lk, [this, n]() { return int(m_tids.size()) == n; });
    }
    ~Threads()
    {
        {
            std::lock_guard g(m_lock);
            m_quit = true;
        }
        m_changed.notify_all();
        for (std::thread &t : m_threads) {
            t.join();
        }
    }
    QString tid(int i) const { return QString::number(m_tids.at(size_t(i))); }

private:
    std::mutex m_lock;
    std::condition_variable m_changed;
    std::vector<pid_t> m_tids;
    std::vector<std::thread> m_threads;
    bool m_quit = false;
};

/* One helper session */
class Helper
{
public:
    /* vitrine-helper-fake session (or the verb in @arguments), or @program
       @arguments that runs it */
    Helper(const QString &root, const QStringList &extraEnv = {},
           const QString &program = HELPER_FAKE, const QStringList &arguments = {})
    {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("VITRINE_HELPER_TEST_ROOT", root);
        env.remove("PKEXEC_UID");
        env.remove("SUDO_UID");
        for (const QString &e : extraEnv) {
            env.insert(e.section('=', 0, 0), e.section('=', 1));
        }
        m_process.setProcessEnvironment(env);
        m_process.setProcessChannelMode(QProcess::SeparateChannels);
        m_process.start(program, program == HELPER_FAKE && arguments.isEmpty() ? QStringList{"session"}
                                                                               : arguments);
        m_process.waitForStarted();
    }
    ~Helper()
    {
        if (m_process.state() != QProcess::NotRunning) {
            m_process.kill();
            m_process.waitForFinished();
        }
    }

    /* The next line, or "" after 5 s */
    QString line()
    {
        QElapsedTimer t;
        t.start();
        while (!m_process.canReadLine() && t.elapsed() < 5000) {
            if (!m_process.waitForReadyRead(100) && m_process.state() == QProcess::NotRunning &&
                !m_process.canReadLine()) {
                break;
            }
        }
        if (!m_process.canReadLine()) {
            return QString();
        }
        const QString l = QString::fromUtf8(m_process.readLine()).trimmed();
        m_lines << l;
        return l;
    }
    /* Lines until one starts with @prefix: that one, or "" */
    QString waitFor(const QString &prefix)
    {
        for (QString l = line(); !l.isNull(); l = line()) {
            if (l.startsWith(prefix)) {
                return l;
            }
        }
        return QString();
    }
    /* Sends @request: the lines up to its answer (ok, skip or error), the answer last */
    QStringList ask(const QString &request)
    {
        QStringList got;
        m_process.write((request + '\n').toUtf8());
        m_process.waitForBytesWritten();
        for (QString l = line(); !l.isNull(); l = line()) {
            got << l;
            if (l.startsWith("ok ") || l.startsWith("skip ") || l.startsWith("error")) {
                break;
            }
        }
        return got;
    }
    QString answer(const QString &request)
    {
        const QStringList got = ask(request);
        return got.isEmpty() ? QString() : got.last();
    }
    /* Sends @request, its answer left to be read */
    void send(const QString &request)
    {
        m_process.write((request + '\n').toUtf8());
        m_process.waitForBytesWritten();
    }
    bool ready() { return waitFor("ready") == "ready 1"; }
    void closeInput() { m_process.closeWriteChannel(); }
    bool finished(int ms = 5000)
    {
        return m_process.state() == QProcess::NotRunning || m_process.waitForFinished(ms);
    }
    bool running() const { return m_process.state() != QProcess::NotRunning; }
    void kill()
    {
        m_process.kill();
        m_process.waitForFinished();
    }
    int exitCode() const { return m_process.exitCode(); }
    qint64 pid() const { return m_process.processId(); }
    /* everything said so far, the rest read now */
    QStringList all()
    {
        while (!line().isNull()) {
        }
        return m_lines;
    }

private:
    QProcess m_process;
    QStringList m_lines;
};

class TestHelper : public QObject
{
    Q_OBJECT

private:
    QString m_root;
    int m_cpus = 4;

    QString path(const QString &rel) const { return m_root + rel; }
    QByteArray value(const QString &rel) const { return readFile(path(rel)).trimmed(); }
    QStringList journal() const
    {
        return QString::fromUtf8(readFile(path("/journal"))).split('\n', Qt::SkipEmptyParts);
    }
    QStringList writes() const
    {
        QStringList w;
        for (const QString &l : journal()) {
            if (l.startsWith("write ")) {
                w << l.mid(6);
            }
        }
        return w;
    }
    void clearJournal() const { QFile::remove(path("/journal")); }
    void setFair(int cpu, qint64 period, qint64 runtime) const
    {
        writeFile(path(QString("%1/cpu%2/period").arg(kFair).arg(cpu)),
                  QByteArray::number(period) + '\n');
        writeFile(path(QString("%1/cpu%2/runtime").arg(kFair).arg(cpu)),
                  QByteArray::number(runtime) + '\n');
    }
    QString fair(int cpu) const
    {
        return QString("%1/%2")
            .arg(value(QString("%1/cpu%2/period").arg(kFair).arg(cpu)),
                 value(QString("%1/cpu%2/runtime").arg(kFair).arg(cpu)));
    }
    bool fairAll(const QString &expected) const
    {
        for (int i = 0; i < m_cpus; i++) {
            if (fair(i) != expected) {
                return false;
            }
        }
        return true;
    }
    QString level() const { return value(QString(kCard) + "/power_dpm_force_performance_level"); }
    void setUdmabuf(qint64 list, qint64 sizeMb) const
    {
        writeFile(path(QString(kUdmabuf) + "/list_limit"), QByteArray::number(list) + '\n');
        writeFile(path(QString(kUdmabuf) + "/size_limit_mb"), QByteArray::number(sizeMb) + '\n');
    }
    /* "list_limit/size_limit_mb" */
    QString udmabuf() const
    {
        return QString("%1/%2").arg(value(QString(kUdmabuf) + "/list_limit"),
                                    value(QString(kUdmabuf) + "/size_limit_mb"));
    }
    QByteArray odTable() const { return readFile(path(QString(kCard) + "/pp_od_clk_voltage")); }
    /* shmem_enabled as the kernel shows it: @choice in brackets among the words */
    void setThp(const QString &choice) const
    {
        QStringList words{"always", "within_size", "advise", "never", "deny", "force"};
        for (QString &word : words) {
            if (word == choice) {
                word = '[' + word + ']';
            }
        }
        writeFile(path(kThp), words.join(' ').toLatin1() + '\n');
    }
    /* the choice in brackets */
    QString thp() const
    {
        const QString text = QString::fromLatin1(value(kThp));
        const qsizetype open = text.indexOf('['), close = text.indexOf(']');
        return open >= 0 && close > open ? text.mid(open + 1, close - open - 1) : QString();
    }
    /* cpuN's cpuidle states, state0 first: "NAME LATENCY_US[ disabled]" */
    void setIdleStates(int cpu, const QStringList &states) const
    {
        const QString dir = path(QString("%1/cpu%2/cpuidle").arg(kCpu).arg(cpu));
        QDir(dir).removeRecursively();
        for (int i = 0; i < states.size(); i++) {
            const QStringList f = states[i].split(' ');
            const QString state = QString("%1/state%2/").arg(dir).arg(i);
            writeFile(state + "name", f.value(0).toLatin1() + '\n');
            writeFile(state + "latency", f.value(1).toLatin1() + '\n');
            writeFile(state + "disable", f.value(2) == "disabled" ? "1\n" : "0\n");
        }
    }
    /* cpuN's energy performance preference, as the test sees the file (the
       helper gets EBUSY for an offline CPU's) */
    QString eppFile(int cpu) const
    {
        return path(QString("%1/cpu%2/cpufreq/energy_performance_preference").arg(kCpu).arg(cpu));
    }
    QString epp(int cpu) const { return QString::fromLatin1(readFile(eppFile(cpu)).trimmed()); }
    void setEpp(int cpu, const QByteArray &value) const { writeFile(eppFile(cpu), value + '\n'); }
    bool eppAll(const QString &expected) const
    {
        for (int i = 0; i < m_cpus; i++) {
            if (epp(i) != expected) {
                return false;
            }
        }
        return true;
    }
    void setOnline(const QByteArray &list) const { writeFile(path(QString(kCpu) + "/online"), list + '\n'); }
    /* The live requests on the fake /dev/cpu_dma_latency, lowest first:
       the files a process holds locked (fakesys.c) */
    QList<int> latencyRequests() const
    {
        const QDir dev(path("/dev"));
        QList<int> out;
        for (const QString &name : dev.entryList({"cpu_dma_latency.*"}, QDir::Files)) {
            const int fd = ::open(QFile::encodeName(dev.filePath(name)).constData(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) {
                continue;
            }
            if (flock(fd, LOCK_SH | LOCK_NB) < 0 && errno == EWOULDBLOCK) {
                out << readFile(dev.filePath(name)).trimmed().toInt();
            }
            ::close(fd);
        }
        std::sort(out.begin(), out.end());
        return out;
    }
    /* what the kernel applies: the lowest request */
    int latency() const
    {
        const QList<int> requests = latencyRequests();
        return requests.isEmpty() ? kNoLatency : requests.first();
    }
    /* what the helpers leave in their state folder */
    QStringList stateFiles() const
    {
        return QDir(path("/run/vitrine-helper")).entryList(QDir::Files | QDir::System);
    }
    bool stateExists(const QString &key) const
    {
        return QFile::exists(path("/run/vitrine-helper/" + key + ".state"));
    }
    /* cardN: an APU like the Radeon 780M */
    void amdApu(int n) const
    {
        const QString card = path(QString("/sys/class/drm/card%1/device").arg(n));
        writeFile(card + "/vendor", "0x1002\n");
        writeFile(card + "/power_dpm_force_performance_level", "auto\n");
        writeFile(card + "/pp_od_clk_voltage", od(800, 2700));
        /* gpu_metrics header: size 120, format 2 (an APU's), content 1 */
        writeFile(card + "/gpu_metrics", QByteArray("\x78\x00\x02\x01", 4) + QByteArray(116, 0));
        QFile::link("../../../../bus/pci/drivers/amdgpu", card + "/driver");
    }
    void setExt(int cpu, qint64 period, qint64 runtime) const
    {
        writeFile(path(QString("%1/cpu%2/period").arg(kExt).arg(cpu)), QByteArray::number(period) + '\n');
        writeFile(path(QString("%1/cpu%2/runtime").arg(kExt).arg(cpu)), QByteArray::number(runtime) + '\n');
    }
    bool extAll(const QString &expected) const
    {
        for (int i = 0; i < m_cpus; i++) {
            if (QString("%1/%2").arg(value(QString("%1/cpu%2/period").arg(kExt).arg(i)),
                                     value(QString("%1/cpu%2/runtime").arg(kExt).arg(i))) != expected) {
                return false;
            }
        }
        return true;
    }
    /* The threads of @pid, and those named as QEMU names its vCPUs */
    static QStringList tids(qint64 pid)
    {
        return QDir(QString("/proc/%1/task").arg(pid)).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    }
    static QStringList vcpus(qint64 pid)
    {
        QStringList out;
        for (const QString &tid : tids(pid)) {
            if (readFile(QString("/proc/%1/task/%2/comm").arg(pid).arg(tid)).startsWith("CPU ")) {
                out << tid;
            }
        }
        return out;
    }
    /* a view thread view-rt made real-time, as schedOf() shows it */
    static QString viewRealtimeSched()
    {
        return QString("%1 1 0").arg(SCHED_FIFO | SCHED_RESET_ON_FORK);
    }
    /* What the fake kernel has for @tid: "policy priority nice", "" if untouched */
    QString schedOf(const QString &tid) const
    {
        for (const QByteArray &line : readFile(path("/sched")).split('\n')) {
            if (line.startsWith(tid.toLatin1() + ' ')) {
                return QString::fromLatin1(line.mid(tid.size() + 1));
            }
        }
        return QString();
    }
    /* the kernel's defaults and an APU like the Radeon 780M */
    void makeTree()
    {
        QDir(m_root).removeRecursively();
        QDir().mkpath(path("/run"));
        writeFile(path("/sys/kernel/security/lockdown"), "[none] integrity confidentiality\n");
        for (int i = 0; i < m_cpus; i++) {
            setFair(i, 1000000000, 50000000);
        }
        amdApu(1);
        writeFile(path("/sys/class/drm/card0/device/vendor"), "0x8086\n");
        /* the kernel's defaults */
        setUdmabuf(1024, 64);
        setThp("never");
        /* the CPUs of a Ryzen 7 260: acpi_idle's states, amd-pstate's
           preference (cpuN/cpufreq is a link to its policy, as in sysfs) */
        for (int i = 0; i < m_cpus; i++) {
            const QString policy = path(QString("%1/cpufreq/policy%2").arg(kCpu).arg(i));
            setIdleStates(i, {"POLL 0", "C1 1", "C2 18", "C3 350"});
            QDir().mkpath(policy);
            QFile::link(QString("../cpufreq/policy%1").arg(i),
                        path(QString("%1/cpu%2/cpufreq").arg(kCpu).arg(i)));
            writeFile(policy + "/scaling_driver", "amd-pstate-epp\n");
            setEpp(i, "balance_performance");
        }
        writeFile(path(QString(kCpu) + "/cpuidle/current_driver"), "acpi_idle\n");
        writeFile(path(kLatency), "");
    }

    /* A PCI device's folder in the fake tree */
    QString pciDir(const QString &address) const { return path("/sys/devices/pci0000:00/" + address); }
    /*
     * A PCI device of the fake bus (fakesys.c): its folder and the bus's link
     * to it, its driver (also the one drivers_probe binds), its IOMMU group
     */
    void pci(const QString &address, const QByteArray &cls, const QString &driver, int group)
    {
        const QString dir = pciDir(address);
        writeFile(dir + "/class", cls + '\n');
        writeFile(dir + "/vendor", "0x1002\n");
        writeFile(dir + "/device", "0x164e\n");
        writeFile(dir + "/driver_override", "(null)\n");
        writeFile(dir + "/fake-native", driver.toLatin1() + '\n');
        QDir().mkpath(path("/sys/bus/pci/devices"));
        QFile::link(dir, path("/sys/bus/pci/devices/" + address));
        if (!driver.isEmpty()) {
            QDir().mkpath(path("/sys/bus/pci/drivers/" + driver));
            QFile::link(path("/sys/bus/pci/drivers/" + driver), dir + "/driver");
        }
        if (group >= 0) {
            const QString groupDir = path(QString("/sys/kernel/iommu_groups/%1").arg(group));
            QDir().mkpath(groupDir + "/devices");
            QFile::link(groupDir, dir + "/iommu_group");
            QFile::link(dir, groupDir + "/devices/" + address);
        }
    }
    /* A device node's folder: its number, its subsystem */
    void devNode(const QString &dir, const QByteArray &number, const QString &subsystem)
    {
        writeFile(dir + "/dev", number + '\n');
        QDir().mkpath(path("/sys/class/" + subsystem));
        QFile::link(path("/sys/class/" + subsystem), dir + "/subsystem");
    }
    /*
     * The fake PCI bus: group 21 a second GPU and its HDMI audio (amdgpu,
     * snd_hda_intel); group 14 a GPU set aside at boot (vfio-pci, the
     * laptop's RTX 5060), its node root's at 0600; group 2 a network card
     * behind its root port; group 17 an NVMe disk; group 18 the GPU the
     * computer started on; group 12 the SMBus and the ISA bridge; group 30
     * a USB controller with a keyboard.  32 GB of memory, a btrfs root on
     * another disk, the test's own locked memory limit (the helper's parent:
     * vitrine) at Fedora's 8 MiB.
     */
    void pciTree()
    {
        const QString gpu = pciDir("0000:6e:00.0"), audio = pciDir("0000:6e:00.1");
        pci("0000:6e:00.0", "0x030000", "amdgpu", 21);
        writeFile(gpu + "/boot_vga", "0\n");
        devNode(gpu + "/drm/card1", "226:1", "drm");
        devNode(gpu + "/drm/renderD129", "226:129", "drm");
        writeFile(gpu + "/drm/card1/card1-DP-1/status", "disconnected\n");
        pci("0000:6e:00.1", "0x040300", "snd_hda_intel", 21);
        devNode(audio + "/sound/card1/controlC1", "116:10", "sound");
        devNode(audio + "/sound/card1/pcmC1D3p", "116:11", "sound");
        pci("0000:01:00.0", "0x030000", "vfio-pci", 14);
        pci("0000:01:00.1", "0x040300", "vfio-pci", 14);
        writeFile(path("/dev/vfio/14"), "");
        writeFile(path("/dev/vfio/14.owner"), "0 0 600\n");
        writeFile(path("/dev/vfio/14.rdev"), "239:14\n");
        pci("0000:00:01.1", "0x060400", "pcieport", 2);
        pci("0000:02:00.0", "0x020000", "r8169", 2);
        writeFile(pciDir("0000:02:00.0") + "/net/enp2s0/operstate", "down\n");
        pci("0000:04:00.0", "0x010802", "nvme", 17);
        const QString nvme = pciDir("0000:04:00.0") + "/nvme/nvme0";
        devNode(nvme, "240:0", "nvme");
        devNode(nvme + "/nvme0n1", "259:0", "block");
        devNode(nvme + "/nvme0n1/nvme0n1p1", "259:1", "block");
        devNode(nvme + "/nvme0n1/nvme0n1p2", "259:2", "block");
        QDir().mkpath(nvme + "/nvme0n1/holders");
        QDir().mkpath(nvme + "/nvme0n1/nvme0n1p1/holders");
        QDir().mkpath(nvme + "/nvme0n1/nvme0n1p2/holders");
        pci("0000:65:00.0", "0x030000", "amdgpu", 18);
        writeFile(pciDir("0000:65:00.0") + "/boot_vga", "1\n");
        pci("0000:00:14.0", "0x0c0500", "piix4_smbus", 12);
        pci("0000:00:14.3", "0x060100", "", 12);
        pci("0000:07:00.0", "0x0c0330", "xhci_hcd", 30);
        devNode(pciDir("0000:07:00.0") + "/usb1/1-1/1-1:1.0/0003:046D:C52B.0001/input/input5/event5",
                "13:69", "input");
        QDir().mkpath(path("/sys/bus/pci/drivers/vfio-pci"));
        writeFile(path("/sys/bus/pci/drivers_probe"), "");
        writeFile(path("/proc/meminfo"), "MemTotal:       32768000 kB\nMemFree:          524288 kB\n");
        writeFile(path("/proc/self/mountinfo"), "22 1 0:21 / / rw,relatime shared:1 - btrfs /dev/sda2 rw\n");
        writeFile(path("/dev/sda2.rdev"), "b 8:2\n");
        writeFile(path(QString("/memlock-%1").arg(getpid())), "8388608 8388608\n");
    }
    /* What the fake bus has @address bound to, "" for none */
    QString driverOf(const QString &address) const
    {
        return QFileInfo(pciDir(address) + "/driver").symLinkTarget().section('/', -1);
    }
    QByteArray overrideOf(const QString &address) const
    {
        return readFile(pciDir(address) + "/driver_override").trimmed();
    }
    bool nodeExists(int group) const { return QFile::exists(path(QString("/dev/vfio/%1").arg(group))); }
    /* "uid gid mode" of the fake /dev/vfio/@group */
    QByteArray nodeOwner(int group) const { return value(QString("/dev/vfio/%1.owner").arg(group)); }
    QByteArray nodeMine() const { return QByteArray::number(getuid()) + " 0 600"; }
    /* the test's own locked memory limit, as the fake kernel has it */
    QByteArray memlock() const { return value(QString("/memlock-%1").arg(getpid())); }
    /* The answer of a devices-bind session to @request, which must change nothing */
    QString refusal(const QString &request, const QString &verb = "devices-bind")
    {
        clearJournal();
        Helper h(m_root, {}, HELPER_FAKE, {verb});
        if (!h.ready()) {
            return "not ready";
        }
        const QString got = h.answer(request);
        h.closeInput();
        h.finished();
        if (!writes().isEmpty() || stateFiles() != QStringList({"lock"})) {
            return "changed: " + writes().join("; ") + " / " + stateFiles().join("; ");
        }
        return got;
    }

private slots:
    void init()
    {
        m_root = QString(TEST_WORK_DIR) + "/" + QTest::currentTestFunction();
        makeTree();
    }

    /* Shorter period: runtime first; longer back: period first */
    void fairServerWriteOrder()
    {
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        QCOMPARE(h.answer("watch " + qemu.pidText()), "ok watch " + qemu.pidText());
        QCOMPARE(h.answer("fair-server on"),
                 QString("ok fair-server on: 4 cpus at 10 ms / 1 ms (was 1000 ms / 50 ms)"));
        QVERIFY(fairAll("10000000/1000000"));
        QVERIFY(stateExists("fair-server"));
        const QStringList w = writes();
        QCOMPARE(w.size(), 8);
        for (int i = 0; i < m_cpus; i++) {
            const QString dir = QString("%1/cpu%2/").arg(kFair).arg(i);
            QVERIFY(w.indexOf(dir + "runtime 1000000") >= 0);
            QVERIFY(w.indexOf(dir + "runtime 1000000") < w.indexOf(dir + "period 10000000"));
        }
        QCOMPARE(h.answer("fair-server on"), QString("ok fair-server on: already"));

        clearJournal();
        const QString pid = qemu.pidText();
        qemu.stop();
        QCOMPARE(h.waitFor("exited"), "exited " + pid);
        QCOMPARE(h.waitFor("restored"),
                 QString("restored fair-server: 4 cpus back to 1000 ms / 50 ms"));
        QCOMPARE(h.waitFor("bye"), QString("bye"));
        QVERIFY(h.finished());
        QCOMPARE(h.exitCode(), 0);
        QVERIFY(fairAll("1000000000/50000000"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        const QStringList back = writes();
        QCOMPARE(back.size(), 8);
        for (int i = 0; i < m_cpus; i++) {
            const QString dir = QString("%1/cpu%2/").arg(kFair).arg(i);
            QVERIFY(back.indexOf(dir + "period 1000000000") <
                    back.indexOf(dir + "runtime 50000000"));
        }
        QVERIFY(!journal().join('\n').contains("FAILED"));
        /* the record of what root did */
        QVERIFY(journal().join('\n').contains("log fair server: 4 cpus back"));
    }

    /* manual, s 0, s 1, c; back: the table it had (never r, which resets a
       user's whole overdrive table), c, the level it had */
    void gpuFloorAutoAndRelease()
    {
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QCOMPARE(h.answer("gpu-floor card1 auto"),
                 QString("ok gpu-floor card1 1800 MHz (was 800 MHz, level auto)"));
        QCOMPARE(level(), QString("manual"));
        QCOMPARE(odTable(), od(1800, 2700));
        QCOMPARE(writes(), QStringList({QString(kCard) + "/power_dpm_force_performance_level manual",
                                        QString(kCard) + "/pp_od_clk_voltage s 0 1800",
                                        QString(kCard) + "/pp_od_clk_voltage s 1 2700",
                                        QString(kCard) + "/pp_od_clk_voltage c"}));
        QCOMPARE(h.answer("gpu-floor card1 auto"), QString("ok gpu-floor card1 auto: already"));

        clearJournal();
        const QStringList end = h.ask("release");
        QVERIFY(h.finished());
        const QStringList all = h.all();
        QVERIFY(all.contains("restored gpu-floor card1: level auto"));
        QCOMPARE(all.last(), QString("bye"));
        QCOMPARE(writes(), QStringList({QString(kCard) + "/pp_od_clk_voltage s 0 800",
                                        QString(kCard) + "/pp_od_clk_voltage s 1 2700",
                                        QString(kCard) + "/pp_od_clk_voltage c",
                                        QString(kCard) + "/power_dpm_force_performance_level auto"}));
        QCOMPARE(level(), QString("auto"));
        QCOMPARE(odTable(), od(800, 2700));
        QVERIFY(!stateExists("gpu-floor-card1"));
        /* the QEMU runs on: release does not wait for it */
        QVERIFY(::kill(pid_t(qemu.pid()), 0) == 0);
    }

    void gpuFloorValues()
    {
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QCOMPARE(h.answer("gpu-floor card1 3000"),
                 QString("error gpu-floor card1: 3000 MHz is outside 800-2700 MHz"));
        QCOMPARE(h.answer("gpu-floor card1 18x"),
                 QString("error gpu-floor card1: give a clock in MHz, auto or off"));
        QCOMPARE(h.answer("gpu-floor ../card1 auto"), QString("error gpu-floor: give the card as cardN"));
        QCOMPARE(h.answer("gpu-floor card01 auto"), QString("error gpu-floor: give the card as cardN"));
        QCOMPARE(h.answer("gpu-floor card0 auto"), QString("skip gpu-floor card0: not an AMD GPU"));
        QCOMPARE(h.answer("gpu-floor card7 auto"), QString("skip gpu-floor card7: no such card"));
        QVERIFY(writes().isEmpty());
        QCOMPARE(h.answer("gpu-floor card1 2000"),
                 QString("ok gpu-floor card1 2000 MHz (was 800 MHz, level auto)"));
        QCOMPARE(odTable(), od(2000, 2700));
        /* another floor: the old one back first */
        const QStringList got = h.ask("gpu-floor card1 1500");
        QCOMPARE(got.first(), QString("restored gpu-floor card1: level auto"));
        QCOMPARE(got.last(), QString("ok gpu-floor card1 1500 MHz (was 800 MHz, level auto)"));
        QCOMPARE(odTable(), od(1500, 2700));
        const QStringList off = h.ask("gpu-floor card1 off");
        QCOMPARE(off, QStringList({"restored gpu-floor card1: level auto", "ok gpu-floor card1 off"}));
        QCOMPARE(level(), QString("auto"));
        QCOMPARE(odTable(), od(800, 2700));
    }

    void gpuFloorSkips()
    {
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        /* a discrete GPU (metrics format 1): auto sets nothing */
        writeFile(path(QString(kCard) + "/gpu_metrics"), QByteArray("\x78\x00\x01\x03", 4));
        QCOMPARE(h.answer("gpu-floor card1 auto"),
                 QString("skip gpu-floor card1: auto sets a floor on AMD APUs only"));
        writeFile(path(QString(kCard) + "/gpu_metrics"), QByteArray("\x78\x00\x02\x01", 4));
        /* a level someone chose */
        writeFile(path(QString(kCard) + "/power_dpm_force_performance_level"), "high\n");
        QCOMPARE(h.answer("gpu-floor card1 auto"),
                 QString("skip gpu-floor card1: the performance level is high, not auto"));
        writeFile(path(QString(kCard) + "/power_dpm_force_performance_level"), "auto\n");
        /* a minimum already higher */
        writeFile(path(QString(kCard) + "/pp_od_clk_voltage"), od(2000, 2700));
        QCOMPARE(h.answer("gpu-floor card1 auto"),
                 QString("skip gpu-floor card1: the minimum is already 2000 MHz"));
        /* no overdrive table (a discrete GPU without the ppfeaturemask bit) */
        QFile::remove(path(QString(kCard) + "/pp_od_clk_voltage"));
        QCOMPARE(h.answer("gpu-floor card1 auto"),
                 QString("skip gpu-floor card1: no overdrive clock table (pp_od_clk_voltage)"));
        QVERIFY(writes().isEmpty());
        QVERIFY(!stateExists("gpu-floor-card1"));
    }

    void fairServerSkips()
    {
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        writeFile(path("/sys/kernel/security/lockdown"), "none [integrity] confidentiality\n");
        QCOMPARE(h.answer("fair-server on"), QString("skip fair-server: kernel lockdown (integrity)"));
        writeFile(path("/sys/kernel/security/lockdown"), "[none] integrity confidentiality\n");
        QDir(path(kFair)).removeRecursively();
        QCOMPARE(h.answer("fair-server on"), QString("skip fair-server: this kernel has no fair server"));
        QDir(path("/sys/kernel/debug")).removeRecursively();
        QCOMPARE(h.answer("fair-server on"), QString("skip fair-server: debugfs is not mounted"));
        QVERIFY(writes().isEmpty());
    }

    /*
     * The kernel's lockdown (Secure Boot) keeps the fair server as it is: no
     * real-time threads then, which would keep kernel workers off their
     * CPUs for up to 950 ms; the GPU floor, in sysfs, still applies
     */
    void lockdownNoRealtime()
    {
        writeFile(path("/sys/kernel/security/lockdown"), "none [integrity] confidentiality\n");
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QCOMPARE(h.answer("fair-server on"), QString("skip fair-server: kernel lockdown (integrity)"));
        QVERIFY(h.answer("gpu-floor card1 auto").startsWith("ok gpu-floor card1 1800 MHz"));
        QCOMPARE(h.answer("rt " + qemu.pidText()),
                 QString("skip rt %1: the fair server is not set: kernel lockdown (integrity)")
                     .arg(qemu.pid()));
        /* behind: nothing real-time to put back, no nice either */
        QCOMPARE(h.answer("behind " + qemu.pidText()),
                 QString("ok behind %1: 0 threads ordinary, 0 vCPUs at nice -5").arg(qemu.pid()));
        QVERIFY(journal().filter(QRegularExpression("^(sched|nice) ")).isEmpty());
        QVERIFY(fairAll("1000000000/50000000"));
        h.ask("release");
        QVERIFY(h.finished());
        QCOMPARE(level(), QString("auto"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    /* No fair server (Linux before 6.12): no real-time threads either */
    void noFairServerNoRealtime()
    {
        QDir(path(kFair)).removeRecursively();
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QCOMPARE(h.answer("fair-server on"), QString("skip fair-server: this kernel has no fair server"));
        QCOMPARE(h.answer("rt " + qemu.pidText()),
                 QString("skip rt %1: the fair server is not set: this kernel has no fair server")
                     .arg(qemu.pid()));
        /* nor without asking for it */
        Helper other(m_root);
        QVERIFY(other.ready());
        other.answer("watch " + qemu.pidText());
        QCOMPARE(other.answer("rt " + qemu.pidText()),
                 QString("skip rt %1: the fair server is not set: the fair server was not asked "
                         "for").arg(qemu.pid()));
        QVERIFY(journal().filter("sched ").isEmpty());
    }

    /* A write refused (cpu2's period here): every CPU back, nothing recorded
       or held, and why; no real-time threads until it is set */
    void fairServerAllOrNothing()
    {
        const QString period2 = path(QString("%1/cpu2/period").arg(kFair));
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QVERIFY(QFile::setPermissions(period2, QFileDevice::ReadOwner));
        QCOMPARE(h.answer("fair-server on"),
                 QString("error fair-server: fair server: cpu2: Permission denied"));
        QVERIFY(fairAll("1000000000/50000000"));
        QVERIFY(!stateExists("fair-server"));
        /* cpu0 and cpu1 set and back, cpu2's runtime written and back */
        const QStringList w = writes();
        QCOMPARE(w.size(), 4 + 4 + 3);
        QVERIFY(w.contains(QString("%1/cpu2/period 10000000 FAILED").arg(kFair)));
        QCOMPARE(w.last(), QString("%1/cpu1/runtime 50000000").arg(kFair));
        QCOMPARE(h.answer("rt " + qemu.pidText()),
                 QString("skip rt %1: the fair server is not set: fair server: cpu2: Permission "
                         "denied").arg(qemu.pid()));
        QVERIFY(journal().filter("sched ").isEmpty());
        /* asked again once it can be: set */
        QVERIFY(QFile::setPermissions(period2, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        QCOMPARE(h.answer("fair-server on"),
                 QString("ok fair-server on: 4 cpus at 10 ms / 1 ms (was 1000 ms / 50 ms)"));
        QVERIFY(h.answer("rt " + qemu.pidText()).startsWith(QString("ok rt %1").arg(qemu.pid())));
        qemu.stop();
        QVERIFY(h.finished());
        QVERIFY(fairAll("1000000000/50000000"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    /* What is written is read back: a CPU that does not keep it is a failure */
    void fairServerReadBack()
    {
        writeFile(path(QString("%1/cpu1/period.stuck").arg(kFair)), "");
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QCOMPARE(h.answer("fair-server on"),
                 QString("error fair-server: fair server: cpu1 does not read back what was written"));
        QVERIFY(fairAll("1000000000/50000000"));
        QVERIFY(!stateExists("fair-server"));
        QVERIFY(h.answer("rt " + qemu.pidText()).startsWith("skip rt "));
    }

    /* Offline CPUs run nothing, and the kernel refuses their writes: left out */
    void offlineCpus()
    {
        writeFile(path("/sys/devices/system/cpu/online"), "0-1,3\n");
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QCOMPARE(h.answer("fair-server on"),
                 QString("ok fair-server on: 3 cpus at 10 ms / 1 ms (was 1000 ms / 50 ms)"));
        QCOMPARE(fair(2), QString("1000000000/50000000"));
        QCOMPARE(fair(3), QString("10000000/1000000"));
        QVERIFY(h.answer("rt " + qemu.pidText()).startsWith("ok rt "));
        h.ask("release");
        QVERIFY(h.finished());
        QVERIFY(h.all().contains("restored fair-server: 3 cpus back to 1000 ms / 50 ms"));
        QVERIFY(fairAll("1000000000/50000000"));
        QVERIFY(!journal().join('\n').contains("FAILED"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    /* Under sched_ext, ordinary tasks wait for the ext server: it is set too,
       and without it (Linux 6.12 to 6.19) there is no bound */
    void schedExt()
    {
        writeFile(path("/sys/kernel/sched_ext/state"), "enabled\n");
        {
            FakeQemu qemu;
            Helper h(m_root);
            QVERIFY(h.ready());
            h.answer("watch " + qemu.pidText());
            QCOMPARE(h.answer("fair-server on"),
                     QString("skip fair-server: a sched_ext scheduler runs, and this kernel has no "
                             "server for its tasks"));
            QVERIFY(fairAll("1000000000/50000000"));
            QVERIFY(h.answer("rt " + qemu.pidText()).startsWith("skip rt "));
        }
        for (int i = 0; i < m_cpus; i++) {
            setExt(i, 1000000000, 50000000);
        }
        {
            FakeQemu qemu;
            Helper h(m_root);
            QVERIFY(h.ready());
            h.answer("watch " + qemu.pidText());
            QCOMPARE(h.answer("fair-server on"),
                     QString("ok fair-server on: 4 cpus at 10 ms / 1 ms (was 1000 ms / 50 ms); "
                             "ext server: 4 cpus at 10 ms / 1 ms (was 1000 ms / 50 ms)"));
            QVERIFY(fairAll("10000000/1000000"));
            QVERIFY(extAll("10000000/1000000"));
            QVERIFY(h.answer("rt " + qemu.pidText()).startsWith("ok rt "));
            qemu.stop();
            QVERIFY(h.finished());
            const QStringList all = h.all();
            QVERIFY(all.contains("restored fair-server: 4 cpus back to 1000 ms / 50 ms"));
            QVERIFY(all.contains("restored ext-server: 4 cpus back to 1000 ms / 50 ms"));
            QVERIFY(fairAll("1000000000/50000000"));
            QVERIFY(extAll("1000000000/50000000"));
            QCOMPARE(stateFiles(), QStringList({"lock"}));
        }
        /* the ext server refused: the fair server let go too, no bound */
        writeFile(path(QString("%1/cpu0/period.stuck").arg(kExt)), "");
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        const QStringList got = h.ask("fair-server on");
        QCOMPARE(got.last(),
                 QString("error fair-server: ext server: cpu0 does not read back what was written"));
        QVERIFY(got.contains("restored fair-server: 4 cpus back to 1000 ms / 50 ms"));
        QVERIFY(fairAll("1000000000/50000000"));
        QVERIFY(h.answer("rt " + qemu.pidText()).startsWith("skip rt "));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    /*
     * Found at the target without a record while other CPUs are not: an
     * earlier run's leftovers (2026-10-04: two CPUs at 10 ms / 1 ms, left
     * for good), put back after with the others, to what they have
     */
    void leftoversAdopted()
    {
        setFair(0, 10000000, 1000000);
        setFair(1, 10000000, 1000000);
        {
            FakeQemu qemu;
            Helper h(m_root);
            QVERIFY(h.ready());
            h.answer("watch " + qemu.pidText());
            QCOMPARE(h.answer("fair-server on"),
                     QString("ok fair-server on: 2 cpus at 10 ms / 1 ms (was 1000 ms / 50 ms); 2 "
                             "cpus found at 10 ms / 1 ms without a record, put back after with "
                             "the others"));
            QVERIFY(fairAll("10000000/1000000"));
            qemu.stop();
            QVERIFY(h.finished());
            QVERIFY(h.all().contains("restored fair-server: 4 cpus back to 1000 ms / 50 ms"));
            QVERIFY(fairAll("1000000000/50000000"));
        }
        /* the others differ among them: the kernel's own */
        setFair(0, 10000000, 1000000);
        setFair(2, 500000000, 25000000);
        {
            FakeQemu qemu;
            Helper h(m_root);
            QVERIFY(h.ready());
            h.answer("watch " + qemu.pidText());
            QVERIFY(h.answer("fair-server on").contains("1 cpus found at 10 ms / 1 ms"));
            qemu.stop();
            QVERIFY(h.finished());
        }
        QCOMPARE(fair(0), QString("1000000000/50000000"));
        QCOMPARE(fair(2), QString("500000000/25000000"));
        QCOMPARE(fair(3), QString("1000000000/50000000"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    /* A CPU that cannot be put back stays recorded: the next helper tries again */
    void failedRestoreKept()
    {
        const QString period1 = path(QString("%1/cpu1/period").arg(kFair));
        {
            FakeQemu qemu;
            Helper h(m_root);
            QVERIFY(h.ready());
            h.answer("watch " + qemu.pidText());
            QVERIFY(h.answer("fair-server on").startsWith("ok fair-server on: 4 cpus"));
            QVERIFY(QFile::setPermissions(period1, QFileDevice::ReadOwner));
            qemu.stop();
            QVERIFY(h.finished());
            QVERIFY(h.all().contains(
                "restored fair-server: 3 cpus back to 1000 ms / 50 ms (some failed, kept for later)"));
        }
        QCOMPARE(fair(1), QString("10000000/1000000"));
        QVERIFY(stateExists("fair-server"));
        QVERIFY(QFile::setPermissions(period1, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        Helper b(m_root);
        QCOMPARE(b.line(), QString("restored fair-server: 1 cpus back to 1000 ms / 50 ms"));
        QVERIFY(b.ready());
        QVERIFY(fairAll("1000000000/50000000"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        b.closeInput();
        QVERIFY(b.finished());
    }

    /*
     * A helper killed while a QEMU's threads are real-time: the next one
     * puts them back, before the fair server they needed
     */
    void deadHelperRealtime()
    {
        FakeQemu qemu;
        {
            Helper a(m_root);
            QVERIFY(a.ready());
            a.answer("watch " + qemu.pidText());
            QVERIFY(a.answer("fair-server on").startsWith("ok fair-server on"));
            QCOMPARE(a.answer("rt " + qemu.pidText()),
                     QString("ok rt %1: 4 of 4 threads real-time").arg(qemu.pid()));
            a.kill();
        }
        QVERIFY(stateExists("sched-" + qemu.pidText()));
        for (const QString &tid : tids(qemu.pid())) {
            QVERIFY2(schedOf(tid).startsWith("1 1 "), qPrintable(schedOf(tid)));
        }
        clearJournal();
        Helper b(m_root);
        QCOMPARE(b.line(), QString("restored rt %1: 4 threads back to SCHED_OTHER").arg(qemu.pid()));
        QCOMPARE(b.line(), QString("restored fair-server: 4 cpus back to 1000 ms / 50 ms"));
        QVERIFY(b.ready());
        for (const QString &tid : tids(qemu.pid())) {
            QVERIFY2(schedOf(tid).startsWith("0 0 "), qPrintable(schedOf(tid)));
        }
        /* the threads first, then the bound */
        const QStringList j = journal();
        int lastSched = -1, firstWrite = -1;
        for (int i = 0; i < j.size(); i++) {
            lastSched = j[i].startsWith("sched ") ? i : lastSched;
            firstWrite = firstWrite < 0 && j[i].startsWith("write ") ? i : firstWrite;
        }
        QVERIFY(lastSched >= 0 && firstWrite > lastSched);
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        b.closeInput();
        QVERIFY(b.finished());
    }

    /*
     * Two helpers share the fair server; the one that made its QEMU's
     * threads real-time dies.  The other, letting the bound go last, puts
     * those threads back first: they need it.
     */
    void deadHelperRealtimeLiveRestorer()
    {
        FakeQemu q1, q2;
        Helper a(m_root), b(m_root);
        QVERIFY(a.ready());
        QVERIFY(b.ready());
        a.answer("watch " + q1.pidText());
        b.answer("watch " + q2.pidText());
        QVERIFY(a.answer("fair-server on").startsWith("ok fair-server on: 4 cpus"));
        QVERIFY(b.answer("fair-server on").startsWith("ok fair-server on: set by another"));
        QVERIFY(a.answer("rt " + q1.pidText()).startsWith("ok rt "));
        a.kill();
        q2.stop();
        QVERIFY(b.finished());
        const QStringList all = b.all();
        const int rt = all.indexOf(QString("restored rt %1: 4 threads back to SCHED_OTHER").arg(q1.pid()));
        const int fair = all.indexOf("restored fair-server: 4 cpus back to 1000 ms / 50 ms");
        QVERIFY2(rt >= 0 && fair > rt, qPrintable(all.join(" | ")));
        for (const QString &tid : tids(q1.pid())) {
            QVERIFY(schedOf(tid).startsWith("0 0 "));
        }
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    /* A sched_ext scheduler started after the fair server was set: no more
       real-time threads until its server is set too */
    void schedExtLater()
    {
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QVERIFY(h.answer("fair-server on").startsWith("ok fair-server on: 4 cpus"));
        writeFile(path("/sys/kernel/sched_ext/state"), "enabled\n");
        QCOMPARE(h.answer("rt " + qemu.pidText()),
                 QString("skip rt %1: the fair server is not set: a sched_ext scheduler started "
                         "after it was set").arg(qemu.pid()));
        /* a kernel without its server: said, still no real-time */
        QCOMPARE(h.answer("fair-server on"),
                 QString("skip fair-server: a sched_ext scheduler runs, and this kernel has no "
                         "server for its tasks"));
        QVERIFY(h.answer("rt " + qemu.pidText()).startsWith("skip rt "));
        /* with it: set now, and real-time again */
        for (int i = 0; i < m_cpus; i++) {
            setExt(i, 1000000000, 50000000);
        }
        QCOMPARE(h.answer("fair-server on"),
                 QString("ok fair-server on: already; ext server: 4 cpus at 10 ms / 1 ms (was "
                         "1000 ms / 50 ms)"));
        QVERIFY(h.answer("rt " + qemu.pidText()).startsWith("ok rt "));
        h.ask("release");
        QVERIFY(h.finished());
        QVERIFY(fairAll("1000000000/50000000"));
        QVERIFY(extAll("1000000000/50000000"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    /* behind takes privilege away: without a record (its folder not
       writable), the threads go ordinary all the same, without the nice */
    void behindWithoutRecord()
    {
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QVERIFY(h.answer("fair-server on").startsWith("ok fair-server on"));
        QVERIFY(h.answer("rt " + qemu.pidText()).startsWith("ok rt "));
        const QString run = path("/run/vitrine-helper");
        QVERIFY(QFile::setPermissions(run, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        const auto back = qScopeGuard([run]() {
            QFile::setPermissions(run, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                           QFileDevice::ExeOwner);
        });
        QCOMPARE(h.answer("behind " + qemu.pidText()),
                 QString("ok behind %1: 4 threads ordinary, 0 vCPUs at nice -5").arg(qemu.pid()));
        for (const QString &tid : tids(qemu.pid())) {
            QCOMPARE(schedOf(tid), QString("0 0 0"));
        }
    }

    /*
     * A thread real-time before rt (QEMU's or a library's own choice, an
     * older QEMU's own vCPU priority) is left as it is: by rt, behind, the
     * release, and the next helper after a crash
     */
    void realtimeBeforeKept()
    {
        FakeQemu qemu;
        const QString worker = tids(qemu.pid()).last();
        writeFile(path("/sched"), worker.toLatin1() + " 1 1 0\n");
        {
            Helper a(m_root);
            QVERIFY(a.ready());
            a.answer("watch " + qemu.pidText());
            QVERIFY(a.answer("fair-server on").startsWith("ok fair-server on"));
            QVERIFY(a.answer("rt " + qemu.pidText()).startsWith("ok rt "));
            QCOMPARE(a.answer("behind " + qemu.pidText()),
                     QString("ok behind %1: 3 threads ordinary, 2 vCPUs at nice -5").arg(qemu.pid()));
            QCOMPARE(schedOf(worker), QString("1 1 0"));
            QVERIFY(readFile(path("/run/vitrine-helper/sched-" + qemu.pidText() + ".state"))
                        .contains(" kept " + worker.toLatin1() + "\n"));
            QVERIFY(a.answer("rt " + qemu.pidText()).startsWith("ok rt "));
            a.kill();
        }
        Helper b(m_root);
        QCOMPARE(b.line(), QString("restored rt %1: 3 threads back to SCHED_OTHER, vCPUs back to "
                                   "nice 0").arg(qemu.pid()));
        QVERIFY(b.ready());
        QCOMPARE(schedOf(worker), QString("1 1 0"));
        b.closeInput();
        QVERIFY(b.finished());
    }

    /* A record kept for an offline CPU (a restore could not put it back)
       stays through the next apply, until the CPU can be put back */
    void keptRecordOffline()
    {
        {
            FakeQemu qemu;
            Helper a(m_root);
            QVERIFY(a.ready());
            a.answer("watch " + qemu.pidText());
            QVERIFY(a.answer("fair-server on").startsWith("ok fair-server on: 4 cpus"));
            a.kill();
        }
        /* cpu3 went offline meanwhile */
        writeFile(path("/sys/devices/system/cpu/online"), "0-2\n");
        FakeQemu qemu;
        Helper b(m_root);
        QCOMPARE(b.line(), QString("restored fair-server: 3 cpus back to 1000 ms / 50 ms (some "
                                   "failed, kept for later)"));
        QVERIFY(b.ready());
        b.answer("watch " + qemu.pidText());
        QVERIFY(b.answer("fair-server on").startsWith("ok fair-server on: 3 cpus"));
        QVERIFY(readFile(path("/run/vitrine-helper/fair-server.state")).contains("cpu3 "));
        qemu.stop();
        QVERIFY(b.finished());
        QVERIFY(readFile(path("/run/vitrine-helper/fair-server.state")).startsWith("cpu3 "));
        QCOMPARE(fair(3), QString("10000000/1000000"));
        /* online again: put back by the next helper */
        writeFile(path("/sys/devices/system/cpu/online"), "0-3\n");
        Helper c(m_root);
        QCOMPARE(c.line(), QString("restored fair-server: 1 cpus back to 1000 ms / 50 ms"));
        QVERIFY(c.ready());
        QVERIFY(fairAll("1000000000/50000000"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        c.closeInput();
        QVERIFY(c.finished());
    }

    /* An online list of many ranges (every other CPU of a big host) */
    void manyOnlineRanges()
    {
        QStringList even;
        for (int i = 0; i <= 130; i += 2) {
            even << QString::number(i);
        }
        writeFile(path("/sys/devices/system/cpu/online"), even.join(',').toLatin1() + '\n');
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QCOMPARE(h.answer("fair-server on"),
                 QString("ok fair-server on: 2 cpus at 10 ms / 1 ms (was 1000 ms / 50 ms)"));
        QCOMPARE(fair(1), QString("1000000000/50000000"));
        QCOMPARE(fair(2), QString("10000000/1000000"));
    }

    /* A CPU whose own undo failed (left half-way) stays recorded; so do the
       leftovers found: the next helper puts them back */
    void failedUndoRecorded()
    {
        setFair(3, 10000000, 1000000);
        writeFile(path(QString("%1/cpu2/runtime.writes").arg(kFair)), "1\n");
        QVERIFY(QFile::setPermissions(path(QString("%1/cpu2/period").arg(kFair)),
                                      QFileDevice::ReadOwner));
        {
            FakeQemu qemu;
            Helper a(m_root);
            QVERIFY(a.ready());
            a.answer("watch " + qemu.pidText());
            QCOMPARE(a.answer("fair-server on"),
                     QString("error fair-server: fair server: cpu2: Permission denied"));
        }
        QCOMPARE(fair(0), QString("1000000000/50000000"));
        QCOMPARE(fair(2), QString("1000000000/1000000"));
        const QByteArray state = readFile(path("/run/vitrine-helper/fair-server.state"));
        QVERIFY2(state.contains("cpu2 ") && state.contains("cpu3 ") && !state.contains("cpu0 "),
                 state.constData());
        QFile::remove(path(QString("%1/cpu2/runtime.writes").arg(kFair)));
        QVERIFY(QFile::setPermissions(path(QString("%1/cpu2/period").arg(kFair)),
                                      QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        Helper b(m_root);
        QCOMPARE(b.line(), QString("restored fair-server: 2 cpus back to 1000 ms / 50 ms"));
        QVERIFY(b.ready());
        QVERIFY(fairAll("1000000000/50000000"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        b.closeInput();
        QVERIFY(b.finished());
    }

    /* The record of a process that is gone, its pid maybe another's now: nothing done */
    void staleSchedRecord()
    {
        FakeQemu qemu;
        QVERIFY(QDir().mkpath(path("/run/vitrine-helper")));
        QFile::setPermissions(path("/run/vitrine-helper"),
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        writeFile(path("/run/vitrine-helper/sched-" + qemu.pidText() + ".state"),
                  "pid " + qemu.pidText().toLatin1() + " start 1 niced 1\n");
        Helper b(m_root);
        QVERIFY(b.ready());
        QVERIFY(journal().filter(QRegularExpression("^(sched|nice) ")).isEmpty());
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        b.closeInput();
        QVERIFY(b.finished());
    }

    /*
     * A VM behind the one in front: every thread real-time made ordinary
     * again, its vCPUs ("CPU n/KVM") at nice -5; in front again: real-time;
     * at the end everything as it was
     */
    void behindAndFront()
    {
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QVERIFY(h.answer("fair-server on").startsWith("ok fair-server on"));
        const QStringList cpus = vcpus(qemu.pid());
        QCOMPARE(cpus.size(), 2);
        QCOMPARE(h.answer("rt " + qemu.pidText()),
                 QString("ok rt %1: 4 of 4 threads real-time").arg(qemu.pid()));
        QCOMPARE(h.answer("behind " + qemu.pidText()),
                 QString("ok behind %1: 4 threads ordinary, 2 vCPUs at nice -5").arg(qemu.pid()));
        for (const QString &tid : tids(qemu.pid())) {
            QCOMPARE(schedOf(tid), QString("0 0 %1").arg(cpus.contains(tid) ? -5 : 0));
        }
        QVERIFY(readFile(path("/run/vitrine-helper/sched-" + qemu.pidText() + ".state"))
                    .contains(" niced 1 rt 1 kept -\n"));
        /* again: nothing more to do */
        QCOMPARE(h.answer("behind " + qemu.pidText()),
                 QString("ok behind %1: 0 threads ordinary, 0 vCPUs at nice -5").arg(qemu.pid()));
        /* in front again */
        QCOMPARE(h.answer("rt " + qemu.pidText()),
                 QString("ok rt %1: 4 of 4 threads real-time").arg(qemu.pid()));
        h.ask("release");
        QVERIFY(h.finished());
        QVERIFY(h.all().contains(QString("restored rt %1: 4 threads back to SCHED_OTHER, vCPUs back "
                                         "to nice 0").arg(qemu.pid())));
        for (const QString &tid : tids(qemu.pid())) {
            QCOMPARE(schedOf(tid), QString("0 0 0"));
        }
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    /*
     * vitrine restarted while its VMs run: the old helper goes on (its input
     * closed) with the VM it made real-time, and the new one sends that VM
     * behind - from the old one's record, which tells its threads made
     * real-time from those real-time before - and another to the front:
     * never two VMs real-time
     */
    void behindAfterRestart()
    {
        FakeQemu q1, q2;
        const QString worker = tids(q1.pid()).last();
        /* real-time before any helper: QEMU's own choice, kept by both */
        writeFile(path("/sched"), worker.toLatin1() + " 1 1 0\n");
        Helper a(m_root);
        QVERIFY(a.ready());
        a.answer("watch " + q1.pidText());
        a.answer("watch " + q2.pidText());
        QVERIFY(a.answer("fair-server on").startsWith("ok fair-server on"));
        QVERIFY(a.answer("behind " + q2.pidText()).startsWith("ok behind"));
        QVERIFY(a.answer("rt " + q1.pidText()).startsWith("ok rt"));
        a.closeInput();
        QVERIFY(!a.finished(300));

        Helper b(m_root);
        QVERIFY(b.ready());
        b.answer("watch " + q1.pidText());
        b.answer("watch " + q2.pidText());
        QCOMPARE(b.answer("fair-server on"), QString("ok fair-server on: set by another vitrine session"));
        QCOMPARE(b.answer("behind " + q1.pidText()),
                 QString("ok behind %1: 3 threads ordinary, 2 vCPUs at nice -5").arg(q1.pid()));
        QCOMPARE(b.answer("rt " + q2.pidText()),
                 QString("ok rt %1: 4 of 4 threads real-time").arg(q2.pid()));
        const QStringList cpus = vcpus(q1.pid());
        for (const QString &tid : tids(q1.pid())) {
            QCOMPARE(schedOf(tid), tid == worker ? QString("1 1 0")
                                   : QString("0 0 %1").arg(cpus.contains(tid) ? -5 : 0));
        }
        for (const QString &tid : tids(q2.pid())) {
            QVERIFY(schedOf(tid).startsWith("1 1 "));
        }
        /* and back: q1 in front again, under the new helper */
        QCOMPARE(b.answer("behind " + q2.pidText()),
                 QString("ok behind %1: 4 threads ordinary, 0 vCPUs at nice -5").arg(q2.pid()));
        QCOMPARE(b.answer("rt " + q1.pidText()),
                 QString("ok rt %1: 4 of 4 threads real-time").arg(q1.pid()));
        b.ask("release");
        QVERIFY(b.finished());
        /* a still holds them: the last one out puts them back */
        QVERIFY(a.running());
        q1.stop();
        q2.stop();
        QVERIFY(a.finished());
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    /*
     * A record that does not say which threads were real-time before (an
     * older helper's), held by a helper still running: not taken - as far
     * as this one knows, those threads are QEMU's own, and they stay
     */
    void olderRecordNotTaken()
    {
        FakeQemu qemu;
        QByteArray sched;
        for (const QString &tid : tids(qemu.pid())) {
            sched += tid.toLatin1() + " 1 1 0\n";
        }
        writeFile(path("/sched"), sched);
        /* its start time, field 22 of /proc/PID/stat */
        const QByteArray stat = readFile(QString("/proc/%1/stat").arg(qemu.pid()));
        const QByteArray start = stat.mid(stat.lastIndexOf(')') + 2).split(' ').value(19);
        QVERIFY(QDir().mkpath(path("/run/vitrine-helper")));
        QFile::setPermissions(path("/run/vitrine-helper"),
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        writeFile(path("/run/vitrine-helper/sched-" + qemu.pidText() + ".state"),
                  "pid " + qemu.pidText().toLatin1() + " start " + start + " niced 0 kept -\n");
        /* the older helper, alive: its hold */
        QFile hold(path("/run/vitrine-helper/sched-" + qemu.pidText() + ".hold"));
        QVERIFY(hold.open(QIODevice::ReadWrite));
        QVERIFY(flock(hold.handle(), LOCK_SH) == 0);
        Helper b(m_root);
        QVERIFY(b.ready());
        b.answer("watch " + qemu.pidText());
        QVERIFY(b.answer("fair-server on").startsWith("ok fair-server on"));
        QCOMPARE(b.answer("behind " + qemu.pidText()),
                 QString("ok behind %1: 0 threads ordinary, 2 vCPUs at nice -5").arg(qemu.pid()));
        for (const QString &tid : tids(qemu.pid())) {
            QVERIFY(schedOf(tid).startsWith("1 1 "));
        }
        b.ask("release");
        QVERIFY(b.finished());
        hold.close();
    }

    /* Behind from the start (a VM that starts while another is in front) */
    void behindFromTheStart()
    {
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QVERIFY(h.answer("fair-server on").startsWith("ok fair-server on"));
        QCOMPARE(h.answer("behind " + qemu.pidText()),
                 QString("ok behind %1: 0 threads ordinary, 2 vCPUs at nice -5").arg(qemu.pid()));
        qemu.stop();
        QVERIFY(h.finished());
        QVERIFY(journal().filter("sched ").isEmpty());
        QCOMPARE(journal().filter(QRegularExpression("^nice \\d+ -5$")).size(), 2);
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    /* Restore only what still holds the value written */
    void restoreOnlyIfStillOurs()
    {
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QVERIFY(h.answer("fair-server on").startsWith("ok fair-server on: 4 cpus"));
        QVERIFY(h.answer("gpu-floor card1 auto").startsWith("ok gpu-floor card1 1800"));
        /* someone else changes cpu2 and the GPU's level meanwhile */
        setFair(2, 20000000, 2000000);
        writeFile(path(QString(kCard) + "/power_dpm_force_performance_level"), "high\n");
        qemu.stop();
        QVERIFY(h.finished());
        const QStringList all = h.all();
        QVERIFY(all.contains("left fair-server: 1 cpus changed by someone else since"));
        QVERIFY(all.contains("left gpu-floor card1: changed by someone else since"));
        QCOMPARE(fair(0), QString("1000000000/50000000"));
        QCOMPARE(fair(2), QString("20000000/2000000"));
        QCOMPARE(fair(3), QString("1000000000/50000000"));
        QCOMPARE(level(), QString("high"));
        QVERIFY(!stateExists("fair-server"));
        QVERIFY(!stateExists("gpu-floor-card1"));
    }

    /* Values already at the target (vm-host-session's): nothing written, nothing restored */
    void alreadySetElsewhere()
    {
        for (int i = 0; i < m_cpus; i++) {
            setFair(i, 10000000, 1000000);
        }
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QCOMPARE(h.answer("fair-server on"), QString("ok fair-server on: already 10 ms / 1 ms"));
        QVERIFY(!stateExists("fair-server"));
        qemu.stop();
        QVERIFY(h.finished());
        QVERIFY(writes().isEmpty());
        QVERIFY(fairAll("10000000/1000000"));
    }

    /* Two helpers (vitrine restarted, another user): the last one out restores */
    void sharedHolds()
    {
        FakeQemu q1, q2;
        Helper a(m_root), b(m_root);
        QVERIFY(a.ready());
        QVERIFY(b.ready());
        a.answer("watch " + q1.pidText());
        b.answer("watch " + q2.pidText());
        QVERIFY(a.answer("fair-server on").startsWith("ok fair-server on: 4 cpus"));
        QVERIFY(a.answer("gpu-floor card1 auto").startsWith("ok gpu-floor card1 1800"));
        clearJournal();
        QCOMPARE(b.answer("fair-server on"), QString("ok fair-server on: set by another vitrine session"));
        QCOMPARE(b.answer("gpu-floor card1 auto"),
                 QString("ok gpu-floor card1: set by another vitrine session"));
        QVERIFY(writes().isEmpty());
        /* the first one's VM ends: b still holds them */
        q1.stop();
        QVERIFY(a.finished());
        QVERIFY(!a.all().join('\n').contains("restored"));
        QVERIFY(fairAll("10000000/1000000"));
        QCOMPARE(level(), QString("manual"));
        /* the last one restores */
        q2.stop();
        QVERIFY(b.finished());
        QVERIFY(b.all().contains("restored fair-server: 4 cpus back to 1000 ms / 50 ms"));
        QVERIFY(fairAll("1000000000/50000000"));
        QCOMPARE(level(), QString("auto"));
        QCOMPARE(odTable(), od(800, 2700));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    /* A helper killed holding the settings: the next one restores them */
    void crashRecovery()
    {
        FakeQemu qemu;
        {
            Helper a(m_root);
            QVERIFY(a.ready());
            a.answer("watch " + qemu.pidText());
            QVERIFY(a.answer("fair-server on").startsWith("ok"));
            QVERIFY(a.answer("gpu-floor card1 auto").startsWith("ok"));
            a.kill();
        }
        QVERIFY(fairAll("10000000/1000000"));
        QVERIFY(stateExists("fair-server"));
        Helper b(m_root);
        const QString first = b.line(), second = b.line();
        QVERIFY(QStringList({first, second}).contains("restored fair-server: 4 cpus back to 1000 ms / 50 ms"));
        QVERIFY(QStringList({first, second}).contains("restored gpu-floor card1: level auto"));
        QVERIFY(b.ready());
        QVERIFY(fairAll("1000000000/50000000"));
        QCOMPARE(level(), QString("auto"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        b.closeInput();
        QVERIFY(b.finished());
    }

    /*
     * A helper killed between the two writes of a CPU's change (runtime
     * and period), setting it or putting it back: the next one finishes
     * putting it back - it is its own half-way pair, not another tool's
     */
    void fairServerHalfWay_data()
    {
        QTest::addColumn<int>("killAt");
        QTest::addColumn<bool>("restoring");
        /* setting: cpu0's runtime, then its period... (8 writes) */
        QTest::newRow("setting, after cpu0's runtime") << 1 << false;
        QTest::newRow("setting, after cpu2's runtime") << 5 << false;
        /* putting back: cpu0's period, then its runtime... */
        QTest::newRow("restoring, after cpu0's period") << 9 << true;
        QTest::newRow("restoring, after cpu3's period") << 15 << true;
    }
    void fairServerHalfWay()
    {
        QFETCH(int, killAt);
        QFETCH(bool, restoring);
        FakeQemu qemu;
        {
            Helper a(m_root, {QString("VITRINE_HELPER_TEST_KILL_AT=%1").arg(killAt)});
            QVERIFY(a.ready());
            a.answer("watch " + qemu.pidText());
            const QString on = a.answer("fair-server on");
            if (restoring) {
                QVERIFY(on.startsWith("ok fair-server on: 4 cpus"));
                qemu.stop();
            }
            QVERIFY(a.finished());
        }
        QVERIFY(journal().contains("killed"));
        QVERIFY(!fairAll("1000000000/50000000"));
        QVERIFY(stateExists("fair-server"));
        Helper b(m_root);
        QCOMPARE(b.line(), QString("restored fair-server: 4 cpus back to 1000 ms / 50 ms"));
        QVERIFY(b.ready());
        b.closeInput();
        QVERIFY(b.finished());
        QVERIFY(!b.all().join('\n').contains("left fair-server"));
        QVERIFY(fairAll("1000000000/50000000"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    /*
     * A helper killed in its own sequence of a GPU floor - manual, s 0,
     * s 1, c; back: s 0, s 1, c, the level - leaves the level at manual:
     * the next one puts it back, and a floor can be set again
     */
    void gpuFloorHalfWay_data()
    {
        QTest::addColumn<int>("killAt");
        QTest::addColumn<bool>("restoring");
        QTest::newRow("setting, after manual") << 1 << false;
        QTest::newRow("setting, after s 0") << 2 << false;
        QTest::newRow("setting, after s 1") << 3 << false;
        QTest::newRow("setting, after c") << 4 << false;
        QTest::newRow("restoring, after s 0") << 5 << true;
        QTest::newRow("restoring, after s 1") << 6 << true;
        QTest::newRow("restoring, after c") << 7 << true;
    }
    void gpuFloorHalfWay()
    {
        QFETCH(int, killAt);
        QFETCH(bool, restoring);
        {
            FakeQemu qemu;
            Helper a(m_root, {QString("VITRINE_HELPER_TEST_KILL_AT=%1").arg(killAt)});
            QVERIFY(a.ready());
            a.answer("watch " + qemu.pidText());
            const QString floor = a.answer("gpu-floor card1 auto");
            if (restoring) {
                QVERIFY(floor.startsWith("ok gpu-floor card1 1800 MHz"));
                qemu.stop();
            }
            QVERIFY(a.finished());
        }
        QVERIFY(journal().contains("killed"));
        QCOMPARE(level(), QString("manual"));
        QVERIFY(stateExists("gpu-floor-card1"));

        FakeQemu qemu;
        Helper b(m_root);
        QCOMPARE(b.line(), QString("restored gpu-floor card1: level auto"));
        QVERIFY(b.ready());
        QCOMPARE(level(), QString("auto"));
        QCOMPARE(odTable(), od(800, 2700));
        /* nothing left in the way */
        b.answer("watch " + qemu.pidText());
        QCOMPARE(b.answer("gpu-floor card1 auto"),
                 QString("ok gpu-floor card1 1800 MHz (was 800 MHz, level auto)"));
        qemu.stop();
        QVERIFY(b.finished());
        QCOMPARE(level(), QString("auto"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    /* A user's own table, under level auto: put back as it was, not reset */
    void gpuFloorKeepsUserTable()
    {
        writeFile(path(QString(kCard) + "/pp_od_clk_voltage"), od(900, 2400));
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QCOMPARE(h.answer("gpu-floor card1 auto"),
                 QString("ok gpu-floor card1 1800 MHz (was 900 MHz, level auto)"));
        h.ask("release");
        QVERIFY(h.finished());
        QVERIFY(writes().contains(QString(kCard) + "/pp_od_clk_voltage s 1 2400"));
        QVERIFY(!writes().contains(QString(kCard) + "/pp_od_clk_voltage r"));
        QCOMPARE(level(), QString("auto"));
    }

    /* What is written is read back: a floor the driver does not keep is undone */
    void gpuFloorReadBack()
    {
        /* commits taken, not kept (the fake's <file>.stuck) */
        writeFile(path(QString(kCard) + "/pp_od_clk_voltage.stuck"), "");
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QCOMPARE(h.answer("gpu-floor card1 auto"),
                 QString("error gpu-floor card1: the floor did not take (the lowest clock reads "
                         "800 MHz)"));
        QCOMPARE(level(), QString("auto"));
        QVERIFY(!stateExists("gpu-floor-card1"));
    }

    /* A udmabuf limit the kernel does not keep: undone, said */
    void udmabufReadBack()
    {
        writeFile(path(QString(kUdmabuf) + "/size_limit_mb.stuck"), "");
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QCOMPARE(h.answer("udmabuf " + qemu.pidText()),
                 QString("error udmabuf %1: a limit did not take").arg(qemu.pid()));
        QCOMPARE(udmabuf(), QString("1024/64"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    /* The state an older helper left (no "was"): the floor as written is still its own */
    void gpuFloorOlderState()
    {
        const QString card = path(kCard);
        writeFile(card + "/power_dpm_force_performance_level", "manual\n");
        writeFile(card + "/pp_od_clk_voltage", od(1800, 2700));
        QVERIFY(QDir().mkpath(path("/run/vitrine-helper")));
        QFile::setPermissions(path("/run/vitrine-helper"),
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        writeFile(path("/run/vitrine-helper/gpu-floor-card1.state"), "level auto\nmin 1800\nmax 2700\n");
        Helper b(m_root);
        QCOMPARE(b.line(), QString("restored gpu-floor card1: level auto"));
        QVERIFY(b.ready());
        QCOMPARE(level(), QString("auto"));
        QCOMPARE(odTable(), od(800, 2700));
        b.closeInput();
        QVERIFY(b.finished());
    }

    /* Raised while the QEMU that asked runs, back after it */
    void udmabufRaiseAndRestore()
    {
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QCOMPARE(h.answer("udmabuf " + qemu.pidText()),
                 QString("ok udmabuf %1: list_limit 65536 (was 1024), size_limit_mb 2048 (was 64)")
                     .arg(qemu.pid()));
        QCOMPARE(udmabuf(), QString("65536/2048"));
        QVERIFY(stateExists("udmabuf"));
        QCOMPARE(writes(), QStringList({QString(kUdmabuf) + "/list_limit 65536",
                                        QString(kUdmabuf) + "/size_limit_mb 2048"}));
        QVERIFY(journal().join('\n').contains("log uid "));
        QCOMPARE(h.answer("udmabuf " + qemu.pidText()),
                 QString("ok udmabuf %1: already").arg(qemu.pid()));

        clearJournal();
        qemu.stop();
        QCOMPARE(h.waitFor("restored"), QString("restored udmabuf: list_limit 1024, size_limit_mb 64"));
        QCOMPARE(h.waitFor("bye"), QString("bye"));
        QVERIFY(h.finished());
        QCOMPARE(udmabuf(), QString("1024/64"));
        QCOMPARE(writes(), QStringList({QString(kUdmabuf) + "/list_limit 1024",
                                        QString(kUdmabuf) + "/size_limit_mb 64"}));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        QVERIFY(journal().join('\n').contains("log udmabuf: back to list_limit 1024, size_limit_mb 64"));
    }

    /* Raised only: a higher value (the kernel's command line) stays as it is */
    void udmabufKeepsHigher()
    {
        setUdmabuf(131072, 64);
        {
            FakeQemu qemu;
            Helper h(m_root);
            QVERIFY(h.ready());
            h.answer("watch " + qemu.pidText());
            QCOMPARE(h.answer("udmabuf " + qemu.pidText()),
                     QString("ok udmabuf %1: list_limit 131072 already, size_limit_mb 2048 (was 64)")
                         .arg(qemu.pid()));
            QCOMPARE(writes(), QStringList({QString(kUdmabuf) + "/size_limit_mb 2048"}));
            qemu.stop();
            QVERIFY(h.finished());
            QVERIFY(h.all().contains("restored udmabuf: size_limit_mb 64"));
            QCOMPARE(udmabuf(), QString("131072/64"));
        }
        /* both higher already (this laptop's boot): nothing written, nothing kept */
        setUdmabuf(65536, 4096);
        clearJournal();
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QCOMPARE(h.answer("udmabuf " + qemu.pidText()),
                 QString("ok udmabuf %1: list_limit 65536 already, size_limit_mb 4096 already")
                     .arg(qemu.pid()));
        QVERIFY(!stateExists("udmabuf"));
        qemu.stop();
        QVERIFY(h.finished());
        QVERIFY(!h.all().join('\n').contains("udmabuf: "));
        QVERIFY(writes().isEmpty());
        QCOMPARE(udmabuf(), QString("65536/4096"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    /* Put back only where it still holds what the helper wrote */
    void udmabufRestoreOnlyIfOurs()
    {
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QVERIFY(h.answer("udmabuf " + qemu.pidText()).startsWith("ok udmabuf"));
        /* an administrator's change meanwhile */
        writeFile(path(QString(kUdmabuf) + "/list_limit"), "32768\n");
        qemu.stop();
        QVERIFY(h.finished());
        const QStringList all = h.all();
        QVERIFY(all.contains("restored udmabuf: size_limit_mb 64"));
        QVERIFY(all.contains("left udmabuf: list_limit changed by someone else since"));
        QCOMPARE(udmabuf(), QString("32768/64"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    /* Killed holding them, or between two writes: the next helper puts them back */
    void udmabufCrashRecovery_data()
    {
        QTest::addColumn<int>("killAt");
        QTest::addColumn<bool>("restoring");
        QTest::newRow("held") << 0 << false;
        QTest::newRow("setting, after list_limit") << 1 << false;
        QTest::newRow("restoring, after list_limit") << 3 << true;
    }
    void udmabufCrashRecovery()
    {
        QFETCH(int, killAt);
        QFETCH(bool, restoring);
        {
            FakeQemu qemu;
            Helper a(m_root, killAt ? QStringList{QString("VITRINE_HELPER_TEST_KILL_AT=%1").arg(killAt)}
                                    : QStringList());
            QVERIFY(a.ready());
            a.answer("watch " + qemu.pidText());
            const QString on = a.answer("udmabuf " + qemu.pidText());
            if (!killAt) {
                QVERIFY(on.startsWith("ok udmabuf"));
                a.kill();
            } else if (restoring) {
                QVERIFY(on.startsWith("ok udmabuf"));
                qemu.stop();
            }
            QVERIFY(a.finished());
        }
        QVERIFY(udmabuf() != "1024/64");
        QVERIFY(stateExists("udmabuf"));
        Helper b(m_root);
        QCOMPARE(b.line(), QString("restored udmabuf: list_limit 1024, size_limit_mb 64"));
        QVERIFY(b.ready());
        QCOMPARE(udmabuf(), QString("1024/64"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        b.closeInput();
        QVERIFY(b.finished());
        QVERIFY(!b.all().join('\n').contains("left udmabuf"));
    }

    /* Two helpers (vitrine restarted, another user): the last one out restores */
    void udmabufTwoHelpers()
    {
        FakeQemu q1, q2;
        Helper a(m_root), b(m_root);
        QVERIFY(a.ready());
        QVERIFY(b.ready());
        a.answer("watch " + q1.pidText());
        b.answer("watch " + q2.pidText());
        QVERIFY(a.answer("udmabuf " + q1.pidText()).startsWith("ok udmabuf"));
        clearJournal();
        QCOMPARE(b.answer("udmabuf " + q2.pidText()),
                 QString("ok udmabuf %1: set by another vitrine session").arg(q2.pid()));
        QVERIFY(writes().isEmpty());
        q1.stop();
        QVERIFY(a.finished());
        QVERIFY(!a.all().join('\n').contains("restored"));
        QCOMPARE(udmabuf(), QString("65536/2048"));
        q2.stop();
        QVERIFY(b.finished());
        QVERIFY(b.all().contains("restored udmabuf: list_limit 1024, size_limit_mb 64"));
        QCOMPARE(udmabuf(), QString("1024/64"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    /* Held for the QEMUs that asked: the last of those lets go, whatever
       else the helper still watches */
    void udmabufPerQemu()
    {
        FakeQemu q1, q2, q3;
        Helper h(m_root);
        QVERIFY(h.ready());
        for (const FakeQemu *q : {&q1, &q2, &q3}) {
            h.answer("watch " + q->pidText());
        }
        QVERIFY(h.answer("fair-server on").startsWith("ok fair-server on"));
        QVERIFY(h.answer("udmabuf " + q1.pidText()).startsWith("ok udmabuf"));
        QCOMPARE(h.answer("udmabuf " + q2.pidText()), QString("ok udmabuf %1: already").arg(q2.pid()));
        const QString first = q1.pidText();
        q1.stop();
        QCOMPARE(h.waitFor("exited"), "exited " + first);
        QTest::qWait(200);
        QCOMPARE(udmabuf(), QString("65536/2048"));
        q2.stop();
        QCOMPARE(h.waitFor("restored"), QString("restored udmabuf: list_limit 1024, size_limit_mb 64"));
        QCOMPARE(udmabuf(), QString("1024/64"));
        /* q3 runs on, untouched by it: the fair server still held */
        QVERIFY(h.running());
        QVERIFY(fairAll("10000000/1000000"));
        QVERIFY(!stateExists("udmabuf"));
        /* asked again: raised again */
        QVERIFY(h.answer("udmabuf " + q3.pidText()).startsWith("ok udmabuf"));
        QCOMPARE(udmabuf(), QString("65536/2048"));
        h.ask("release");
        QVERIFY(h.finished());
        QCOMPARE(udmabuf(), QString("1024/64"));
        QVERIFY(fairAll("1000000000/50000000"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    void udmabufSkips()
    {
        FakeQemu qemu, other;
        Helper h(m_root);
        QVERIFY(h.ready());
        QCOMPARE(h.answer("udmabuf " + qemu.pidText()), QString("error udmabuf: watch a QEMU first"));
        h.answer("watch " + qemu.pidText());
        QCOMPARE(h.answer("udmabuf " + other.pidText()), QString("error udmabuf: watch the process first"));
        QCOMPARE(h.answer("udmabuf on"), QString("error udmabuf: watch the process first"));
        /* a value that is not one: nothing written, nothing held */
        writeFile(path(QString(kUdmabuf) + "/list_limit"), "-1\n");
        QCOMPARE(h.answer("udmabuf " + qemu.pidText()),
                 QString("error udmabuf %1: cannot read its limits").arg(qemu.pid()));
        QVERIFY(!stateExists("udmabuf"));
        /* a module not loaded: no parameters */
        QDir(path("/sys/module/udmabuf")).removeRecursively();
        QCOMPARE(h.answer("udmabuf " + qemu.pidText()),
                 QString("skip udmabuf %1: the udmabuf module is not loaded").arg(qemu.pid()));
        QVERIFY(writes().isEmpty());
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    /* The second write refused: the first one undone, nothing held */
    void udmabufHalfWay()
    {
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QFile::setPermissions(path(QString(kUdmabuf) + "/size_limit_mb"), QFileDevice::ReadOwner);
        QCOMPARE(h.answer("udmabuf " + qemu.pidText()),
                 QString("error udmabuf %1: Permission denied").arg(qemu.pid()));
        QCOMPARE(writes(), QStringList({QString(kUdmabuf) + "/list_limit 65536",
                                        QString(kUdmabuf) + "/size_limit_mb 2048 FAILED",
                                        QString(kUdmabuf) + "/list_limit 1024"}));
        QCOMPARE(udmabuf(), QString("1024/64"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        qemu.stop();
        QVERIFY(h.finished());
        QVERIFY(!h.all().join('\n').contains("restored"));
    }

    /* A state left by a crash, the module's parameters gone since: dropped */
    void udmabufStaleStateWithoutModule()
    {
        {
            FakeQemu qemu;
            Helper a(m_root);
            QVERIFY(a.ready());
            a.answer("watch " + qemu.pidText());
            QVERIFY(a.answer("udmabuf " + qemu.pidText()).startsWith("ok udmabuf"));
            a.kill();
            QVERIFY(a.finished());
        }
        QVERIFY(stateExists("udmabuf"));
        QDir(path("/sys/module/udmabuf")).removeRecursively();
        Helper b(m_root);
        QCOMPARE(b.line(), QString("restored udmabuf: nothing (some failed)"));
        QVERIFY(b.ready());
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        b.closeInput();
        QVERIFY(b.finished());
    }

    /* Every hold taken (the fair server, udmabuf, eight cards): the next says why */
    /* Huge pages for shared memory: advise from never while a VM runs, never after */
    void thpAdviseAndRestore()
    {
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        /* nothing applied while no VM runs; letting go is always allowed */
        QCOMPARE(h.answer("thp on"), QString("error thp: watch a QEMU first"));
        QCOMPARE(h.answer("thp off"), QString("ok thp off"));
        h.answer("watch " + qemu.pidText());
        QCOMPARE(h.answer("thp on"), QString("ok thp on: advise (was never)"));
        QCOMPARE(thp(), QString("advise"));
        QVERIFY(stateExists("thp"));
        QCOMPARE(writes(), QStringList({QString(kThp) + " advise"}));
        QVERIFY(journal().join('\n').contains("log uid "));
        QCOMPARE(h.answer("thp on"), QString("ok thp on: already"));
        QCOMPARE(h.answer("thp maybe"), QString("error thp: unknown request"));

        clearJournal();
        qemu.stop();
        QCOMPARE(h.waitFor("restored"), QString("restored thp: shmem huge pages back to never"));
        QCOMPARE(h.waitFor("bye"), QString("bye"));
        QVERIFY(h.finished());
        QCOMPARE(thp(), QString("never"));
        QCOMPARE(writes(), QStringList({QString(kThp) + " never"}));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        QVERIFY(journal().join('\n').contains("log shmem huge pages back to never"));
    }

    /* thp off puts it back at once, the VM still running; and so does release */
    void thpOff()
    {
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QVERIFY(h.answer("thp on").startsWith("ok thp on: advise"));
        QCOMPARE(h.ask("thp off"),
                 QStringList({"restored thp: shmem huge pages back to never", "ok thp off"}));
        QCOMPARE(thp(), QString("never"));
        QVERIFY(!stateExists("thp"));
        QVERIFY(h.answer("thp on").startsWith("ok thp on: advise"));
        h.ask("release");
        QVERIFY(h.finished());
        QVERIFY(h.all().contains("restored thp: shmem huge pages back to never"));
        QCOMPARE(thp(), QString("never"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    /* From never only: any other choice is the host owner's, and stays */
    void thpOtherChoices_data()
    {
        QTest::addColumn<QString>("choice");
        QTest::addColumn<QString>("answer");
        QTest::newRow("advise") << "advise" << "ok thp on: already advise, left as it is";
        QTest::newRow("within_size") << "within_size"
                                     << "ok thp on: already within_size, left as it is";
        QTest::newRow("always") << "always" << "ok thp on: already always, left as it is";
        QTest::newRow("force") << "force" << "ok thp on: already force, left as it is";
        QTest::newRow("deny") << "deny"
                              << "skip thp: denied on this host (shmem_enabled is deny), left as "
                                 "it is";
        QTest::newRow("no such file") << ""
                                      << "skip thp: this kernel has no transparent huge pages";
    }
    void thpOtherChoices()
    {
        QFETCH(QString, choice);
        QFETCH(QString, answer);
        if (choice.isEmpty()) {
            QFile::remove(path(kThp));
        } else {
            setThp(choice);
        }
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QCOMPARE(h.answer("thp on"), answer);
        QVERIFY(!stateExists("thp"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        qemu.stop();
        QVERIFY(h.finished());
        const QString all = h.all().join('\n');
        QVERIFY(!all.contains("restored thp") && !all.contains("left thp"));
        QVERIFY(writes().isEmpty());
        QCOMPARE(thp(), choice);
    }

    /* Put back only where it still reads what the helper wrote */
    void thpRestoreOnlyIfOurs()
    {
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QVERIFY(h.answer("thp on").startsWith("ok thp on: advise"));
        /* an administrator's choice meanwhile */
        setThp("within_size");
        qemu.stop();
        QVERIFY(h.finished());
        QVERIFY(h.all().contains("left thp: changed by someone else since"));
        QCOMPARE(thp(), QString("within_size"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        QVERIFY(journal().join('\n').contains("log shmem huge pages left at within_size"));
    }

    /* Taken and not kept: back to never, nothing held or recorded */
    void thpReadBack()
    {
        writeFile(path(QString(kThp) + ".stuck"), "");
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QCOMPARE(h.answer("thp on"), QString("error thp: the setting did not take"));
        QCOMPARE(thp(), QString("never"));
        QVERIFY(!stateExists("thp"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        qemu.stop();
        QVERIFY(h.finished());
    }

    /* Killed holding it, setting it or putting it back: the next helper puts it back */
    void thpCrashRecovery_data()
    {
        QTest::addColumn<int>("killAt");
        QTest::addColumn<bool>("restoring");
        QTest::addColumn<QString>("restored");
        QTest::newRow("held") << 0 << false << "restored thp: shmem huge pages back to never";
        QTest::newRow("setting, after the write")
            << 1 << false << "restored thp: shmem huge pages back to never";
        QTest::newRow("restoring, after the write")
            << 2 << true << "restored thp: shmem huge pages never already";
    }
    void thpCrashRecovery()
    {
        QFETCH(int, killAt);
        QFETCH(bool, restoring);
        QFETCH(QString, restored);
        {
            FakeQemu qemu;
            Helper a(m_root, killAt ? QStringList{QString("VITRINE_HELPER_TEST_KILL_AT=%1").arg(killAt)}
                                    : QStringList());
            QVERIFY(a.ready());
            a.answer("watch " + qemu.pidText());
            const QString on = a.answer("thp on");
            if (!killAt) {
                QVERIFY(on.startsWith("ok thp on: advise"));
                a.kill();
            } else if (restoring) {
                QVERIFY(on.startsWith("ok thp on: advise"));
                qemu.stop();
            }
            QVERIFY(a.finished());
        }
        QVERIFY(stateExists("thp"));
        Helper b(m_root);
        QCOMPARE(b.line(), restored);
        QVERIFY(b.ready());
        QCOMPARE(thp(), QString("never"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        b.closeInput();
        QVERIFY(b.finished());
        QVERIFY(!b.all().join('\n').contains("left thp"));
    }

    /* Two helpers (vitrine restarted, another user): the second one joins
       the first's hold, and the last one out restores */
    void thpTwoHelpers()
    {
        FakeQemu q1, q2;
        Helper a(m_root), b(m_root);
        QVERIFY(a.ready());
        QVERIFY(b.ready());
        a.answer("watch " + q1.pidText());
        b.answer("watch " + q2.pidText());
        QVERIFY(a.answer("thp on").startsWith("ok thp on: advise"));
        clearJournal();
        QCOMPARE(b.answer("thp on"), QString("ok thp on: set by another vitrine session"));
        QVERIFY(writes().isEmpty());
        q1.stop();
        QVERIFY(a.finished());
        QVERIFY(!a.all().join('\n').contains("restored"));
        QCOMPARE(thp(), QString("advise"));
        q2.stop();
        QVERIFY(b.finished());
        QVERIFY(b.all().contains("restored thp: shmem huge pages back to never"));
        QCOMPARE(thp(), QString("never"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    /* The CPU latency request: held while VMs run by the helper's open
       file, nothing recorded; let go by off and by the last VM's end */
    void cpuLatencyHeldAndLetGo()
    {
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        /* nothing applied while no VM runs; letting go is always allowed */
        QCOMPARE(h.answer("cpu-latency on"), QString("error cpu-latency: watch a QEMU first"));
        QCOMPARE(h.answer("cpu-latency off"), QString("ok cpu-latency off"));
        h.answer("watch " + qemu.pidText());
        QCOMPARE(h.answer("cpu-latency on"),
                 QString("ok cpu-latency on: 1 us (C1 on every cpu; C2, C3 unused)"));
        QCOMPARE(latencyRequests(), QList<int>({1}));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        QVERIFY(journal().contains("request /dev/cpu_dma_latency 1"));
        QVERIFY(journal().join('\n').contains("log uid "));
        QCOMPARE(h.answer("cpu-latency on"), QString("ok cpu-latency on: already 1 us"));
        QCOMPARE(h.answer("cpu-latency maybe"), QString("error cpu-latency: unknown request"));
        QCOMPARE(h.ask("cpu-latency off"),
                 QStringList({"restored cpu-latency: request of 1 us ended", "ok cpu-latency off"}));
        QVERIFY(latencyRequests().isEmpty());
        QCOMPARE(h.answer("cpu-latency off"), QString("ok cpu-latency off"));

        QVERIFY(h.answer("cpu-latency on").startsWith("ok cpu-latency on: 1 us"));
        QCOMPARE(latency(), 1);
        qemu.stop();
        QCOMPARE(h.waitFor("restored"), QString("restored cpu-latency: request of 1 us ended"));
        QCOMPARE(h.waitFor("bye"), QString("bye"));
        QVERIFY(h.finished());
        QCOMPARE(latency(), kNoLatency);
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        QVERIFY(journal().join('\n').contains("log cpu latency request of 1 us ended"));
    }

    /*
     * The value: the wake-up time of the CPUs' first usable idle state, the
     * slowest CPU's, so that each one may rest there - never below (a CPU
     * would only poll, as with a fixed 1 us on Intel's C1 of 2 us) and never 0
     */
    void cpuLatencyValue_data()
    {
        QTest::addColumn<QStringList>("states");
        QTest::addColumn<QStringList>("cpu3");      // when not the same
        QTest::addColumn<QString>("answer");
        const QStringList amd{"POLL 0", "C1 1", "C2 18", "C3 350"};
        QTest::newRow("AMD: C1, 1 us")
            << amd << QStringList() << "ok cpu-latency on: 1 us (C1 on every cpu; C2, C3 unused)";
        /* intel_idle's Haswell to Comet Lake tables */
        QTest::newRow("Intel: C1, 2 us")
            << QStringList{"POLL 0", "C1 2", "C1E 10", "C3 33", "C6 133"} << QStringList()
            << "ok cpu-latency on: 2 us (C1 on every cpu; C1E, C3, C6 unused)";
        QTest::newRow("C1 disabled")
            << QStringList{"POLL 0", "C1 1 disabled", "C2 18", "C3 350"} << QStringList()
            << "ok cpu-latency on: 18 us (C2 on every cpu; C3 unused)";
        QTest::newRow("a first state of 0 us")
            << QStringList{"POLL 0", "C1 0", "C2 18"} << QStringList()
            << "ok cpu-latency on: 1 us (C1 on every cpu; C2 unused)";
        QTest::newRow("nothing deeper")
            << QStringList{"POLL 0", "C1 1"} << QStringList() << "ok cpu-latency on: 1 us (C1 on every cpu)";
        QTest::newRow("one cpu slower")
            << amd << QStringList{"POLL 0", "C1E 2", "C6 133"}
            << "ok cpu-latency on: 2 us (C1E of cpu3, the slowest first idle state; C6 unused)";
        QTest::newRow("one cpu without")
            << amd << QStringList{"POLL 0"}
            << "ok cpu-latency on: 1 us (C1 on 3 cpus, none on 1; C2, C3 unused)";
    }
    void cpuLatencyValue()
    {
        QFETCH(QStringList, states);
        QFETCH(QStringList, cpu3);
        QFETCH(QString, answer);
        for (int i = 0; i < m_cpus; i++) {
            setIdleStates(i, i == 3 && !cpu3.isEmpty() ? cpu3 : states);
        }
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QCOMPARE(h.answer("cpu-latency on"), answer);
        const int us = QRegularExpression("on: (\\d+) us").match(answer).captured(1).toInt();
        QCOMPARE(latencyRequests(), QList<int>({us}));
        h.ask("release");
        QVERIFY(h.finished());
        QVERIFY(h.all().contains(QString("restored cpu-latency: request of %1 us ended").arg(us)));
        QVERIFY(latencyRequests().isEmpty());
    }

    /* What it cannot do here: skipped, or refused, and nothing held */
    void cpuLatencySkips_data()
    {
        QTest::addColumn<QString>("what");
        QTest::addColumn<QString>("answer");
        QTest::newRow("no device") << "no device"
                                   << "skip cpu-latency: this kernel has no /dev/cpu_dma_latency";
        QTest::newRow("only polling")
            << "only polling"
            << "skip cpu-latency: the cpus have no idle state but polling (cpuidle driver acpi_idle)";
        QTest::newRow("no cpuidle") << "no cpuidle"
                                    << "skip cpu-latency: the cpus have no idle states (no cpuidle driver)";
        QTest::newRow("refused") << "refused"
                                 << "error cpu-latency: /dev/cpu_dma_latency: Permission denied";
    }
    void cpuLatencySkips()
    {
        QFETCH(QString, what);
        QFETCH(QString, answer);
        if (what == "no device") {
            QFile::remove(path(kLatency));
        } else if (what == "only polling") {
            for (int i = 0; i < m_cpus; i++) {
                setIdleStates(i, {"POLL 0"});
            }
        } else if (what == "no cpuidle") {
            for (int i = 0; i < m_cpus; i++) {
                QDir(path(QString("%1/cpu%2/cpuidle").arg(kCpu).arg(i))).removeRecursively();
            }
            writeFile(path(QString(kCpu) + "/cpuidle/current_driver"), "none\n");
        } else {
            writeFile(path(kLatency) + ".refuse", "");
        }
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QCOMPARE(h.answer("cpu-latency on"), answer);
        QVERIFY(latencyRequests().isEmpty());
        QCOMPARE(h.ask("cpu-latency off"), QStringList({"ok cpu-latency off"}));
        qemu.stop();
        QVERIFY(h.finished());
        QVERIFY(!h.all().join('\n').contains("restored cpu-latency"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    /* Two helpers (vitrine restarted, another user): a request each, the
       kernel applies the lowest; the one ending first leaves the other's */
    void cpuLatencyTwoHelpers()
    {
        FakeQemu q1, q2;
        Helper a(m_root), b(m_root);
        QVERIFY(a.ready());
        QVERIFY(b.ready());
        a.answer("watch " + q1.pidText());
        b.answer("watch " + q2.pidText());
        QVERIFY(a.answer("cpu-latency on").startsWith("ok cpu-latency on: 1 us"));
        QVERIFY(b.answer("cpu-latency on").startsWith("ok cpu-latency on: 1 us"));
        QCOMPARE(latencyRequests(), QList<int>({1, 1}));
        q1.stop();
        QVERIFY(a.finished());
        QVERIFY(a.all().contains("restored cpu-latency: request of 1 us ended"));
        QCOMPARE(latencyRequests(), QList<int>({1}));
        q2.stop();
        QVERIFY(b.finished());
        QCOMPARE(latency(), kNoLatency);
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    /* A helper killed holding it: the request goes with it (the kernel's
       doing), and the next helper finds nothing to put back */
    void cpuLatencyCrash()
    {
        FakeQemu qemu;
        {
            Helper a(m_root);
            QVERIFY(a.ready());
            a.answer("watch " + qemu.pidText());
            QVERIFY(a.answer("cpu-latency on").startsWith("ok cpu-latency on: 1 us"));
            QCOMPARE(latencyRequests(), QList<int>({1}));
            a.kill();
        }
        QCOMPARE(latency(), kNoLatency);
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        Helper b(m_root);
        QCOMPARE(b.line(), QString("ready 1"));
        b.closeInput();
        QVERIFY(b.finished());
    }

    /* Another program's request (tuned's latency-performance, the research
       side's vm-host-session) is its own: the kernel applies the lowest, and
       it stays after the helper */
    void cpuLatencyOthersLeftAlone()
    {
        const QString theirs = path(kLatency) + ".1";
        writeFile(theirs, "20\n");
        const int fd = ::open(QFile::encodeName(theirs).constData(), O_RDONLY | O_CLOEXEC);
        QVERIFY(fd >= 0);
        auto close = qScopeGuard([fd]() { ::close(fd); });
        QCOMPARE(flock(fd, LOCK_EX), 0);
        QCOMPARE(latency(), 20);
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QVERIFY(h.answer("cpu-latency on").startsWith("ok cpu-latency on: 1 us"));
        QCOMPARE(latencyRequests(), QList<int>({1, 20}));
        QCOMPARE(latency(), 1);
        qemu.stop();
        QVERIFY(h.finished());
        QCOMPARE(latencyRequests(), QList<int>({20}));
    }

    /* Each CPU's preference at performance while VMs run, recorded first;
       back to its own value after the last one */
    void eppPerformanceAndRestore()
    {
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        QCOMPARE(h.answer("epp on"), QString("error epp: watch a QEMU first"));
        QCOMPARE(h.answer("epp off"), QString("ok epp off"));
        h.answer("watch " + qemu.pidText());
        QCOMPARE(h.answer("epp on"), QString("ok epp on: performance on 4 cpus (was balance_performance)"));
        QVERIFY(eppAll("performance"));
        QVERIFY(stateExists("epp"));
        QStringList set;
        for (int i = 0; i < m_cpus; i++) {
            set << QString("%1/cpu%2/cpufreq/energy_performance_preference performance").arg(kCpu).arg(i);
        }
        QCOMPARE(writes(), set);
        QVERIFY(journal().join('\n').contains("log uid "));
        QCOMPARE(h.answer("epp on"), QString("ok epp on: already"));
        QCOMPARE(h.answer("epp maybe"), QString("error epp: unknown request"));

        clearJournal();
        qemu.stop();
        QCOMPARE(h.waitFor("restored"), QString("restored epp: 4 cpus back to balance_performance"));
        QCOMPARE(h.waitFor("bye"), QString("bye"));
        QVERIFY(h.finished());
        QVERIFY(eppAll("balance_performance"));
        QCOMPARE(writes(), set.replaceInStrings(" performance", " balance_performance"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        QVERIFY(journal().join('\n').contains(
            "log energy performance preference: 4 cpus back to balance_performance"));
    }

    /* epp off puts it back at once, the VM still running; and so does release */
    void eppOffAndRelease()
    {
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QVERIFY(h.answer("epp on").startsWith("ok epp on: performance on 4 cpus"));
        QCOMPARE(h.ask("epp off"),
                 QStringList({"restored epp: 4 cpus back to balance_performance", "ok epp off"}));
        QVERIFY(eppAll("balance_performance"));
        QVERIFY(!stateExists("epp"));
        QVERIFY(h.answer("epp on").startsWith("ok epp on: performance on 4 cpus"));
        QVERIFY(h.answer("cpu-latency on").startsWith("ok cpu-latency on: 1 us"));
        h.ask("release");
        QVERIFY(h.finished());
        const QStringList all = h.all();
        QVERIFY(all.contains("restored epp: 4 cpus back to balance_performance"));
        QVERIFY(all.contains("restored cpu-latency: request of 1 us ended"));
        QCOMPARE(all.last(), QString("bye"));
        QVERIFY(eppAll("balance_performance"));
        QCOMPARE(latency(), kNoLatency);
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    /* CPUs at performance already: left as they are, and put back by no one */
    void eppAlready()
    {
        FakeQemu qemu;
        {
            for (int i = 0; i < m_cpus; i++) {
                setEpp(i, "performance");
            }
            Helper h(m_root);
            QVERIFY(h.ready());
            h.answer("watch " + qemu.pidText());
            QCOMPARE(h.answer("epp on"), QString("ok epp on: already performance on 4 cpus, left as it is"));
            QVERIFY(writes().isEmpty());
            QCOMPARE(stateFiles(), QStringList({"lock"}));
            h.ask("release");
            QVERIFY(h.finished());
            QVERIFY(!h.all().join('\n').contains("restored epp"));
            QVERIFY(eppAll("performance"));
        }
        /* some of them: those are listed, and stay */
        for (int i = 0; i < m_cpus; i++) {
            setEpp(i, i == 1 ? "performance" : "balance_performance");
        }
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QCOMPARE(h.answer("epp on"), QString("ok epp on: performance on 3 cpus (was "
                                             "balance_performance); 1 at performance already"));
        QVERIFY(readFile(path("/run/vitrine-helper/epp.state")).contains("cpu1 performance -\n"));
        /* changed by someone since: still not the helper's to put back */
        setEpp(1, "power");
        h.ask("release");
        QVERIFY(h.finished());
        const QString all = h.all().join('\n');
        QVERIFY(all.contains("restored epp: 3 cpus back to balance_performance"));
        QVERIFY(!all.contains("left epp"));
        QCOMPARE(epp(1), QString("power"));
        QCOMPARE(epp(2), QString("balance_performance"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    /* Each CPU back to its own value: words, or a raw number (amd-pstate
       and intel_pstate take those, and show them) */
    void eppOwnValues()
    {
        setEpp(0, "power");
        setEpp(2, "128");
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QCOMPARE(h.answer("epp on"),
                 QString("ok epp on: performance on 4 cpus (was power, balance_performance, 128)"));
        QVERIFY(eppAll("performance"));
        qemu.stop();
        QCOMPARE(h.waitFor("restored"),
                 QString("restored epp: 4 cpus back to power, balance_performance, 128"));
        QVERIFY(h.finished());
        QCOMPARE(epp(0), QString("power"));
        QCOMPARE(epp(1), QString("balance_performance"));
        QCOMPARE(epp(2), QString("128"));
        QCOMPARE(epp(3), QString("balance_performance"));
    }

    /* Put back only where it still reads performance: a power profile chosen
       meanwhile (power-profiles-daemon, tuned) keeps its value */
    void eppRestoreOnlyIfOurs()
    {
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QVERIFY(h.answer("epp on").startsWith("ok epp on: performance on 4 cpus"));
        setEpp(2, "power");
        qemu.stop();
        QVERIFY(h.finished());
        const QStringList all = h.all();
        QVERIFY(all.contains("restored epp: 3 cpus back to balance_performance"));
        QVERIFY(all.contains("left epp: 1 cpus changed by someone else since"));
        QCOMPARE(epp(2), QString("power"));
        QCOMPARE(epp(3), QString("balance_performance"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        QVERIFY(journal().join('\n').contains("1 left as changed since"));
    }

    /* No preference here: skipped, saying which cpufreq driver there is */
    void eppSkips_data()
    {
        QTest::addColumn<QString>("driver");
        QTest::addColumn<QString>("answer");
        QTest::newRow("acpi-cpufreq") << "acpi-cpufreq"
                                      << "skip epp: acpi-cpufreq has no energy performance preference "
                                         "(amd-pstate and intel_pstate have one, in active mode)";
        QTest::newRow("no cpufreq") << ""
                                    << "skip epp: no cpufreq driver, so no energy performance preference";
    }
    void eppSkips()
    {
        QFETCH(QString, driver);
        QFETCH(QString, answer);
        for (int i = 0; i < m_cpus; i++) {
            const QString policy = path(QString("%1/cpufreq/policy%2").arg(kCpu).arg(i));
            QFile::remove(policy + "/energy_performance_preference");
            if (driver.isEmpty()) {
                QFile::remove(path(QString("%1/cpu%2/cpufreq").arg(kCpu).arg(i)));
                QDir(policy).removeRecursively();
            } else {
                writeFile(policy + "/scaling_driver", driver.toLatin1() + '\n');
            }
        }
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QCOMPARE(h.answer("epp on"), answer);
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        qemu.stop();
        QVERIFY(h.finished());
        QVERIFY(!h.all().join('\n').contains("restored epp"));
        QVERIFY(writes().isEmpty());
    }

    /* A CPU without one beside others: those are set */
    void eppSomeWithout()
    {
        QFile::remove(eppFile(3));
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QCOMPARE(h.answer("epp on"),
                 QString("ok epp on: performance on 3 cpus (was balance_performance); 1 without one"));
        qemu.stop();
        QCOMPARE(h.waitFor("restored"), QString("restored epp: 3 cpus back to balance_performance"));
        QVERIFY(h.finished());
        QCOMPARE(epp(0), QString("balance_performance"));
        QVERIFY(!QFile::exists(eppFile(3)));
    }

    /* All or nothing: taken and not kept, or refused (busy: a driver that
       sets it itself), those written before go back; nothing held */
    void eppAllOrNothing_data()
    {
        QTest::addColumn<QString>("sibling");
        QTest::addColumn<QString>("answer");
        QTest::newRow("not kept") << ".stuck" << "error epp: cpu2 does not keep what was written";
        QTest::newRow("refused") << ".writes"
                                 << "error epp: cpu2 refuses it (busy: offline, or its driver sets it "
                                    "itself)";
    }
    void eppAllOrNothing()
    {
        QFETCH(QString, sibling);
        QFETCH(QString, answer);
        writeFile(eppFile(2) + sibling, sibling == ".writes" ? "0\n" : "");
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QCOMPARE(h.answer("epp on"), answer);
        QVERIFY(eppAll("balance_performance"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        const QStringList w = writes();
        QVERIFY(w.contains(QString("%1/cpu0/cpufreq/energy_performance_preference balance_performance").arg(kCpu)));
        QVERIFY(w.contains(QString("%1/cpu1/cpufreq/energy_performance_preference balance_performance").arg(kCpu)));
        QVERIFY(!w.join('\n').contains("cpu3/"));
        qemu.stop();
        QVERIFY(h.finished());
        QVERIFY(!h.all().join('\n').contains("restored epp"));
    }

    /* An undo refused: that CPU stays recorded, and the next helper puts it back */
    void eppUndoRefused()
    {
        /* cpu1 takes one write (performance), then refuses; cpu2 refuses at once */
        writeFile(eppFile(1) + ".writes", "1\n");
        writeFile(eppFile(2) + ".writes", "0\n");
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QVERIFY(h.answer("epp on").startsWith("error epp: cpu2 refuses it"));
        QCOMPARE(epp(0), QString("balance_performance"));
        QCOMPARE(epp(1), QString("performance"));
        QCOMPARE(stateFiles(), QStringList({"epp.state", "lock"}));
        QCOMPARE(readFile(path("/run/vitrine-helper/epp.state")),
                 QByteArray("cpu1 balance_performance performance\n"));
        QFile::remove(eppFile(1) + ".writes");
        Helper b(m_root);
        QCOMPARE(b.line(), QString("restored epp: 1 cpus back to balance_performance"));
        QVERIFY(b.ready());
        QVERIFY(eppAll("balance_performance"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        b.closeInput();
        QVERIFY(b.finished());
    }

    /* Offline CPUs (their policy inactive: EBUSY) are left out, and taken
       by a later epp on - the next VM's start - once online */
    void eppOffline()
    {
        setOnline("0-2");
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QCOMPARE(h.answer("epp on"),
                 QString("ok epp on: performance on 3 cpus (was balance_performance); 1 offline"));
        QCOMPARE(epp(3), QString("balance_performance"));
        QVERIFY(!writes().join('\n').contains("cpu3/"));
        setOnline("0-3");
        QCOMPARE(h.answer("epp on"),
                 QString("ok epp on: already; 1 cpus online since at performance (was balance_performance)"));
        QVERIFY(eppAll("performance"));
        QCOMPARE(h.answer("epp on"), QString("ok epp on: already"));
        qemu.stop();
        QCOMPARE(h.waitFor("restored"), QString("restored epp: 4 cpus back to balance_performance"));
        QVERIFY(h.finished());
        QVERIFY(eppAll("balance_performance"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    /* A CPU offline at the end keeps the preference (the driver holds it):
       recorded, and put back by the next helper once online */
    void eppOfflineAtRestore()
    {
        FakeQemu qemu;
        {
            Helper h(m_root);
            QVERIFY(h.ready());
            h.answer("watch " + qemu.pidText());
            QVERIFY(h.answer("epp on").startsWith("ok epp on: performance on 4 cpus"));
            setOnline("0-2");
            qemu.stop();
            QCOMPARE(h.waitFor("restored"), QString("restored epp: 3 cpus back to balance_performance; "
                                                    "1 kept for later (offline or refused)"));
            QVERIFY(h.finished());
        }
        QCOMPARE(epp(3), QString("performance"));
        QCOMPARE(readFile(path("/run/vitrine-helper/epp.state")),
                 QByteArray("cpu3 balance_performance performance\n"));
        {
            /* still offline: kept again */
            Helper b(m_root);
            QCOMPARE(b.line(), QString("restored epp: 1 cpus kept for later (offline or refused)"));
            b.closeInput();
            QVERIFY(b.finished());
            QVERIFY(stateExists("epp"));
        }
        setOnline("0-3");
        Helper c(m_root);
        QCOMPARE(c.line(), QString("restored epp: 1 cpus back to balance_performance"));
        QVERIFY(c.ready());
        QVERIFY(eppAll("balance_performance"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        c.closeInput();
        QVERIFY(c.finished());
    }

    /* Killed holding it, setting it or putting it back: the next helper puts it back */
    void eppCrashRecovery_data()
    {
        QTest::addColumn<int>("killAt");
        QTest::addColumn<bool>("restoring");
        QTest::newRow("held") << 0 << false;
        QTest::newRow("setting, after two writes") << 2 << false;
        QTest::newRow("restoring, after a write") << 5 << true;
    }
    void eppCrashRecovery()
    {
        QFETCH(int, killAt);
        QFETCH(bool, restoring);
        {
            FakeQemu qemu;
            Helper a(m_root, killAt ? QStringList{QString("VITRINE_HELPER_TEST_KILL_AT=%1").arg(killAt)}
                                    : QStringList());
            QVERIFY(a.ready());
            a.answer("watch " + qemu.pidText());
            const QString on = a.answer("epp on");
            if (!killAt) {
                QVERIFY(on.startsWith("ok epp on: performance on 4 cpus"));
                a.kill();
            } else if (restoring) {
                QVERIFY(on.startsWith("ok epp on: performance on 4 cpus"));
                qemu.stop();
            }
            QVERIFY(a.finished());
        }
        QVERIFY(stateExists("epp"));
        QVERIFY(!eppAll("balance_performance"));
        Helper b(m_root);
        QCOMPARE(b.line(), QString("restored epp: 4 cpus back to balance_performance"));
        QVERIFY(b.ready());
        QVERIFY(eppAll("balance_performance"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        b.closeInput();
        QVERIFY(b.finished());
        QVERIFY(!b.all().join('\n').contains("left epp"));
    }

    /* Two helpers: the second one joins the first's hold, the last one out restores */
    void eppTwoHelpers()
    {
        FakeQemu q1, q2;
        Helper a(m_root), b(m_root);
        QVERIFY(a.ready());
        QVERIFY(b.ready());
        a.answer("watch " + q1.pidText());
        b.answer("watch " + q2.pidText());
        QVERIFY(a.answer("epp on").startsWith("ok epp on: performance on 4 cpus"));
        clearJournal();
        QCOMPARE(b.answer("epp on"), QString("ok epp on: set by another vitrine session"));
        QVERIFY(writes().isEmpty());
        q1.stop();
        QVERIFY(a.finished());
        QVERIFY(!a.all().join('\n').contains("restored"));
        QVERIFY(eppAll("performance"));
        q2.stop();
        QVERIFY(b.finished());
        QVERIFY(b.all().contains("restored epp: 4 cpus back to balance_performance"));
        QVERIFY(eppAll("balance_performance"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    void holdsExhausted()
    {
        for (int n = 2; n <= 9; n++) {
            amdApu(n);
        }
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QVERIFY(h.answer("fair-server on").startsWith("ok fair-server on"));
        QVERIFY(h.answer("udmabuf " + qemu.pidText()).startsWith("ok udmabuf"));
        QVERIFY(h.answer("thp on").startsWith("ok thp on: advise"));
        QVERIFY(h.answer("epp on").startsWith("ok epp on: performance on 4 cpus"));
        for (int n = 1; n <= 8; n++) {
            QVERIFY(h.answer(QString("gpu-floor card%1 auto").arg(n)).startsWith("ok gpu-floor"));
        }
        QCOMPARE(h.answer("gpu-floor card9 auto"),
                 QString("error gpu-floor card9: cannot take the hold: No space left on device"));
        h.ask("release");
        QVERIFY(h.finished());
        QCOMPARE(udmabuf(), QString("1024/64"));
        QCOMPARE(thp(), QString("never"));
        QVERIFY(eppAll("balance_performance"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    /* vitrine quits while its VMs run: the helper keeps the settings until the last one ends */
    void inputClosedWhileVmsRun()
    {
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QVERIFY(h.answer("fair-server on").startsWith("ok"));
        h.closeInput();
        QVERIFY(!h.finished(500));
        QVERIFY(fairAll("10000000/1000000"));
        qemu.stop();
        QVERIFY(h.finished());
        QVERIFY(fairAll("1000000000/50000000"));
    }

    /* Nothing watched: stdin closed means the end, and nothing was applied */
    void inputClosedWithoutVms()
    {
        Helper h(m_root);
        QVERIFY(h.ready());
        QCOMPARE(h.answer("fair-server on"), QString("error fair-server: watch a QEMU first"));
        QCOMPARE(h.answer("gpu-floor card1 auto"), QString("error gpu-floor: watch a QEMU first"));
        QCOMPARE(h.answer("rt 1234"), QString("error rt: watch a QEMU first"));
        h.closeInput();
        QVERIFY(h.finished());
        QCOMPARE(h.all().last(), QString("bye"));
        QVERIFY(writes().isEmpty());
    }

    void watchValidation()
    {
        FakeQemu qemu;
        QProcess sleeper;
        sleeper.start("sleep", {"300"});
        QVERIFY(sleeper.waitForStarted());
        Helper h(m_root);
        QVERIFY(h.ready());
        QCOMPARE(h.answer("watch abc"), QString("error watch: give a process id"));
        QCOMPARE(h.answer("watch 1"), QString("error watch: give a process id"));
        QCOMPARE(h.answer("watch 0123"), QString("error watch: give a process id"));
        QCOMPARE(h.answer("watch 99999999"), QString("error watch: give a process id"));
        QCOMPARE(h.answer("watch " + QString::number(sleeper.processId())),
                 QString("error watch %1: not a QEMU (qemu-system-*, qemu-kvm)").arg(sleeper.processId()));
        /* a pid no process has: pid_max is 4194304 */
        QVERIFY(h.answer("watch 4194303").startsWith("error watch 4194303: "));
        QCOMPARE(h.answer("watch " + qemu.pidText()), "ok watch " + qemu.pidText());
        QCOMPARE(h.answer("watch " + qemu.pidText()), "ok watch " + qemu.pidText() + ": already");
        QCOMPARE(h.answer("rt 4194303"), QString("error rt: watch the process first"));
        QCOMPARE(h.answer("frobnicate"), QString("error frobnicate: unknown request"));
        QCOMPARE(h.answer("watch"), QString("error watch: unknown request"));
        QCOMPARE(h.answer("fair-server maybe"), QString("error fair-server: unknown request"));
        QCOMPARE(h.answer("watch 1 2 3 4"), QString("error: too many words"));
        QCOMPARE(h.answer(QString("watch \t1")), QString("error: requests are printable ASCII"));
        QCOMPARE(h.answer(QString(600, 'x')), QString("error: request too long"));
        /* after a long one, the next request is read as it should */
        QCOMPARE(h.answer("watch " + qemu.pidText()), "ok watch " + qemu.pidText() + ": already");
        sleeper.kill();
        sleeper.waitForFinished();
    }

    /* Another user's QEMU (PKEXEC_UID says who asked) */
    void watchOtherUser()
    {
        FakeQemu qemu;
        Helper h(m_root, {"PKEXEC_UID=65534"});
        QVERIFY(h.ready());
        QCOMPARE(h.answer("watch " + qemu.pidText()),
                 QString("error watch %1: not a process of uid 65534").arg(qemu.pid()));
    }

    void realtimeThreads()
    {
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QVERIFY(h.answer("fair-server on").startsWith("ok fair-server on"));
        /* the stand-in has its main thread and 3 more */
        QCOMPARE(h.answer("rt " + qemu.pidText()),
                 QString("ok rt %1: 4 of 4 threads real-time").arg(qemu.pid()));
        const QStringList tids = QDir(QString("/proc/%1/task").arg(qemu.pid()))
                                     .entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QCOMPARE(tids.size(), 4);
        for (const QString &tid : tids) {
            QVERIFY(journal().contains("sched " + tid + " fifo 1"));
        }
        /* again: nothing more to do */
        clearJournal();
        QCOMPARE(h.answer("rt " + qemu.pidText()),
                 QString("ok rt %1: 4 of 4 threads real-time").arg(qemu.pid()));
        QVERIFY(journal().filter("sched ").isEmpty());
        /* released while it runs: back to SCHED_OTHER */
        h.ask("release");
        QVERIFY(h.finished());
        QVERIFY(h.all().contains(QString("restored rt %1: 4 threads back to SCHED_OTHER").arg(qemu.pid())));
        for (const QString &tid : tids) {
            QVERIFY(journal().contains("sched " + tid + " other 0"));
        }
    }

    /*
     * A real-time time limit on the QEMU - PipeWire's module-rt sets RTKit's
     * 200 ms for QEMU's whole process when RTKit gives its audio thread
     * real-time - goes before any thread is made real-time: the kernel would
     * SIGKILL QEMU the first time a vCPU runs that long without sleeping.
     * Never put back: behind and the release leave it as rt made it.
     */
    void rtLiftsRealtimeTimeLimit()
    {
        FakeQemu qemu(FAKE_QEMU, {"FAKE_QEMU_RTTIME=200000"});
        QTRY_VERIFY(QString::fromLatin1(readFile(QString("/proc/%1/limits").arg(qemu.pid())))
                        .contains(QRegularExpression("Max realtime timeout +200000 +200000 ")));
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QVERIFY(h.answer("fair-server on").startsWith("ok fair-server on"));
        clearJournal();
        QCOMPARE(h.answer("rt " + qemu.pidText()),
                 QString("ok rt %1: 4 of 4 threads real-time").arg(qemu.pid()));
        const QStringList j = journal();
        const qsizetype lifted =
            j.indexOf(QString("rttime %1 unlimited unlimited").arg(qemu.pid()));
        QVERIFY2(lifted >= 0, qPrintable(j.join('\n')));
        const QStringList fifo = j.filter(QRegularExpression("^sched \\d+ fifo 1$"));
        QCOMPARE(fifo.size(), 4);
        QVERIFY(lifted < j.indexOf(fifo.first()));
        QVERIFY(!j.filter(QString("log uid %1: real-time time limit of pid %2 lifted (soft "
                                  "200000 us, hard 200000 us)")
                              .arg(getuid()).arg(qemu.pid())).isEmpty());
        /* lifted already: nothing to do the next time */
        clearJournal();
        QVERIFY(h.answer("behind " + qemu.pidText()).startsWith("ok behind "));
        QVERIFY(h.answer("rt " + qemu.pidText()).startsWith("ok rt "));
        QVERIFY(journal().filter("rttime ").isEmpty());
        h.ask("release");
        QVERIFY(h.finished());
        QVERIFY(journal().filter("rttime ").isEmpty());
    }

    /* A limit that cannot be lifted: no thread is made real-time */
    void rtWithoutLiftNoRealtime()
    {
        FakeQemu qemu(FAKE_QEMU, {"FAKE_QEMU_RTTIME=200000"});
        writeFile(path("/rttime.refuse"), "");
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QVERIFY(h.answer("fair-server on").startsWith("ok fair-server on"));
        QCOMPARE(h.answer("rt " + qemu.pidText()),
                 QString("error rt %1: its real-time time limit cannot be lifted, which would "
                         "end it: Operation not permitted")
                     .arg(qemu.pid()));
        QVERIFY(journal().filter("sched ").isEmpty());
        QVERIFY(!QFile::exists(path("/run/vitrine-helper/sched-" + qemu.pidText() + ".state")));
    }

    /* A QEMU without one (as vitrine starts it): nothing to lift */
    void rtWithoutTimeLimit()
    {
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QVERIFY(h.answer("fair-server on").startsWith("ok fair-server on"));
        QVERIFY(h.answer("rt " + qemu.pidText()).startsWith("ok rt "));
        QVERIFY(journal().filter("rttime ").isEmpty());
    }

    /* A QEMU that has exec'ed another program since watch: no rt for it */
    void rtAfterExec()
    {
        FakeQemu qemu;
        Helper h(m_root);
        QVERIFY(h.ready());
        QCOMPARE(h.answer("watch " + qemu.pidText()), "ok watch " + qemu.pidText());
        ::kill(pid_t(qemu.pid()), SIGUSR1);
        QTRY_VERIFY(!QFileInfo(QString("/proc/%1/exe").arg(qemu.pid()))
                         .symLinkTarget()
                         .endsWith("/qemu-system-x86_64"));
        QCOMPARE(h.answer("rt " + qemu.pidText()),
                 QString("error rt %1: no longer a QEMU of uid %2").arg(qemu.pid()).arg(getuid()));
        QVERIFY(journal().filter("sched ").isEmpty());
    }

    /*
     * view-rt: threads of the process that started the helper - the test,
     * here; vitrine's view of the VM in front, as pkexec's caller - at
     * SCHED_FIFO 1, only with a watched QEMU in front and the fair server
     * set, recorded first; view-behind puts them back
     */
    void viewRealtime()
    {
        FakeQemu qemu;
        Threads view(3);
        const QString me = QString::number(getpid());
        Helper h(m_root);
        QVERIFY(h.ready());
        QCOMPARE(h.answer("view-rt " + view.tid(0)), QString("error view-rt: watch a QEMU first"));
        h.answer("watch " + qemu.pidText());
        QCOMPARE(h.answer("view-rt " + view.tid(0)),
                 QString("skip view-rt %1: the fair server is not set: the fair server was not "
                         "asked for").arg(view.tid(0)));
        QVERIFY(h.answer("fair-server on").startsWith("ok fair-server on"));
        QCOMPARE(h.answer("view-rt " + view.tid(0)),
                 QString("skip view-rt %1: no VM in front").arg(view.tid(0)));
        QCOMPARE(h.answer("behind " + qemu.pidText()),
                 QString("ok behind %1: 0 threads ordinary, 2 vCPUs at nice -5").arg(qemu.pid()));
        QCOMPARE(h.answer("view-rt " + view.tid(0)),
                 QString("skip view-rt %1: no VM in front").arg(view.tid(0)));
        QVERIFY(journal().filter("sched " + view.tid(0)).isEmpty());
        QVERIFY(h.answer("rt " + qemu.pidText()).startsWith("ok rt "));
        clearJournal();
        QCOMPARE(h.answer("view-rt " + view.tid(0)), QString("ok view-rt %1: real-time").arg(view.tid(0)));
        QCOMPARE(h.answer("view-rt " + view.tid(1)), QString("ok view-rt %1: real-time").arg(view.tid(1)));
        /* SCHED_RESET_ON_FORK: what they start begins ordinary */
        QCOMPARE(schedOf(view.tid(0)), viewRealtimeSched());
        QCOMPARE(schedOf(view.tid(1)), viewRealtimeSched());
        QCOMPARE(schedOf(view.tid(2)), QString());
        QVERIFY(journal().contains("sched " + view.tid(0) + " fifo 1 reset-on-fork"));
        QVERIFY(journal().contains(QString("log uid %1: SCHED_FIFO 1 on thread %2 of pid %3 (its "
                                           "view of the VM in front)")
                                       .arg(getuid()).arg(view.tid(0), me)));
        /* recorded, as a QEMU's threads are, for a helper that dies */
        QVERIFY(readFile(path("/run/vitrine-helper/sched-" + me + ".state"))
                    .endsWith(" niced 0 rt 1 view 1 kept -\n"));
        /* again: as it is */
        clearJournal();
        QCOMPARE(h.answer("view-rt " + view.tid(0)),
                 QString("ok view-rt %1: real-time already").arg(view.tid(0)));
        QVERIFY(journal().filter("sched ").isEmpty());
        QCOMPARE(h.answer("view-behind"), QString("ok view-behind: 2 threads ordinary"));
        QCOMPARE(schedOf(view.tid(0)), QString("0 0 0"));
        QCOMPARE(schedOf(view.tid(1)), QString("0 0 0"));
        QCOMPARE(h.answer("view-behind"), QString("ok view-behind: 0 threads ordinary"));
        /* the QEMU's threads stay as they are */
        for (const QString &tid : tids(qemu.pid())) {
            QVERIFY2(schedOf(tid).startsWith("1 1 "), qPrintable(schedOf(tid)));
        }
        /* real-time again; the release puts it back */
        QCOMPARE(h.answer("view-rt " + view.tid(2)), QString("ok view-rt %1: real-time").arg(view.tid(2)));
        h.ask("release");
        QVERIFY(h.finished());
        const QStringList all = h.all();
        QVERIFY2(all.contains(QString("restored view-rt %1: 1 threads back to SCHED_OTHER").arg(me)),
                 qPrintable(all.join('\n')));
        /* sent behind once: its vCPUs too */
        QVERIFY(all.contains(QString("restored rt %1: 4 threads back to SCHED_OTHER, vCPUs back to "
                                     "nice 0").arg(qemu.pid())));
        QCOMPARE(all.filter("restored view-rt ").size(), 1);
        QCOMPARE(schedOf(view.tid(2)), QString("0 0 0"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        QVERIFY(fairAll("1000000000/50000000"));
    }

    /*
     * The view's threads go back by themselves once no watched QEMU is in
     * front: the one in front sent behind, or gone; and with the bound, at
     * fair-server off
     */
    void viewFollowsTheFront()
    {
        FakeQemu q1, q2;
        Threads view(2);
        const QString me = QString::number(getpid());
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + q1.pidText());
        h.answer("watch " + q2.pidText());
        QVERIFY(h.answer("fair-server on").startsWith("ok fair-server on"));
        QVERIFY(h.answer("rt " + q1.pidText()).startsWith("ok rt "));
        QCOMPARE(h.answer("view-rt " + view.tid(0)), QString("ok view-rt %1: real-time").arg(view.tid(0)));
        /* another VM behind: the view stays */
        QVERIFY(h.answer("behind " + q2.pidText()).startsWith("ok behind "));
        QCOMPARE(schedOf(view.tid(0)), viewRealtimeSched());
        /* the one in front behind: its view with it */
        QVERIFY(h.answer("behind " + q1.pidText()).startsWith("ok behind "));
        QCOMPARE(h.line(), QString("restored view-rt %1: 1 threads back to SCHED_OTHER").arg(me));
        QCOMPARE(schedOf(view.tid(0)), QString("0 0 0"));
        QVERIFY(journal().contains(
            QString("log pid %1: 1 threads of its VM view back to SCHED_OTHER").arg(me)));
        /* another in front, and it ends */
        QVERIFY(h.answer("rt " + q2.pidText()).startsWith("ok rt "));
        QCOMPARE(h.answer("view-rt " + view.tid(1)), QString("ok view-rt %1: real-time").arg(view.tid(1)));
        const QString gone = q2.pidText();
        q2.stop();
        QCOMPARE(h.waitFor("exited "), "exited " + gone);
        QCOMPARE(h.line(), QString("restored view-rt %1: 1 threads back to SCHED_OTHER").arg(me));
        QCOMPARE(schedOf(view.tid(1)), QString("0 0 0"));
        /* no real-time without the bound */
        QVERIFY(h.answer("rt " + q1.pidText()).startsWith("ok rt "));
        QCOMPARE(h.answer("view-rt " + view.tid(0)), QString("ok view-rt %1: real-time").arg(view.tid(0)));
        const QStringList off = h.ask("fair-server off");
        QVERIFY2(off.contains(QString("restored view-rt %1: 1 threads back to SCHED_OTHER").arg(me)),
                 qPrintable(off.join('\n')));
        QCOMPARE(off.last(), QString("ok fair-server off"));
        QCOMPARE(schedOf(view.tid(0)), QString("0 0 0"));
        QCOMPARE(h.answer("view-rt " + view.tid(0)),
                 QString("skip view-rt %1: the fair server is not set: the fair server was let go")
                     .arg(view.tid(0)));
        /* its last QEMU gone, the helper ends with nothing left */
        QVERIFY(h.answer("fair-server on").startsWith("ok fair-server on"));
        QVERIFY(h.answer("rt " + q1.pidText()).startsWith("ok rt "));
        QCOMPARE(h.answer("view-rt " + view.tid(1)), QString("ok view-rt %1: real-time").arg(view.tid(1)));
        q1.stop();
        QVERIFY(h.finished());
        QCOMPARE(h.all().filter("restored view-rt ").size(), 4);
        QCOMPARE(schedOf(view.tid(1)), QString("0 0 0"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    /*
     * Only what view-rt made real-time goes back: threads at SCHED_FIFO 1
     * before its first change are kept, real-time ones of other kinds
     * (RTKit gives SCHED_RR) and SCHED_IDLE ones are left alone; and only
     * threads of the process that started the helper
     */
    void viewKeepsOthers()
    {
        FakeQemu qemu;
        Threads view(4);
        /* the fake kernel's: one at SCHED_FIFO 1, one at SCHED_RR 1, one at SCHED_IDLE */
        writeFile(path("/sched"), QString("%1 1 1 0\n%2 2 1 0\n%3 5 0 0\n")
                                      .arg(view.tid(0), view.tid(1), view.tid(2)).toLatin1());
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QVERIFY(h.answer("fair-server on").startsWith("ok fair-server on"));
        QVERIFY(h.answer("rt " + qemu.pidText()).startsWith("ok rt "));
        QCOMPARE(h.answer("view-rt " + view.tid(0)),
                 QString("ok view-rt %1: real-time already").arg(view.tid(0)));
        QCOMPARE(h.answer("view-rt " + view.tid(1)),
                 QString("ok view-rt %1: real-time already").arg(view.tid(1)));
        QCOMPARE(h.answer("view-rt " + view.tid(2)),
                 QString("skip view-rt %1: SCHED_IDLE, its own choice").arg(view.tid(2)));
        QCOMPARE(h.answer("view-rt " + view.tid(3)), QString("ok view-rt %1: real-time").arg(view.tid(3)));
        QCOMPARE(schedOf(view.tid(3)), viewRealtimeSched());
        QVERIFY(readFile(path("/run/vitrine-helper/sched-" + QString::number(getpid()) + ".state"))
                    .endsWith(" rt 1 view 1 kept " + view.tid(0).toLatin1() + "\n"));
        /* the QEMU's threads, the QEMU itself, and what is not a thread id */
        const QString qemuThread = tids(qemu.pid()).last();
        QCOMPARE(h.answer("view-rt " + qemuThread),
                 QString("error view-rt %1: not a thread of pid %2").arg(qemuThread).arg(getpid()));
        QCOMPARE(h.answer("view-rt " + qemu.pidText()),
                 QString("error view-rt %1: not a thread of pid %2").arg(qemu.pid()).arg(getpid()));
        QCOMPARE(h.answer("view-rt 4194303"),
                 QString("error view-rt 4194303: not a thread of pid %1").arg(getpid()));
        for (const QString &bad : {"0", "abc", "0123", "99999999", "-1"}) {
            QCOMPARE(h.answer("view-rt " + bad), QString("error view-rt: give a thread id"));
        }
        QCOMPARE(h.answer("view-rt"), QString("error view-rt: unknown request"));
        QCOMPARE(h.answer("view-behind now"), QString("error view-behind: unknown request"));
        QCOMPARE(h.answer("view-behind"), QString("ok view-behind: 1 threads ordinary"));
        QCOMPARE(schedOf(view.tid(0)), QString("1 1 0"));
        QCOMPARE(schedOf(view.tid(1)), QString("2 1 0"));
        QCOMPARE(schedOf(view.tid(2)), QString("5 0 0"));
        QCOMPARE(schedOf(view.tid(3)), QString("0 0 0"));
        for (const QString &tid : tids(qemu.pid())) {
            QVERIFY2(schedOf(tid).startsWith("1 1 "), qPrintable(schedOf(tid)));
        }
        /* and at the end */
        QCOMPARE(h.answer("view-rt " + view.tid(3)), QString("ok view-rt %1: real-time").arg(view.tid(3)));
        h.ask("release");
        QVERIFY(h.finished());
        QCOMPARE(schedOf(view.tid(0)), QString("1 1 0"));
        QCOMPARE(schedOf(view.tid(1)), QString("2 1 0"));
        QCOMPARE(schedOf(view.tid(2)), QString("5 0 0"));
        QCOMPARE(schedOf(view.tid(3)), QString("0 0 0"));
    }

    /* A process that runs real-time already (MAX_KEPT threads or more at
       SCHED_FIFO 1, as under chrt): left as it is */
    void viewProcessRealtimeAlready()
    {
        FakeQemu qemu;
        Threads view(17);
        QByteArray sched;
        for (int i = 0; i < 16; i++) {
            sched += view.tid(i).toLatin1() + " 1 1 0\n";
        }
        writeFile(path("/sched"), sched);
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QVERIFY(h.answer("fair-server on").startsWith("ok fair-server on"));
        QVERIFY(h.answer("rt " + qemu.pidText()).startsWith("ok rt "));
        QCOMPARE(h.answer("view-rt " + view.tid(16)),
                 QString("skip view-rt %1: pid %2 runs real-time already (16 threads or more at "
                         "SCHED_FIFO 1)").arg(view.tid(16)).arg(getpid()));
        QCOMPARE(schedOf(view.tid(16)), QString());
        QCOMPARE(h.answer("view-behind"), QString("ok view-behind: 0 threads ordinary"));
        h.ask("release");
        QVERIFY(h.finished());
        for (int i = 0; i < 16; i++) {
            QCOMPARE(schedOf(view.tid(i)), QString("1 1 0"));
        }
    }

    /* Only threads of the process that started the helper: started by a
       shell that waits for it, the test's threads are another process's */
    void viewOnlyItsParent()
    {
        FakeQemu qemu;
        Threads view(1);
        Helper h(m_root, {}, "/bin/sh", {"-c", QString("%1 session; :").arg(HELPER_FAKE)});
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QVERIFY(h.answer("fair-server on").startsWith("ok fair-server on"));
        QVERIFY(h.answer("rt " + qemu.pidText()).startsWith("ok rt "));
        QCOMPARE(h.answer("view-rt " + view.tid(0)),
                 QString("error view-rt %1: not a thread of pid %2").arg(view.tid(0)).arg(h.pid()));
        QCOMPARE(schedOf(view.tid(0)), QString());
    }

    /* A helper killed while the view's threads are real-time: the next one
       puts them back from the record, before the bound they needed */
    void viewDeadHelper()
    {
        FakeQemu qemu;
        Threads view(2);
        const QString me = QString::number(getpid());
        {
            Helper a(m_root);
            QVERIFY(a.ready());
            a.answer("watch " + qemu.pidText());
            QVERIFY(a.answer("fair-server on").startsWith("ok fair-server on"));
            QVERIFY(a.answer("rt " + qemu.pidText()).startsWith("ok rt "));
            QCOMPARE(a.answer("view-rt " + view.tid(0)), QString("ok view-rt %1: real-time").arg(view.tid(0)));
            QCOMPARE(a.answer("view-rt " + view.tid(1)), QString("ok view-rt %1: real-time").arg(view.tid(1)));
            a.kill();
        }
        QVERIFY(stateExists("sched-" + me));
        QCOMPARE(schedOf(view.tid(0)), viewRealtimeSched());
        Helper b(m_root);
        QStringList before;
        for (QString l = b.line(); !l.isNull() && !l.startsWith("ready"); l = b.line()) {
            before << l;
        }
        const qsizetype viewLine = before.indexOf(
            QString("restored view-rt %1: 2 threads back to SCHED_OTHER").arg(me));
        const qsizetype qemuLine = before.indexOf(
            QString("restored rt %1: 4 threads back to SCHED_OTHER").arg(qemu.pid()));
        const qsizetype fairLine = before.indexOf("restored fair-server: 4 cpus back to 1000 ms / 50 ms");
        QVERIFY2(viewLine >= 0 && qemuLine >= 0 && fairLine > viewLine && fairLine > qemuLine,
                 qPrintable(before.join('\n')));
        QCOMPARE(schedOf(view.tid(0)), QString("0 0 0"));
        QCOMPARE(schedOf(view.tid(1)), QString("0 0 0"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        b.closeInput();
        QVERIFY(b.finished());
    }

    /* A caller that stops reading the replies: the watch goes on all the same */
    void callerNotReading()
    {
        FakeQemu qemu;
        int sv[2];
        QVERIFY(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, sv[1], 0);
        posix_spawn_file_actions_adddup2(&actions, sv[1], 1);
        qputenv("VITRINE_HELPER_TEST_ROOT", m_root.toLocal8Bit());
        char *argv[] = {const_cast<char *>(HELPER_FAKE), const_cast<char *>("session"), nullptr};
        pid_t pid;
        QCOMPARE(posix_spawn(&pid, HELPER_FAKE, &actions, nullptr, argv, environ), 0);
        posix_spawn_file_actions_destroy(&actions);
        qunsetenv("VITRINE_HELPER_TEST_ROOT");
        ::close(sv[1]);
        /* a check that fails returns at once: the helper goes too, not left
           holding the fake tree for the tests after this one */
        bool reaped = false;
        const auto cleanup = qScopeGuard([&]() {
            if (!reaped) {
                ::kill(pid, SIGKILL);
                waitpid(pid, nullptr, 0);
            }
            ::close(sv[0]);
        });
        fcntl(sv[0], F_SETFL, O_NONBLOCK);
        /* each one answered: far more replies than the socket holds */
        QByteArray requests = "watch " + qemu.pidText().toLatin1() + "\nfair-server on\n";
        for (int i = 0; i < 40000; i++) {
            requests += "watch " + qemu.pidText().toLatin1() + '\n';
        }
        QElapsedTimer clock;
        clock.start();
        qsizetype sent = 0;
        while (sent < requests.size() && clock.elapsed() < 20000) {
            const ssize_t n = ::send(sv[0], requests.constData() + sent,
                                     size_t(requests.size() - sent), MSG_NOSIGNAL);
            if (n > 0) {
                sent += n;
            } else {
                struct pollfd out = {sv[0], POLLOUT, 0};
                poll(&out, 1, 100);
            }
        }
        QCOMPARE(sent, requests.size());
        QTRY_VERIFY(fairAll("10000000/1000000"));
        qemu.stop();
        QTRY_VERIFY_WITH_TIMEOUT(fairAll("1000000000/50000000"), 5000);
        /*
         * It ends, with status 0.  Reaped here, once - posix_spawn's child
         * is not Qt's to reap - and not in QTRY_VERIFY, which evaluates its
         * condition again after it holds: the waitpid() after the one that
         * reaped it got ECHILD, and the status was never looked at.
         */
        int status = 0;
        pid_t r = 0;
        QElapsedTimer waited;
        waited.start();
        while ((r = waitpid(pid, &status, WNOHANG)) == 0 && waited.elapsed() < 5000) {
            QTest::qWait(20);
        }
        reaped = r == pid;
        QCOMPARE(r, pid);
        QVERIFY(WIFEXITED(status));
        QCOMPARE(WEXITSTATUS(status), 0);
    }

    /* The stand-in QEMU ends with its stdin, as when the test that started
       it dies: a crashed test leaves no stand-ins behind, nor helpers
       watching them */
    void fakeQemuEndsWithTheTest()
    {
        QProcess qemu;
        qemu.start(FAKE_QEMU, {});
        QVERIFY(qemu.waitForStarted());
        QVERIFY(!qemu.waitForFinished(200));
        qemu.closeWriteChannel();
        QVERIFY(qemu.waitForFinished(2000));
        QCOMPARE(qemu.exitStatus(), QProcess::NormalExit);
    }

    /* SIGTERM (a shutdown): everything back before the end */
    void terminated()
    {
        FakeQemu qemu;
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("VITRINE_HELPER_TEST_ROOT", m_root);
        p.setProcessEnvironment(env);
        p.start(HELPER_FAKE, {"session"});
        QVERIFY(p.waitForStarted());
        p.write(QString("watch %1\nfair-server on\n").arg(qemu.pid()).toUtf8());
        QTRY_VERIFY(fairAll("10000000/1000000"));
        p.terminate();
        QVERIFY(p.waitForFinished());
        QVERIFY(fairAll("1000000000/50000000"));
        QVERIFY(QString::fromUtf8(p.readAllStandardOutput()).endsWith("bye\n"));
    }

    /*
     * Signals as root's own, whatever its caller left: pkexec passes ignored
     * ones and the mask on, and an ignored SIGCHLD has the kernel reap the
     * group tools before their status is read.  Only SIGHUP and SIGPIPE are
     * ignored, only SIGINT and SIGTERM blocked (the session's signalfd).
     */
    void signalsReset()
    {
        auto field = [](qint64 pid, const QByteArray &name) {
            const QList<QByteArray> lines = readFile(QString("/proc/%1/status").arg(pid)).split('\n');
            for (const QByteArray &l : lines) {
                if (l.startsWith(name + ":")) {
                    return l.mid(name.size() + 1).trimmed().toULongLong(nullptr, 16);
                }
            }
            return ~0ULL;
        };
        const qulonglong ignored = 1ULL << (SIGHUP - 1) | 1ULL << (SIGPIPE - 1);
        const qulonglong blocked = 1ULL << (SIGINT - 1) | 1ULL << (SIGTERM - 1);
        {
            /* the shell's trap: ignored through the exec */
            Helper h(m_root, {}, "/bin/sh", {"-c", "trap '' CHLD USR1 TERM; exec \"$0\" session",
                                             HELPER_FAKE});
            QVERIFY(h.ready());
            QCOMPARE(field(h.pid(), "SigIgn"), ignored);
            QCOMPARE(field(h.pid(), "SigBlk"), blocked);
        }
        /* a mask with SIGCHLD and SIGUSR1 blocked */
        int sv[2];
        QVERIFY(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);
        posix_spawn_file_actions_t actions;
        posix_spawnattr_t attr;
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGCHLD);
        sigaddset(&mask, SIGUSR1);
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, sv[1], 0);
        posix_spawn_file_actions_adddup2(&actions, sv[1], 1);
        posix_spawnattr_init(&attr);
        posix_spawnattr_setsigmask(&attr, &mask);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK);
        qputenv("VITRINE_HELPER_TEST_ROOT", m_root.toLocal8Bit());
        char *argv[] = {const_cast<char *>(HELPER_FAKE), const_cast<char *>("session"), nullptr};
        pid_t pid;
        QCOMPARE(posix_spawn(&pid, HELPER_FAKE, &actions, &attr, argv, environ), 0);
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attr);
        qunsetenv("VITRINE_HELPER_TEST_ROOT");
        ::close(sv[1]);
        const auto cleanup = qScopeGuard([&]() {
            ::close(sv[0]);
            ::kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
        });
        char ready[16] = "";
        struct pollfd in = {sv[0], POLLIN, 0};
        QVERIFY(poll(&in, 1, 5000) == 1 && read(sv[0], ready, sizeof(ready) - 1) > 0);
        QCOMPARE(QByteArray(ready), QByteArray("ready 1\n"));
        QCOMPARE(field(pid, "SigBlk"), blocked);
        QCOMPARE(field(pid, "SigIgn"), ignored);
    }

    /*
     * setup-group: the caller (and only the caller) in the vitrine group,
     * created if need be; nothing changed when done already; no argument
     */
    void setupGroup()
    {
        struct passwd *pw = getpwuid(getuid());
        QVERIFY(pw);
        const QString me = QString::fromLocal8Bit(pw->pw_name);
        const QString groupFile = path("/etc/group");
        auto run = [this](const QStringList &args, const QString &uid = {}, QString *out = nullptr) {
            QProcess p;
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert("VITRINE_HELPER_TEST_ROOT", m_root);
            env.remove("SUDO_UID");
            if (uid.isEmpty()) {
                env.remove("PKEXEC_UID");
            } else {
                env.insert("PKEXEC_UID", uid);
            }
            p.setProcessEnvironment(env);
            p.start(HELPER_FAKE, args);
            p.waitForFinished();
            if (out) {
                *out = QString::fromUtf8(p.readAllStandardOutput() + p.readAllStandardError()).trimmed();
            }
            return p.exitCode();
        };
        const QString uid = QString::number(getuid());
        QVERIFY(writeFile(groupFile, "root:x:0:\nwheel:x:10:" + me.toLocal8Bit() + "\n"));
        QString out;

        /* no group yet: created, the caller added */
        QCOMPARE(run({"setup-group"}, uid, &out), 0);
        QCOMPARE(out, "ok setup-group: " + me + " added to vitrine, group created");
        QCOMPARE(readFile(groupFile), "root:x:0:\nwheel:x:10:" + me.toLocal8Bit() +
                                          "\nvitrine:x:977:" + me.toLocal8Bit() + "\n");
        QCOMPARE(journal().filter(QRegularExpression("^(groupadd|gpasswd) ")),
                 QStringList({"groupadd --system vitrine", "gpasswd -a " + me + " vitrine"}));
        QVERIFY(journal().contains(QString("log uid %1: %2 added to the group vitrine").arg(uid, me)));

        /* again: nothing to do, nothing done */
        clearJournal();
        QCOMPARE(run({"setup-group"}, uid, &out), 0);
        QCOMPARE(out, "ok setup-group: " + me + " is in vitrine already");
        QVERIFY(journal().isEmpty());
        /* without PKEXEC_UID, unprivileged (the test build): the real uid */
        QCOMPARE(run({"setup-group"}, {}, &out), 0);
        QCOMPARE(out, "ok setup-group: " + me + " is in vitrine already");

        /* a group with other members: the caller added after them, they stay */
        QVERIFY(writeFile(groupFile, "vitrine:x:977:alice,bob\n"));
        QCOMPARE(run({"setup-group"}, uid, &out), 0);
        QCOMPARE(out, "ok setup-group: " + me + " added to vitrine");
        QCOMPARE(readFile(groupFile), "vitrine:x:977:alice,bob," + me.toLocal8Bit() + "\n");
        /* a member through the primary group is one */
        QVERIFY(writeFile(groupFile, QString("vitrine:x:%1:\n").arg(pw->pw_gid).toLocal8Bit()));
        QCOMPARE(run({"setup-group"}, uid, &out), 0);
        QCOMPARE(out, "ok setup-group: " + me + " is in vitrine already");

        /* nobody else, no other group: no argument names one */
        QVERIFY(writeFile(groupFile, "vitrine:x:977:\n"));
        clearJournal();
        for (const QStringList &args : {QStringList{"setup-group", "alice"},
                                        QStringList{"setup-group", "vitrine", "alice"},
                                        QStringList{"setup-group", "--user=alice"},
                                        QStringList{"setup-group", "wheel"},
                                        QStringList{"setup-group", ""}}) {
            QCOMPARE(run(args, uid, &out), 2);
            QVERIFY2(out.startsWith("usage: "), qPrintable(out));
        }
        /* root, a uid nobody has, no uid that parses */
        QCOMPARE(run({"setup-group"}, "0", &out), 1);
        QCOMPARE(out, QString("error setup-group: root needs no group"));
        QCOMPARE(run({"setup-group"}, "3999999999", &out), 1);
        QCOMPARE(out, QString("error setup-group: uid 3999999999 is not in the user database"));
        QCOMPARE(run({"setup-group"}, uid + "x", &out), 1);
        QVERIFY(out.contains("no PKEXEC_UID"));
        QCOMPARE(readFile(groupFile), QByteArray("vitrine:x:977:\n"));
        QVERIFY(journal().filter(QRegularExpression("^(groupadd|gpasswd) ")).isEmpty());

        /* no verb at all: not the session, which pkexec would run under its
           generic action */
        QCOMPARE(run({}, uid, &out), 2);
        QVERIFY2(out.startsWith("usage: "), qPrintable(out));

        /*
         * A group named vitrine that setup-group did not create: not joined,
         * nothing changed (the caller would get that group's files)
         */
        const QString foreign = "error setup-group: a group named vitrine exists that vitrine did "
                                "not create: ";
        auto notJoined = [&](const QByteArray &groups, const QString &why) {
            QString text;
            writeFile(groupFile, groups);
            clearJournal();
            const int status = run({"setup-group"}, uid, &text);
            if (status != 1 || text != foreign + why || readFile(groupFile) != groups ||
                !journal().isEmpty()) {
                qWarning() << groups << status << text << journal();
                return false;
            }
            return true;
        };
        /* root's id; another user's private group, at a user's id */
        QVERIFY(notJoined("vitrine:x:0:\n", "its id is 0"));
        QVERIFY(notJoined("vitrine:x:1005:\n", "its id 1005 is not a system group's"));
        /* a privileged group's id, whichever comes first */
        QVERIFY(notJoined("disk:x:6:\nvitrine:x:6:\n", "its id 6 is also disk's"));
        QVERIFY(notJoined("vitrine:x:6:\ndisk:x:6:\n", "its id 6 is also disk's"));
        /* a system user's private group (useradd -r vitrine) */
        QVERIFY(writeFile(path("/etc/passwd"), "root:x:0:0::/root:/bin/sh\n"
                                               "vitrine:x:977:977::/home/vitrine:/bin/sh\n"));
        QVERIFY(notJoined("vitrine:x:977:\n", "it is the primary group of vitrine"));
        QFile::remove(path("/etc/passwd"));
        /* login.defs' range for system groups */
        QVERIFY(writeFile(path("/etc/login.defs"), "# comment\nSYS_GID_MIN   201\n"
                                                   "SYS_GID_MAX   499\n"));
        QVERIFY(notJoined("vitrine:x:977:\n", "its id 977 is not a system group's"));
        QVERIFY(writeFile(path("/etc/login.defs"), "SYS_GID_MAX 1999\n"));
        QVERIFY(writeFile(groupFile, "vitrine:x:1005:\n"));
        QCOMPARE(run({"setup-group"}, uid, &out), 0);
        QCOMPARE(out, "ok setup-group: " + me + " added to vitrine");
        QFile::remove(path("/etc/login.defs"));

        /* the database cannot be written: said, nothing half done */
        QVERIFY(writeFile(groupFile, "vitrine:x:977:\n"));
        chmod(qPrintable(groupFile), 0444);
        QCOMPARE(run({"setup-group"}, uid, &out), 1);
        QCOMPARE(out, QString("error setup-group: cannot add the user to the group: "
                              "Permission denied"));
        chmod(qPrintable(groupFile), 0644);
        QFile::remove(groupFile);
        QDir().rmdir(path("/etc"));
        QCOMPARE(run({"setup-group"}, uid, &out), 1);
        QCOMPARE(out, QString("error setup-group: cannot create the group: "
                              "No such file or directory"));
    }

    /* The test build needs its fake tree */
    void fakeNeedsRoot()
    {
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove("VITRINE_HELPER_TEST_ROOT");
        p.setProcessEnvironment(env);
        p.start(HELPER_FAKE, {});
        QVERIFY(p.waitForFinished());
        QCOMPARE(p.exitCode(), 1);
        QVERIFY(p.readAllStandardError().contains("VITRINE_HELPER_TEST_ROOT"));
    }

    void odParse()
    {
        /* through the helper: a discrete RDNA3 table, with MCLK and offsets */
        FakeQemu qemu;
        writeFile(path(QString(kCard) + "/pp_od_clk_voltage"),
                  "OD_SCLK:\n0: 500Mhz\n1: 2500Mhz\nOD_MCLK:\n0: 97Mhz\n1: 1249MHz\n"
                  "OD_VDDGFX_OFFSET:\n0mV\nOD_RANGE:\nSCLK:     500Mhz       3150Mhz\n"
                  "MCLK:      97Mhz       1500Mhz\n");
        Helper h(m_root);
        QVERIFY(h.ready());
        h.answer("watch " + qemu.pidText());
        QCOMPARE(h.answer("gpu-floor card1 3200"),
                 QString("error gpu-floor card1: 3200 MHz is outside 500-3150 MHz"));
    }

    /*
     * devices-bind: a GPU and its audio function taken from their drivers -
     * the record first, then for each its driver_override, its driver's
     * unbind, vfio-pci's bind - the group's node the caller's, the parent's
     * locked memory limit raised; back to the parent's limit once its QEMU
     * is watched, everything back when that QEMU ends
     */
    void vfioBindAndGiveBack()
    {
        pciTree();
        const QString me = QString::number(getpid());
        Helper h(m_root, {}, HELPER_FAKE, {"devices-bind"});
        QVERIFY(h.ready());
        QCOMPARE(h.answer("vfio 0000:6e:00.0,0000:6e:00.1 3072"),
                 QString("ok vfio 0000:6e:00.0=amdgpu,0000:6e:00.1=snd_hda_intel: IOMMU group 21 on "
                         "vfio-pci, /dev/vfio/21 for uid %1, locked memory 3072 MiB (was 8 MiB)")
                     .arg(getuid()));
        QCOMPARE(driverOf("0000:6e:00.0"), QString("vfio-pci"));
        QCOMPARE(driverOf("0000:6e:00.1"), QString("vfio-pci"));
        QCOMPARE(overrideOf("0000:6e:00.0"), QByteArray("vfio-pci"));
        QCOMPARE(nodeOwner(21), nodeMine());
        QCOMPARE(memlock(), QByteArray("3221225472 3221225472"));
        QVERIFY(stateExists("vfio-21"));
        QVERIFY(stateExists("memlock-" + me));
        QCOMPARE(writes(), QStringList({"/sys/bus/pci/devices/0000:6e:00.0/driver_override vfio-pci",
                                        "/sys/bus/pci/drivers/amdgpu/unbind 0000:6e:00.0",
                                        "/sys/bus/pci/drivers/vfio-pci/bind 0000:6e:00.0",
                                        "/sys/bus/pci/devices/0000:6e:00.1/driver_override vfio-pci",
                                        "/sys/bus/pci/drivers/snd_hda_intel/unbind 0000:6e:00.1",
                                        "/sys/bus/pci/drivers/vfio-pci/bind 0000:6e:00.1"}));
        const QStringList j = journal();
        QVERIFY(j.contains(QString("chown /dev/vfio/21 %1 0 600").arg(getuid())));
        QVERIFY(j.contains(QString("memlock %1 3221225472 3221225472").arg(me)));
        QVERIFY(!j.filter(QRegularExpression("^log uid \\d+: IOMMU group 21 on vfio-pci")).isEmpty());
        /* the same again: nothing more */
        clearJournal();
        QCOMPARE(h.answer("vfio 0000:6e:00.0,0000:6e:00.1 3072"),
                 QString("ok vfio 0000:6e:00.0,0000:6e:00.1: already"));
        QVERIFY(writes().isEmpty());

        /* its QEMU has a copy of the limit: the parent's goes back */
        FakeQemu qemu;
        QCOMPARE(h.answer("watch " + qemu.pidText()), "ok watch " + qemu.pidText());
        QCOMPARE(h.line(), QString("restored memlock %1: 8 MiB").arg(me));
        QCOMPARE(memlock(), QByteArray("8388608 8388608"));
        QVERIFY(!stateExists("memlock-" + me));
        QVERIFY(readFile(path("/run/vitrine-helper/vfio-21.state"))
                    .contains(QString("\nqemu %1 ").arg(qemu.pid()).toLatin1()));

        /* the VM ends: everything back, and the helper with it */
        clearJournal();
        const QString pid = qemu.pidText();
        qemu.stop();
        QCOMPARE(h.waitFor("exited"), "exited " + pid);
        QCOMPARE(h.line(), QString("restored vfio 21: 0000:6e:00.0 back to amdgpu, 0000:6e:00.1 back "
                                   "to snd_hda_intel"));
        QCOMPARE(h.waitFor("bye"), QString("bye"));
        QVERIFY(h.finished());
        QCOMPARE(driverOf("0000:6e:00.0"), QString("amdgpu"));
        QCOMPARE(driverOf("0000:6e:00.1"), QString("snd_hda_intel"));
        QCOMPARE(overrideOf("0000:6e:00.0"), QByteArray("(null)"));
        QCOMPARE(overrideOf("0000:6e:00.1"), QByteArray("(null)"));
        QVERIFY(!nodeExists(21));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        QCOMPARE(writes(), QStringList({"/sys/bus/pci/drivers/vfio-pci/unbind 0000:6e:00.0",
                                        "/sys/bus/pci/devices/0000:6e:00.0/driver_override ",
                                        "/sys/bus/pci/drivers/amdgpu/bind 0000:6e:00.0",
                                        "/sys/bus/pci/drivers/vfio-pci/unbind 0000:6e:00.1",
                                        "/sys/bus/pci/devices/0000:6e:00.1/driver_override ",
                                        "/sys/bus/pci/drivers/snd_hda_intel/bind 0000:6e:00.1"}));
        QVERIFY(journal().contains(QString("chown /dev/vfio/21 0 0 600")));
        QVERIFY(!journal().join('\n').contains("WHILE IN USE"));
    }

    /*
     * devices: a group on vfio-pci already (bound at boot, as the laptop's
     * RTX 5060 is): its node and the limit only, no driver touched, and it
     * stays on vfio-pci after
     */
    void vfioAlreadyOnVfioPci()
    {
        pciTree();
        Helper h(m_root, {}, HELPER_FAKE, {"devices"});
        QVERIFY(h.ready());
        QCOMPARE(h.answer("vfio 0000:01:00.0,0000:01:00.1 3072"),
                 QString("ok vfio 0000:01:00.0=vfio-pci,0000:01:00.1=vfio-pci: IOMMU group 14 on "
                         "vfio-pci, /dev/vfio/14 for uid %1, locked memory 3072 MiB (was 8 MiB)")
                     .arg(getuid()));
        QVERIFY(writes().isEmpty());
        QCOMPARE(nodeOwner(14), nodeMine());
        /* QEMU opens the group */
        FakeQemu qemu(FAKE_QEMU, {"FAKE_QEMU_HOLD=" + path("/dev/vfio/14")});
        h.answer("watch " + qemu.pidText());
        QCOMPARE(h.line(), QString("restored memlock %1: 8 MiB").arg(getpid()));
        const QString pid = qemu.pidText();
        qemu.stop();
        QCOMPARE(h.waitFor("exited"), "exited " + pid);
        QCOMPARE(h.line(), QString("restored vfio 14: /dev/vfio/14 back to uid 0"));
        QVERIFY(h.finished());
        QCOMPARE(nodeOwner(14), QByteArray("0 0 600"));
        QCOMPARE(driverOf("0000:01:00.0"), QString("vfio-pci"));
        QCOMPARE(driverOf("0000:01:00.1"), QString("vfio-pci"));
        QVERIFY(writes().isEmpty());
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    /* devices takes no device from a driver: devices-bind does */
    void vfioDevicesTakesNothing()
    {
        pciTree();
        QCOMPARE(refusal("vfio 0000:6e:00.0,0000:6e:00.1 3072", "devices"),
                 QString("error vfio 0000:6e:00.0: 0000:6e:00.0 is bound to amdgpu: taking it from this "
                         "computer needs vitrine-helper devices-bind"));
        /* nor binds one that has no driver */
        pci("0000:09:00.0", "0x048000", "", 33);
        QCOMPARE(refusal("vfio 0000:09:00.0 1024", "devices"),
                 QString("error vfio 0000:09:00.0: 0000:09:00.0 has no driver: binding it to vfio-pci "
                         "needs vitrine-helper devices-bind"));
        QCOMPARE(memlock(), QByteArray("8388608 8388608"));
        /* the tuning session takes none at all */
        Helper h(m_root);
        QVERIFY(h.ready());
        QCOMPARE(h.answer("vfio 0000:01:00.0,0000:01:00.1 1024"),
                 QString("error vfio: PCI devices go through vitrine-helper devices or devices-bind"));
    }

    /* What the host uses stays with it */
    void vfioRefusesWhatTheHostUses()
    {
        pciTree();
        QCOMPARE(refusal("vfio 0000:65:00.0 3072"),
                 QString("error vfio 0000:65:00.0: 0000:65:00.0 is the GPU this computer started on "
                         "(boot_vga)"));
        writeFile(pciDir("0000:6e:00.0") + "/drm/card1/card1-DP-1/status", "connected\n");
        QCOMPARE(refusal("vfio 0000:6e:00.0,0000:6e:00.1 3072"),
                 QString("error vfio 0000:6e:00.0: a display is connected to 0000:6e:00.0 (DP-1)"));
        writeFile(pciDir("0000:6e:00.0") + "/drm/card1/card1-DP-1/status", "disconnected\n");
        /* the compositor holds the GPU, a browser its render node */
        writeFile(path("/users"), "c 226:1 2345 gnome-shell\nc 226:129 2400 firefox\n");
        QCOMPARE(refusal("vfio 0000:6e:00.0,0000:6e:00.1 3072"),
                 QString("error vfio 0000:6e:00.0: 0000:6e:00.0 is in use by gnome-shell (pid 2345), "
                         "firefox (pid 2400)"));
        /* an ALSA client of its audio that is no sound server */
        writeFile(path("/users"), "c 116:11 3002 audacity\n");
        QCOMPARE(refusal("vfio 0000:6e:00.0,0000:6e:00.1 3072"),
                 QString("error vfio 0000:6e:00.0: 0000:6e:00.1 is in use by audacity (pid 3002)"));
        /* the keyboard on a USB controller: logind and the compositor have it */
        writeFile(path("/users"), "c 13:69 812 systemd-logind\nc 13:69 2345 gnome-shell\n");
        QCOMPARE(refusal("vfio 0000:07:00.0 1024"),
                 QString("error vfio 0000:07:00.0: 0000:07:00.0 is in use by systemd-logind (pid 812), "
                         "gnome-shell (pid 2345)"));
        QFile::remove(path("/users"));
        /* a network interface up */
        writeFile(pciDir("0000:02:00.0") + "/net/enp2s0/operstate", "up\n");
        QCOMPARE(refusal("vfio 0000:02:00.0 1024"),
                 QString("error vfio 0000:02:00.0: the network interface enp2s0 of 0000:02:00.0 is up: "
                         "take it down first (sudo ip link set enp2s0 down)"));
        writeFile(pciDir("0000:02:00.0") + "/net/enp2s0/operstate", "down\n");
        /* platform devices, a bridge */
        QCOMPARE(refusal("vfio 0000:00:14.0 1024"),
                 QString("error vfio 0000:00:14.0: 0000:00:14.0 is the SMBus controller: it stays with "
                         "this computer"));
        QCOMPARE(refusal("vfio 0000:00:01.1,0000:02:00.0 1024"),
                 QString("error vfio 0000:00:01.1: 0000:00:01.1 is a bridge: it stays with this "
                         "computer"));
        /* the whole group or nothing: the GPU without its audio */
        QCOMPARE(refusal("vfio 0000:6e:00.0 3072"),
                 QString("error vfio 0000:6e:00.0: 0000:6e:00.1 shares IOMMU group 21 and is bound to "
                         "snd_hda_intel: pass it through too"));
        /* NVIDIA's own nodes, which sysfs does not list */
        pci("0000:0a:00.0", "0x030000", "nvidia", 40);
        writeFile(path("/proc/devices"), "Character devices:\n  1 mem\n195 nvidia\n195 nvidia-modeset\n"
                                         "\nBlock devices:\n  8 sd\n");
        writeFile(path("/users"), "c 195:255 4000 nvidia-smi\n");
        QCOMPARE(refusal("vfio 0000:0a:00.0 1024"),
                 QString("error vfio 0000:0a:00.0: 0000:0a:00.0 is in use by nvidia-smi (pid 4000)"));
        QFile::remove(path("/users"));
    }

    /*
     * The sound servers let go of a card that goes: theirs is no use that
     * keeps it - told by the program they run, not by the name they give
     */
    void vfioSoundServersLetGo()
    {
        pciTree();
        /* a program that calls itself pipewire */
        writeFile(path("/users"), "c 116:10 3003 pipewire /home/someone/pipewire\n");
        QCOMPARE(refusal("vfio 0000:6e:00.0,0000:6e:00.1 3072"),
                 QString("error vfio 0000:6e:00.0: 0000:6e:00.1 is in use by pipewire (pid 3003)"));
        writeFile(path("/users"), "c 116:10 3000 wireplumber /usr/bin/wireplumber\n"
                                  "c 116:11 3001 pipewire /usr/bin/pipewire\n");
        Helper h(m_root, {}, HELPER_FAKE, {"devices-bind"});
        QVERIFY(h.ready());
        QVERIFY(h.answer("vfio 0000:6e:00.0,0000:6e:00.1 3072").startsWith("ok vfio "));
        QCOMPARE(driverOf("0000:6e:00.1"), QString("vfio-pci"));
        h.ask("release");
        QVERIFY(h.finished());
        QCOMPARE(driverOf("0000:6e:00.1"), QString("snd_hda_intel"));
    }

    /* A disk mounted (by number, by source as btrfs's), swapped on, under LUKS */
    void vfioRefusesDisksInUse()
    {
        pciTree();
        const QString disk = pciDir("0000:04:00.0") + "/nvme/nvme0/nvme0n1";
        writeFile(path("/proc/self/mountinfo"), "22 1 0:21 / / rw - btrfs /dev/sda2 rw\n"
                                                "30 22 259:2 / /home rw - ext4 /dev/nvme0n1p2 rw\n");
        QCOMPARE(refusal("vfio 0000:04:00.0 1024"),
                 QString("error vfio 0000:04:00.0: nvme0n1p2 on 0000:04:00.0 is mounted on /home"));
        writeFile(path("/proc/self/mountinfo"), "22 1 0:21 / / rw - btrfs /dev/sda2 rw\n"
                                                "31 22 0:40 /@data /data rw - btrfs /dev/nvme0n1p1 rw\n");
        writeFile(path("/dev/nvme0n1p1.rdev"), "b 259:1\n");
        QCOMPARE(refusal("vfio 0000:04:00.0 1024"),
                 QString("error vfio 0000:04:00.0: nvme0n1p1 on 0000:04:00.0 is mounted on /data"));
        writeFile(path("/proc/self/mountinfo"), "22 1 0:21 / / rw - btrfs /dev/sda2 rw\n");
        writeFile(path("/proc/swaps"), "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n"
                                       "/dev/nvme0n1p2                          partition\t8388604\t\t0\t\t-2\n");
        writeFile(path("/dev/nvme0n1p2.rdev"), "b 259:2\n");
        QCOMPARE(refusal("vfio 0000:04:00.0 1024"),
                 QString("error vfio 0000:04:00.0: nvme0n1p2 on 0000:04:00.0 is in use as swap"));
        QFile::remove(path("/proc/swaps"));
        writeFile(disk + "/nvme0n1p2/holders/dm-0", "");
        QCOMPARE(refusal("vfio 0000:04:00.0 1024"),
                 QString("error vfio 0000:04:00.0: nvme0n1p2 on 0000:04:00.0 is in use by dm-0 (an "
                         "encrypted or LVM volume, RAID)"));
        QFile::remove(disk + "/nvme0n1p2/holders/dm-0");
        /* none of that: it goes */
        Helper h(m_root, {}, HELPER_FAKE, {"devices-bind"});
        QVERIFY(h.ready());
        QVERIFY(h.answer("vfio 0000:04:00.0 1024").startsWith("ok vfio 0000:04:00.0=nvme: "));
        h.ask("release");
        QVERIFY(h.finished());
        QCOMPARE(driverOf("0000:04:00.0"), QString("nvme"));
    }

    /* A group another VM has, or another program */
    void vfioGroupInUse()
    {
        pciTree();
        {
            FakeQemu other(FAKE_QEMU, {"FAKE_QEMU_HOLD=" + path("/dev/vfio/14")});
            QCOMPARE(refusal("vfio 0000:01:00.0,0000:01:00.1 3072", "devices"),
                     QString("error vfio 0000:01:00.0: IOMMU group 14 is in use by another program"));
            writeFile(path("/users"), QString("c 239:14 %1 qemu-system-x86\n").arg(other.pid()).toLatin1());
            QCOMPARE(refusal("vfio 0000:01:00.0,0000:01:00.1 3072", "devices"),
                     QString("error vfio 0000:01:00.0: IOMMU group 14 is in use by qemu-system-x86 (pid "
                             "%1)").arg(other.pid()));
            QFile::remove(path("/users"));
        }
        /* another VM of vitrine's starting, its helper holding the group */
        Helper a(m_root, {}, HELPER_FAKE, {"devices"});
        QVERIFY(a.ready());
        QVERIFY(a.answer("vfio 0000:01:00.0,0000:01:00.1 3072").startsWith("ok vfio "));
        Helper b(m_root, {}, HELPER_FAKE, {"devices"});
        QVERIFY(b.ready());
        QCOMPARE(b.answer("vfio 0000:01:00.0,0000:01:00.1 2048"),
                 QString("error vfio 0000:01:00.0: IOMMU group 14 is in use by another VM"));
        QCOMPARE(nodeOwner(14), nodeMine());
        a.ask("release");
        QVERIFY(a.finished());
        QCOMPARE(nodeOwner(14), QByteArray("0 0 600"));
        b.closeInput();
        QVERIFY(b.finished());
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    void vfioValidation()
    {
        pciTree();
        Helper h(m_root, {}, HELPER_FAKE, {"devices-bind"});
        QVERIFY(h.ready());
        const QString addresses("error vfio: give the PCI addresses as 0000:00:00.0, separated by commas");
        for (const char *bad : {"6e:00.0", "0000:6E:00.0", "0000:6e:20.0", "0000:6e:00.8",
                                "0000:6e:00.0,", ",0000:6e:00.0", "0000:6e:00.0,,0000:6e:00.1",
                                "../../../xx", "0000:6e:00.0/", "0000:6e:00.00", "00000:6e:00.0"}) {
            QCOMPARE(h.answer(QString("vfio %1 1024").arg(bad)), addresses);
        }
        const QString memory("error vfio: give the memory to lock in MiB");
        for (const char *bad : {"0", "-1", "abc", "01024", "1e3", "99999999999999999999"}) {
            QCOMPARE(h.answer(QString("vfio 0000:6e:00.0,0000:6e:00.1 %1").arg(bad)), memory);
        }
        QCOMPARE(h.answer("vfio 0000:99:00.0 1024"), QString("error vfio 0000:99:00.0: no such PCI device"));
        /* a device without an IOMMU group: the IOMMU is off */
        writeFile(pciDir("0000:08:00.0") + "/class", "0x020000\n");
        QFile::link(pciDir("0000:08:00.0"), path("/sys/bus/pci/devices/0000:08:00.0"));
        QCOMPARE(h.answer("vfio 0000:08:00.0 1024"),
                 QString("error vfio 0000:08:00.0: it has no IOMMU group: the IOMMU is off"));
        QCOMPARE(h.answer("vfio 0000:6e:00.0"), QString("error vfio: unknown request"));
        QCOMPARE(h.answer("vfio"), QString("error vfio: unknown request"));
        /* the host settings are another session's */
        QCOMPARE(h.answer("fair-server on"), QString("error fair-server: unknown request"));
        QCOMPARE(h.answer("rt 1"), QString("error rt: unknown request"));
        QVERIFY(writes().isEmpty());
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        QCOMPARE(memlock(), QByteArray("8388608 8388608"));
    }

    /* No QEMU came in time: back, and the helper waits on for its input */
    void vfioNoQemuCame()
    {
        pciTree();
        Helper h(m_root, {"VITRINE_HELPER_TEST_VFIO_EXPIRY_MS=300"}, HELPER_FAKE, {"devices-bind"});
        QVERIFY(h.ready());
        QVERIFY(h.answer("vfio 0000:6e:00.0,0000:6e:00.1 3072").startsWith("ok vfio "));
        QCOMPARE(h.waitFor("restored vfio"),
                 QString("restored vfio 21: 0000:6e:00.0 back to amdgpu, 0000:6e:00.1 back to "
                         "snd_hda_intel (no QEMU came for it)"));
        QCOMPARE(h.line(), QString("restored memlock %1: 8 MiB").arg(getpid()));
        QVERIFY(h.running());
        QCOMPARE(driverOf("0000:6e:00.0"), QString("amdgpu"));
        QCOMPARE(memlock(), QByteArray("8388608 8388608"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        h.closeInput();
        QVERIFY(h.finished());
    }

    /* The VM's start failed before its QEMU (or vitrine went): its input ends */
    void vfioInputClosed()
    {
        pciTree();
        Helper h(m_root, {}, HELPER_FAKE, {"devices-bind"});
        QVERIFY(h.ready());
        QVERIFY(h.answer("vfio 0000:6e:00.0,0000:6e:00.1 3072").startsWith("ok vfio "));
        h.closeInput();
        QVERIFY(h.finished());
        const QStringList all = h.all();
        QVERIFY2(all.contains("restored vfio 21: 0000:6e:00.0 back to amdgpu, 0000:6e:00.1 back to "
                              "snd_hda_intel"), qPrintable(all.join('\n')));
        QVERIFY(all.contains(QString("restored memlock %1: 8 MiB").arg(getpid())));
        QCOMPARE(all.last(), QString("bye"));
        QCOMPARE(driverOf("0000:6e:00.1"), QString("snd_hda_intel"));
        QCOMPARE(memlock(), QByteArray("8388608 8388608"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    /*
     * Released while its QEMU has the group open: nothing unbound (it would
     * wait in the kernel), the node root's again, the record kept; the next
     * helper, of whatever verb, gives it back once it is free
     */
    void vfioReleasedWhileInUse()
    {
        pciTree();
        FakeQemu qemu;
        {
            Helper h(m_root, {}, HELPER_FAKE, {"devices-bind"});
            QVERIFY(h.ready());
            QVERIFY(h.answer("vfio 0000:6e:00.0,0000:6e:00.1 3072").startsWith("ok vfio "));
            FakeQemu holder(FAKE_QEMU, {"FAKE_QEMU_HOLD=" + path("/dev/vfio/21")});
            h.answer("watch " + qemu.pidText());
            clearJournal();
            h.ask("release");
            QVERIFY(h.finished());
            const QStringList all = h.all();
            QVERIFY2(all.contains("left vfio 21: IOMMU group 21 is in use by another program: it stays "
                                  "on vfio-pci until a later vitrine-helper finds it free"),
                     qPrintable(all.join('\n')));
            QVERIFY(!journal().join('\n').contains("WHILE IN USE"));
            QVERIFY(writes().isEmpty());
            QCOMPARE(driverOf("0000:6e:00.0"), QString("vfio-pci"));
            QCOMPARE(nodeOwner(21), QByteArray("0 0 600"));
            QVERIFY(stateExists("vfio-21"));
        }
        /* free now: the next helper's start - a tuning session - gives it back */
        Helper next(m_root);
        QStringList before;
        for (QString l = next.line(); !l.isNull() && !l.startsWith("ready"); l = next.line()) {
            before << l;
        }
        QVERIFY2(before.contains("restored vfio 21: 0000:6e:00.0 back to amdgpu, 0000:6e:00.1 back to "
                                 "snd_hda_intel"), qPrintable(before.join('\n')));
        QCOMPARE(driverOf("0000:6e:00.0"), QString("amdgpu"));
        QCOMPARE(driverOf("0000:6e:00.1"), QString("snd_hda_intel"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        next.closeInput();
        QVERIFY(next.finished());
    }

    /* Its QEMU ended, and something else has the group open: left, recorded */
    void vfioHeldAfterItsQemu()
    {
        pciTree();
        Helper h(m_root, {}, HELPER_FAKE, {"devices-bind"});
        QVERIFY(h.ready());
        QVERIFY(h.answer("vfio 0000:6e:00.0,0000:6e:00.1 3072").startsWith("ok vfio "));
        FakeQemu qemu;
        h.answer("watch " + qemu.pidText());
        FakeQemu other(FAKE_QEMU, {"FAKE_QEMU_HOLD=" + path("/dev/vfio/21")});
        writeFile(path("/users"), QString("c 239:21 %1 qemu-system-x86\n").arg(other.pid()).toLatin1());
        qemu.stop();
        QCOMPARE(h.waitFor("left vfio"),
                 QString("left vfio 21: IOMMU group 21 is in use by qemu-system-x86 (pid %1): it stays "
                         "on vfio-pci until a later vitrine-helper finds it free").arg(other.pid()));
        QVERIFY(h.finished());
        QCOMPARE(driverOf("0000:6e:00.0"), QString("vfio-pci"));
        QVERIFY(stateExists("vfio-21"));
        QVERIFY(!journal().join('\n').contains("WHILE IN USE"));
    }

    /* A driver that does not take its device back: said, its override as it was */
    void vfioDriverDidNotComeBack()
    {
        pciTree();
        Helper h(m_root, {}, HELPER_FAKE, {"devices-bind"});
        QVERIFY(h.ready());
        QVERIFY(h.answer("vfio 0000:6e:00.0,0000:6e:00.1 3072").startsWith("ok vfio "));
        writeFile(path("/sys/bus/pci/drivers/amdgpu/bind.refuse"), "");
        h.ask("release");
        QVERIFY(h.finished());
        const QStringList all = h.all();
        QVERIFY2(all.contains("restored vfio 21: 0000:6e:00.1 back to snd_hda_intel"),
                 qPrintable(all.join('\n')));
        QVERIFY(all.contains("left vfio 21: 0000:6e:00.0 has no driver: amdgpu did not take it back"));
        QCOMPARE(driverOf("0000:6e:00.0"), QString());
        QCOMPARE(overrideOf("0000:6e:00.0"), QByteArray("(null)"));
        /* not the kernel's choice: another driver could take it (radeon,
           nouveau, vfio-pci by ids=) */
        QVERIFY(writes().filter("drivers_probe").isEmpty());
        QCOMPARE(stateFiles(), QStringList({"lock"}));

        /* its module unloaded meanwhile: the kernel finds it one */
        makeTree();
        pciTree();
        Helper again(m_root, {}, HELPER_FAKE, {"devices-bind"});
        QVERIFY(again.ready());
        QVERIFY(again.answer("vfio 0000:6e:00.0,0000:6e:00.1 3072").startsWith("ok vfio "));
        QDir(path("/sys/bus/pci/drivers/amdgpu")).removeRecursively();
        writeFile(pciDir("0000:6e:00.0") + "/fake-native", "amdgpu-next\n");
        QDir().mkpath(path("/sys/bus/pci/drivers/amdgpu-next"));
        clearJournal();
        again.ask("release");
        QVERIFY(again.finished());
        QVERIFY(writes().contains("/sys/bus/pci/drivers_probe 0000:6e:00.0"));
        QCOMPARE(driverOf("0000:6e:00.0"), QString("amdgpu-next"));
        QVERIFY(again.all().contains("left vfio 21: 0000:6e:00.0 is on amdgpu-next: amdgpu did not "
                                     "take it back"));
    }

    /* vfio-pci refuses a device: all of it back as it was, without a word of "restored" */
    void vfioBindRefused()
    {
        pciTree();
        writeFile(path("/sys/bus/pci/drivers/vfio-pci/bind.refuse"), "");
        Helper h(m_root, {}, HELPER_FAKE, {"devices-bind"});
        QVERIFY(h.ready());
        QCOMPARE(h.answer("vfio 0000:6e:00.0,0000:6e:00.1 3072"),
                 QString("error vfio 0000:6e:00.0: vfio-pci did not take 0000:6e:00.0: Invalid argument"));
        QCOMPARE(driverOf("0000:6e:00.0"), QString("amdgpu"));
        QCOMPARE(driverOf("0000:6e:00.1"), QString("snd_hda_intel"));
        QCOMPARE(overrideOf("0000:6e:00.0"), QByteArray("(null)"));
        QCOMPARE(memlock(), QByteArray("8388608 8388608"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        h.ask("release");
        QVERIFY(h.finished());
        QVERIFY(h.all().filter("restored").isEmpty());
    }

    /* vfio-pci loaded when it is not; nothing done when it cannot be */
    void vfioLoadsVfioPci()
    {
        pciTree();
        QDir(path("/sys/bus/pci/drivers/vfio-pci")).removeRecursively();
        {
            Helper h(m_root, {}, HELPER_FAKE, {"devices-bind"});
            QVERIFY(h.ready());
            QVERIFY(h.answer("vfio 0000:6e:00.0,0000:6e:00.1 3072").startsWith("ok vfio "));
            const QStringList j = journal().filter(QRegularExpression("^(modprobe|write) "));
            QCOMPARE(j.first(), QString("modprobe vfio-pci"));
            h.ask("release");
            QVERIFY(h.finished());
        }
        makeTree();
        pciTree();
        QDir(path("/sys/bus/pci/drivers/vfio-pci")).removeRecursively();
        writeFile(path("/modprobe.refuse"), "");
        QCOMPARE(refusal("vfio 0000:6e:00.0,0000:6e:00.1 3072"),
                 QString("error vfio: cannot load vfio-pci: modprobe: FATAL: Module vfio-pci not found"));
        QCOMPARE(driverOf("0000:6e:00.0"), QString("amdgpu"));
    }

    /*
     * Killed anywhere in the way: the next helper, of any verb, gives back
     * what the records say - the devices, the node, the parent's limit
     */
    void vfioKilledHalfWay_data()
    {
        QTest::addColumn<int>("at");
        /* each device's override, unbind and bind; the node's owner; the limit */
        for (int at = 1; at <= 8; at++) {
            QTest::addRow("write %d", at) << at;
        }
    }

    void vfioKilledHalfWay()
    {
        QFETCH(int, at);
        pciTree();
        {
            Helper h(m_root, {QString("VITRINE_HELPER_TEST_KILL_AT=%1").arg(at)}, HELPER_FAKE,
                     {"devices-bind"});
            QVERIFY(h.ready());
            h.ask("vfio 0000:6e:00.0,0000:6e:00.1 3072");
            QVERIFY(h.finished());
        }
        QVERIFY(journal().contains("killed"));
        Helper next(m_root, {}, HELPER_FAKE, {"devices"});
        QVERIFY(next.ready());
        QCOMPARE(driverOf("0000:6e:00.0"), QString("amdgpu"));
        QCOMPARE(driverOf("0000:6e:00.1"), QString("snd_hda_intel"));
        QCOMPARE(overrideOf("0000:6e:00.0"), QByteArray("(null)"));
        QCOMPARE(overrideOf("0000:6e:00.1"), QByteArray("(null)"));
        QVERIFY(!nodeExists(21));
        QCOMPARE(memlock(), QByteArray("8388608 8388608"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        next.closeInput();
        QVERIFY(next.finished());
    }

    /*
     * Two VMs starting side by side raise the same parent's limit: the first
     * one saves it, the higher value holds, the last one out puts it back
     */
    void vfioLimitShared()
    {
        pciTree();
        Helper a(m_root, {}, HELPER_FAKE, {"devices"});
        Helper b(m_root, {}, HELPER_FAKE, {"devices-bind"});
        QVERIFY(a.ready());
        QVERIFY(b.ready());
        QVERIFY(a.answer("vfio 0000:01:00.0,0000:01:00.1 3072").endsWith(", locked memory 3072 MiB (was 8 MiB)"));
        QVERIFY(b.answer("vfio 0000:07:00.0 5120").endsWith(", locked memory 5120 MiB (was 8 MiB)"));
        QCOMPARE(memlock(), QByteArray("5368709120 5368709120"));
        FakeQemu q1, q2;
        QCOMPARE(a.answer("watch " + q1.pidText()), "ok watch " + q1.pidText());
        /*
         * One request more, answered once the watch before it is done: its
         * reply comes before the limit's hold is let go, and the last helper
         * out is the one that says it put the limit back
         */
        QCOMPARE(a.answer("watch " + q1.pidText()), "ok watch " + q1.pidText() + ": already");
        /* the second VM's QEMU is not started yet: the limit stays */
        QCOMPARE(memlock(), QByteArray("5368709120 5368709120"));
        QCOMPARE(b.answer("watch " + q2.pidText()), "ok watch " + q2.pidText());
        QCOMPARE(b.line(), QString("restored memlock %1: 8 MiB").arg(getpid()));
        QCOMPARE(memlock(), QByteArray("8388608 8388608"));
        a.ask("release");
        b.ask("release");
        QVERIFY(a.finished());
        QVERIFY(b.finished());
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        QCOMPARE(nodeOwner(14), QByteArray("0 0 600"));
        QCOMPARE(driverOf("0000:07:00.0"), QString("xhci_hcd"));
    }

    /* More memory than the computer has, with a virtual IOMMU say: capped, and said */
    void vfioLimitCapped()
    {
        pciTree();
        Helper h(m_root, {}, HELPER_FAKE, {"devices"});
        QVERIFY(h.ready());
        QCOMPARE(h.answer("vfio 0000:01:00.0,0000:01:00.1 40000"),
                 QString("ok vfio 0000:01:00.0=vfio-pci,0000:01:00.1=vfio-pci: IOMMU group 14 on "
                         "vfio-pci, /dev/vfio/14 for uid %1, locked memory 33024 MiB (was 8 MiB), "
                         "capped at this computer's memory and 1 GiB")
                     .arg(getuid()));
        QCOMPARE(memlock(), QByteArray::number(33024LL << 20) + ' ' + QByteArray::number(33024LL << 20));
        h.ask("release");
        QVERIFY(h.finished());
        QCOMPARE(memlock(), QByteArray("8388608 8388608"));
    }

    /*
     * The give-back checks it is the device it took: not one unplugged since,
     * nor another one at its address
     */
    void vfioDeviceGoneOrAnother()
    {
        pciTree();
        {
            Helper h(m_root, {}, HELPER_FAKE, {"devices-bind"});
            QVERIFY(h.ready());
            QVERIFY(h.answer("vfio 0000:6e:00.0,0000:6e:00.1 3072").startsWith("ok vfio "));
            /* unplugged */
            QFile::remove(path("/sys/bus/pci/devices/0000:6e:00.0"));
            QFile::remove(path("/sys/kernel/iommu_groups/21/devices/0000:6e:00.0"));
            QDir(pciDir("0000:6e:00.0")).removeRecursively();
            clearJournal();
            h.ask("release");
            QVERIFY(h.finished());
            const QStringList all = h.all();
            QVERIFY2(all.contains("left vfio 21: 0000:6e:00.0 is gone"), qPrintable(all.join('\n')));
            QVERIFY(all.contains("restored vfio 21: 0000:6e:00.1 back to snd_hda_intel"));
            QVERIFY(writes().filter("0000:6e:00.0").isEmpty());
            QCOMPARE(stateFiles(), QStringList({"lock"}));
        }
        makeTree();
        pciTree();
        Helper h(m_root, {}, HELPER_FAKE, {"devices-bind"});
        QVERIFY(h.ready());
        QVERIFY(h.answer("vfio 0000:6e:00.0,0000:6e:00.1 3072").startsWith("ok vfio "));
        /* another device at its address since (hot-plugged), left alone */
        writeFile(pciDir("0000:6e:00.0") + "/device", "0x1234\n");
        clearJournal();
        h.ask("release");
        QVERIFY(h.finished());
        const QStringList all = h.all();
        QVERIFY2(all.contains("left vfio 21: 0000:6e:00.0 is another device now (1002:1234, it was "
                              "1002:164e): left as it is"),
                 qPrintable(all.join('\n')));
        QVERIFY(writes().filter("0000:6e:00.0").isEmpty());
        QCOMPARE(driverOf("0000:6e:00.0"), QString("vfio-pci"));
        QCOMPARE(driverOf("0000:6e:00.1"), QString("snd_hda_intel"));
    }

    /*
     * A failure after the devices are on vfio-pci - the node's owner, the
     * limit: all of it back, in the order it is given back (function 0
     * first), without a word of "restored"
     */
    void vfioFailsLate()
    {
        pciTree();
        for (const char *refuse : {"/chown.refuse", "/memlock.refuse"}) {
            writeFile(path(refuse), "");
            Helper h(m_root, {}, HELPER_FAKE, {"devices-bind"});
            QVERIFY(h.ready());
            const QString got = h.answer("vfio 0000:6e:00.0,0000:6e:00.1 3072");
            QVERIFY2(got.startsWith(QString(refuse) == "/chown.refuse"
                                        ? "error vfio: cannot give /dev/vfio/21 to uid "
                                        : "error vfio: cannot raise the locked memory limit: "),
                     qPrintable(got));
            QCOMPARE(driverOf("0000:6e:00.0"), QString("amdgpu"));
            QCOMPARE(driverOf("0000:6e:00.1"), QString("snd_hda_intel"));
            QVERIFY(!nodeExists(21));
            QCOMPARE(memlock(), QByteArray("8388608 8388608"));
            QCOMPARE(stateFiles(), QStringList({"lock"}));
            const QStringList w = writes();
            const qsizetype gpu = w.indexOf("/sys/bus/pci/drivers/vfio-pci/unbind 0000:6e:00.0");
            const qsizetype audio = w.indexOf("/sys/bus/pci/drivers/vfio-pci/unbind 0000:6e:00.1");
            QVERIFY2(gpu >= 0 && audio > gpu, qPrintable(w.join('\n')));
            h.ask("release");
            QVERIFY(h.finished());
            QVERIFY(h.all().filter("restored").isEmpty());
            QFile::remove(path(refuse));
            clearJournal();
        }
    }

    /* A driver_override of its own and pci-stub: given back as they were */
    void vfioOverrideAndStub()
    {
        pciTree();
        writeFile(pciDir("0000:6e:00.0") + "/driver_override", "amdgpu\n");
        pci("0000:0b:00.0", "0x0c0330", "pci-stub", 41);
        Helper h(m_root, {}, HELPER_FAKE, {"devices-bind"});
        QVERIFY(h.ready());
        QCOMPARE(h.answer("vfio 0000:0b:00.0 1024"),
                 QString("ok vfio 0000:0b:00.0=pci-stub: IOMMU group 41 on vfio-pci, /dev/vfio/41 "
                         "for uid %1, locked memory 1024 MiB (was 8 MiB)").arg(getuid()));
        QVERIFY(h.answer("vfio 0000:6e:00.0,0000:6e:00.1 3072").startsWith("ok vfio "));
        QCOMPARE(driverOf("0000:0b:00.0"), QString("vfio-pci"));
        QCOMPARE(overrideOf("0000:6e:00.0"), QByteArray("vfio-pci"));
        h.ask("release");
        QVERIFY(h.finished());
        QCOMPARE(driverOf("0000:0b:00.0"), QString("pci-stub"));
        QCOMPARE(overrideOf("0000:0b:00.0"), QByteArray("(null)"));
        QCOMPARE(driverOf("0000:6e:00.0"), QString("amdgpu"));
        QCOMPARE(overrideOf("0000:6e:00.0"), QByteArray("amdgpu"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
    }

    /* Killed between the record and its first write: the next helper finds nothing to undo */
    void vfioKilledBeforeAnyWrite()
    {
        pciTree();
        {
            Helper h(m_root, {"VITRINE_HELPER_TEST_KILL_BEFORE=driver_override"}, HELPER_FAKE,
                     {"devices-bind"});
            QVERIFY(h.ready());
            h.ask("vfio 0000:6e:00.0,0000:6e:00.1 3072");
            QVERIFY(h.finished());
        }
        QVERIFY(stateExists("vfio-21"));
        QVERIFY(writes().isEmpty());
        Helper next(m_root, {}, HELPER_FAKE, {"devices"});
        QStringList before;
        for (QString l = next.line(); !l.isNull() && !l.startsWith("ready"); l = next.line()) {
            before << l;
        }
        QVERIFY2(before.contains("restored vfio 21: 0000:6e:00.0 back to amdgpu, 0000:6e:00.1 back to "
                                 "snd_hda_intel"), qPrintable(before.join('\n')));
        QCOMPARE(driverOf("0000:6e:00.0"), QString("amdgpu"));
        QCOMPARE(overrideOf("0000:6e:00.0"), QByteArray("(null)"));
        QCOMPARE(stateFiles(), QStringList({"lock"}));
        next.closeInput();
        QVERIFY(next.finished());
    }

    /*
     * A second helper's raise that fails leaves the record saying what is in
     * place: the last one out still puts the limit back
     */
    void vfioLimitRaiseFails()
    {
        pciTree();
        Helper a(m_root, {}, HELPER_FAKE, {"devices"});
        Helper b(m_root, {}, HELPER_FAKE, {"devices-bind"});
        QVERIFY(a.ready());
        QVERIFY(b.ready());
        QVERIFY(a.answer("vfio 0000:01:00.0,0000:01:00.1 3072").startsWith("ok vfio "));
        writeFile(path("/memlock.refuse"), "");
        QCOMPARE(b.answer("vfio 0000:07:00.0 5120"),
                 QString("error vfio: cannot raise the locked memory limit: Operation not permitted"));
        QFile::remove(path("/memlock.refuse"));
        QCOMPARE(memlock(), QByteArray("3221225472 3221225472"));
        QCOMPARE(driverOf("0000:07:00.0"), QString("xhci_hcd"));
        FakeQemu qemu;
        QCOMPARE(a.answer("watch " + qemu.pidText()), "ok watch " + qemu.pidText());
        /* answered once the watch before it is done */
        QCOMPARE(a.answer("watch " + qemu.pidText()), "ok watch " + qemu.pidText() + ": already");
        QCOMPARE(memlock(), QByteArray("3221225472 3221225472"));
        b.closeInput();
        QVERIFY(b.finished());
        QVERIFY2(b.all().contains(QString("restored memlock %1: 8 MiB").arg(getpid())),
                 qPrintable(b.all().join('\n')));
        QCOMPARE(memlock(), QByteArray("8388608 8388608"));
        a.ask("release");
        QVERIFY(a.finished());
    }

    /*
     * Its time up while something has the group open (its QEMU, the watch
     * late): the group waits on, the parent's limit goes back at once
     */
    void vfioLimitBackAtExpiry()
    {
        pciTree();
        Helper h(m_root, {"VITRINE_HELPER_TEST_VFIO_EXPIRY_MS=300"}, HELPER_FAKE, {"devices"});
        QVERIFY(h.ready());
        QVERIFY(h.answer("vfio 0000:01:00.0,0000:01:00.1 3072").startsWith("ok vfio "));
        FakeQemu holder(FAKE_QEMU, {"FAKE_QEMU_HOLD=" + path("/dev/vfio/14")});
        QCOMPARE(h.waitFor("restored memlock"), QString("restored memlock %1: 8 MiB").arg(getpid()));
        QCOMPARE(memlock(), QByteArray("8388608 8388608"));
        QVERIFY(stateExists("vfio-14"));
        QCOMPARE(nodeOwner(14), nodeMine());
        /* closed: the next time, it goes */
        holder.stop();
        QCOMPARE(h.waitFor("restored vfio"),
                 QString("restored vfio 14: /dev/vfio/14 back to uid 0 (no QEMU came for it)"));
        QCOMPARE(nodeOwner(14), QByteArray("0 0 600"));
        h.closeInput();
        QVERIFY(h.finished());
    }

    /*
     * A driver slow to let go does not hold up the other helpers: the
     * global lock is not held across the binding (the group's hold keeps
     * them off the group)
     */
    void vfioBindingTakesNoLock()
    {
        pciTree();
        writeFile(path("/sys/bus/pci/drivers/amdgpu/unbind.slow"), "3000\n");
        FakeQemu qemu;
        Helper a(m_root, {}, HELPER_FAKE, {"devices-bind"});
        QVERIFY(a.ready());
        a.send("vfio 0000:6e:00.0,0000:6e:00.1 3072");
        /* well into amdgpu's unbind */
        QTRY_VERIFY(journal().contains("write /sys/bus/pci/devices/0000:6e:00.0/driver_override "
                                       "vfio-pci"));
        QElapsedTimer clock;
        clock.start();
        Helper b(m_root);
        QVERIFY(b.ready());
        QCOMPARE(b.answer("watch " + qemu.pidText()), "ok watch " + qemu.pidText());
        QVERIFY(b.answer("fair-server on").startsWith("ok fair-server on"));
        QVERIFY2(clock.elapsed() < 2000, qPrintable(QString::number(clock.elapsed())));
        /* another helper does not take the group meanwhile */
        Helper c(m_root, {}, HELPER_FAKE, {"devices-bind"});
        QVERIFY(c.ready());
        QCOMPARE(c.answer("vfio 0000:6e:00.0,0000:6e:00.1 2048"),
                 QString("error vfio 0000:6e:00.0: IOMMU group 21 is in use by another VM"));
        QVERIFY(a.waitFor("ok vfio ").startsWith("ok vfio 0000:6e:00.0=amdgpu"));
        a.ask("release");
        b.ask("release");
        c.closeInput();
        QVERIFY(a.finished());
        QVERIFY(b.finished());
        QVERIFY(c.finished());
        QCOMPARE(driverOf("0000:6e:00.0"), QString("amdgpu"));
    }

    void polkitFiles()
    {
        QFile policy(HELPER_POLICY);
        QVERIFY(policy.open(QIODevice::ReadOnly));
        QXmlStreamReader xml(&policy);
        /* action -> its annotations and defaults */
        QMap<QString, QMap<QString, QString>> actions;
        QString action;
        while (!xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement()) {
                continue;
            }
            if (xml.name() == u"action") {
                action = xml.attributes().value("id").toString();
                actions[action];
            } else if (xml.name() == u"annotate") {
                const QString key = xml.attributes().value("key").toString();
                actions[action][key] = xml.readElementText();
            } else if (xml.name().startsWith(u"allow_")) {
                const QString key = xml.name().toString();
                actions[action][key] = xml.readElementText();
            }
        }
        QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));
        /*
         * One action per verb, each with its argv1: pkexec takes the first
         * action whose path matches and whose argv1, if any, is the first
         * argument, in no fixed order - one without argv1 would match every
         * verb
         */
        const QMap<QString, QString> verbs{{"org.vitrine.helper", "session"},
                                           {"org.vitrine.helper.devices", "devices"},
                                           {"org.vitrine.helper.devices-bind", "devices-bind"},
                                           {"org.vitrine.helper.setup-group", "setup-group"}};
        QCOMPARE(actions.keys(), verbs.keys());
        for (const QString &id : verbs.keys()) {
            const auto a = actions[id];
            /* taking a device from the host: the password kept a few minutes */
            const QString wanted = id.endsWith(".devices-bind") ? "auth_admin_keep" : "auth_admin";
            QCOMPARE(a.value("org.freedesktop.policykit.exec.path"), QString(HELPER_PATH));
            QCOMPARE(a.value("org.freedesktop.policykit.exec.argv1"), verbs[id]);
            for (const char *when : {"allow_any", "allow_inactive", "allow_active"}) {
                QCOMPARE(a.value(when), wanted);
            }
        }
        /*
         * The group's rule: the session and devices; devices-bind only in a
         * comment (the user's choice: taking a device wants a password);
         * never setup-group (and no setcap, which is gone: QEMU gets no
         * capability)
         */
        const QByteArray rules = readFile(HELPER_RULES);
        QVERIFY(rules.contains("var granted = action.id == \"org.vitrine.helper\" ||\n"));
        QVERIFY(rules.contains("action.id == \"org.vitrine.helper.devices\";\n"));
        for (const QByteArray &line : rules.split('\n')) {
            if (line.contains("\"org.vitrine.helper.devices-bind\"")) {
                QVERIFY2(line.trimmed().startsWith("//"), line.constData());
            }
        }
        QVERIFY(rules.contains("if (granted && subject.local && subject.active && "
                               "subject.isInGroup(\"vitrine\"))"));
        QVERIFY(!rules.contains("\"org.vitrine.helper.setup-group\""));
        QVERIFY(!rules.contains("setcap"));
        QVERIFY(!rules.contains("indexOf") && !rules.contains("startsWith"));
    }
};

QTEST_GUILESS_MAIN(TestHelper)
#include "test_helper.moc"
