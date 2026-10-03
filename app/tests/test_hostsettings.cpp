// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HostSettings with the helper's test build (vitrine-helper-fake) on a fake
 * sysfs/debugfs tree, a stand-in QEMU, and the preferences.
 */
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <csignal>
#include <memory>

#include <pwd.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "core/hostsettings.h"
#include "core/paths.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"

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
    return f.open(QIODevice::ReadOnly) ? f.readAll().trimmed() : QByteArray();
}

static QByteArray od(int min, int max)
{
    return QString("OD_SCLK:\n0:        %1Mhz\n1:       %2Mhz\nOD_RANGE:\n"
                   "SCLK:     800Mhz       2700Mhz\n")
        .arg(min).arg(max).toLatin1();
}

/* An AMD card under @sys: its gpu_metrics format revision (2: APU, 1: discrete) */
static void amdCard(const QString &sys, const QString &card, char format, bool overdrive = true)
{
    const QString device = sys + "/class/drm/" + card + "/device";
    writeFile(device + "/vendor", "0x1002\n");
    writeFile(device + "/power_dpm_force_performance_level", "auto\n");
    writeFile(device + "/gpu_metrics", QByteArray("\x78\x00", 2) + format + QByteArray("\x01", 1));
    if (overdrive) {
        writeFile(device + "/pp_od_clk_voltage", od(800, 2700));
    }
    QFile::link("../../../../bus/pci/drivers/amdgpu", device + "/driver");
}

/* QEMU's end of QMP: running, and taking every other command */
class FakeQmp : public QObject
{
public:
    explicit FakeQmp(const QString &path)
    {
        QLocalServer::removeServer(path);
        m_server.listen(path);
        connect(&m_server, &QLocalServer::newConnection, this, [this]() {
            m_peer = m_server.nextPendingConnection();
            connect(m_peer, &QLocalSocket::readyRead, this, &FakeQmp::read);
            m_peer->write(R"({"QMP": {"version": {"qemu": {"major": 11}}, "capabilities": []}})"
                          "\n");
        });
    }
    QList<QJsonObject> commands;

private:
    void read()
    {
        m_buffer += m_peer->readAll();
        qsizetype nl;
        while ((nl = m_buffer.indexOf('\n')) >= 0) {
            const QJsonObject command = QJsonDocument::fromJson(m_buffer.left(nl)).object();
            m_buffer.remove(0, nl + 1);
            const QByteArray result = command["execute"] == "query-status"
                                          ? R"({"running": true, "status": "running"})"
                                          : "{}";
            commands << command;
            m_peer->write("{\"return\": " + result + ", \"id\": " +
                          QByteArray::number(command["id"].toInteger()) + "}\n");
        }
    }

    QLocalServer m_server;
    QLocalSocket *m_peer = nullptr;
    QByteArray m_buffer;
};

class FakeQemu
{
public:
    explicit FakeQemu(const QStringList &arguments = {})
    {
        m_process.start(FAKE_QEMU, arguments);
        m_process.waitForStarted();
        m_pid = m_process.processId();
    }
    ~FakeQemu() { stop(); }
    qint64 pid() const { return m_pid; }
    void stop()
    {
        if (m_process.state() != QProcess::NotRunning) {
            m_process.kill();
            m_process.waitForFinished();
        }
    }

private:
    QProcess m_process;
    qint64 m_pid = 0;
};

class TestHostSettings : public QObject
{
    Q_OBJECT

private:
    QString m_root;
    QByteArray m_path = qgetenv("PATH");

    QString fair(int cpu) const
    {
        return QString("%1/%2").arg(
            readFile(QString("%1%2/cpu%3/period").arg(m_root, kFair).arg(cpu)),
            readFile(QString("%1%2/cpu%3/runtime").arg(m_root, kFair).arg(cpu)));
    }
    QString level() const
    {
        return readFile(m_root + kCard + "/power_dpm_force_performance_level");
    }
    QByteArray odTable() const { return readFile(m_root + kCard + "/pp_od_clk_voltage"); }
    /* A HostSettings on the fake tree, through the helper's test build */
    void fake(HostSettings &hs) const
    {
        hs.setHelperCommand({HELPER_FAKE});
        hs.setSysRoot(m_root + "/sys");
    }
    static bool saw(const QSignalSpy &spy, const QString &start)
    {
        for (const QList<QVariant> &args : spy) {
            if (args.value(0).toString().startsWith(start)) {
                return true;
            }
        }
        return false;
    }

private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void init()
    {
        /* its own keys only: the file is the app's (and, run by hand, may
           be another test's) */
        QSettings(Paths::settingsPath(), QSettings::IniFormat).remove("host");
        qunsetenv("VITRINE_HELPER");
        qputenv("PATH", m_path);
        m_root = QString(TEST_WORK_DIR) + "/hostsettings-" + QTest::currentTestFunction();
        QDir(m_root).removeRecursively();
        QDir().mkpath(m_root + "/run");
        writeFile(m_root + "/sys/kernel/security/lockdown", "[none] integrity confidentiality\n");
        for (int cpu = 0; cpu < 2; cpu++) {
            writeFile(QString("%1%2/cpu%3/period").arg(m_root, kFair).arg(cpu), "1000000000\n");
            writeFile(QString("%1%2/cpu%3/runtime").arg(m_root, kFair).arg(cpu), "50000000\n");
        }
        amdCard(m_root + "/sys", "card1", 2);
        writeFile(m_root + "/sys/class/drm/card0/device/vendor", "0x8086\n");
        qputenv("VITRINE_HELPER_TEST_ROOT", m_root.toLocal8Bit());
    }

