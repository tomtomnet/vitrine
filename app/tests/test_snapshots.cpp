// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include "core/snapshots.h"
#include "core/vmrunner.h"

/* Runs $QGM_TEST_QEMU, else the qemu-system-x86_64 in PATH, headless */
static QString testQemu()
{
    const QString env = qEnvironmentVariable("QGM_TEST_QEMU");
    return env.isEmpty() ? QStandardPaths::findExecutable("qemu-system-x86_64") : env;
}

/* What qemu-img info --output=json -U printed, cut down */
static const char kInfo[] = R"({
    "filename": "disk.qcow2", "format": "qcow2", "virtual-size": 67108864,
    "snapshots": [
        {"icount": 0, "vm-clock-nsec": 0, "name": "offline1", "date-sec": 1790340757,
         "date-nsec": 623427000, "vm-clock-sec": 0, "id": "1", "vm-state-size": 0},
        {"vm-clock-nsec": 17581621, "name": "live1", "date-sec": 1790340769,
         "date-nsec": 261696000, "vm-clock-sec": 1, "id": "2", "vm-state-size": 821653}
    ]
})";

/* What query-block answered, cut down: the running state is on the first disk only */
static const char kQueryBlock[] = R"([
    {"device": "virtio0", "removable": false, "inserted": {"ro": false, "file": "disk.qcow2",
     "drv": "qcow2", "image": {"filename": "disk.qcow2", "format": "qcow2", "snapshots": [
        {"name": "offline1", "date-sec": 1790340757, "date-nsec": 623427000,
         "vm-clock-sec": 0, "vm-clock-nsec": 0, "id": "1", "vm-state-size": 0},
        {"name": "live1", "date-sec": 1790340769, "date-nsec": 261696000,
         "vm-clock-sec": 1, "vm-clock-nsec": 17581621, "id": "2", "vm-state-size": 821653}]}}},
    {"device": "virtio1", "removable": false, "inserted": {"ro": false, "file": "data.qcow2",
     "drv": "qcow2", "image": {"filename": "data.qcow2", "format": "qcow2", "snapshots": [
        {"name": "live1", "date-sec": 1790340769, "date-nsec": 261696000,
         "vm-clock-sec": 1, "vm-clock-nsec": 17581621, "id": "2", "vm-state-size": 0}]}}},
    {"device": "ide2-cd0", "removable": true, "inserted": {"ro": true, "file": "fedora.iso",
     "drv": "raw", "image": {"filename": "fedora.iso", "format": "raw"}}},
    {"device": "ide1-cd0", "removable": true, "tray_open": false}
])";

class TestSnapshots : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir tmp;
    int count = 0;

    QString qemuImg() const { return QFileInfo(testQemu()).dir().filePath("qemu-img"); }

    void create(const QString &file, const QString &format = "qcow2")
    {
        QCOMPARE(QProcess::execute(qemuImg(), {"create", "-q", "-f", format, file, "16M"}), 0);
    }

    /* The names, and whether they hold the running state: "a*" for a with it */
    static QStringList names(const VmSnapshots &snapshots)
    {
        QStringList list;
        for (const VmSnapshots::Snapshot &s : snapshots.snapshots()) {
            list << s.name + (s.stateBytes > 0 ? "*" : "");
        }
        return list;
    }

    /* @action, then its error */
    static QString act(VmSnapshots &snapshots, const std::function<void()> &action)
    {
        QSignalSpy finished(&snapshots, &VmSnapshots::finished);
        QSignalSpy listed(&snapshots, &VmSnapshots::listed);

        action();
        if (finished.isEmpty() && !finished.wait(30000)) {
            return "no answer";
        }
        const QString error = finished.first().first().toString();
        /* the list after it */
        if (listed.isEmpty()) {
            listed.wait(10000);
        }
        return error;
    }

    static QString list(VmSnapshots &snapshots)
    {
        QSignalSpy listed(&snapshots, &VmSnapshots::listed);

        snapshots.refresh();
        if (listed.isEmpty() && !listed.wait(10000)) {
            return "no answer";
        }
        return listed.first().first().toString();
    }

