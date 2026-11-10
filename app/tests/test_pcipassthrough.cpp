// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * PciPassthrough: what a VM start needs for its PCI devices, the words of
 * the PCI settings, and a start through vitrine-helper's test build on a
 * fake PCI bus (helper/tests/fakesys.c), its QEMU a stand-in.  The fake
 * device nodes have numbers no node of this computer's has (4000:N), as
 * the app looks for them in the processes of the user running the tests.
 */
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <algorithm>

#include <unistd.h>

#include "core/hostdevices.h"
#include "core/paths.h"
#include "core/pcipassthrough.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"

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

/* A QEMU stand-in, stopped with the object */
class FakeQemu
{
public:
    FakeQemu()
    {
        m_process.start(FAKE_QEMU, {});
        m_process.waitForStarted();
        m_pid = m_process.processId();
        /* until it runs its program: the helper takes only a QEMU it sees */
        const QString exe = QFileInfo(FAKE_QEMU).canonicalFilePath();
        QElapsedTimer clock;
        clock.start();
        while (QFileInfo(QString("/proc/%1/exe").arg(m_pid)).canonicalFilePath() != exe &&
               clock.elapsed() < 5000) {
            QThread::msleep(5);
        }
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

class TestPciPassthrough : public QObject
{
    Q_OBJECT

private:
    QString m_root;

    QString path(const QString &rel) const { return m_root + rel; }
    QString pciDir(const QString &address) const { return path("/sys/devices/pci0000:00/" + address); }
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
        const QString groupDir = path(QString("/sys/kernel/iommu_groups/%1").arg(group));
        QDir().mkpath(groupDir + "/devices");
        QFile::link(groupDir, dir + "/iommu_group");
        QFile::link(dir, groupDir + "/devices/" + address);
    }
    void devNode(const QString &dir, const QByteArray &number, const QString &subsystem)
    {
        writeFile(dir + "/dev", number + '\n');
        QDir().mkpath(path("/sys/class/" + subsystem));
        QFile::link(path("/sys/class/" + subsystem), dir + "/subsystem");
    }
    QString driverOf(const QString &address) const
    {
        return QFileInfo(pciDir(address) + "/driver").symLinkTarget().section('/', -1);
    }
    /* a second GPU and its audio (group 21), a GPU set aside at boot (14),
       the GPU the computer started on (18) */
    void tree()
    {
        pci("0000:6e:00.0", "0x030000", "amdgpu", 21);
        writeFile(pciDir("0000:6e:00.0") + "/boot_vga", "0\n");
        devNode(pciDir("0000:6e:00.0") + "/drm/card1", "4000:1", "drm");
        writeFile(pciDir("0000:6e:00.0") + "/drm/card1/card1-DP-1/status", "disconnected\n");
        pci("0000:6e:00.1", "0x040300", "snd_hda_intel", 21);
        devNode(pciDir("0000:6e:00.1") + "/sound/card1/controlC1", "4000:10", "sound");
        pci("0000:01:00.0", "0x030000", "vfio-pci", 14);
        pci("0000:01:00.1", "0x040300", "vfio-pci", 14);
        writeFile(path("/dev/vfio/14"), "");
        writeFile(path("/dev/vfio/14.owner"), "0 0 600\n");
        pci("0000:65:00.0", "0x030000", "amdgpu", 18);
        writeFile(pciDir("0000:65:00.0") + "/boot_vga", "1\n");
        QDir().mkpath(path("/sys/bus/pci/drivers/vfio-pci"));
        writeFile(path("/sys/bus/pci/drivers_probe"), "");
        writeFile(path("/proc/meminfo"), "MemTotal:       32768000 kB\n");
        writeFile(path(QString("/memlock-%1").arg(getpid())), "8388608 8388608\n");
    }
    /* A PciPassthrough on the fake tree, through the helper's test build */
    void fake(PciPassthrough &pt) const
    {
        pt.setHelperCommand({HELPER_FAKE});
        pt.setSysRoot(path("/sys"));
        pt.setProcRoot(path("/proc"));
    }
    static bool saw(const QSignalSpy &spy, const QString &start)
    {
        return count(spy, start) > 0;
    }
    static qsizetype count(const QSignalSpy &spy, const QString &start)
    {
        return std::count_if(spy.cbegin(), spy.cend(), [&start](const QList<QVariant> &args) {
            return args.value(1).toString().startsWith(start);
        });
    }
    const QStringList m_both{"0000:6e:00.0", "0000:6e:00.1"};

private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void init()
    {
        m_root = QString(TEST_WORK_DIR) + "/pcipassthrough-" + QTest::currentTestFunction();
        QDir(m_root).removeRecursively();
        QDir().mkpath(path("/run"));
        tree();
        qputenv("VITRINE_HELPER_TEST_ROOT", m_root.toLocal8Bit());
        qunsetenv("VITRINE_HELPER");
    }

    /* What a start asks the helper for */
    void plan()
    {
        const QString sys = path("/sys"), proc = path("/proc");
        const qint64 limit = HostDevices::memlockLimit();

        QCOMPARE(PciPassthrough::plan({"0000:6e:00.0", "0000:6e:00.1"}, 3072, sys, proc).verb,
                 QString("devices-bind"));
        /* the GPU alone: its audio function is the host's */
        const PciPassthrough::Plan alone = PciPassthrough::plan({"0000:6e:00.0"}, 3072, sys, proc);
        QVERIFY2(alone.problem.startsWith("PCI passthrough: 6e:00.1 ") &&
                     alone.problem.endsWith(" shares IOMMU group 21 and is bound to snd_hda_intel: "
                                            "pass it through too."),
                 qPrintable(alone.problem));
        QVERIFY(alone.verb.isEmpty());
        QCOMPARE(PciPassthrough::plan({"0000:65:00.0"}, 3072, sys, proc).problem,
                 QString("PCI passthrough: 65:00.0 is the GPU this computer started on: it keeps it "
                         "for its own screen."));
        QCOMPARE(PciPassthrough::plan({"0000:99:00.0"}, 3072, sys, proc).problem,
                 QString("PCI passthrough: 0000:99:00.0 is not in this computer."));
        /* set aside at boot: the node, the limit */
        QFile::setPermissions(path("/dev/vfio/14"), QFileDevice::Permissions());
        QCOMPARE(PciPassthrough::plan({"0000:01:00.0", "0000:01:00.1"}, 3072, sys, proc).verb,
                 QString("devices"));
        QFile::setPermissions(path("/dev/vfio/14"), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        if (limit < 0 || limit >= (3072LL << 20)) {
            QSKIP("the locked memory limit is enough for any VM here");
        }
        QCOMPARE(PciPassthrough::plan({"0000:01:00.0", "0000:01:00.1"}, 3072, sys, proc).verb,
                 QString("devices"));
        /* all set up by hand: nothing to ask for */
        QCOMPARE(PciPassthrough::plan({"0000:01:00.0", "0000:01:00.1"}, limit >> 20, sys, proc).verb,
                 QString());
    }

    /* The words of the PCI settings */
    void status()
    {
        const HostDevices::OpenNodes open;
        const QString sys = path("/sys");
        const QStringList both{"0000:6e:00.0", "0000:6e:00.1"};

        /* without the helper: what doing it by hand still lacks */
        qputenv("VITRINE_HELPER", path("/nowhere").toLocal8Bit());
        PciPassthrough::Status s =
            PciPassthrough::status(HostDevices::pciDevice("0000:6e:00.0", sys), both, 2048, open, sys);
        QCOMPARE(s.text, QString("0000:6e:00.0 is bound to amdgpu instead of vfio-pci"));
        QVERIFY(s.details.contains("vitrine-helper, which is not installed"));
        QVERIFY(!s.ok);
        /* with it: taken at start */
        qputenv("VITRINE_HELPER", HELPER_FAKE);
        s = PciPassthrough::status(HostDevices::pciDevice("0000:6e:00.0", sys), both, 2048, open, sys);
        QCOMPARE(s.text, QString("Taken from amdgpu at start"));
        QVERIFY(s.details.contains("administrator's password"));
        QVERIFY(s.ok);
        s = PciPassthrough::status(HostDevices::pciDevice("0000:65:00.0", sys), {"0000:65:00.0"}, 2048,
                                   open, sys);
        QCOMPARE(s.text, QString("65:00.0 is the GPU this computer started on: it keeps it for its own "
                                 "screen"));
        QVERIFY(!s.ok);
        QFile::setPermissions(path("/dev/vfio/14"), QFileDevice::Permissions());
        s = PciPassthrough::status(HostDevices::pciDevice("0000:01:00.0", sys),
                                   {"0000:01:00.0", "0000:01:00.1"}, 2048, open, sys);
        QCOMPARE(s.text, QString("Ready at start"));
        QVERIFY(s.details.contains("without a password for members of the vitrine group"));
        QFile::setPermissions(path("/dev/vfio/14"), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        qunsetenv("VITRINE_HELPER");
    }

    /* The helper's refusals, as a start's error says them */
    void explain()
    {
        QCOMPARE(PciPassthrough::explain("error vfio 0000:6e:00.0: 0000:6e:00.0 is in use by "
                                         "gnome-shell (pid 2345)"),
                 QString("PCI passthrough: 0000:6e:00.0 is in use by gnome-shell (pid 2345). GNOME "
                         "uses every GPU it finds: a udev rule tagging this one "
                         "TAG+=\"mutter-device-ignore\" keeps GNOME away from it from the next "
                         "login, or bind it to vfio-pci at boot (README.md, PCI passthrough)."));
        QCOMPARE(PciPassthrough::explain("error vfio 0000:02:00.0: the network interface enp2s0 of "
                                         "0000:02:00.0 is up: take it down first (sudo ip link set "
                                         "enp2s0 down)"),
                 QString("PCI passthrough: the network interface enp2s0 of 0000:02:00.0 is up: take "
                         "it down first (sudo ip link set enp2s0 down)."));
        QVERIFY(PciPassthrough::explain("error vfio 0000:65:00.0: 0000:65:00.0 is the GPU this "
                                        "computer started on (boot_vga)")
                    .endsWith(" Choose another GPU as the primary one in the firmware settings, or "
                              "pass another GPU through."));
        QCOMPARE(PciPassthrough::explain("error vfio: cannot load vfio-pci: modprobe: FATAL: Module "
                                         "vfio-pci not found"),
                 QString("PCI passthrough: cannot load vfio-pci: modprobe: FATAL: Module vfio-pci "
                         "not found."));
    }

    /*
     * A start: the helper takes the GPU and its audio from their drivers,
     * the QEMU launched is watched, and its end gives them back
     */
    void startAndStop()
    {
        PciPassthrough pt(nullptr);
        QSignalSpy lines(&pt, &PciPassthrough::helperLine),
            finished(&pt, &PciPassthrough::helperFinished);
        QString answer = "none";
        fake(pt);
        pt.prepare("vm1", "VM 1", {"0000:6e:00.0", "0000:6e:00.1"}, 3072,
                   [&answer](const QString &error) { answer = error; });
        QTRY_COMPARE(answer, QString());
        QCOMPARE(driverOf("0000:6e:00.0"), QString("vfio-pci"));
        QCOMPARE(driverOf("0000:6e:00.1"), QString("vfio-pci"));
        QCOMPARE(pt.takenFrom("vm1", "0000:6e:00.0"), QString("amdgpu"));
        QCOMPARE(pt.takenFrom("vm1", "0000:6e:00.1"), QString("snd_hda_intel"));
        QCOMPARE(readFile(path(QString("/memlock-%1").arg(getpid()))), QByteArray("3221225472 3221225472"));
        FakeQemu qemu;
        pt.launched("vm1", qemu.pid());
        QTRY_VERIFY(saw(lines, QString("ok watch %1").arg(qemu.pid())));
        QTRY_COMPARE(readFile(path(QString("/memlock-%1").arg(getpid()))), QByteArray("8388608 8388608"));
        QVERIFY(pt.helperRunning("vm1"));
        qemu.stop();
        pt.stopped("vm1");
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(saw(lines, "restored vfio 21: 0000:6e:00.0 back to amdgpu, 0000:6e:00.1 back to "
                           "snd_hda_intel"));
        QCOMPARE(driverOf("0000:6e:00.0"), QString("amdgpu"));
        QVERIFY(!pt.helperRunning("vm1"));
        QCOMPARE(pt.takenFrom("vm1", "0000:6e:00.0"), QString());
    }

    /* What only root sees refuses it: the helper's words, nothing taken */
    void refusedByTheHelper()
    {
        writeFile(path("/users"), "c 4000:1 2345 gnome-shell\n");
        PciPassthrough pt(nullptr);
        QSignalSpy finished(&pt, &PciPassthrough::helperFinished);
        QString answer = "none";
        fake(pt);
        pt.prepare("vm1", "VM 1", {"0000:6e:00.0", "0000:6e:00.1"}, 3072,
                   [&answer](const QString &error) { answer = error; });
        QTRY_VERIFY(answer != "none");
        QVERIFY2(answer.startsWith("PCI passthrough: 0000:6e:00.0 is in use by gnome-shell (pid "
                                   "2345). GNOME uses every GPU it finds"),
                 qPrintable(answer));
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(driverOf("0000:6e:00.0"), QString("amdgpu"));
    }

    /* Set aside at boot, the node and the limit enough: no helper at all */
    void readyAlready()
    {
        const qint64 limit = HostDevices::memlockLimit();
        if (limit >= 0 && limit < (2LL << 20)) {
            QSKIP("the locked memory limit is below 2 MiB");
        }
        PciPassthrough pt(nullptr);
        QString answer = "none";
        fake(pt);
        pt.setHelperCommand({"/bin/false"});
        pt.prepare("vm1", "VM 1", {"0000:01:00.0", "0000:01:00.1"}, 1,
                   [&answer](const QString &error) { answer = error; });
        QCOMPARE(answer, QString());
        QVERIFY(!pt.helperRunning("vm1"));
    }

    /* The start ends before QEMU (the answer came, or not yet): all goes back */
    void stoppedBeforeQemu()
    {
        PciPassthrough pt(nullptr);
        QSignalSpy finished(&pt, &PciPassthrough::helperFinished);
        QString answer = "none";
        fake(pt);
        pt.prepare("vm1", "VM 1", {"0000:6e:00.0", "0000:6e:00.1"}, 3072,
                   [&answer](const QString &error) { answer = error; });
        QTRY_COMPARE(answer, QString());
        pt.stopped("vm1");
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(driverOf("0000:6e:00.0"), QString("amdgpu"));
        QCOMPARE(readFile(path(QString("/memlock-%1").arg(getpid()))), QByteArray("8388608 8388608"));

        /* Force Off while it is asked: its request never goes, the answer comes to nothing */
        answer = "none";
        pt.prepare("vm1", "VM 1", {"0000:6e:00.0", "0000:6e:00.1"}, 3072,
                   [&answer](const QString &error) { answer = error; });
        pt.stopped("vm1");
        QTRY_COMPARE(finished.size(), 2);
        QCOMPARE(answer, QString("none"));
        QCOMPARE(driverOf("0000:6e:00.0"), QString("amdgpu"));
    }

    /* Two VMs, one group: the second does not start while the first runs */
    void twoVmsOneGroup()
    {
        PciPassthrough pt(nullptr);
        QString first = "none", second = "none";
        fake(pt);
        pt.prepare("vm1", "VM 1", {"0000:6e:00.0", "0000:6e:00.1"}, 3072,
                   [&first](const QString &error) { first = error; });
        QTRY_COMPARE(first, QString());
        pt.prepare("vm2", "VM 2", {"0000:6e:00.1", "0000:6e:00.0"}, 2048,
                   [&second](const QString &error) { second = error; });
        QCOMPARE(second, QString("PCI passthrough: 0000:6e:00.1 goes to VM 1, which runs."));
        pt.stopped("vm1");
        QTRY_COMPARE(driverOf("0000:6e:00.0"), QString("amdgpu"));
    }

    /*
     * A start waiting for the helper of the VM's run before, ended meanwhile
     * (Force Off): it never comes, not at its time, nor when that helper ends
     */
    void stoppedWhileWaiting()
    {
        PciPassthrough pt(nullptr);
        QSignalSpy lines(&pt, &PciPassthrough::helperLine),
            finished(&pt, &PciPassthrough::helperFinished);
        QString first = "none", second = "none";
        fake(pt);
        pt.setWaitMs(200);
        pt.prepare("vm1", "VM 1", m_both, 3072, [&first](const QString &error) { first = error; });
        QTRY_COMPARE(first, QString());
        FakeQemu qemu;
        pt.launched("vm1", qemu.pid());
        QTRY_VERIFY(saw(lines, "ok watch "));
        /* its helper runs on, watching: the next start waits for it */
        pt.prepare("vm1", "VM 1", m_both, 3072, [&second](const QString &error) { second = error; });
        QVERIFY(pt.helperRunning("vm1"));
        pt.stopped("vm1");
        QTest::qWait(600);
        QCOMPARE(second, QString("none"));
        QCOMPARE(count(lines, "ready"), 1);
        qemu.stop();
        QTRY_COMPARE(finished.size(), 1);
        QTest::qWait(300);
        QCOMPARE(second, QString("none"));
        QCOMPARE(count(lines, "ready"), 1);
        QCOMPARE(driverOf("0000:6e:00.0"), QString("amdgpu"));
    }

    /*
     * devices, planned for devices on vfio-pci, finds them on their drivers:
     * its start gave back what a dead helper had left.  Once more, with
     * devices-bind
     */
    void retriedWithDevicesBind()
    {
        const qint64 limit = HostDevices::memlockLimit();
        if (limit < 0 || limit >= (3072LL << 20)) {
            QSKIP("the locked memory limit is enough: no helper at all");
        }
        /* what a helper killed after its binding left: the devices, the node, its record */
        for (const QString &a : m_both) {
            QFile::remove(pciDir(a) + "/driver");
            QFile::link(path("/sys/bus/pci/drivers/vfio-pci"), pciDir(a) + "/driver");
            writeFile(pciDir(a) + "/driver_override", "vfio-pci\n");
        }
        writeFile(path("/dev/vfio/21"), "");
        writeFile(path("/dev/vfio/21.owner"), QByteArray::number(getuid()) + " 0 600\n");
        writeFile(path("/dev/vfio/21.rdev"), "239:21\n");
        const QString state = path("/run/vitrine-helper");
        QDir().mkpath(state);
        QFile::setPermissions(state, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                         QFileDevice::ExeOwner);
        writeFile(state + "/vfio-21.state",
                  QString("group 21\nuid %1\ndev 0000:6e:00.0 amdgpu - 1002:164e\n"
                          "dev 0000:6e:00.1 snd_hda_intel - 1002:164e\nnode 0 0 600\n")
                      .arg(getuid())
                      .toLatin1());
        QCOMPARE(PciPassthrough::plan(m_both, 3072, path("/sys"), path("/proc")).verb,
                 QString("devices"));

        PciPassthrough pt(nullptr);
        QSignalSpy lines(&pt, &PciPassthrough::helperLine),
            finished(&pt, &PciPassthrough::helperFinished);
        QString answer = "none";
        fake(pt);
        pt.prepare("vm1", "VM 1", m_both, 3072, [&answer](const QString &error) { answer = error; });
        QTRY_COMPARE(answer, QString());
        QVERIFY(saw(lines, "restored vfio 21: 0000:6e:00.0 back to amdgpu"));
        QVERIFY(saw(lines, "error vfio 0000:6e:00.0: 0000:6e:00.0 is bound to amdgpu: taking it "
                           "from this computer needs vitrine-helper devices-bind"));
        QVERIFY(saw(lines, "ok vfio 0000:6e:00.0=amdgpu,0000:6e:00.1=snd_hda_intel"));
        QCOMPARE(driverOf("0000:6e:00.0"), QString("vfio-pci"));
        pt.stopped("vm1");
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(driverOf("0000:6e:00.0"), QString("amdgpu"));
    }

    /* A helper that never answers - stuck in a driver - does not keep the start for good */
    void helperDoesNotAnswer()
    {
        PciPassthrough pt(nullptr);
        QString answer = "none";
        fake(pt);
        pt.setHelperCommand({"/bin/sh", "-c", "sleep 3", "sh"});
        pt.setAnswerMs(300);
        pt.prepare("vm1", "VM 1", m_both, 3072, [&answer](const QString &error) { answer = error; });
        QTRY_COMPARE(answer, QString("PCI passthrough: vitrine-helper did not answer in time."));
    }

    /* The helper's own error before any answer: said as it is */
    void helperErrorSaid()
    {
        PciPassthrough pt(nullptr);
        QSignalSpy finished(&pt, &PciPassthrough::helperFinished);
        QString answer = "none";
        fake(pt);
        pt.setHelperCommand(
            {"/bin/sh", "-c", "echo 'error: cannot use /run/vitrine-helper'; exit 1", "sh"});
        pt.prepare("vm1", "VM 1", m_both, 3072, [&answer](const QString &error) { answer = error; });
        QTRY_COMPARE(answer, QString("PCI passthrough: vitrine-helper: cannot use /run/vitrine-helper."));
        QTRY_COMPARE(finished.size(), 1);
    }

    /* Through a VM's runner, as the app has them */
    void throughTheRunner()
    {
        /* its runtime folder is named after it: none of this computer's VMs' */
        const QString id = QString("vitrine-test-%1-pci").arg(getpid());
        QTemporaryDir vms;
        writeFile(vms.filePath(id + "/vm.args"), "-name vm1\n-m 1G\n-display none\n"
                                                 "-device vfio-pci,host=6e:00.0\n"
                                                 "-device vfio-pci,host=6e:00.1\n");
        VmStore store(vms.path());
        PciPassthrough pt(&store);
        QSignalSpy lines(&pt, &PciPassthrough::helperLine),
            finished(&pt, &PciPassthrough::helperFinished);
        fake(pt);
        Paths::setQemuBinary(FAKE_QEMU);
        Vm *vm = store.find(id);
        QVERIFY(vm);
        QSignalSpy launched(vm->runner(), &VmRunner::qemuLaunched);
        vm->runner()->start(vm->args());
        QTRY_VERIFY(saw(lines, "ok vfio 0000:6e:00.0=amdgpu,0000:6e:00.1=snd_hda_intel"));
        QTRY_COMPARE(launched.size(), 1);
        /* the stand-in ends at once, its input none: the devices go back with it */
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QCOMPARE(driverOf("0000:6e:00.0"), QString("amdgpu"));
        QTRY_COMPARE(vm->runner()->state(), VmRunner::State::Stopped);
        QVERIFY(QString::fromUtf8(readFile(vm->runner()->logPath()))
                    .contains("vitrine: PCI devices: 0000:6e:00.0=amdgpu,0000:6e:00.1=snd_hda_intel"));
        Paths::setQemuBinary({});
    }
};

QTEST_GUILESS_MAIN(TestPciPassthrough)
#include "test_pcipassthrough.moc"
