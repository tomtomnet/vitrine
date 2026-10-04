// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The udmabuf limits as read for this user, the lines of qemu.log that say
 * a guest buffer was refused, and UdmabufWatch on VMs found running (a
 * stand-in QEMU and its monitor) without host tuning.
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

#include <cerrno>
#include <memory>

#include <unistd.h>

#include "core/hostsettings.h"
#include "core/paths.h"
#include "core/udmabuf.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"

static bool writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

static bool appendFile(const QString &path, const QByteArray &data)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Append) && f.write(data) == data.size();
}

static QString readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
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
            QLocalSocket *peer = m_server.nextPendingConnection();
            connect(peer, &QLocalSocket::readyRead, this, [this, peer]() { read(peer); });
            peer->write(R"({"QMP": {"version": {"qemu": {"major": 11}}, "capabilities": []}})"
                        "\n");
        });
    }

private:
    void read(QLocalSocket *peer)
    {
        QByteArray &buffer = m_buffers[peer];
        buffer += peer->readAll();
        qsizetype nl;
        while ((nl = buffer.indexOf('\n')) >= 0) {
            const QJsonObject command = QJsonDocument::fromJson(buffer.left(nl)).object();
            buffer.remove(0, nl + 1);
            const QByteArray result = command["execute"] == "query-status"
                                          ? R"({"running": true, "status": "running"})"
                                          : "{}";
            peer->write("{\"return\": " + result + ", \"id\": " +
                        QByteArray::number(command["id"].toInteger()) + "}\n");
        }
    }

    QLocalServer m_server;
    QHash<QLocalSocket *, QByteArray> m_buffers;
};

/* A VM found running: a stand-in QEMU with the runner's -qmp, its pid file
   and its monitor */
class FakeVm
{
public:
    FakeVm(const QString &vms, const QString &id, const QByteArray &args)
    {
        QDir(vms).mkpath(id);
        writeFile(vms + "/" + id + "/vm.args", "-name " + id.toLatin1() + "\n" + args);
        const QString run = Paths::vmRuntimeDir(id);
        m_qemu.start(FAKE_QEMU, {"-qmp", QString("unix:%1/qmp.sock,server=on,wait=off").arg(run)});
        m_qemu.waitForStarted();
        writeFile(run + "/qemu.pid", QByteArray::number(m_qemu.processId()) + "\n");
        m_qmp = std::make_unique<FakeQmp>(run + "/qmp.sock");
    }
    ~FakeVm() { stop(); }
    /* QEMU ends: its monitor goes, the process too */
    void stop()
    {
        m_qmp.reset();
        if (m_qemu.state() != QProcess::NotRunning) {
            m_qemu.kill();
            m_qemu.waitForFinished();
        }
    }

private:
    QProcess m_qemu;
    std::unique_ptr<FakeQmp> m_qmp;
};

static const QByteArray kNative = "-device virtio-vga-gl,blob=on,drm_native_context=on\n"
                                  "-display dbus,p2p=yes\n";
static const QByteArray kVirgl = "-device virtio-vga-gl\n-display dbus,p2p=yes\n";

/* From a refused 4K window (QEMU with vitrine's patches, amdgpu native context) */
static const QByteArray kRefusal =
    "2026-10-03T23:16:05.596504Z qemu-system-x86_64: warning: virtio_gpu_create_udmabuf_fd: "
    "UDMABUF_CREATE_LIST: Invalid argument (7473 entries, 32043008 bytes)\n"
    "virtio_gpu_virgl_process_cmd: ctrl 0x10c, error 0x1201\n"
    "drm: amdgpu_get_object_from_res_id:203: [2|kwin_wayland]: Couldn't find res_id: 388 "
    "[amdgpu_ccmd_bo_query_info]\n"
    "drm: amdgpu_ccmd_bo_query_info:718: [2|kwin_wayland]: Cannot find object\n"
    "virtio_gpu_virgl_process_cmd: ctrl 0x102, error 0x1203\n";

class TestUdmabuf : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_tmp;
    QString m_sys;
    QString m_device;

    void setLimits(qint64 list, qint64 sizeMb) const
    {
        writeFile(m_sys + "/module/udmabuf/parameters/list_limit", QByteArray::number(list) + '\n');
        writeFile(m_sys + "/module/udmabuf/parameters/size_limit_mb",
                  QByteArray::number(sizeMb) + '\n');
    }

