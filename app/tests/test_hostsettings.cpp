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

#include <grp.h>
#include <pwd.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "core/hostsettings.h"
#include "core/paths.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"

static const char kFair[] = "/sys/kernel/debug/sched/fair_server";
static const char kUdmabuf[] = "/sys/module/udmabuf/parameters";

/* What QCOMPARE prints of a Status */
char *toString(const HostSettings::Status &status)
{
    return qstrdup(QString("Status(%1, \"%2\")")
                       .arg(int(status.problem)).arg(status.detail).toUtf8().constData());
}
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
    /* "list_limit/size_limit_mb" */
    QString udmabuf() const
    {
        return QString("%1/%2").arg(readFile(m_root + kUdmabuf + "/list_limit"),
                                    readFile(m_root + kUdmabuf + "/size_limit_mb"));
    }
    static bool answered(const QSignalSpy &spy, qint64 pid, bool raised, const QString &why)
    {
        for (const QList<QVariant> &args : spy) {
            if (args.value(0).toLongLong() == pid && args.value(1).toBool() == raised &&
                args.value(2).toString() == why) {
                return true;
            }
        }
        return false;
    }
    /* A HostSettings on the fake tree, through the helper's test build */
    void fake(HostSettings &hs) const
    {
        hs.setHelperCommand({HELPER_FAKE, "session"});
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
        qunsetenv("VITRINE_GROUP");
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
        /* the kernel's defaults */
        writeFile(m_root + kUdmabuf + "/list_limit", "1024\n");
        writeFile(m_root + kUdmabuf + "/size_limit_mb", "64\n");
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

    /* Not installed: the VMs run untuned, shown as such, nothing asked */
    void notInstalled()
    {
        qputenv("VITRINE_HELPER", "/nonexistent/vitrine-helper");
        FakeQemu qemu;
        HostSettings hs(nullptr);
        QSignalSpy notices(&hs, &HostSettings::notice), changed(&hs, &HostSettings::untunedChanged),
            suggested(&hs, &HostSettings::groupSetupSuggested);
        hs.setSysRoot(m_root + "/sys");
        QVERIFY(!HostSettings::helperInstalled());
        hs.tune(qemu.pid());
        /* a VM gone at once is no news */
        hs.tune(qemu.pid() + 100000);
        QTRY_VERIFY(hs.untuned());
        QCOMPARE(hs.untunedCount(), 1);
        QCOMPARE(hs.untunedStatus().problem, HostSettings::Problem::NotInstalled);
        QCOMPARE(hs.untunedStatus().why(), QString("vitrine-helper is not installed"));
        QCOMPARE(changed.size(), 1);
        QTest::qWait(200);
        QVERIFY(notices.isEmpty());
        QVERIFY(suggested.isEmpty());
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
        script("pkcheck", QString("#!/bin/sh\necho \"$*\" >> %1/pkcheck.log\n"
                                  "cat %1/pkcheck.err >&2 2>/dev/null\n"
                                  "exit $(cat %1/answer)\n").arg(m_root).toUtf8());
        /* pkexec --disable-internal-agent HELPER [ARG...] */
        script("pkexec", QString("#!/bin/sh\necho \"$*\" >> %1/pkexec.log\n"
                                 "shift 2\nexec %2 \"$@\"\n")
                             .arg(m_root, HELPER_FAKE).toUtf8());
        qputenv("PATH", bin.toLocal8Bit() + ':' + m_path);
        /* installed, as far as HostSettings looks */
        qputenv("VITRINE_HELPER", HELPER_FAKE);
    }
    /* <root>/bin/@name, a stand-in on PATH */
    void script(const QString &name, const QByteArray &text) const
    {
        const QString path = m_root + "/bin/" + name;
        QFile::remove(path);
        writeFile(path, text);
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
    }
    QStringList polkitLog(const QString &name) const
    {
        return QString::fromUtf8(readFile(m_root + "/" + name + ".log")).split('\n', Qt::SkipEmptyParts);
    }

private slots:
    /*
     * polkit is asked before each start of the helper, its answer never
     * kept: the group joined while vitrine runs counts at the next VM
     * start, for the VMs that ran untuned until then too, and left again
     * means no pkexec - which would bring the
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
        QTRY_VERIFY(hs.untuned());
        /* a member is told what else it takes (this user may be one) */
        QCOMPARE(hs.untunedStatus(),
                 HostSettings::classify(true, 2, QString(), HostSettings::vitrineGroupExists(),
                                        HostSettings::inVitrineGroup()));
        QVERIFY(!hs.helperRunning());
        QVERIFY(polkitLog("pkexec").isEmpty());

        /* joined the group */
        writeFile(m_root + "/answer", "0\n");
        hs.tune(q2.pid());
        QTRY_VERIFY(saw(lines, QString("ok rt %1").arg(q2.pid())));
        QCOMPARE(polkitLog("pkcheck").size(), 2);
        QCOMPARE(polkitLog("pkexec").size(), 1);
        /* the installed helper, the one the action names, and its verb */
        QCOMPARE(polkitLog("pkexec").first(),
                 QString("--disable-internal-agent " VITRINE_HELPER_PATH " session"));
        /* the one started before, untuned until now, with it */
        QTRY_VERIFY(saw(lines, QString("ok rt %1").arg(q1.pid())));
        QTRY_VERIFY(!hs.untuned());
        QCOMPARE(polkitLog("pkexec").size(), 1);
        q1.stop();
        q2.stop();
        QTRY_COMPARE(finished.size(), 1);

        /* left it again: asked again, no pkexec, untuned */
        writeFile(m_root + "/answer", "2\n");
        hs.tune(q3.pid());
        QTRY_COMPARE(polkitLog("pkcheck").size(), 3);
        QTRY_COMPARE(hs.untunedCount(), 1);
        QVERIFY(notices.isEmpty());
        QTest::qWait(200);
        QVERIFY(!hs.helperRunning());
        QCOMPARE(polkitLog("pkexec").size(), 1);
        QCOMPARE(fair(0), QString("1000000000/50000000"));
    }

    /* pkexec refusing (as without the group): untuned, never asked again */
    void refused()
    {
        FakeQemu qemu, q2;
        HostSettings hs(nullptr);
        QSignalSpy notices(&hs, &HostSettings::notice), finished(&hs, &HostSettings::helperFinished);
        hs.setSysRoot(m_root + "/sys");
        hs.setHelperCommand({"/bin/sh", "-c",
                             "echo 'Error executing command as another user: Not authorized'; "
                             "echo; echo 'This incident has been reported.'; exit 127"});
        hs.tune(qemu.pid());
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(hs.untuned());
        QCOMPARE(hs.untunedStatus(),
                 HostSettings::Status({HostSettings::Problem::Failed,
                                       "Error executing command as another user: Not authorized"}));
        hs.tune(q2.pid());
        QTest::qWait(300);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(hs.untunedCount(), 2);
        QVERIFY(notices.isEmpty());
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
        hs.setHelperCommand({"sudo", "-n", helper, "session"});
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
        /* no QEMU: a shell, with the runner's -qmp among its arguments; in a
           process group of its own, so that its sleep ends with it */
        QProcess other;
        other.setChildProcessModifier([]() { setpgid(0, 0); });
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
        /* shown as untuned, by name, not as a notice */
        QVERIFY(notices.isEmpty());
        QVERIFY(hs.untuned());
        QCOMPARE(hs.untunedStatus().why(),
                 QString("vitrine-helper does not take other: not a QEMU (qemu-system-*, qemu-kvm)"));
        /* its end ends that */
        ::kill(-pid_t(other.processId()), SIGKILL);
        other.waitForFinished();
        QTRY_VERIFY(!hs.untuned());
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
            /* the action pkexec would use: a rule may grant it apart */
            QVERIFY(polkitLog("pkcheck").first().startsWith(
                "--action-id org.vitrine.helper.setcap --process "));
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

    /* polkit's answers and the user database, as the state they make */
    void classify_data()
    {
        using P = HostSettings::Problem;
        QTest::addColumn<bool>("installed");
        QTest::addColumn<int>("status");
        QTest::addColumn<QString>("error");
        QTest::addColumn<bool>("exists");
        QTest::addColumn<bool>("member");
        QTest::addColumn<int>("problem");
        QTest::addColumn<QString>("why");
        const QString notRegistered = "Error checking for authorization org.vitrine.helper: "
                                      "GDBus.Error:org.freedesktop.PolicyKit1.Error.Failed: Action "
                                      "org.vitrine.helper is not registered\n";
        QTest::newRow("active") << true << 0 << "" << true << true << int(P::None) << "";
        /* an administrator's rule may allow it without the group */
        QTest::newRow("active, no group") << true << 0 << "" << false << false << int(P::None) << "";
        QTest::newRow("not installed") << false << 0 << "" << true << true << int(P::NotInstalled)
                                       << "vitrine-helper is not installed";
        QTest::newRow("no pkcheck") << true << -1 << "" << true << true << int(P::NoPolkit)
                                    << "polkit's pkcheck is not installed";
        QTest::newRow("no policy") << true << 127 << notRegistered << true << true
                                   << int(P::NoPolicy) << "the helper's polkit policy is not installed";
        QTest::newRow("no group") << true << 2 << "" << false << false << int(P::NoGroup)
                                  << "you are not in the vitrine group, which does not exist yet";
        QTest::newRow("not a member") << true << 2 << "" << true << false << int(P::NotMember)
                                      << "you are not in the vitrine group";
        QTest::newRow("member, not local")
            << true << 2 << "" << true << true << int(P::NotLocal)
            << "polkit wants a password here: the vitrine group's rule applies in a local, active "
               "desktop session only";
        QTest::newRow("dismissed") << true << 3 << "" << true << false << int(P::NotMember)
                                   << "you are not in the vitrine group";
        QTest::newRow("polkit says no") << true << 1 << "" << true << true << int(P::Failed)
                                        << "polkit does not allow it here";
        QTest::newRow("polkit down") << true << 127 << "Error: no polkitd\nmore" << true << true
                                     << int(P::Failed) << "Error: no polkitd";
        QTest::newRow("silent") << true << 127 << "" << true << true << int(P::Failed)
                                << "polkit did not answer";
    }
    void classify()
    {
        QFETCH(bool, installed);
        QFETCH(int, status);
        QFETCH(QString, error);
        QFETCH(bool, exists);
        QFETCH(bool, member);
        QFETCH(int, problem);
        QFETCH(QString, why);
        const HostSettings::Status got = HostSettings::classify(installed, status, error, exists, member);
        QCOMPARE(int(got.problem), problem);
        QCOMPARE(got.why(), why);
        QCOMPARE(got.active(), problem == int(HostSettings::Problem::None));
        QCOMPARE(got.needsGroup(), problem == int(HostSettings::Problem::NoGroup) ||
                                       problem == int(HostSettings::Problem::NotMember));
    }

    /* The group from the user database (VITRINE_GROUP: one this user has, or not) */
    void groupFacts()
    {
        const struct passwd *pw = getpwuid(getuid());
        const struct group *own = getgrgid(getgid());
        QVERIFY(pw && own);
        qputenv("VITRINE_GROUP", "vitrine-test-no-such-group");
        QVERIFY(!HostSettings::vitrineGroupExists());
        QVERIFY(!HostSettings::inVitrineGroup());
        qputenv("VITRINE_GROUP", own->gr_name);
        QVERIFY(HostSettings::vitrineGroupExists());
        QVERIFY(HostSettings::inVitrineGroup());
        /* root's group: there, and this user in it only if listed */
        const struct group *root = getgrgid(0);
        QVERIFY(root);
        bool listed = getgid() == 0;
        for (char **m = root->gr_mem; *m; m++) {
            listed |= strcmp(*m, pw->pw_name) == 0;
        }
        qputenv("VITRINE_GROUP", root->gr_name);
        QVERIFY(HostSettings::vitrineGroupExists());
        QCOMPARE(HostSettings::inVitrineGroup(), listed);
    }

    /* The state for the Preferences: without interaction, polkit's answer
       and the user database */
    void check()
    {
        auto state = [this]() {
            HostSettings::Status got{HostSettings::Problem::Failed, "no answer"};
            bool answered = false;
            HostSettings::check(this, [&](const HostSettings::Status &s) {
                got = s;
                answered = true;
            });
            /* "no answer" stays if none comes */
            (void)QTest::qWaitFor([&]() { return answered; }, 5000);
            return got;
        };
        using P = HostSettings::Problem;
        qputenv("VITRINE_HELPER", "/nonexistent/vitrine-helper");
        QCOMPARE(state().problem, P::NotInstalled);

        fakePolkit();
        writeFile(m_root + "/answer", "0\n");
        QCOMPARE(state(), HostSettings::Status());
        /* asked without interaction, for this process */
        QVERIFY(polkitLog("pkcheck").last().startsWith(
            QString("--action-id org.vitrine.helper --process %1,").arg(getpid())));
        QVERIFY(!polkitLog("pkcheck").last().contains("interaction"));
        writeFile(m_root + "/answer", "2\n");
        qputenv("VITRINE_GROUP", "vitrine-test-no-such-group");
        QCOMPARE(state().problem, P::NoGroup);
        qputenv("VITRINE_GROUP", getgrgid(getgid())->gr_name);
        QCOMPARE(state().problem, P::NotLocal);
        writeFile(m_root + "/answer", "127\n");
        writeFile(m_root + "/pkcheck.err", "Error checking for authorization org.vitrine.helper: "
                                           "Action org.vitrine.helper is not registered\n");
        QCOMPARE(state().problem, P::NoPolicy);
        /* no pkcheck at all */
        qputenv("PATH", (m_root + "/nothing").toLocal8Bit());
        QCOMPARE(state().problem, P::NoPolkit);
        QVERIFY(polkitLog("pkexec").isEmpty());
    }

    /*
     * Several VMs starting at once, untuned for want of the group: one
     * offer to set it up, not one each, and none later in the run; none
     * when another reason keeps tuning off, or with tuning off
     */
    void askOnce()
    {
        fakePolkit();
        writeFile(m_root + "/answer", "2\n");
        qputenv("VITRINE_GROUP", "vitrine-test-no-such-group");
        FakeQemu q1, q2, q3, q4;
        {
            HostSettings hs(nullptr);
            QSignalSpy suggested(&hs, &HostSettings::groupSetupSuggested);
            hs.setSysRoot(m_root + "/sys");
            hs.tune(q1.pid());
            hs.tune(q2.pid());
            hs.tune(q3.pid());
            QTRY_COMPARE(hs.untunedCount(), 3);
            QCOMPARE(suggested.size(), 1);
            QCOMPARE(qvariant_cast<HostSettings::Status>(suggested[0][0]).problem,
                     HostSettings::Problem::NoGroup);
            QCOMPARE(polkitLog("pkcheck").size(), 1);
            /* a VM started later: untuned, not asked again */
            hs.tune(q4.pid());
            QTRY_COMPARE(hs.untunedCount(), 4);
            QTest::qWait(100);
            QCOMPARE(suggested.size(), 1);
            QVERIFY(polkitLog("pkexec").isEmpty());
        }
        /* a member whose session polkit will not take: nothing to set up */
        qputenv("VITRINE_GROUP", getgrgid(getgid())->gr_name);
        {
            HostSettings hs(nullptr);
            QSignalSpy suggested(&hs, &HostSettings::groupSetupSuggested);
            hs.setSysRoot(m_root + "/sys");
            hs.tune(q1.pid());
            QTRY_VERIFY(hs.untuned());
            QCOMPARE(hs.untunedStatus().problem, HostSettings::Problem::NotLocal);
            QTest::qWait(100);
            QVERIFY(suggested.isEmpty());
        }
        /* tuning off: nothing asked, nothing shown */
        qputenv("VITRINE_GROUP", "vitrine-test-no-such-group");
        HostSettings::setEnabled(false);
        {
            HostSettings hs(nullptr);
            QSignalSpy suggested(&hs, &HostSettings::groupSetupSuggested),
                changed(&hs, &HostSettings::untunedChanged);
            hs.setSysRoot(m_root + "/sys");
            hs.tune(q1.pid());
            QTest::qWait(300);
            QVERIFY(suggested.isEmpty());
            QVERIFY(changed.isEmpty());
            QVERIFY(!hs.untuned());
        }
    }

    /*
     * The status bar's warning: only with tuning on and a VM running
     * untuned; gone when tuning is turned off, when the VM stops, and when
     * it gets tuned
     */
    void untunedRules()
    {
        QTemporaryDir runtime(QDir::tempPath() + "/vt-XXXXXX");
        QTemporaryDir vms;
        QVERIFY(runtime.isValid() && vms.isValid());
        qputenv("XDG_RUNTIME_DIR", runtime.path().toLocal8Bit());
        QDir(vms.path()).mkpath("one");
        writeFile(vms.path() + "/one/vm.args", "-name one\n");
        const QString run = Paths::vmRuntimeDir("one");
        auto qemu = std::make_unique<FakeQemu>(
            QStringList{"-qmp", QString("unix:%1/qmp.sock,server=on,wait=off").arg(run)});
        writeFile(run + "/qemu.pid", QByteArray::number(qemu->pid()) + "\n");
        auto qmp = std::make_unique<FakeQmp>(run + "/qmp.sock");
        VmStore store(vms.path());
        Vm *vm = store.vms().first();
        fakePolkit();
        writeFile(m_root + "/answer", "2\n");
        HostSettings hs(&store);
        QSignalSpy changed(&hs, &HostSettings::untunedChanged), lines(&hs, &HostSettings::helperLine);
        hs.setSysRoot(m_root + "/sys");

        /* off: a VM starts, nothing shown */
        HostSettings::setEnabled(false);
        vm->runner()->attach(vm->args());
        QTRY_COMPARE(vm->runner()->state(), VmRunner::State::Running);
        QTest::qWait(200);
        QVERIFY(!hs.untuned());
        QVERIFY(changed.isEmpty());

        /* on: untuned, shown */
        HostSettings::setEnabled(true);
        hs.preferencesChanged();
        QTRY_VERIFY(hs.untuned());
        QCOMPARE(hs.untunedCount(), 1);
        /* off again: gone at once */
        changed.clear();
        HostSettings::setEnabled(false);
        hs.preferencesChanged();
        QCOMPARE(changed.size(), 1);
        QVERIFY(!hs.untuned());
        /* on, and polkit says yes now: tuned, gone */
        HostSettings::setEnabled(true);
        hs.preferencesChanged();
        QTRY_VERIFY(hs.untuned());
        changed.clear();
        writeFile(m_root + "/answer", "0\n");
        hs.preferencesChanged();
        QTRY_VERIFY(saw(lines, QString("ok watch %1").arg(qemu->pid())));
        QTRY_VERIFY(!hs.untuned());
        QVERIFY(!changed.isEmpty());
        QTRY_VERIFY(saw(lines, QString("ok rt %1").arg(qemu->pid())));

        /* the VM stops (its QMP closes: the runner's sign), the helper with it */
        qmp.reset();
        qemu.reset();
        QTRY_COMPARE(vm->runner()->state(), VmRunner::State::Stopped);
        QTRY_VERIFY(!hs.helperRunning());
        /* started again, untuned (polkit says no now), then stopped */
        writeFile(m_root + "/answer", "2\n");
        qemu = std::make_unique<FakeQemu>(
            QStringList{"-qmp", QString("unix:%1/qmp.sock,server=on,wait=off").arg(run)});
        writeFile(run + "/qemu.pid", QByteArray::number(qemu->pid()) + "\n");
        qmp = std::make_unique<FakeQmp>(run + "/qmp.sock");
        vm->runner()->attach(vm->args());
        QTRY_VERIFY(hs.untuned());
        changed.clear();
        qmp.reset();
        qemu.reset();
        QTRY_COMPARE(vm->runner()->state(), VmRunner::State::Stopped);
        QVERIFY(!changed.isEmpty());
        QVERIFY(!hs.untuned());
    }

private:
    /* setUpGroup()'s answer */
    struct SetupResult {
        bool called = false;
        HostSettings::Setup result = HostSettings::Setup::Failed;
        QString error;
        HostSettings::Status now;
    };
    static std::function<void(HostSettings::Setup, const QString &, const HostSettings::Status &)>
    into(SetupResult &r)
    {
        return [&r](HostSettings::Setup result, const QString &error, const HostSettings::Status &now) {
            r = {true, result, error, now};
        };
    }

private slots:
    /*
     * Set Up: vitrine-helper setup-group through pkexec, then polkit asked
     * again and the VMs that ran untuned tuned
     */
    void setUpGroup()
    {
        fakePolkit();
        const struct passwd *pw = getpwuid(getuid());
        QVERIFY(pw);
        writeFile(m_root + "/etc/group", "wheel:x:10:\n");
        writeFile(m_root + "/answer", "2\n");
        qputenv("VITRINE_GROUP", "vitrine-test-no-such-group");
        FakeQemu q1, q2;
        HostSettings hs(nullptr);
        QSignalSpy lines(&hs, &HostSettings::helperLine);
        hs.setSysRoot(m_root + "/sys");
        hs.tune(q1.pid());
        hs.tune(q2.pid());
        QTRY_COMPARE(hs.untunedCount(), 2);

        /* polkit takes the new member (it reads the user database each time) */
        writeFile(m_root + "/answer", "0\n");
        SetupResult r, second;
        hs.setUpGroup(into(r));
        QVERIFY(hs.settingUpGroup());
        /* one at a time */
        hs.setUpGroup(into(second));
        QVERIFY(second.called);
        QCOMPARE(second.result, HostSettings::Setup::Failed);
        QTRY_VERIFY(r.called);
        QCOMPARE(r.result, HostSettings::Setup::Done);
        QVERIFY(r.now.active());
        QVERIFY(!hs.settingUpGroup());
        QCOMPARE(polkitLog("pkexec").first(),
                 QString("--disable-internal-agent " VITRINE_HELPER_PATH " setup-group"));
        QCOMPARE(readFile(m_root + "/etc/group"),
                 QByteArray("wheel:x:10:\nvitrine:x:977:") + pw->pw_name);
        /* the running VMs, tuned now */
        QTRY_VERIFY(saw(lines, QString("ok rt %1").arg(q1.pid())));
        QTRY_VERIFY(saw(lines, QString("ok rt %1").arg(q2.pid())));
        QTRY_VERIFY(!hs.untuned());
    }

    /* Set up, but polkit still wants a password: why, for the VMs too */
    void setUpGroupNotYet()
    {
        fakePolkit();
        writeFile(m_root + "/etc/group", "");
        writeFile(m_root + "/answer", "2\n");
        FakeQemu qemu;
        HostSettings hs(nullptr);
        hs.setSysRoot(m_root + "/sys");
        qputenv("VITRINE_GROUP", "vitrine-test-no-such-group");
        hs.tune(qemu.pid());
        QTRY_VERIFY(hs.untuned());
        QCOMPARE(hs.untunedStatus().problem, HostSettings::Problem::NoGroup);

        /* the user database does not show it yet (a cache): log in again */
        SetupResult r;
        hs.setUpGroup(into(r));
        QTRY_VERIFY(r.called);
        QCOMPARE(r.result, HostSettings::Setup::Done);
        QCOMPARE(r.now.problem, HostSettings::Problem::NoGroup);
        QVERIFY(r.now.needsGroup());
        /* it shows, but polkit wants a password (not a local session...) */
        qputenv("VITRINE_GROUP", getgrgid(getgid())->gr_name);
        r = {};
        hs.setUpGroup(into(r));
        QTRY_VERIFY(r.called);
        QCOMPARE(r.result, HostSettings::Setup::Done);
        QCOMPARE(r.now.problem, HostSettings::Problem::NotLocal);
        QCOMPARE(hs.untunedStatus().problem, HostSettings::Problem::NotLocal);
        QVERIFY(!hs.helperRunning());
    }

    /* The password dialog dismissed, the helper refusing, nothing to run */
    void setUpGroupFails()
    {
        fakePolkit();
        writeFile(m_root + "/answer", "2\n");
        HostSettings hs(nullptr);
        SetupResult r;

        /* KDE's agent: Cancel is pkexec's "Not authorized" */
        script("pkexec", "#!/bin/sh\necho 'Error executing command as another user: "
                                   "Not authorized'\necho\necho 'This incident has been "
                                   "reported.'\nexit 127\n");
        hs.setUpGroup(into(r));
        QTRY_VERIFY(r.called);
        QCOMPARE(r.result, HostSettings::Setup::Cancelled);
        /* dismissed (GNOME Shell's agent): 126, and pkexec says so */
        script("pkexec", "#!/bin/sh\necho 'Error executing command as another user: "
                         "Request dismissed' >&2\nexit 126\n");
        r = {};
        hs.setUpGroup(into(r));
        QTRY_VERIFY(r.called);
        QCOMPARE(r.result, HostSettings::Setup::Cancelled);
        QVERIFY(r.error.isEmpty());
        script("pkexec", "#!/bin/sh\nexit 126\n");
        r = {};
        hs.setUpGroup(into(r));
        QTRY_VERIFY(r.called);
        QCOMPARE(r.result, HostSettings::Setup::Cancelled);
        /* no polkit agent (ssh, waypipe): why, as a clause of the box's sentence */
        script("pkexec", "#!/bin/sh\necho 'Error executing command as another user: "
                         "No authentication agent found.'\nexit 127\n");
        r = {};
        hs.setUpGroup(into(r));
        QTRY_VERIFY(r.called);
        QCOMPARE(r.result, HostSettings::Setup::Failed);
        QCOMPARE(r.error, QString("No authentication agent found"));
        /* the group tools' error, without its period */
        script("pkexec", "#!/bin/sh\necho 'error setup-group: cannot add the user to the group: "
                         "gpasswd: cannot lock /etc/group; try again later.'\nexit 1\n");
        r = {};
        hs.setUpGroup(into(r));
        QTRY_VERIFY(r.called);
        QCOMPARE(r.result, HostSettings::Setup::Failed);
        QCOMPARE(r.error, QString("cannot add the user to the group: gpasswd: cannot lock "
                                  "/etc/group; try again later"));
        /* the helper's own error */
        fakePolkit();
        writeFile(m_root + "/etc/group", "vitrine:x:977:\n");
        QFile::setPermissions(m_root + "/etc/group", QFileDevice::ReadOwner);
        r = {};
        hs.setUpGroup(into(r));
        QTRY_VERIFY(r.called);
        QCOMPARE(r.result, HostSettings::Setup::Failed);
        QCOMPARE(r.error, QString("cannot add the user to the group: Permission denied"));
        QFile::setPermissions(m_root + "/etc/group", QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        /* no pkexec, no helper */
        qputenv("PATH", (m_root + "/nothing").toLocal8Bit());
        r = {};
        hs.setUpGroup(into(r));
        QTRY_VERIFY(r.called);
        QCOMPARE(r.error, QString("pkexec is not installed"));
        qputenv("VITRINE_HELPER", "/nonexistent/vitrine-helper");
        r = {};
        hs.setUpGroup(into(r));
        QVERIFY(r.called);
        QCOMPARE(r.error, QString("vitrine-helper is not installed"));
        QVERIFY(!hs.settingUpGroup());
    }

    /*
     * The helper installed while a VM runs untuned: the warning's reason
     * follows when vitrine asks again (the window back in front), with the
     * offer to set up the group; and a VM started later brings the earlier
     * ones' reason up to date too
     */
    void installedMeanwhile()
    {
        using P = HostSettings::Problem;
        qputenv("VITRINE_HELPER", "/nonexistent/vitrine-helper");
        FakeQemu q1, q2;
        HostSettings hs(nullptr);
        QSignalSpy suggested(&hs, &HostSettings::groupSetupSuggested);
        hs.setSysRoot(m_root + "/sys");
        hs.tune(q1.pid());
        QTRY_VERIFY(hs.untuned());
        QCOMPARE(hs.untunedStatus().problem, P::NotInstalled);
        QVERIFY(suggested.isEmpty());

        /* sudo cmake --install: installed, no group yet */
        fakePolkit();
        writeFile(m_root + "/answer", "2\n");
        qputenv("VITRINE_GROUP", "vitrine-test-no-such-group");
        hs.recheck();
        QTRY_COMPARE(hs.untunedStatus().problem, P::NoGroup);
        QCOMPARE(suggested.size(), 1);
        QCOMPARE(polkitLog("pkcheck").size(), 1);
        /* again: asked, nothing new, not offered twice */
        hs.recheck();
        QTRY_COMPARE(polkitLog("pkcheck").size(), 2);
        QTest::qWait(100);
        QCOMPARE(suggested.size(), 1);
        QVERIFY(polkitLog("pkexec").isEmpty());
        /* one recheck at a time, when nobody waits for its answer */
        hs.recheck();
        hs.recheck();
        QTRY_COMPARE(polkitLog("pkcheck").size(), 3);
        QTest::qWait(200);
        QCOMPARE(polkitLog("pkcheck").size(), 3);

        /* no recheck: a VM start says it for the VMs running already */
        HostSettings hs2(nullptr);
        hs2.setSysRoot(m_root + "/sys");
        qputenv("VITRINE_HELPER", "/nonexistent/vitrine-helper");
        hs2.tune(q1.pid());
        QTRY_VERIFY(hs2.untuned());
        fakePolkit();
        hs2.tune(q2.pid());
        QTRY_COMPARE(hs2.untunedCount(), 2);
        QTRY_COMPARE(hs2.untunedStatus().problem, P::NoGroup);
        q2.stop();
        QTRY_COMPARE(hs2.untunedCount(), 1);
        QCOMPARE(hs2.untunedStatus().problem, P::NoGroup);
    }

    /*
     * The group joined by hand (a terminal) while VMs run untuned: tuned as
     * soon as vitrine asks again - Preferences opened, or the next VM start,
     * which takes the earlier ones along
     */
    void joinedByHand()
    {
        fakePolkit();
        writeFile(m_root + "/answer", "2\n");
        qputenv("VITRINE_GROUP", "vitrine-test-no-such-group");
        FakeQemu q1, q2, q3;
        {
            HostSettings hs(nullptr);
            QSignalSpy lines(&hs, &HostSettings::helperLine);
            hs.setSysRoot(m_root + "/sys");
            hs.tune(q1.pid());
            QTRY_VERIFY(hs.untuned());
            /* sudo groupadd; sudo usermod -aG: polkit says yes now */
            writeFile(m_root + "/answer", "0\n");
            /* Preferences: the state it shows is the status bar's */
            HostSettings::Status shown{HostSettings::Problem::Failed, "no answer"};
            hs.recheck(this, [&](const HostSettings::Status &now) { shown = now; });
            QTRY_VERIFY(shown.active());
            QTRY_VERIFY(saw(lines, QString("ok rt %1").arg(q1.pid())));
            QTRY_VERIFY(!hs.untuned());
            QCOMPARE(polkitLog("pkexec").size(), 1);
        }
        QFile::remove(m_root + "/pkexec.log");
        writeFile(m_root + "/answer", "2\n");
        {
            HostSettings hs(nullptr);
            QSignalSpy lines(&hs, &HostSettings::helperLine);
            hs.setSysRoot(m_root + "/sys");
            hs.tune(q2.pid());
            QTRY_VERIFY(hs.untuned());
            writeFile(m_root + "/answer", "0\n");
            hs.tune(q3.pid());
            QTRY_VERIFY(saw(lines, QString("ok rt %1").arg(q3.pid())));
            QTRY_VERIFY(saw(lines, QString("ok rt %1").arg(q2.pid())));
            QTRY_VERIFY(!hs.untuned());
            QCOMPARE(polkitLog("pkexec").size(), 1);
        }
    }

    /*
     * recheck(): without an answer to give, nothing while no VM runs
     * untuned (or tuning is off); with one, always asked, the answer only
     * to a context still there
     */
    void recheckRules()
    {
        fakePolkit();
        writeFile(m_root + "/answer", "2\n");
        qputenv("VITRINE_GROUP", "vitrine-test-no-such-group");
        HostSettings hs(nullptr);
        QSignalSpy suggested(&hs, &HostSettings::groupSetupSuggested),
            changed(&hs, &HostSettings::untunedChanged);
        hs.setSysRoot(m_root + "/sys");
        hs.recheck();
        QTest::qWait(200);
        QVERIFY(polkitLog("pkcheck").isEmpty());

        HostSettings::Status shown;
        bool answered = false;
        hs.recheck(this, [&](const HostSettings::Status &now) {
            shown = now;
            answered = true;
        });
        QTRY_VERIFY(answered);
        QCOMPARE(shown.problem, HostSettings::Problem::NoGroup);
        QVERIFY(!hs.untuned());
        QVERIFY(suggested.isEmpty());
        QVERIFY(changed.isEmpty());

        auto called = std::make_shared<bool>(false);
        {
            QObject gone;
            hs.recheck(&gone, [called](const HostSettings::Status &) { *called = true; });
        }
        QTRY_COMPARE(polkitLog("pkcheck").size(), 2);
        QTest::qWait(200);
        QVERIFY(!*called);

        /* tuning off: a VM untuned is no warning, and nothing is asked */
        FakeQemu qemu;
        hs.tune(qemu.pid());
        QTRY_VERIFY(hs.untuned());
        HostSettings::setEnabled(false);
        hs.preferencesChanged();
        const int asked = int(polkitLog("pkcheck").size());
        hs.recheck();
        QTest::qWait(200);
        QCOMPARE(int(polkitLog("pkcheck").size()), asked);
    }

    /*
     * The warning's explanation or Preferences show the state with their
     * own Set Up: no question on top of them, nor when they close and the
     * window comes back to the front
     */
    void noOfferOverOwnSetUp()
    {
        qputenv("VITRINE_HELPER", "/nonexistent/vitrine-helper");
        FakeQemu qemu;
        HostSettings hs(nullptr);
        QSignalSpy suggested(&hs, &HostSettings::groupSetupSuggested);
        hs.setSysRoot(m_root + "/sys");
        hs.tune(qemu.pid());
        QTRY_VERIFY(hs.untuned());
        /* installed since, no group */
        fakePolkit();
        writeFile(m_root + "/answer", "2\n");
        qputenv("VITRINE_GROUP", "vitrine-test-no-such-group");
        HostSettings::Status shown;
        bool answered = false;
        hs.recheck(this, [&](const HostSettings::Status &now) {
            shown = now;
            answered = true;
        });
        QTRY_VERIFY(answered);
        QCOMPARE(shown.problem, HostSettings::Problem::NoGroup);
        QCOMPARE(hs.untunedStatus().problem, HostSettings::Problem::NoGroup);
        QVERIFY(suggested.isEmpty());
        hs.recheck();
        QTRY_COMPARE(polkitLog("pkcheck").size(), 2);
        QTest::qWait(100);
        QVERIFY(suggested.isEmpty());
    }

    /*
     * Set Up clicked (Preferences) before the first VM start: that start's
     * offer would come on top of the password dialog, and its Set Up fail
     * as one is under way.  Not offered then, nor later in the run.
     */
    void noOfferDuringSetUp()
    {
        fakePolkit();
        writeFile(m_root + "/answer", "2\n");
        qputenv("VITRINE_GROUP", "vitrine-test-no-such-group");
        /* a password dialog open for a second, then dismissed */
        script("pkexec", QString("#!/bin/sh\necho \"$*\" >> %1/pkexec.log\nsleep 1\nexit 126\n")
                             .arg(m_root).toUtf8());
        FakeQemu qemu;
        HostSettings hs(nullptr);
        QSignalSpy suggested(&hs, &HostSettings::groupSetupSuggested);
        hs.setSysRoot(m_root + "/sys");
        SetupResult r;
        hs.setUpGroup(into(r));
        hs.tune(qemu.pid());
        QTRY_VERIFY(hs.untuned());
        QVERIFY(hs.settingUpGroup());
        QTRY_VERIFY(r.called);
        QCOMPARE(r.result, HostSettings::Setup::Cancelled);
        hs.recheck();
        QTRY_COMPARE(polkitLog("pkcheck").size(), 2);
        QTest::qWait(100);
        QVERIFY(suggested.isEmpty());
        QVERIFY(hs.untuned());
    }

    /* The warning's reason: one Set Up fixes, before a VM's own */
    void untunedStatusPrefersGroup()
    {
        using P = HostSettings::Problem;
        fakePolkit();
        writeFile(m_root + "/answer", "0\n");
        /* not a QEMU: the helper will not take it */
        QProcess other;
        other.start("sleep", {"300"});
        QVERIFY(other.waitForStarted());
        FakeQemu qemu;
        HostSettings hs(nullptr);
        QSignalSpy finished(&hs, &HostSettings::helperFinished);
        hs.setSysRoot(m_root + "/sys");
        hs.tune(other.processId());
        QTRY_VERIFY(hs.untuned());
        QCOMPARE(hs.untunedStatus().problem, P::Failed);
        QTRY_COMPARE(finished.size(), 1);

        writeFile(m_root + "/answer", "2\n");
        qputenv("VITRINE_GROUP", "vitrine-test-no-such-group");
        hs.tune(qemu.pid());
        QTRY_COMPARE(hs.untunedCount(), 2);
        QCOMPARE(hs.untunedStatus().problem, P::NoGroup);
        /* the refused one keeps its own reason */
        qemu.stop();
        QTRY_COMPARE(hs.untunedCount(), 1);
        QCOMPARE(hs.untunedStatus().problem, P::Failed);
        other.kill();
        other.waitForFinished();
    }

    /*
     * A QEMU whose GPU has native context gets the udmabuf limits raised
     * too, while it runs; the others do not ask for them
     */
    void udmabufForNativeContext()
    {
        FakeQemu q1, q2;
        HostSettings hs(nullptr);
        QSignalSpy lines(&hs, &HostSettings::helperLine), notices(&hs, &HostSettings::notice),
            answers(&hs, &HostSettings::udmabufAnswered), finished(&hs, &HostSettings::helperFinished);
        fake(hs);
        hs.tune(q1.pid(), true);
        QTRY_VERIFY(saw(lines, QString("ok rt %1").arg(q1.pid())));
        QVERIFY(saw(lines, QString("ok udmabuf %1: list_limit 65536 (was 1024), size_limit_mb 2048 "
                                   "(was 64)").arg(q1.pid())));
        QCOMPARE(answers.size(), 1);
        QVERIFY(answered(answers, q1.pid(), true, QString()));
        QCOMPARE(udmabuf(), QString("65536/2048"));
        hs.tune(q2.pid());
        QTRY_VERIFY(saw(lines, QString("ok rt %1").arg(q2.pid())));
        QVERIFY(!saw(lines, QString("ok udmabuf %1").arg(q2.pid())));
        QCOMPARE(answers.size(), 1);
        /* the one that asked ends: back, while the other runs on */
        q1.stop();
        QTRY_VERIFY(saw(lines, "restored udmabuf: list_limit 1024, size_limit_mb 64"));
        QCOMPARE(udmabuf(), QString("1024/64"));
        QVERIFY(hs.helperRunning());
        QCOMPARE(fair(0), QString("10000000/1000000"));
        q2.stop();
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(notices.isEmpty());
    }

    /* Why they are not raised, for the start check; never a notice of its own */
    void udmabufNotRaised()
    {
        FakeQemu q1, q2, q3;
        {
            /* the module not loaded: the helper skips them */
            QDir(m_root + "/sys/module/udmabuf").removeRecursively();
            HostSettings hs(nullptr);
            QSignalSpy lines(&hs, &HostSettings::helperLine), notices(&hs, &HostSettings::notice),
                answers(&hs, &HostSettings::udmabufAnswered);
            fake(hs);
            hs.tune(q1.pid(), true);
            QTRY_VERIFY(saw(lines, QString("ok rt %1").arg(q1.pid())));
            QVERIFY(answered(answers, q1.pid(), false, "the udmabuf module is not loaded"));
            QVERIFY(notices.isEmpty());
        }
        {
            /* a helper from before udmabuf: "unknown request", without a pid */
            HostSettings hs(nullptr);
            QSignalSpy notices(&hs, &HostSettings::notice),
                answers(&hs, &HostSettings::udmabufAnswered);
            hs.setSysRoot(m_root + "/sys");
            hs.setHelperCommand(
                {"/bin/sh", "-c",
                 "echo 'ready 1'; while read -r verb arg rest; do case $verb in "
                 "watch) echo \"ok watch $arg\";; "
                 "fair-server) echo 'ok fair-server on: already 10 ms / 1 ms';; "
                 "gpu-floor) echo \"ok gpu-floor $arg auto: already\";; "
                 "rt) echo \"ok rt $arg: 1 of 1 threads real-time\";; "
                 "*) echo \"error $verb: unknown request\";; esac; done"});
            hs.tune(q2.pid(), true);
            QTRY_COMPARE(answers.size(), 1);
            QVERIFY(answered(answers, q2.pid(), false,
                             "the installed vitrine-helper is older than Vitrine and cannot raise "
                             "them: install it again"));
            QVERIFY(notices.isEmpty());
        }
        {
            /* tuning off: at once */
            HostSettings::setEnabled(false);
            HostSettings hs(nullptr);
            QSignalSpy answers(&hs, &HostSettings::udmabufAnswered);
            fake(hs);
            hs.tune(q3.pid(), true);
            QCOMPARE(answers.size(), 1);
            QVERIFY(answered(answers, q3.pid(), false, "host tuning is off"));
            QVERIFY(!hs.helperRunning());
        }
    }

    /* Untuned for want of the group, then joined: not raised, then raised */
    void udmabufUntunedThenTuned()
    {
        fakePolkit();
        writeFile(m_root + "/answer", "2\n");
        FakeQemu qemu;
        HostSettings hs(nullptr);
        QSignalSpy lines(&hs, &HostSettings::helperLine),
            answers(&hs, &HostSettings::udmabufAnswered);
        hs.setSysRoot(m_root + "/sys");
        hs.tune(qemu.pid(), true);
        QTRY_VERIFY(hs.untuned());
        QVERIFY(answered(answers, qemu.pid(), false, hs.untunedStatus().why()));
        QCOMPARE(udmabuf(), QString("1024/64"));
        writeFile(m_root + "/answer", "0\n");
        hs.recheck();
        QTRY_VERIFY(answered(answers, qemu.pid(), true, QString()));
        QCOMPARE(udmabuf(), QString("65536/2048"));
        /* tuning turned off: back at once, and said */
        HostSettings::setEnabled(false);
        hs.preferencesChanged();
        QVERIFY(answered(answers, qemu.pid(), false, "host tuning is off"));
        QTRY_COMPARE(udmabuf(), QString("1024/64"));
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