private slots:
    /* the QEMU and qemu-img of the VMs come from #qemu: the preferences are shared with
       the other suites, which may change them meanwhile */
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    void drives()
    {
        const ArgsFile args = ArgsFile::parse(
            "-drive if=pflash,format=qcow2,readonly=on,file=OVMF_CODE_4M.qcow2\n"
            "-drive if=pflash,format=qcow2,file=OVMF_VARS_4M.qcow2\n"
            "-drive file=disk.qcow2,format=qcow2,if=virtio\n"
            "-drive file=/data/raw.img,if=virtio\n"
            "-drive file=fedora.iso,media=cdrom\n"
            "-drive file=shared.qcow2,if=virtio,readonly=on\n"
            "-drive file=scratch.qcow2,if=virtio,snapshot=on\n"
            "-hdb old.qcow2\n"
            "-cdrom other.iso\n");
        QStringList seen;

        for (const VmSnapshots::Drive &d : VmSnapshots::drives(args, "/vms/a")) {
            seen << QString("%1 %2%3").arg(d.file, d.format, d.pflash ? " pflash" : "");
        }
        QCOMPARE(seen, QStringList({"/vms/a/disk.qcow2 qcow2", "/data/raw.img raw",
                                    "/vms/a/old.qcow2 qcow2",
                                    "/vms/a/OVMF_VARS_4M.qcow2 qcow2 pflash"}));
    }

    void merge()
    {
        const QJsonArray fromImg = QJsonDocument::fromJson(kInfo)["snapshots"].toArray();
        const QList<VmSnapshots::Snapshot> one = VmSnapshots::merge({{"disk.qcow2", fromImg}});

        QCOMPARE(one.size(), 2);
        QCOMPARE(one[0].name, "offline1");
        QCOMPARE(one[0].stateBytes, 0);
        QCOMPARE(one[1].name, "live1");
        QCOMPARE(one[1].stateBytes, 821653);
        QCOMPARE(one[1].vmClockMs, 1017);
        QCOMPARE(one[1].date, QDateTime::fromMSecsSinceEpoch(1790340769261));

        /* by name across the disks, the running state from the one that has it */
        const auto images = VmSnapshots::images(QJsonDocument::fromJson(kQueryBlock).array());
        QCOMPARE(images.size(), 2);
        QCOMPARE(images[1].first, "data.qcow2");
        const QList<VmSnapshots::Snapshot> all = VmSnapshots::merge(images);
        QCOMPARE(all.size(), 2);
        QCOMPARE(all[0].files, QStringList({"disk.qcow2"}));
        QCOMPARE(all[1].name, "live1");
        QCOMPARE(all[1].files, QStringList({"disk.qcow2", "data.qcow2"}));
        QCOMPARE(all[1].stateBytes, 821653);
    }

    void disksOnly()
    {
        QVERIFY(VmSnapshots::needsDisksOnly("virgl is not yet migratable"));
        QVERIFY(VmSnapshots::needsDisksOnly("Device 'pflash1' is writable but does not support "
                                            "snapshots"));
        QVERIFY(!VmSnapshots::needsDisksOnly("Snapshot 'x' does not exist in one or more devices"));
        QVERIFY(VmSnapshots::disksOnlyReason("virgl is not yet migratable").contains("3D"));
        QVERIFY(VmSnapshots::disksOnlyReason("Device 'pflash1' is writable but does not support "
                                             "snapshots").contains("UEFI"));

        const QJsonArray actions =
            VmSnapshots::diskActions(QJsonDocument::fromJson(kQueryBlock).array(), "s");
        QCOMPARE(actions.size(), 2);
        QCOMPARE(actions[0]["type"].toString(), "blockdev-snapshot-internal-sync");
        QCOMPARE(actions[0]["data"]["device"].toString(), "virtio0");
        QCOMPARE(actions[1]["data"]["device"].toString(), "virtio1");
        QCOMPARE(actions[1]["data"]["name"].toString(), "s");
    }

    void hmp()
    {
        QCOMPARE(VmSnapshots::hmpError(""), QString());
        QCOMPARE(VmSnapshots::hmpError("Error: Snapshot 'x' does not exist in one or more "
                                       "devices\r\n"),
                 "Snapshot 'x' does not exist in one or more devices");
        /* no raw string here: moc misreads R"(...\"...)" and skips the class */
        QCOMPARE(VmSnapshots::hmpQuote("my \"snap\" \\ 1"), "\"my \\\"snap\\\" \\\\ 1\"");
        QVERIFY(VmSnapshots::isValidName("2026-09-25 14:55:01"));
        /* QEMU would take it for the ID of another */
        QVERIFY(!VmSnapshots::isValidName("12"));
        QVERIFY(!VmSnapshots::isValidName(" padded"));
        QVERIFY(!VmSnapshots::isValidName(""));
        QVERIFY(VmSnapshots::explain("Device 'pflash1' is writable but does not support "
                                     "snapshots").contains("UEFI variables"));
        QVERIFY(VmSnapshots::explain("Device 'virtio1' is writable but does not support "
                                     "snapshots").contains("virtio1"));
        QVERIFY(VmSnapshots::explain("This is a disk-only snapshot. Revert to it  offline "
                                     "using qemu-img").contains("stop the VM"));
    }

    /* qemu-img on the files of a stopped VM */
    void stopped()
    {
        if (!QFileInfo(qemuImg()).isExecutable()) {
            QSKIP("no qemu-img, set QGM_TEST_QEMU");
        }
        const QString dir = tmp.filePath("stopped");
        QDir().mkpath(dir);
        create(dir + "/disk.qcow2");
        create(dir + "/data.qcow2");
        create(dir + "/raw.img", "raw");
        VmRunner runner("qgm-snap-stopped", dir);
        VmSnapshots snapshots(&runner);
        snapshots.setVm(ArgsFile::parse("#qemu " + testQemu() + "\n"
                                        "-drive file=disk.qcow2,format=qcow2,if=virtio\n"
                                        "-drive file=data.qcow2,format=qcow2,if=virtio\n"
                                        "-drive file=raw.img,format=raw,if=virtio\n"),
                        dir);

        QCOMPARE(list(snapshots), QString());
        QVERIFY(snapshots.snapshots().isEmpty());
        QCOMPARE(act(snapshots, [&]() { snapshots.take("before update"); }), QString());
        QCOMPARE(names(snapshots), QStringList({"before update"}));
        QCOMPARE(snapshots.snapshots()[0].files,
                 QStringList({dir + "/disk.qcow2", dir + "/data.qcow2"}));
        /* the same name again: replaced, not doubled */
        QCOMPARE(act(snapshots, [&]() { snapshots.take("before update"); }), QString());
        QCOMPARE(act(snapshots, [&]() { snapshots.take("second"); }), QString());
        QCOMPARE(names(snapshots), QStringList({"before update", "second"}));
        QCOMPARE(act(snapshots, [&]() { snapshots.restore("before update"); }), QString());
        QCOMPARE(act(snapshots, [&]() { snapshots.remove("second"); }), QString());
        QCOMPARE(names(snapshots), QStringList({"before update"}));
        QVERIFY(act(snapshots, [&]() { snapshots.take("42"); }).contains("digits"));
        QVERIFY(act(snapshots, [&]() { snapshots.remove("nosuch"); }).contains("nosuch"));

        /* a disk that fails midway: none keeps the snapshot */
        QFile::setPermissions(dir + "/data.qcow2", QFileDevice::ReadOwner);
        QVERIFY(!act(snapshots, [&]() { snapshots.take("half"); }).isEmpty());
        QFile::setPermissions(dir + "/data.qcow2", QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        QCOMPARE(list(snapshots), QString());
        QCOMPARE(names(snapshots), QStringList({"before update"}));
    }

    /* QEMU while the VM runs: savevm, loadvm, delvm */
    void running()
    {
        if (!QFileInfo(testQemu()).isExecutable() || !QFileInfo(qemuImg()).isExecutable()) {
            QSKIP("no QEMU build, set QGM_TEST_QEMU");
        }
        const QString dir = tmp.filePath("running");
        QDir().mkpath(dir);
        create(dir + "/disk.qcow2");
        create(dir + "/data.qcow2");
        QCOMPARE(QProcess::execute(qemuImg(), {"snapshot", "-c", "offline", dir + "/disk.qcow2"}), 0);
        QCOMPARE(QProcess::execute(qemuImg(), {"snapshot", "-c", "offline", dir + "/data.qcow2"}), 0);
        const ArgsFile args = ArgsFile::parse("#qemu " + testQemu() + "\n"
                                              "-machine q35\n-m 64\n-nodefaults\n-display none\n"
                                              "-drive file=disk.qcow2,format=qcow2,if=virtio\n"
                                              "-drive file=data.qcow2,format=qcow2,if=virtio\n");
        VmRunner runner(QString("qgm-snap-%1-%2").arg(QCoreApplication::applicationPid()).arg(++count), dir);
        VmSnapshots snapshots(&runner);
        snapshots.setVm(args, dir);

        runner.start(args);
        QTRY_COMPARE_WITH_TIMEOUT(runner.state(), VmRunner::State::Running, 20000);
        QCOMPARE(list(snapshots), QString());
        QCOMPARE(names(snapshots), QStringList({"offline"}));
        QCOMPARE(act(snapshots, [&]() { snapshots.take("live \"one\""); }), QString());
        QCOMPARE(names(snapshots), QStringList({"offline", "live \"one\"*"}));
        QCOMPARE(snapshots.snapshots()[1].files.size(), 2);
        QCOMPARE(act(snapshots, [&]() { snapshots.restore("live \"one\""); }), QString());
        QTRY_COMPARE_WITH_TIMEOUT(runner.state(), VmRunner::State::Running, 5000);
        /* the disks only: refused before QEMU stops the VM for it */
        QVERIFY(act(snapshots, [&]() { snapshots.restore("offline"); }).contains("stop the VM"));
        QCOMPARE(runner.state(), VmRunner::State::Running);
        QCOMPARE(act(snapshots, [&]() { snapshots.remove("offline"); }), QString());
        QCOMPARE(names(snapshots), QStringList({"live \"one\"*"}));

        /* the running state from the start */
        runner.forceOff();
        QTRY_COMPARE_WITH_TIMEOUT(runner.state(), VmRunner::State::Stopped, 10000);
        QCOMPARE(list(snapshots), QString());
        QCOMPARE(names(snapshots), QStringList({"live \"one\"*"}));
        ArgsFile from = args;
        from.add("loadvm", "live \"one\"");
        runner.start(from);
        QTRY_COMPARE_WITH_TIMEOUT(runner.state(), VmRunner::State::Running, 20000);
        QCOMPARE(act(snapshots, [&]() { snapshots.remove("live \"one\""); }), QString());
        QVERIFY(snapshots.snapshots().isEmpty());
        runner.forceOff();
        QTRY_COMPARE_WITH_TIMEOUT(runner.state(), VmRunner::State::Stopped, 10000);
    }

    /* 3D graphics: QEMU cannot save the running state of virgl, so the disks only */
    void virgl()
    {
        if (!QFileInfo(testQemu()).isExecutable() || !QFileInfo(qemuImg()).isExecutable()) {
            QSKIP("no QEMU build, set QGM_TEST_QEMU");
        }
        const QString dir = tmp.filePath("virgl");
        QDir().mkpath(dir);
        create(dir + "/disk.qcow2");
        create(dir + "/data.qcow2");
        const ArgsFile args = ArgsFile::parse("#qemu " + testQemu() + "\n"
                                              "-machine q35\n-m 128\n-nodefaults\n"
                                              "-display egl-headless\n"
                                              "-device virtio-vga-gl,blob=on,hostmem=64M\n"
                                              "-drive file=disk.qcow2,format=qcow2,if=virtio\n"
                                              "-drive file=data.qcow2,format=qcow2,if=virtio\n");
        VmRunner runner(QString("qgm-snap-%1-%2").arg(QCoreApplication::applicationPid()).arg(++count), dir);
        VmSnapshots snapshots(&runner);
        QSignalSpy notice(&snapshots, &VmSnapshots::notice);
        snapshots.setVm(args, dir);

        runner.start(args);
        for (int i = 0; i < 200 && runner.state() != VmRunner::State::Running &&
                        !(i > 10 && runner.state() == VmRunner::State::Stopped); i++) {
            QTest::qWait(100);
        }
        if (runner.state() != VmRunner::State::Running) {
            QSKIP(qPrintable("QEMU does not run with virgl here: " + runner.errorString()));
        }
        QCOMPARE(act(snapshots, [&]() { snapshots.take("gl"); }), QString());
        QCOMPARE(names(snapshots), QStringList({"gl"}));
        QCOMPARE(snapshots.snapshots()[0].files.size(), 2);
        QCOMPARE(notice.size(), 1);
        QVERIFY2(notice[0][0].toString().contains("virgl"), qPrintable(notice[0][0].toString()));
        /* again: it replaces the first, as savevm would */
        QCOMPARE(act(snapshots, [&]() { snapshots.take("gl"); }), QString());
        QCOMPARE(names(snapshots), QStringList({"gl"}));
        QCOMPARE(runner.state(), VmRunner::State::Running);
        runner.forceOff();
        QTRY_COMPARE_WITH_TIMEOUT(runner.state(), VmRunner::State::Stopped, 10000);
    }

    /* A file QEMU writes to that is not qcow2 */
    void rawFiles()
    {
        if (!QFileInfo(testQemu()).isExecutable() || !QFileInfo(qemuImg()).isExecutable()) {
            QSKIP("no QEMU build, set QGM_TEST_QEMU");
        }
        const QString dir = tmp.filePath("raw");
        QDir().mkpath(dir);
        create(dir + "/disk.qcow2");
        create(dir + "/raw.img", "raw");
        const ArgsFile args = ArgsFile::parse("#qemu " + testQemu() + "\n"
                                              "-machine q35\n-m 64\n-nodefaults\n-display none\n"
                                              "-drive file=disk.qcow2,format=qcow2,if=virtio\n"
                                              "-drive file=raw.img,format=raw,if=virtio\n");
        VmRunner runner(QString("qgm-snap-%1-%2").arg(QCoreApplication::applicationPid()).arg(++count), dir);
        VmSnapshots snapshots(&runner);
        snapshots.setVm(args, dir);

        runner.start(args);
        QTRY_COMPARE_WITH_TIMEOUT(runner.state(), VmRunner::State::Running, 20000);
        /* the qcow2 disk alone, as when the VM is stopped, and a word on it */
        QSignalSpy notice(&snapshots, &VmSnapshots::notice);
        QCOMPARE(act(snapshots, [&]() { snapshots.take("x"); }), QString());
        QCOMPARE(names(snapshots), QStringList({"x"}));
        QCOMPARE(snapshots.snapshots()[0].files.size(), 1);
        QCOMPARE(QFileInfo(snapshots.snapshots()[0].files[0]).fileName(), "disk.qcow2");
        QCOMPARE(notice.size(), 1);
        QVERIFY2(notice[0][0].toString().contains("virtio1"), qPrintable(notice[0][0].toString()));
        QCOMPARE(runner.state(), VmRunner::State::Running);
        runner.forceOff();
        QTRY_COMPARE_WITH_TIMEOUT(runner.state(), VmRunner::State::Stopped, 10000);
    }
};

QTEST_GUILESS_MAIN(TestSnapshots)
#include "test_snapshots.moc"