private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void init()
    {
        QSettings(Paths::settingsPath(), QSettings::IniFormat).remove("host");
        m_sys = m_tmp.path() + "/" + QTest::currentTestFunction() + "/sys";
        QDir(m_sys).removeRecursively();
        /* a file that opens read-write stands for the device */
        m_device = m_tmp.path() + "/" + QTest::currentTestFunction() + "/udmabuf";
        writeFile(m_device, QByteArray());
        setLimits(1024, 64);
    }

    void limits_data()
    {
        QTest::addColumn<qint64>("list");
        QTest::addColumn<qint64>("sizeMb");
        QTest::addColumn<bool>("low");
        QTest::newRow("the kernel's defaults") << qint64(1024) << qint64(64) << true;
        QTest::newRow("host tuning's") << qint64(65536) << qint64(2048) << false;
        QTest::newRow("the minimum") << qint64(16384) << qint64(128) << false;
        QTest::newRow("one entry short") << qint64(16383) << qint64(128) << true;
        QTest::newRow("one MB short") << qint64(16384) << qint64(127) << true;
    }
    void limits()
    {
        QFETCH(qint64, list);
        QFETCH(qint64, sizeMb);
        QFETCH(bool, low);
        setLimits(list, sizeMb);
        const Udmabuf::Limits l = Udmabuf::read(m_sys, m_device);
        QCOMPARE(l.listLimit, list);
        QCOMPARE(l.sizeLimitMb, sizeMb);
        QCOMPARE(l.deviceErrno, 0);
        QVERIFY(l.known());
        QVERIFY(l.raisable());
        QCOMPARE(l.low(), low);
    }

    void problems()
    {
        Udmabuf::Limits l = Udmabuf::read(m_sys, m_device);
        QCOMPARE(Udmabuf::problem(l),
                 QString("the host's udmabuf limits are 1024 entries and 64 MB, below what native "
                         "context needs (16384 and 128 MB)"));

        /* no device: host tuning cannot help */
        l = Udmabuf::read(m_sys, m_device + ".none");
        QCOMPARE(l.deviceErrno, ENOENT);
        QVERIFY(l.low());
        QVERIFY(!l.raisable());
        QCOMPARE(Udmabuf::problem(l), QString("/dev/udmabuf does not exist: the kernel has no "
                                              "udmabuf driver, or its module is not loaded"));
        if (geteuid() != 0) {
            QFile::setPermissions(m_device, QFileDevice::ReadOwner);
            l = Udmabuf::read(m_sys, m_device);
            QCOMPARE(l.deviceErrno, EACCES);
            QCOMPARE(Udmabuf::problem(l), QString("/dev/udmabuf is not open to you: systemd gives "
                                                  "it to the user of the local desktop session"));
        }

        /* no parameters: a module not loaded, or something not a number */
        QDir(m_sys).removeRecursively();
        l = Udmabuf::read(m_sys, m_device + ".none");
        QVERIFY(!l.known());
        writeFile(m_sys + "/module/udmabuf/parameters/list_limit", "-1\n");
        writeFile(m_sys + "/module/udmabuf/parameters/size_limit_mb", "64\n");
        l = Udmabuf::read(m_sys, QStringLiteral("/dev/null"));
        QCOMPARE(l.listLimit, qint64(-1));
        QCOMPARE(l.sizeLimitMb, qint64(64));
        QVERIFY(l.low());
        QVERIFY(!l.raisable());
        QCOMPARE(Udmabuf::problem(l), QString("the host's udmabuf limits cannot be read"));
    }

    /* What the docs and the explanation tell to type */
    void persistentWays()
    {
        QCOMPARE(Udmabuf::grubbyCommand(),
                 QString("sudo grubby --update-kernel=ALL --args='udmabuf.list_limit=65536 "
                         "udmabuf.size_limit_mb=2048'"));
        QCOMPARE(Udmabuf::tmpfilesPath(), QString("/etc/tmpfiles.d/udmabuf.conf"));
        QCOMPARE(Udmabuf::tmpfilesContent(),
                 QString("w /sys/module/udmabuf/parameters/list_limit - - - - 65536\n"
                         "w /sys/module/udmabuf/parameters/size_limit_mb - - - - 2048\n"));
    }

    void logCount()
    {
        Udmabuf::LogCount count;
        for (const QByteArray &line : (kRefusal + kRefusal).split('\n')) {
            count.scan(line);
        }
        QCOMPARE(count.createList, 2);
        QCOMPARE(count.outOfMemory, 2);
        QCOMPARE(count.refusing, 0);
        QCOMPARE(count.unknownResource, 2);
        QCOMPARE(count.refusals(), 2);

        /* with -d guest_errors: one more line per refusal, still one refusal */
        count.scan("virgl_cmd_resource_create_blob: no dma-buf for guest blob 388, refusing it");
        count.scan("virgl_cmd_resource_create_blob: no dma-buf for guest blob 389, refusing it");
        count.scan("virgl_cmd_resource_create_blob: no dma-buf for guest blob 390, refusing it");
        QCOMPARE(count.refusals(), 3);

        /* only the renderer's lines (Xe, an older QEMU) */
        Udmabuf::LogCount xe;
        xe.scan("drm: xe_ccmd_vm_bind:612: [3|kwin_wayland]: invalid res_id 52 in bind op 0");
        QVERIFY(xe.any());
        QCOMPARE(xe.refusals(), 1);

        /* vitrine's own notes, and the rest, count for nothing */
        Udmabuf::LogCount none;
        none.scan("vitrine: QEMU will refuse guest windows: UDMABUF_CREATE_LIST, error 0x1201");
        none.scan("virtio_gpu_virgl_process_cmd: ctrl 0x102, error 0x1203");
        none.scan("");
        QVERIFY(!none.any());
        QCOMPARE(none.refusals(), 0);
    }

    /*
     * VMs found running without host tuning: the native-context one gets
     * the start check (limits too low: a note in its log, an issue), the
     * other none; refusals in any VM's log are counted as they come; an
     * issue goes with its VM's run
     */
    void watchWithoutTuning()
    {
        QTemporaryDir runtime(QDir::tempPath() + "/vu-XXXXXX");
        QTemporaryDir vms;
        QVERIFY(runtime.isValid() && vms.isValid());
        qputenv("XDG_RUNTIME_DIR", runtime.path().toLocal8Bit());
        FakeVm native(vms.path(), "native", kNative), virgl(vms.path(), "virgl", kVirgl);
        VmStore store(vms.path());
        UdmabufWatch watch(&store, nullptr);
        watch.setSysRoot(m_sys);
        watch.setDevice(m_device);
        watch.setPollInterval(100000);
        QSignalSpy changed(&watch, &UdmabufWatch::changed);
        for (Vm *vm : store.vms()) {
            vm->runner()->attach(vm->args());
        }
        QTRY_COMPARE(store.find("native")->runner()->state(), VmRunner::State::Running);
        QTRY_COMPARE(store.find("virgl")->runner()->state(), VmRunner::State::Running);

        QList<UdmabufWatch::Issue> issues = watch.issues();
        QCOMPARE(issues.size(), 1);
        QCOMPARE(issues[0].vmId, QString("native"));
        QVERIFY(issues[0].limitsLow);
        QCOMPARE(issues[0].notRaised, QString("host tuning is off"));
        QCOMPARE(issues[0].text(),
                 QString("Guest windows drawn by the CPU will be copied: the host's udmabuf limits "
                         "are 1024 entries and 64 MB, below what native context needs (16384 and "
                         "128 MB)"));
        QCOMPARE(changed.size(), 1);
        const QString nativeLog = store.find("native")->runner()->logPath();
        const QString virglLog = store.find("virgl")->runner()->logPath();
        QCOMPARE(readFile(nativeLog),
                 QString("vitrine: the host's udmabuf limits are 1024 entries and 64 MB, below what "
                         "native context needs (16384 and 128 MB): QEMU will refuse guest windows "
                         "drawn by the CPU (Qt Widgets and GTK apps, cursors), which the guest then "
                         "copies\n"
                         "vitrine: host tuning would raise them while VMs run: host tuning is off\n"
                         "vitrine: to raise them at each boot: " + Udmabuf::grubbyCommand() +
                         " (see docs/host-tuning.md)\n"));
        QVERIFY(readFile(virglLog).isEmpty());

        /* the refusals, as QEMU writes them; a line not ended yet waits */
        appendFile(nativeLog, kRefusal + kRefusal + "virtio_gpu_virgl_process_cmd: ctrl 0x10c, err");
        appendFile(virglLog, kRefusal);
        watch.poll();
        issues = watch.issues();
        QCOMPARE(issues.size(), 2);
        QCOMPARE(issues[0].log.refusals(), 2);
        QCOMPARE(issues[0].text(), QString("Some guest windows are copied: the host's udmabuf "
                                           "limits are too low (2 refused)"));
        QCOMPARE(issues[1].vmId, QString("virgl"));
        QVERIFY(!issues[1].limitsLow);
        QCOMPARE(issues[1].log.refusals(), 1);
        const int seen = int(changed.size());
        watch.poll();
        QCOMPARE(changed.size(), seen);
        appendFile(nativeLog, "or 0x1201\n");
        watch.poll();
        QCOMPARE(watch.issues()[0].log.outOfMemory, 3);
        QCOMPARE(watch.issues()[0].log.refusals(), 3);
        /* the note once per run */
        QCOMPARE(readFile(nativeLog).count("vitrine: "), 3);

        /* the log emptied (the log view's Clear): read again from its start */
        writeFile(virglLog, QByteArray());
        watch.poll();
        appendFile(virglLog, kRefusal);
        watch.poll();
        QCOMPARE(watch.issues()[1].log.refusals(), 2);

        /* a run ends: its issue goes */
        native.stop();
        QTRY_COMPARE(store.find("native")->runner()->state(), VmRunner::State::Stopped);
        issues = watch.issues();
        QCOMPARE(issues.size(), 1);
        QCOMPARE(issues[0].vmId, QString("virgl"));
        virgl.stop();
        QTRY_VERIFY(watch.issues().isEmpty());
    }

    /* Limits high enough, or tuning that raised them: nothing to say */
    void watchLimitsFine()
    {
        setLimits(65536, 2048);
        QTemporaryDir runtime(QDir::tempPath() + "/vu-XXXXXX");
        QTemporaryDir vms;
        QVERIFY(runtime.isValid() && vms.isValid());
        qputenv("XDG_RUNTIME_DIR", runtime.path().toLocal8Bit());
        FakeVm native(vms.path(), "native", kNative);
        VmStore store(vms.path());
        UdmabufWatch watch(&store, nullptr);
        watch.setSysRoot(m_sys);
        watch.setDevice(m_device);
        QSignalSpy changed(&watch, &UdmabufWatch::changed);
        store.find("native")->runner()->attach(store.find("native")->args());
        QTRY_COMPARE(store.find("native")->runner()->state(), VmRunner::State::Running);
        QVERIFY(watch.issues().isEmpty());
        QVERIFY(changed.isEmpty());
        QVERIFY(readFile(store.find("native")->runner()->logPath()).isEmpty());
    }

    /* No device: said, whatever host tuning does (it cannot help) */
    void watchNoDevice()
    {
        setLimits(65536, 2048);
        QTemporaryDir runtime(QDir::tempPath() + "/vu-XXXXXX");
        QTemporaryDir vms;
        QVERIFY(runtime.isValid() && vms.isValid());
        qputenv("XDG_RUNTIME_DIR", runtime.path().toLocal8Bit());
        FakeVm native(vms.path(), "native", kNative);
        VmStore store(vms.path());
        UdmabufWatch watch(&store, nullptr);
        watch.setSysRoot(m_sys);
        watch.setDevice(m_device + ".none");
        store.find("native")->runner()->attach(store.find("native")->args());
        QTRY_COMPARE(watch.issues().size(), 1);
        const UdmabufWatch::Issue issue = watch.issues().first();
        QVERIFY(issue.limitsLow);
        QVERIFY(issue.notRaised.isEmpty());
        QCOMPARE(readFile(store.find("native")->runner()->logPath()),
                 QString("vitrine: /dev/udmabuf does not exist: the kernel has no udmabuf driver, "
                         "or its module is not loaded: QEMU will refuse guest windows drawn by the "
                         "CPU (Qt Widgets and GTK apps, cursors), which the guest then copies\n"));
    }
};

QTEST_GUILESS_MAIN(TestUdmabuf)
#include "test_udmabuf.moc"
