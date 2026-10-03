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
#include <QProcess>
#include <QRegularExpression>
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
static const char kCard[] = "/sys/class/drm/card1/device";

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
    Helper(const QString &root, const QStringList &extraEnv = {})
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
        m_process.start(HELPER_FAKE, {});
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
    /* the kernel's defaults and an APU like the Radeon 780M */
    void makeTree()
    {
        QDir(m_root).removeRecursively();
        QDir().mkpath(path("/run"));
        writeFile(path("/sys/kernel/security/lockdown"), "[none] integrity confidentiality\n");
        for (int i = 0; i < m_cpus; i++) {
            setFair(i, 1000000000, 50000000);
        }
        const QString card = path(kCard);
        writeFile(card + "/vendor", "0x1002\n");
        writeFile(card + "/power_dpm_force_performance_level", "auto\n");
        writeFile(card + "/pp_od_clk_voltage", od(800, 2700));
        /* gpu_metrics header: size 120, format 2 (an APU's), content 1 */
        writeFile(card + "/gpu_metrics", QByteArray("\x78\x00\x02\x01", 4) + QByteArray(116, 0));
        QFile::link("../../../../bus/pci/drivers/amdgpu", card + "/driver");
        writeFile(path("/sys/class/drm/card0/device/vendor"), "0x8086\n");
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
        char *argv[] = {const_cast<char *>(HELPER_FAKE), nullptr};
        pid_t pid;
        QCOMPARE(posix_spawn(&pid, HELPER_FAKE, &actions, nullptr, argv, environ), 0);
        posix_spawn_file_actions_destroy(&actions);
        qunsetenv("VITRINE_HELPER_TEST_ROOT");
        ::close(sv[1]);
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
        /* it ends; Qt's own SIGCHLD handling may reap it before this does */
        int status = 0;
        pid_t r = 0;
        QTRY_VERIFY((r = waitpid(pid, &status, WNOHANG)) != 0);
        if (r == pid) {
            QVERIFY(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        } else {
            QVERIFY(errno == ECHILD && !QFile::exists(QString("/proc/%1").arg(pid)));
        }
        ::close(sv[0]);
    }

    /* SIGTERM (a shutdown): everything back before the end */
    void terminated()
    {
        FakeQemu qemu;
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("VITRINE_HELPER_TEST_ROOT", m_root);
        p.setProcessEnvironment(env);
        p.start(HELPER_FAKE, {});
        QVERIFY(p.waitForStarted());
        p.write(QString("watch %1\nfair-server on\n").arg(qemu.pid()).toUtf8());
        QTRY_VERIFY(fairAll("10000000/1000000"));
        p.terminate();
        QVERIFY(p.waitForFinished());
        QVERIFY(fairAll("1000000000/50000000"));
        QVERIFY(QString::fromUtf8(p.readAllStandardOutput()).endsWith("bye\n"));
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
        QString action, execPath, active;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name() == u"action") {
                action = xml.attributes().value("id").toString();
            } else if (xml.isStartElement() && xml.name() == u"annotate" &&
                       xml.attributes().value("key") == u"org.freedesktop.policykit.exec.path") {
                execPath = xml.readElementText();
            } else if (xml.isStartElement() && xml.name() == u"allow_active") {
                active = xml.readElementText();
            }
        }
        QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));
        QCOMPARE(action, QString("org.vitrine.helper"));
        QCOMPARE(execPath, QString(HELPER_PATH));
        QCOMPARE(active, QString("auth_admin"));
        const QByteArray rules = readFile(HELPER_RULES);
        QVERIFY(rules.contains("action.id == \"org.vitrine.helper\""));
        QVERIFY(rules.contains("subject.isInGroup(\"vitrine\")"));
        QVERIFY(rules.contains("subject.local && subject.active"));
    }
};

QTEST_GUILESS_MAIN(TestHelper)
#include "test_helper.moc"
