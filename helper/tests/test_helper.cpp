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

#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static const char kFair[] = "/sys/kernel/debug/sched/fair_server";
static const char kExt[] = "/sys/kernel/debug/sched/ext_server";
static const char kCard[] = "/sys/class/drm/card1/device";
static const char kUdmabuf[] = "/sys/module/udmabuf/parameters";

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
    FakeQemu(const QString &program = FAKE_QEMU)
    {
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

/* One helper session */
class Helper
{
public:
    /* vitrine-helper-fake session, or @program @arguments that runs it */
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
        m_process.start(program, program == HELPER_FAKE ? QStringList{"session"} : arguments);
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
    /* The threads of @pid */
    static QStringList tids(qint64 pid)
    {
        return QDir(QString("/proc/%1/task").arg(pid)).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
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

    /* manual, s 0, s 1, c; back: r, c, the level it had */
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
        QCOMPARE(writes(), QStringList({QString(kCard) + "/pp_od_clk_voltage r",
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
     * s 1, c; back: r, c, the level - leaves the level at manual: the next
     * one puts it back, and a floor can be set again
     */
    void gpuFloorHalfWay_data()
    {
        QTest::addColumn<int>("killAt");
        QTest::addColumn<bool>("restoring");
        QTest::newRow("setting, after manual") << 1 << false;
        QTest::newRow("setting, after s 0") << 2 << false;
        QTest::newRow("setting, after s 1") << 3 << false;
        QTest::newRow("setting, after c") << 4 << false;
        QTest::newRow("restoring, after r") << 5 << true;
        QTest::newRow("restoring, after c") << 6 << true;
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
        for (int n = 1; n <= 8; n++) {
            QVERIFY(h.answer(QString("gpu-floor card%1 auto").arg(n)).startsWith("ok gpu-floor"));
        }
        QCOMPARE(h.answer("gpu-floor card9 auto"),
                 QString("error gpu-floor card9: cannot take the hold: No space left on device"));
        h.ask("release");
        QVERIFY(h.finished());
        QCOMPARE(udmabuf(), QString("1024/64"));
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

    void setcapValidation()
    {
        struct passwd *pw = getpwuid(getuid());
        QVERIFY(pw);
        const QString home = QDir(pw->pw_dir).canonicalPath();
        const QString work = QDir(TEST_WORK_DIR).absolutePath() + "/setcap";
        if (!work.startsWith(home + '/')) {
            QSKIP("the build folder is not in the home folder");
        }
        QDir(work).removeRecursively();
        const QString bin = work + "/data/vitrine/stack/0123abcd/bin";
        const QString qemu = bin + "/qemu-system-x86_64";
        QVERIFY(QDir().mkpath(bin));
        QVERIFY(QFile::copy(FAKE_QEMU, qemu));
        chmod(qPrintable(qemu), 0755);

        auto setcap = [this](const QString &target, QString *out = nullptr) {
            QProcess p;
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert("VITRINE_HELPER_TEST_ROOT", m_root);
            env.remove("PKEXEC_UID");
            env.remove("SUDO_UID");
            p.setProcessEnvironment(env);
            p.start(HELPER_FAKE, {"setcap", target});
            p.waitForFinished();
            if (out) {
                *out = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
            }
            return p.exitCode();
        };
        QString out;
        QCOMPARE(setcap(qemu, &out), 0);
        QCOMPARE(out, "ok setcap " + qemu + ": cap_sys_nice=ep, mode 0700");
        QVERIFY(journal().contains("setcap " + qemu));
        QCOMPARE(int(QFileInfo(qemu).permissions() & 0x0077), 0);
        struct stat st;
        QCOMPARE(stat(qPrintable(qemu), &st), 0);
        QCOMPARE(st.st_mode & 07777, mode_t(0700));

        auto refused = [&](const QString &target, const QString &why) {
            QString text;
            const int status = setcap(target, &text);
            if (status != 1 || text != "error setcap " + target + ": " + why) {
                qWarning() << target << text;
                return false;
            }
            return true;
        };
        QVERIFY(refused("relative/qemu-system-x86_64", "give an absolute path"));
        QVERIFY(refused("/tmp/vitrine/stack/x/bin/qemu-system-x86_64", "not in the caller's home folder"));
        QVERIFY(refused(bin + "/../bin/qemu-system-x86_64", "not a plain path"));
        QVERIFY(refused(bin + "//qemu-system-x86_64", "not a plain path"));
        QVERIFY(refused(work + "/data/vitrine/stack/bin/qemu-system-x86_64", "not a QEMU of vitrine's stack"));
        QVERIFY(refused(bin + "/qemu-system-X86", "not a QEMU of vitrine's stack"));
        QVERIFY(refused(bin + "/qemu-img", "not a QEMU of vitrine's stack"));
        QVERIFY(refused(bin + "/qemu-system-aarch64", "No such file or directory"));
        /* a link on the way, or as the file */
        QVERIFY(QFile::link(work + "/data/vitrine/stack/0123abcd", work + "/data/vitrine/stack/current"));
        QVERIFY(refused(work + "/data/vitrine/stack/current/bin/qemu-system-x86_64",
                        "a link in the path: give the real path"));
        QVERIFY(QFile::link(qemu, bin + "/qemu-system-i386"));
        QVERIFY(refused(bin + "/qemu-system-i386", "a link in the path: give the real path"));
        /* not an executable for this machine */
        QVERIFY(writeFile(bin + "/qemu-system-riscv64", "#!/bin/sh\n"));
        QVERIFY(refused(bin + "/qemu-system-riscv64", "not an executable for this machine"));
        /* a second link to the file */
        QVERIFY(::link(qPrintable(qemu), qPrintable(work + "/other")) == 0);
        QVERIFY(refused(qemu, "a file with other links or a setuid/setgid bit"));
        QFile::remove(work + "/other");
        /* a folder others may write to */
        chmod(qPrintable(bin), 0777);
        QVERIFY(refused(qemu, "a folder on the way is writable by others"));
        chmod(qPrintable(bin), 0755);
        /* setuid */
        chmod(qPrintable(qemu), 04700);
        QVERIFY(refused(qemu, "a file with other links or a setuid/setgid bit"));
        chmod(qPrintable(qemu), 0700);
        QCOMPARE(setcap(qemu), 0);
        /* another user asking */
        {
            QProcess p;
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert("VITRINE_HELPER_TEST_ROOT", m_root);
            env.insert("PKEXEC_UID", "0");
            p.setProcessEnvironment(env);
            p.start(HELPER_FAKE, {"setcap", qemu});
            p.waitForFinished();
            QCOMPARE(p.exitCode(), 1);
            QVERIFY(QString::fromUtf8(p.readAllStandardOutput()).contains("not in the caller's home folder"));
        }
        QDir(work).removeRecursively();
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
                                           {"org.vitrine.helper.setcap", "setcap"},
                                           {"org.vitrine.helper.setup-group", "setup-group"}};
        QCOMPARE(actions.keys(), verbs.keys());
        for (const QString &id : verbs.keys()) {
            const auto a = actions[id];
            QCOMPARE(a.value("org.freedesktop.policykit.exec.path"), QString(HELPER_PATH));
            QCOMPARE(a.value("org.freedesktop.policykit.exec.argv1"), verbs[id]);
            for (const char *when : {"allow_any", "allow_inactive", "allow_active"}) {
                QCOMPARE(a.value(when), QString("auth_admin"));
            }
        }
        /* the group's rule: session and setcap, never setup-group */
        const QByteArray rules = readFile(HELPER_RULES);
        QVERIFY(rules.contains("(action.id == \"org.vitrine.helper\" || "
                               "action.id == \"org.vitrine.helper.setcap\")"));
        QVERIFY(!rules.contains("\"org.vitrine.helper.setup-group\""));
        QVERIFY(!rules.contains("indexOf") && !rules.contains("startsWith"));
        QVERIFY(rules.contains("subject.isInGroup(\"vitrine\")"));
        QVERIFY(rules.contains("subject.local && subject.active"));
    }
};

QTEST_GUILESS_MAIN(TestHelper)
#include "test_helper.moc"