    void preferences()
    {
        QVERIFY(HostSettings::enabled());
        QCOMPARE(HostSettings::gpuFloor(), QString("auto"));
        HostSettings::setGpuFloor("1500");
        QCOMPARE(HostSettings::gpuFloor(), QString("1500"));
        HostSettings::setGpuFloor("off");
        QCOMPARE(HostSettings::gpuFloor(), QString("off"));
        /* anything else is the default */
        HostSettings::setGpuFloor("1500; rm -rf");
        QCOMPARE(HostSettings::gpuFloor(), QString("auto"));
        HostSettings::setGpuFloor("0800");
        QCOMPARE(HostSettings::gpuFloor(), QString("auto"));
        HostSettings::setEnabled(false);
        QVERIFY(!HostSettings::enabled());
    }

    void amdCards()
    {
        const QString sys = m_root + "/sys";
        amdCard(sys, "card2", 1);                    // discrete
        amdCard(sys, "card3", 2, false);             // no overdrive table
        QDir().mkpath(sys + "/class/drm/card1-eDP-1");
        QCOMPARE(HostSettings::amdCards(sys, false), QStringList({"card1", "card2"}));
        QCOMPARE(HostSettings::amdCards(sys, true), QStringList({"card1"}));
        QVERIFY(HostSettings::amdCards(m_root + "/nothing", false).isEmpty());
    }

    /* A VM start: watched, tuned; its end: everything back, the helper gone */
    void tuneAndRevert()
    {
        FakeQemu qemu;
        HostSettings hs(nullptr);
        QSignalSpy lines(&hs, &HostSettings::helperLine), notices(&hs, &HostSettings::notice),
            finished(&hs, &HostSettings::helperFinished);
        fake(hs);
        hs.tune(qemu.pid());
        QTRY_VERIFY(saw(lines, QString("ok rt %1").arg(qemu.pid())));
        QVERIFY(saw(lines, "ok fair-server on: 2 cpus"));
        QVERIFY(saw(lines, "ok gpu-floor card1 1800 MHz"));
        QCOMPARE(fair(0), QString("10000000/1000000"));
        QCOMPARE(level(), QString("manual"));
        QCOMPARE(odTable(), od(1800, 2700).trimmed());
        QVERIFY(notices.isEmpty());
        QVERIFY(hs.helperRunning());

        qemu.stop();
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!hs.helperRunning());
        QCOMPARE(fair(0), QString("1000000000/50000000"));
        QCOMPARE(level(), QString("auto"));
        QCOMPARE(odTable(), od(800, 2700).trimmed());
        QVERIFY(notices.isEmpty());
    }

    /* A second VM joins the same helper; the settings go with the last one */
    void twoVms()
    {
        FakeQemu q1, q2;
        HostSettings hs(nullptr);
        QSignalSpy lines(&hs, &HostSettings::helperLine), finished(&hs, &HostSettings::helperFinished);
        fake(hs);
        hs.tune(q1.pid());
        QTRY_VERIFY(saw(lines, QString("ok rt %1").arg(q1.pid())));
        hs.tune(q2.pid());
        QTRY_VERIFY(saw(lines, QString("ok rt %1").arg(q2.pid())));
        QVERIFY(saw(lines, "ok fair-server on: already"));
        QVERIFY(saw(lines, "ok gpu-floor card1 auto: already"));
        q1.stop();
        QTRY_VERIFY(saw(lines, QString("exited %1").arg(q1.pid())));
        QTest::qWait(200);
        QVERIFY(hs.helperRunning());
        QCOMPARE(fair(1), QString("10000000/1000000"));
        q2.stop();
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(fair(1), QString("1000000000/50000000"));
        QCOMPARE(level(), QString("auto"));
    }

    void floorOff()
    {
        HostSettings::setGpuFloor("off");
        FakeQemu qemu;
        HostSettings hs(nullptr);
        QSignalSpy lines(&hs, &HostSettings::helperLine);
        fake(hs);
        hs.tune(qemu.pid());
        QTRY_VERIFY(saw(lines, QString("ok rt %1").arg(qemu.pid())));
        QVERIFY(saw(lines, "ok gpu-floor card1 off"));
        QCOMPARE(level(), QString("auto"));
        QCOMPARE(fair(0), QString("10000000/1000000"));
    }

    void floorFixed()
    {
        HostSettings::setGpuFloor("2000");
        FakeQemu qemu;
        HostSettings hs(nullptr);
        QSignalSpy lines(&hs, &HostSettings::helperLine);
        fake(hs);
        hs.tune(qemu.pid());
        QTRY_VERIFY(saw(lines, QString("ok rt %1").arg(qemu.pid())));
        QVERIFY(saw(lines, "ok gpu-floor card1 2000 MHz"));
        QCOMPARE(odTable(), od(2000, 2700).trimmed());
    }

    void disabled()
    {
        HostSettings::setEnabled(false);
        FakeQemu qemu;
        HostSettings hs(nullptr);
        QSignalSpy lines(&hs, &HostSettings::helperLine), notices(&hs, &HostSettings::notice);
        fake(hs);
        hs.tune(qemu.pid());
        QTest::qWait(300);
        QVERIFY(!hs.helperRunning());
        QVERIFY(lines.isEmpty());
        QVERIFY(notices.isEmpty());
        QCOMPARE(fair(0), QString("1000000000/50000000"));
    }

    /* Turned off while VMs run: everything back at once (or, missed, at
       the next start) */
    void disabledWhileRunning_data()
    {
        QTest::addColumn<bool>("told");
        QTest::newRow("preferences") << true;
        QTest::newRow("next start") << false;
    }
    void disabledWhileRunning()
    {
        QFETCH(bool, told);
        FakeQemu q1, q2;
        HostSettings hs(nullptr);
        QSignalSpy lines(&hs, &HostSettings::helperLine), finished(&hs, &HostSettings::helperFinished);
        fake(hs);
        hs.tune(q1.pid());
        QTRY_VERIFY(saw(lines, QString("ok rt %1").arg(q1.pid())));
        HostSettings::setEnabled(false);
        if (told) {
            hs.preferencesChanged();
        } else {
            hs.tune(q2.pid());
        }
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(fair(0), QString("1000000000/50000000"));
        QCOMPARE(level(), QString("auto"));
        QVERIFY(saw(lines, QString("restored rt %1").arg(q1.pid())));
    }

    /*
     * The preferences while a VM runs: turned on, the running VM is tuned;
     * another floor replaces the one held; turned off, everything back
     */
    void preferencesWhileRunning()
    {
        QTemporaryDir runtime(QDir::tempPath() + "/vt-XXXXXX");
        QTemporaryDir vms;
        QVERIFY(runtime.isValid() && vms.isValid());
        qputenv("XDG_RUNTIME_DIR", runtime.path().toLocal8Bit());
        QDir(vms.path()).mkpath("one");
        writeFile(vms.path() + "/one/vm.args", "-name one\n");
        const QString run = Paths::vmRuntimeDir("one");
        FakeQemu qemu({"-qmp", QString("unix:%1/qmp.sock,server=on,wait=off").arg(run)});
        writeFile(run + "/qemu.pid", QByteArray::number(qemu.pid()) + "\n");
        FakeQmp qmp(run + "/qmp.sock");
        VmStore store(vms.path());
        HostSettings hs(&store);
        QSignalSpy lines(&hs, &HostSettings::helperLine), finished(&hs, &HostSettings::helperFinished);
        fake(hs);
        /* it started while tuning was off */
        HostSettings::setEnabled(false);
        store.vms().first()->runner()->attach(store.vms().first()->args());
        QTRY_COMPARE(store.vms().first()->runner()->state(), VmRunner::State::Running);
        QTest::qWait(100);
        QVERIFY(!hs.helperRunning());

        HostSettings::setEnabled(true);
        hs.preferencesChanged();
        QTRY_VERIFY(saw(lines, QString("ok rt %1").arg(qemu.pid())));
        QCOMPARE(fair(0), QString("10000000/1000000"));
        QCOMPARE(odTable(), od(1800, 2700).trimmed());

        HostSettings::setGpuFloor("2000");
        hs.preferencesChanged();
        QTRY_VERIFY(saw(lines, "ok gpu-floor card1 2000 MHz"));
        QCOMPARE(odTable(), od(2000, 2700).trimmed());

        HostSettings::setEnabled(false);
        hs.preferencesChanged();
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(fair(0), QString("1000000000/50000000"));
        QCOMPARE(level(), QString("auto"));
        QCOMPARE(odTable(), od(800, 2700).trimmed());
    }

    /* auto: a floor on APUs only, a fixed one chosen before goes from the others */
    void autoFloorElsewhere()
    {
        amdCard(m_root + "/sys", "card2", 1);   // discrete
        HostSettings::setGpuFloor("auto");
        FakeQemu qemu;
        HostSettings hs(nullptr);
        QSignalSpy lines(&hs, &HostSettings::helperLine), notices(&hs, &HostSettings::notice);
        fake(hs);
        hs.tune(qemu.pid());
        QTRY_VERIFY(saw(lines, QString("ok rt %1").arg(qemu.pid())));
        QVERIFY(saw(lines, "ok gpu-floor card1 1800 MHz"));
        QVERIFY(saw(lines, "ok gpu-floor card2 off"));
        QVERIFY(notices.isEmpty());
    }

    /* Not installed: said once, the VMs run untuned */
    void notInstalled()
    {
        qputenv("VITRINE_HELPER", "/nonexistent/vitrine-helper");
        FakeQemu qemu;
        HostSettings hs(nullptr);
        QSignalSpy notices(&hs, &HostSettings::notice);
        hs.setSysRoot(m_root + "/sys");
        QVERIFY(!HostSettings::helperInstalled());
        hs.tune(qemu.pid());
        hs.tune(qemu.pid() + 1);
        QTRY_COMPARE(notices.size(), 1);
        QCOMPARE(notices.first().first().toString(),
                 QString("Host tuning is off: vitrine-helper is not installed."));
        QTest::qWait(200);
        QCOMPARE(notices.size(), 1);
        QVERIFY(!hs.helperRunning());
    }

private:
    /*
     * polkit (pkcheck, then pkexec) on PATH, as stand-ins: pkcheck answers
     * what <root>/answer says, pkexec runs the helper's test build; both
     * write their arguments to <root>/pkcheck.log and pkexec.log
     */
    void fakePolkit() const
    {
        const QString bin = m_root + "/bin";
        writeFile(bin + "/pkcheck", QString("#!/bin/sh\necho \"$*\" >> %1/pkcheck.log\n"
                                            "exit $(cat %1/answer)\n").arg(m_root).toUtf8());
        /* pkexec --disable-internal-agent HELPER [ARG...] */
        writeFile(bin + "/pkexec", QString("#!/bin/sh\necho \"$*\" >> %1/pkexec.log\n"
                                           "shift 2\nexec %2 \"$@\"\n")
                                       .arg(m_root, HELPER_FAKE).toUtf8());
        for (const QString &name : {"pkcheck", "pkexec"}) {
            QFile::setPermissions(bin + "/" + name, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
        }
        qputenv("PATH", bin.toLocal8Bit() + ':' + m_path);
        /* installed, as far as HostSettings looks */
        qputenv("VITRINE_HELPER", HELPER_FAKE);
    }
    QStringList polkitLog(const QString &name) const
    {
        return QString::fromUtf8(readFile(m_root + "/" + name + ".log")).split('\n', Qt::SkipEmptyParts);
    }

private slots:
    /*
     * polkit is asked before each start of the helper, its answer never
     * kept: the group joined while vitrine runs counts at the next VM
     * start, and left again means no pkexec - which would bring the
     * desktop's password dialog at a VM start
     */
    void polkitEachTime()
    {
        fakePolkit();
        FakeQemu q1, q2, q3;
        HostSettings hs(nullptr);
        QSignalSpy lines(&hs, &HostSettings::helperLine), notices(&hs, &HostSettings::notice),
            finished(&hs, &HostSettings::helperFinished);
        hs.setSysRoot(m_root + "/sys");

        writeFile(m_root + "/answer", "2\n");
        hs.tune(q1.pid());
        QTRY_COMPARE(notices.size(), 1);
        /* a member is told what else it takes (this user may be one) */
        QCOMPARE(notices[0][0].toString(),
                 HostSettings::inVitrineGroup()
                     ? QString("Host tuning is off: polkit wants a password here: the vitrine "
                               "group's rule applies in a local, active desktop session only.")
                     : QString("Host tuning is off: it needs membership of the vitrine group."));
        QVERIFY(!hs.helperRunning());
        QVERIFY(polkitLog("pkexec").isEmpty());

        /* joined the group */
        writeFile(m_root + "/answer", "0\n");
        hs.tune(q2.pid());
        QTRY_VERIFY(saw(lines, QString("ok rt %1").arg(q2.pid())));
        QCOMPARE(polkitLog("pkcheck").size(), 2);
        QCOMPARE(polkitLog("pkexec").size(), 1);
        /* the installed helper, the one the action names */
        QVERIFY(polkitLog("pkexec").first().startsWith("--disable-internal-agent /"));
        QVERIFY(!polkitLog("pkexec").first().contains(HELPER_FAKE));
        q2.stop();
        QTRY_COMPARE(finished.size(), 1);

        /* left it again: asked again, no pkexec, and said again */
        writeFile(m_root + "/answer", "2\n");
        hs.tune(q3.pid());
        QTRY_COMPARE(polkitLog("pkcheck").size(), 3);
        QTRY_COMPARE(notices.size(), 2);
        QTest::qWait(200);
        QVERIFY(!hs.helperRunning());
        QCOMPARE(polkitLog("pkexec").size(), 1);
        QCOMPARE(fair(0), QString("1000000000/50000000"));
    }

    /* pkexec refusing (as without the group): said once, never asked again */
    void refused()
    {
        FakeQemu qemu;
        HostSettings hs(nullptr);
        QSignalSpy notices(&hs, &HostSettings::notice), finished(&hs, &HostSettings::helperFinished);
        hs.setSysRoot(m_root + "/sys");
        hs.setHelperCommand({"/bin/sh", "-c",
                             "echo 'Error executing command as another user: Not authorized'; "
                             "echo; echo 'This incident has been reported.'; exit 127"});
        hs.tune(qemu.pid());
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(notices.size(), 1);
        QCOMPARE(notices.first().first().toString(),
                 QString("Host tuning is off: Error executing command as another user: Not authorized."));
        hs.tune(qemu.pid());
        QTest::qWait(300);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(notices.size(), 1);
    }

    /* What cannot apply here is said, once */
    void skipSaid()
    {
        writeFile(m_root + "/sys/kernel/security/lockdown", "none [integrity] confidentiality\n");
        FakeQemu q1, q2;
        HostSettings hs(nullptr);
        QSignalSpy lines(&hs, &HostSettings::helperLine), notices(&hs, &HostSettings::notice);
        fake(hs);
        hs.tune(q1.pid());
        QTRY_VERIFY(saw(lines, QString("ok rt %1").arg(q1.pid())));
        hs.tune(q2.pid());
        QTRY_VERIFY(saw(lines, QString("ok rt %1").arg(q2.pid())));
        QCOMPARE(notices.size(), 1);
        QCOMPARE(notices.first().first().toString(),
                 QString("Host tuning: fair-server: kernel lockdown (integrity)"));
    }

    /* A VM gone before the helper could watch it: no news, and the helper ends */
    void vmGoneAtOnce()
    {
        qint64 pid;
        {
            FakeQemu qemu;
            pid = qemu.pid();
        }
        HostSettings hs(nullptr);
        QSignalSpy notices(&hs, &HostSettings::notice), finished(&hs, &HostSettings::helperFinished),
            lines(&hs, &HostSettings::helperLine);
        fake(hs);
        hs.tune(pid);
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(saw(lines, QString("error watch %1").arg(pid)));
        QVERIFY(notices.isEmpty());
        QCOMPARE(fair(0), QString("1000000000/50000000"));
    }

    /* vitrine quits while its VMs run: the helper goes on and reverts after them */
    void outlivesTheApp()
    {
        FakeQemu qemu;
        {
            HostSettings hs(nullptr);
            QSignalSpy lines(&hs, &HostSettings::helperLine);
            fake(hs);
            hs.tune(qemu.pid());
            QTRY_VERIFY(saw(lines, QString("ok rt %1").arg(qemu.pid())));
        }
        QTest::qWait(300);
        QCOMPARE(fair(0), QString("10000000/1000000"));
        qemu.stop();
        QTRY_COMPARE(fair(0), QString("1000000000/50000000"));
        QTRY_COMPARE(level(), QString("auto"));
    }

    /*
     * The app's side on the real host: the installed-form helper through
     * sudo instead of pkexec, real sysfs and debugfs.  helper/tests/
     * e2e-host.sh runs it (VITRINE_HELPER_E2E: the helper, VITRINE_TEST_QEMU:
     * a QEMU); skipped otherwise.
     */
    void realHost()
    {
        const QString helper = qEnvironmentVariable("VITRINE_HELPER_E2E");
        const QString qemuBinary = qEnvironmentVariable("VITRINE_TEST_QEMU");
        if (helper.isEmpty() || qemuBinary.isEmpty()) {
            QSKIP("for helper/tests/e2e-host.sh");
        }
        qunsetenv("VITRINE_HELPER_TEST_ROOT");
        auto root = [](const QString &file) {
            QProcess cat;
            cat.start("sudo", {"-n", "cat", file});
            cat.waitForFinished();
            return QString::fromLatin1(cat.readAllStandardOutput()).trimmed();
        };
        const QString fairDir = "/sys/kernel/debug/sched/fair_server/cpu0/";
        const QString fairBefore = root(fairDir + "period") + '/' + root(fairDir + "runtime");
        const QStringList apus = HostSettings::amdCards("/sys", true);
        const QString levelFile = apus.isEmpty() ? QString()
            : "/sys/class/drm/" + apus.first() + "/device/power_dpm_force_performance_level";
        const QByteArray levelBefore = readFile(levelFile);
        QVERIFY(!fairBefore.startsWith('/'));

        QProcess qemu;
        /* gone with the test, crash included: the root helper then sees it
           exit and puts the host back */
        qemu.setChildProcessModifier([]() { prctl(PR_SET_PDEATHSIG, SIGKILL); });
        qemu.start(qemuBinary, {"-machine", "none", "-display", "none", "-S"});
        QVERIFY(qemu.waitForStarted());
        HostSettings hs(nullptr);
        QSignalSpy lines(&hs, &HostSettings::helperLine), notices(&hs, &HostSettings::notice),
            finished(&hs, &HostSettings::helperFinished);
        hs.setHelperCommand({"sudo", "-n", helper});
        hs.tune(qemu.processId());
        QTRY_VERIFY_WITH_TIMEOUT(saw(lines, QString("ok rt %1").arg(qemu.processId())), 10000);
        for (const QList<QVariant> &line : lines) {
            qInfo("helper: %s", qPrintable(line.first().toString()));
        }
        QVERIFY(notices.isEmpty());
        QCOMPARE(root(fairDir + "period") + '/' + root(fairDir + "runtime"),
                 QString("10000000/1000000"));
        if (!levelFile.isEmpty() && levelBefore == "auto") {
            QCOMPARE(readFile(levelFile), QByteArray("manual"));
        }
        /* every thread SCHED_FIFO (/proc/PID/task/TID/stat field 41: 1) */
        const QDir tasks(QString("/proc/%1/task").arg(qemu.processId()));
        for (const QString &tid : tasks.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString stat = QString::fromLatin1(readFile(tasks.filePath(tid) + "/stat"));
            QCOMPARE(stat.mid(stat.lastIndexOf(')') + 2).split(' ').value(38), QString("1"));
        }

        qemu.kill();
        qemu.waitForFinished();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QCOMPARE(root(fairDir + "period") + '/' + root(fairDir + "runtime"), fairBefore);
        if (!levelFile.isEmpty()) {
            QCOMPARE(readFile(levelFile), levelBefore);
        }
    }

    /*
     * VMs found running (as at vitrine's start): tuned as their runners see
     * them run, then focus priority between them.  "three" starts while
     * "one" is in front; "four" shows in QEMU's own window (SDL), whose
     * focus is not known.
     */
    void runningVmsAndFocus()
    {
        /* sockets need a short path: a runtime folder of the test's own */
        QTemporaryDir runtime(QDir::tempPath() + "/vt-XXXXXX");
        QTemporaryDir vms;
        QVERIFY(runtime.isValid() && vms.isValid());
        qputenv("XDG_RUNTIME_DIR", runtime.path().toLocal8Bit());
        std::vector<std::unique_ptr<FakeQemu>> qemus;
        std::vector<std::unique_ptr<FakeQmp>> qmps;
        for (const QString id : {"one", "two", "three", "four"}) {
            QDir(vms.path()).mkpath(id);
            writeFile(vms.path() + "/" + id + "/vm.args",
                      "-name " + id.toLatin1() +
                          (id == "four" ? "\n-display sdl\n" : "\n-display dbus,p2p=yes\n"));
            const QString run = Paths::vmRuntimeDir(id);
            /* what the runner looks for: its -qmp argument, its pid file */
            qemus.push_back(std::make_unique<FakeQemu>(
                QStringList{"-qmp", QString("unix:%1/qmp.sock,server=on,wait=off").arg(run)}));
            writeFile(run + "/qemu.pid", QByteArray::number(qemus.back()->pid()) + "\n");
            qmps.push_back(std::make_unique<FakeQmp>(run + "/qmp.sock"));
        }
        VmStore store(vms.path());
        HostSettings hs(&store);
        QSignalSpy lines(&hs, &HostSettings::helperLine), notices(&hs, &HostSettings::notice);
        fake(hs);
        QCOMPARE(HostSettings::instance(), &hs);
        for (const QString id : {"one", "two", "four"}) {
            store.find(id)->runner()->attach(store.find(id)->args());
        }
        for (int i : {0, 1, 3}) {
            QTRY_VERIFY(saw(lines, QString("ok rt %1").arg(qemus[i]->pid())));
        }
        QCOMPARE(fair(0), QString("10000000/1000000"));
        QVERIFY(notices.isEmpty());

        /* "one" in front: "two" ordinary (nice 0: the stand-in has no
           CAP_SYS_NICE), "one" real-time again through the helper */
        lines.clear();
        hs.setFront("one");
        QTRY_VERIFY(saw(lines, QString("ok rt %1").arg(qemus[0]->pid())));
        auto priority = [](const FakeQmp &qmp) {
            for (const QJsonObject &c : qmp.commands) {
                if (c["execute"] == "x-vcpu-priority") {
                    return QJsonDocument(c["arguments"].toObject()).toJson(QJsonDocument::Compact);
                }
            }
            return QByteArray();
        };
        QTRY_COMPARE(priority(*qmps[1]), QByteArray(R"({"nice":0,"realtime":false})"));
        QCOMPARE(priority(*qmps[0]), QByteArray());
        /* the same again: nothing */
        lines.clear();
        hs.setFront("one");
        QTest::qWait(100);
        QVERIFY(lines.isEmpty());
        /* a VM started meanwhile: behind once the helper's rt is done with it */
        store.find("three")->runner()->attach(store.find("three")->args());
        QTRY_VERIFY(saw(lines, QString("ok rt %1").arg(qemus[2]->pid())));
        QTRY_COMPARE(priority(*qmps[2]), QByteArray(R"({"nice":0,"realtime":false})"));
        /* SDL: left as it is */
        QCOMPARE(priority(*qmps[3]), QByteArray());
        /* "two" in front */
        hs.setFront("two");
        QTRY_VERIFY(saw(lines, QString("ok rt %1").arg(qemus[1]->pid())));
        QTRY_COMPARE(priority(*qmps[0]), QByteArray(R"({"nice":0,"realtime":false})"));
        QCOMPARE(priority(*qmps[3]), QByteArray());
        /* unknown VMs change nothing */
        hs.setFront("five");
        hs.setFront(QString());

        /* tuning turned off: QEMU's own priorities back to ordinary too */
        for (const auto &qmp : qmps) {
            qmp->commands.clear();
        }
        HostSettings::setEnabled(false);
        hs.preferencesChanged();
        for (int i : {0, 1, 2}) {
            QTRY_COMPARE(priority(*qmps[i]), QByteArray(R"({"nice":0,"realtime":false})"));
        }
        QCOMPARE(priority(*qmps[3]), QByteArray());
        QTRY_VERIFY(!hs.helperRunning());

        for (auto &qemu : qemus) {
            qemu->stop();
        }
        QTRY_COMPARE(fair(0), QString("1000000000/50000000"));
        QTRY_VERIFY(!hs.helperRunning());
    }

    /*
     * A VM whose QEMU the helper refuses to watch (another name: a #qemu
     * line, a wrapper): said once, and the helper is not started again for
     * it - each start, refused again, used up the restarts of the run
     */
    void refusedWatch()
    {
        QTemporaryDir runtime(QDir::tempPath() + "/vt-XXXXXX");
        QTemporaryDir vms;
        QVERIFY(runtime.isValid() && vms.isValid());
        qputenv("XDG_RUNTIME_DIR", runtime.path().toLocal8Bit());
        QDir(vms.path()).mkpath("other");
        writeFile(vms.path() + "/other/vm.args", "-name other\n");
        const QString run = Paths::vmRuntimeDir("other");
        /* no QEMU: a shell, with the runner's -qmp among its arguments */
        QProcess other;
        other.start("/bin/sh", {"-c", "sleep 300; :", "sh", "-qmp",
                                QString("unix:%1/qmp.sock,server=on,wait=off").arg(run)});
        QVERIFY(other.waitForStarted());
        writeFile(run + "/qemu.pid", QByteArray::number(other.processId()) + "\n");
        FakeQmp qmp(run + "/qmp.sock");
        VmStore store(vms.path());
        HostSettings hs(&store);
        QSignalSpy lines(&hs, &HostSettings::helperLine), notices(&hs, &HostSettings::notice),
            finished(&hs, &HostSettings::helperFinished);
        fake(hs);
        for (Vm *vm : store.vms()) {
            vm->runner()->attach(vm->args());
        }
        QTRY_COMPARE(finished.size(), 1);
        QTest::qWait(500);
        QCOMPARE(finished.size(), 1);
        QVERIFY(!hs.helperRunning());
        QCOMPARE(notices.size(), 1);
        QVERIFY(notices[0][0].toString().startsWith(
            QString("Host tuning: watch %1: not a QEMU").arg(other.processId())));
        other.kill();
        other.waitForFinished();
    }

    /* A helper that dies while a VM runs: started again for it */
    void helperDies()
    {
        QTemporaryDir runtime(QDir::tempPath() + "/vt-XXXXXX");
        QTemporaryDir vms;
        QVERIFY(runtime.isValid() && vms.isValid());
        qputenv("XDG_RUNTIME_DIR", runtime.path().toLocal8Bit());
        QDir(vms.path()).mkpath("one");
        writeFile(vms.path() + "/one/vm.args", "-name one\n");
        const QString run = Paths::vmRuntimeDir("one");
        FakeQemu qemu({"-qmp", QString("unix:%1/qmp.sock,server=on,wait=off").arg(run)});
        writeFile(run + "/qemu.pid", QByteArray::number(qemu.pid()) + "\n");
        FakeQmp qmp(run + "/qmp.sock");
        VmStore store(vms.path());
        HostSettings hs(&store);
        QSignalSpy lines(&hs, &HostSettings::helperLine), finished(&hs, &HostSettings::helperFinished);
        fake(hs);
        store.vms().first()->runner()->attach(store.vms().first()->args());
        QTRY_VERIFY(saw(lines, QString("ok rt %1").arg(qemu.pid())));

        /* the helper: this test's child named vitrine-helper-fake */
        pid_t helper = 0;
        for (const QString &entry : QDir("/proc").entryList(QDir::Dirs)) {
            const QByteArray stat = readFile("/proc/" + entry + "/stat");
            const qsizetype end = stat.lastIndexOf(')');
            if (stat.contains("(vitrine-helper-") && end > 0 &&
                stat.mid(end + 4).split(' ').value(0).toLongLong() == getpid()) {
                helper = pid_t(entry.toInt());
            }
        }
        QVERIFY(helper > 0);
        lines.clear();
        ::kill(helper, SIGKILL);
        QTRY_COMPARE(finished.size(), 1);
        QTRY_VERIFY(saw(lines, QString("ok rt %1").arg(qemu.pid())));
        QVERIFY(hs.helperRunning());
        qemu.stop();
        QTRY_COMPARE(finished.size(), 2);
        QCOMPARE(fair(0), QString("1000000000/50000000"));
    }

    /*
     * vitrine's QEMU without its capability (built before the group was
     * joined, or with tuning off): given at the start, without a word when
     * polkit says no (a VM start says it), once per run
     */
    void capabilityCatchUp()
    {
        const QString stack = Paths::stackDir();
        const QString bin = stack + "/0123abcd/bin";
        /* the helper's rule: a QEMU in the caller's home folder */
        const struct passwd *pw = getpwuid(getuid());
        if (!pw || !QFileInfo(stack).absoluteFilePath().startsWith(
                       QDir(pw->pw_dir).canonicalPath() + '/')) {
            QSKIP("the test's data folder is not in the home folder");
        }
        const QString qemu = bin + "/" + Paths::qemuSystemName();
        QDir(stack).removeRecursively();
        QVERIFY(QDir().mkpath(bin));
        QVERIFY(QFile::copy(FAKE_QEMU, qemu));
        QFile::setPermissions(qemu, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                        QFileDevice::ExeOwner);
        QVERIFY(QFile::link("0123abcd", stack + "/current"));
        QCOMPARE(Paths::stackQemu(), QFileInfo(qemu).canonicalFilePath());
        fakePolkit();
        auto setcaps = [this]() {
            return QString::fromUtf8(readFile(m_root + "/journal")).split('\n').filter("setcap ");
        };

        writeFile(m_root + "/answer", "2\n");
        {
            HostSettings hs(nullptr);
            QSignalSpy notices(&hs, &HostSettings::notice);
            QTRY_COMPARE(polkitLog("pkcheck").size(), 1);
            QTest::qWait(200);
            QVERIFY(polkitLog("pkexec").isEmpty());
            QVERIFY(notices.isEmpty());
        }
        writeFile(m_root + "/answer", "0\n");
        {
            HostSettings hs(nullptr);
            QSignalSpy notices(&hs, &HostSettings::notice);
            QTRY_COMPARE(polkitLog("pkexec").size(), 1);
            QCOMPARE(polkitLog("pkexec").first(),
                     "--disable-internal-agent " VITRINE_HELPER_PATH " setcap " +
                         QFileInfo(qemu).canonicalFilePath());
            /* the helper's test build set it, in its journal: not on the file */
            QTRY_COMPARE(setcaps().size(), 1);
            QVERIFY(notices.isEmpty());
            /* turned off and on again: tried once per run */
            HostSettings::setEnabled(false);
            hs.preferencesChanged();
            HostSettings::setEnabled(true);
            hs.preferencesChanged();
            QTest::qWait(200);
            QCOMPARE(polkitLog("pkexec").size(), 1);
        }
        QDir(stack).removeRecursively();
    }

    void capabilityWithoutHelper()
    {
        qputenv("VITRINE_HELPER", "/nonexistent/vitrine-helper");
        QString error = "unset";
        HostSettings::grantCapability(FAKE_QEMU, this, [&](const QString &e) { error = e; });
        QCOMPARE(error, QString("vitrine-helper is not installed"));
    }
};

QTEST_GUILESS_MAIN(TestHostSettings)
#include "test_hostsettings.moc"
