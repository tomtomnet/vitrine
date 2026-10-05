// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The status bar's statistics: the parsers of what QEMU, /proc, the DRM
 * fdinfo and the guest tools' agent tell, the rates between two readings,
 * and the sampler on a VM found running (the stand-in QEMU, a monitor and
 * an agent answering as told).
 */
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

#include "core/guesttools.h"
#include "core/paths.h"
#include "core/vmrunner.h"
#include "core/vmstats.h"
#include "core/vmstore.h"

using namespace VmStats;

/* Waits on QEMU's stand-in and on the sampler's polls, a second apart:
   long, for a host busy with the other tests (ctest -j) */
static const int kSlow = 20000;

/*
 * @process once its exec is done: QProcess says started as soon as vfork
 * lets the parent go on, before the kernel has set the new command line
 * (/proc/PID/cmdline is empty, or the test's own, for a moment), and the
 * runner knows its QEMU by that command line
 */
static bool waitExec(const QProcess &process, const QString &argument)
{
    return QTest::qWaitFor([&]() {
        QFile f(QString("/proc/%1/cmdline").arg(process.processId()));
        return f.open(QIODevice::ReadOnly) && f.readAll().contains(argument.toLocal8Bit());
    }, kSlow);
}

static bool writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

static QJsonArray jsonArray(const char *text)
{
    return QJsonDocument::fromJson(text).array();
}

/* A client of this host's amdgpu (Linux 7.2), as its fdinfo reads */
static const char kAmdgpu[] = "pos:\t0\n"
                              "flags:\t02100002\n"
                              "mnt_id:\t41\n"
                              "ino:\t650\n"
                              "drm-driver:\tamdgpu\n"
                              "drm-client-id:\t7518\n"
                              "drm-pdev:\t0000:65:00.0\n"
                              "pasid:\t7517\n"
                              "drm-total-cpu:\t0\n"
                              "drm-shared-cpu:\t0\n"
                              "drm-resident-cpu:\t0\n"
                              "drm-purgeable-cpu:\t0\n"
                              "drm-total-gtt:\t34880 KiB\n"
                              "drm-shared-gtt:\t0\n"
                              "drm-resident-gtt:\t34880 KiB\n"
                              "drm-purgeable-gtt:\t0\n"
                              "drm-total-vram:\t310488 KiB\n"
                              "drm-shared-vram:\t91096 KiB\n"
                              "drm-resident-vram:\t310488 KiB\n"
                              "drm-purgeable-vram:\t768 KiB\n"
                              "drm-total-gds:\t0\n"
                              "drm-shared-gds:\t0\n"
                              "drm-resident-gds:\t0\n"
                              "drm-purgeable-gds:\t0\n"
                              "drm-memory-vram:\t310488 KiB\n"
                              "drm-memory-gtt: \t34880 KiB\n"
                              "drm-memory-cpu: \t0 KiB\n"
                              "amd-evicted-vram:\t0 KiB\n"
                              "amd-requested-vram:\t310488 KiB\n"
                              "amd-requested-gtt:\t34880 KiB\n"
                              "drm-engine-gfx:\t10781351614 ns\n"
                              "drm-engine-compute:\t99508474 ns\n";

/* An older amdgpu (Linux 6.1): drm-memory only */
static const char kAmdgpuOld[] = "pos:\t0\n"
                                 "flags:\t02100002\n"
                                 "drm-driver:\tamdgpu\n"
                                 "drm-pdev:\t0000:03:00.0\n"
                                 "drm-client-id:\t12\n"
                                 "pasid:\t32771\n"
                                 "drm-memory-vram:\t1024 KiB\n"
                                 "drm-memory-gtt: \t2048 KiB\n"
                                 "drm-memory-cpu: \t0 KiB\n"
                                 "drm-engine-gfx:\t1000 ns\n";

/* The kernel's example for xe (drivers/gpu/drm/xe/xe_drm_client.c) */
static const char kXe[] = "pos:    0\n"
                          "flags:  0100002\n"
                          "mnt_id: 26\n"
                          "ino:    685\n"
                          "drm-driver:     xe\n"
                          "drm-client-id:  3\n"
                          "drm-pdev:       0000:03:00.0\n"
                          "drm-total-system:       0\n"
                          "drm-shared-system:      0\n"
                          "drm-active-system:      0\n"
                          "drm-resident-system:    0\n"
                          "drm-purgeable-system:   0\n"
                          "drm-total-gtt:  192 KiB\n"
                          "drm-shared-gtt: 0\n"
                          "drm-active-gtt: 0\n"
                          "drm-resident-gtt:       192 KiB\n"
                          "drm-total-vram0:        23992 KiB\n"
                          "drm-shared-vram0:       16 MiB\n"
                          "drm-active-vram0:       0\n"
                          "drm-resident-vram0:     23992 KiB\n"
                          "drm-total-stolen:       0\n"
                          "drm-shared-stolen:      0\n"
                          "drm-active-stolen:      0\n"
                          "drm-resident-stolen:    0\n"
                          "drm-cycles-rcs: 28257900\n"
                          "drm-total-cycles-rcs:   7655183225\n"
                          "drm-cycles-bcs: 0\n"
                          "drm-total-cycles-bcs:   7655183225\n"
                          "drm-cycles-vcs: 0\n"
                          "drm-total-cycles-vcs:   7655183225\n"
                          "drm-engine-capacity-vcs:        2\n"
                          "drm-cycles-vecs:        0\n"
                          "drm-total-cycles-vecs:  7655183225\n"
                          "drm-engine-capacity-vecs:       2\n"
                          "drm-cycles-ccs: 0\n"
                          "drm-total-cycles-ccs:   7655183225\n"
                          "drm-engine-capacity-ccs:        4\n";

/* The kernel's example for i915 (Documentation/gpu/i915.rst) */
static const char kI915[] = "pos:    0\n"
                            "flags:  0100002\n"
                            "mnt_id: 21\n"
                            "drm-driver: i915\n"
                            "drm-pdev:   0000:00:02.0\n"
                            "drm-client-id:      7\n"
                            "drm-engine-render:  9288864723 ns\n"
                            "drm-engine-copy:    2035071108 ns\n"
                            "drm-engine-video:   0 ns\n"
                            "drm-engine-capacity-video:   2\n"
                            "drm-engine-video-enhance:   0 ns\n";

static DrmClient client(const QByteArray &text)
{
    DrmClient c;
    if (!parseDrmFdinfo(text, &c)) {
        qWarning("not a DRM client");
    }
    return c;
}

/* @text with "KEY: N" changed to "KEY: N + @by" */
static QByteArray bump(QByteArray text, const QByteArray &key, qint64 by)
{
    const qsizetype at = text.indexOf("\n" + key + ":");
    const qsizetype start = text.indexOf(':', at) + 1;
    const qsizetype end = text.indexOf('\n', start);
    const QList<QByteArray> value = text.mid(start, end - start).simplified().split(' ');
    QByteArray now = QByteArray::number(value[0].toLongLong() + by);

    if (value.size() > 1) {
        now += ' ' + value[1];
    }
    return text.replace(start, end - start, "\t" + now);
}

static const EngineUse *engine(const GpuUse &gpu, const QString &name)
{
    for (const EngineUse &e : gpu.engines) {
        if (e.name == name) {
            return &e;
        }
    }
    return nullptr;
}

/* QEMU's end of QMP: running, its disk reading 1 MiB and writing 256 KiB
   more at each query-blockstats; counts the commands */
class FakeQmp : public QObject
{
public:
    explicit FakeQmp(const QString &path)
    {
        QLocalServer::removeServer(path);
        m_server.listen(path);
        connect(&m_server, &QLocalServer::newConnection, this, [this]() {
            QLocalSocket *peer = m_server.nextPendingConnection();
            connect(peer, &QLocalSocket::readyRead, this, [this, peer]() {
                m_buffer += peer->readAll();
                qsizetype nl;
                while ((nl = m_buffer.indexOf('\n')) >= 0) {
                    const QJsonObject c = QJsonDocument::fromJson(m_buffer.left(nl)).object();
                    m_buffer.remove(0, nl + 1);
                    peer->write("{\"return\": " + answer(c["execute"].toString()) +
                                ", \"id\": " + QByteArray::number(c["id"].toInteger()) + "}\n");
                }
            });
            peer->write(R"({"QMP": {"version": {"qemu": {"major": 11}}, "capabilities": []}})"
                        "\n");
        });
    }

    QHash<QString, int> asked;

private:
    QByteArray answer(const QString &command)
    {
        asked[command]++;
        if (command == "query-status") {
            return R"({"running": true, "status": "running"})";
        }
        if (command == "query-commands") {
            return R"([{"name": "query-blockstats"}, {"name": "query-stats"}, )"
                   R"({"name": "query-status"}])";
        }
        if (command == "query-blockstats") {
            const int n = asked[command];
            return QString(R"([{"device": "disk0", "qdev": "/machine/peripheral-anon/device[3]",)"
                           R"( "stats": {"rd_bytes": %1, "wr_bytes": %2, "rd_operations": %3,)"
                           R"( "wr_operations": %4}},)"
                           R"( {"device": "pflash0", "stats": {"rd_bytes": 0, "wr_bytes": 0,)"
                           R"( "rd_operations": 0, "wr_operations": 0}}])")
                .arg(qint64(n) << 20)
                .arg(qint64(n) << 18)
                .arg(n * 8)
                .arg(n * 2)
                .toUtf8();
        }
        if (command == "query-stats") {
            return R"([{"provider": "kvm", "qom-path": "/machine/unattached/device[0]",)"
                   R"( "stats": [{"name": "exits", "value": 1000}]}])";
        }
        return "{}";
    }

    QLocalServer m_server;
    QByteArray m_buffer;
};

/* The guest tools' agent of the guest: hello with @protocol, then the
   guest's counters at each stats, 10 MB received and 1 MB sent a second */
class FakeAgent : public QObject
{
public:
    FakeAgent(const QString &path, int protocol) : m_protocol(protocol)
    {
        QLocalServer::removeServer(path);
        m_server.listen(path);
        connect(&m_server, &QLocalServer::newConnection, this, [this]() {
            QLocalSocket *peer = m_server.nextPendingConnection();
            connections++;
            m_buffer.clear();
            connect(peer, &QLocalSocket::readyRead, this, [this, peer]() {
                m_buffer += peer->readAll();
                qsizetype nl;
                while ((nl = m_buffer.indexOf('\n')) >= 0) {
                    const QJsonObject c = QJsonDocument::fromJson(m_buffer.left(nl)).object();
                    m_buffer.remove(0, nl + 1);
                    answer(peer, c);
                }
            });
            const QJsonObject status{{"tools", "0.1.0-15.fc44"}, {"agent", "0.1.0"}};
            peer->write(QJsonDocument(QJsonObject{{"type", "hello"}, {"protocol", m_protocol},
                                                  {"agent", "0.1.0"}, {"status", status}})
                            .toJson(QJsonDocument::Compact) +
                        "\n");
        });
    }

    int stats = 0;          // asked
    int connections = 0;
    bool silent = false;    // asked, and no answer

    /* the host's end dropped, as when QEMU's socket closes */
    void drop()
    {
        for (QLocalSocket *peer : m_server.findChildren<QLocalSocket *>()) {
            peer->disconnectFromServer();
        }
    }

private:
    void answer(QLocalSocket *peer, const QJsonObject &command)
    {
        QJsonObject reply{{"id", command["id"]}};
        stats += command["cmd"] == "stats";
        if (silent) {
            return;
        }
        if (command["cmd"] == "stats" && m_protocol >= 2) {
            /* a second of the guest's clock between two */
            const QJsonObject card{{"name", "enp0s3"},
                                   {"rxBytes", qint64(stats) * 10000000},
                                   {"txBytes", qint64(stats) * 1000000},
                                   {"rxPackets", stats * 7000},
                                   {"txPackets", stats * 700}};
            reply["type"] = "stats";
            reply["stats"] = QJsonObject{
                {"time", 1000.0 + stats},
                {"net", QJsonArray{card}},
                {"memory", QJsonObject{{"total", qint64(4) << 30},
                                       {"available", qint64(3) << 30}}}};
        } else if (command["cmd"] == "status") {
            reply["type"] = "status";
            reply["status"] = QJsonObject{{"tools", "0.1.0-15.fc44"}};
        } else {
            reply["type"] = "error";
            reply["error"] = "unknown command";
        }
        peer->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + "\n");
    }

    int m_protocol;
    QLocalServer m_server;
    QByteArray m_buffer;
};

/* A VM found running with the stand-in QEMU (threads "CPU 0/KVM",
   "CPU 1/KVM" and "worker"), its monitor, and an agent if asked */
struct Running {
    QTemporaryDir runtime{QDir::tempPath() + "/vs-XXXXXX"};
    QTemporaryDir vms;
    QProcess qemu;
    std::unique_ptr<FakeQmp> qmp;
    std::unique_ptr<FakeAgent> agent;
    std::unique_ptr<VmStore> store;
    Vm *vm = nullptr;

    explicit Running(int agentProtocol = 0)
    {
        qputenv("XDG_RUNTIME_DIR", runtime.path().toLocal8Bit());
        QDir(vms.path()).mkpath("busy");
        writeFile(vms.path() + "/busy/vm.args", "-name Busy\n-m 1G\n-display none\n");
        const QString run = Paths::vmRuntimeDir("busy");
        const QString qmpArg = QString("unix:%1/qmp.sock,server=on,wait=off").arg(run);
        qemu.start(FAKE_QEMU, {"-qmp", qmpArg});
        qemu.waitForStarted();
        waitExec(qemu, qmpArg);
        writeFile(run + "/qemu.pid", QByteArray::number(qemu.processId()) + "\n");
        qmp = std::make_unique<FakeQmp>(run + "/qmp.sock");
        if (agentProtocol > 0) {
            agent = std::make_unique<FakeAgent>(run + "/agent.sock", agentProtocol);
        }
        store = std::make_unique<VmStore>(vms.path());
        vm = store->find("busy");
        vm->runner()->attach(vm->args());
    }
    /* Running, or why not, for a failing test to tell */
    bool waitRunning()
    {
        if (QTest::qWaitFor([this]() { return vm->runner()->state() == VmRunner::State::Running; },
                            kSlow)) {
            return true;
        }
        const QString run = Paths::vmRuntimeDir("busy");
        QFile pid(run + "/qemu.pid"), cmd(QString("/proc/%1/cmdline").arg(qemu.processId()));
        (void)pid.open(QIODevice::ReadOnly);
        (void)cmd.open(QIODevice::ReadOnly);
        QStringList asked;
        for (auto it = qmp->asked.cbegin(); it != qmp->asked.cend(); ++it) {
            asked << QString("%1=%2").arg(it.key()).arg(it.value());
        }
        qWarning("state %d, error '%s'; stand-in pid %lld state %d exit %d/%d; pid file '%s'; "
                 "cmdline '%s'; qmp.sock %d; asked %s; XDG_RUNTIME_DIR %s",
                 int(vm->runner()->state()), qPrintable(vm->runner()->errorString()),
                 qemu.processId(), int(qemu.state()), qemu.exitCode(), int(qemu.exitStatus()),
                 pid.readAll().trimmed().constData(),
                 cmd.readAll().replace('\0', ' ').constData(),
                 int(QFileInfo::exists(run + "/qmp.sock")), qPrintable(asked.join(' ')),
                 qgetenv("XDG_RUNTIME_DIR").constData());
        return false;
    }
    ~Running() { stop(); }
    void stop()
    {
        qmp.reset();
        agent.reset();
        if (qemu.state() != QProcess::NotRunning) {
            qemu.kill();
            qemu.waitForFinished();
        }
    }
};

class TestVmStats : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void vcpuNames()
    {
        QCOMPARE(vcpuIndex("CPU 0/KVM"), 0);
        QCOMPARE(vcpuIndex("CPU 12/KVM"), 12);
        QCOMPARE(vcpuIndex("CPU 3/TCG"), 3);
        QCOMPARE(vcpuIndex("qemu-system-x86"), -1);
        QCOMPARE(vcpuIndex("IO iodisk"), -1);
        QCOMPARE(vcpuIndex("CPU 0"), -1);
        QCOMPARE(vcpuIndex("ALL CPUs/TCG"), -1);
        QCOMPARE(vcpuIndex(QString()), -1);
    }

    void threadNames()
    {
        QTemporaryDir proc;
        writeFile(proc.filePath("100/task/100/comm"), "qemu-system-x86\n");
        writeFile(proc.filePath("100/task/101/comm"), "CPU 0/KVM\n");
        writeFile(proc.filePath("100/task/102/comm"), "IO iodisk\n");

        QHash<qint64, QString> names = readThreadNames(100, {100, 101, 102}, {}, proc.path());
        QCOMPARE(names.size(), 3);
        QCOMPARE(names.value(100), "qemu-system-x86");
        QCOMPARE(names.value(101), "CPU 0/KVM");
        QCOMPARE(names.value(102), "IO iodisk");

        /* known names are not read again; a thread gone is dropped */
        QDir(proc.filePath("100/task/102")).removeRecursively();
        names = readThreadNames(100, {100, 101}, {{101, "known"}, {102, "gone"}}, proc.path());
        QCOMPARE(names.size(), 2);
        QCOMPARE(names.value(101), "known");
        QVERIFY(!names.contains(102));
        /* one that went between the two readings: no name */
        QCOMPARE(readThreadNames(100, {100, 102}, {}, proc.path()).size(), 1);

        QVERIFY(readThreadNames(0, {0}, {}, proc.path()).isEmpty());
        QVERIFY(readThreadNames(200, {200}, {}, proc.path()).isEmpty());
        /* this process: its main thread */
        const qint64 self = QCoreApplication::applicationPid();
        QVERIFY(!readThreadNames(self, {self}).value(self).isEmpty());

        /* without names, QMP's thread ids: each once */
        QCOMPARE(parseVcpuThreads(jsonArray(R"([{"cpu-index": 0, "thread-id": 11},
                                                {"cpu-index": 1, "thread-id": 12}])")),
                 QList<qint64>({11, 12}));
        QCOMPARE(parseVcpuThreads(jsonArray(R"([{"cpu-index": 0, "thread-id": 7},
                                                {"cpu-index": 1, "thread-id": 7},
                                                {"cpu-index": 2, "thread-id": 7}])")),
                 QList<qint64>({7}));
        QVERIFY(parseVcpuThreads(QJsonArray()).isEmpty());
    }

    void busiest()
    {
        const QHash<qint64, PerfStats::Thread> before{
            {1, {1000000000, 0, 10}}, {2, {0, 0, 0}}, {3, {500, 0, 1}}, {4, {9, 0, 1}}};
        const QHash<qint64, PerfStats::Thread> after{
            {1, {1100000000, 0, 20}},       // 10 %
            {2, {800000000, 0, 30}},        // 80 %
            {3, {500, 0, 1}},               // idle: left out
            {4, {5, 0, 1}},                 // went back: left out
            {5, {900000000, 0, 9}}};        // new: no rate yet
        const QHash<qint64, QString> names{{1, "qemu-system-x86"}, {2, "CPU 0/KVM"}};

        QList<ThreadUse> list = busiestThreads(before, after, names, 1000000000, 6);
        QCOMPARE(list.size(), 2);
        QCOMPARE(list[0].tid, 2);
        QCOMPARE(list[0].name, "CPU 0/KVM");
        QCOMPARE(list[0].cpu, 80.0);
        QCOMPARE(list[1].tid, 1);
        QCOMPARE(list[1].cpu, 10.0);
        QCOMPARE(busiestThreads(before, after, names, 1000000000, 1).size(), 1);
        QVERIFY(busiestThreads(before, after, names, 0, 6).isEmpty());
    }

    void memory()
    {
        const QByteArray status = "Name:\tqemu-system-x86\n"
                                  "VmPeak:\t 9000000 kB\n"
                                  "VmRSS:\t 3145728 kB\n"
                                  "RssAnon:\t  204800 kB\n"
                                  "RssFile:\t   40960 kB\n"
                                  "RssShmem:\t 2899968 kB\n"
                                  "VmSwap:\t    1024 kB\n"
                                  "Threads:\t29\n";
        Memory m = parseStatus(status);
        QCOMPARE(m.resident, qint64(3145728) * 1024);
        QCOMPARE(m.anon, qint64(204800) * 1024);
        QCOMPARE(m.file, qint64(40960) * 1024);
        QCOMPARE(m.shmem, qint64(2899968) * 1024);
        QCOMPARE(m.swap, qint64(1024) * 1024);
        QCOMPARE(m.proportional, -1);

        QVERIFY(parseSmapsRollup("55d0c0000000-7ffd00000000 ---p 00000000 00:00 0 [rollup]\n"
                                 "Rss:             3145728 kB\n"
                                 "Pss:             2100000 kB\n"
                                 "Pss_Anon:         204800 kB\n",
                                 &m));
        QCOMPARE(m.proportional, qint64(2100000) * 1024);
        QVERIFY(!parseSmapsRollup("", &m));

        /* gone, or a kernel thread */
        QCOMPARE(parseStatus("").resident, -1);
        QCOMPARE(parseStatus("Name:\tkthreadd\n").resident, -1);
        /* this process */
        QFile self("/proc/self/status");
        QVERIFY(self.open(QIODevice::ReadOnly));
        QVERIFY(parseStatus(self.readAll()).resident > 0);
    }

    void blockstats()
    {
        /* QEMU's own node names start with #, written \u0023 here: moc takes a #
           in a raw string for a directive */
        const QJsonArray before = jsonArray(R"([
            {"device": "disk0", "qdev": "/machine/peripheral-anon/device[3]/virtio-backend",
             "stats": {"rd_bytes": 1048576, "wr_bytes": 0, "rd_operations": 10,
                       "wr_operations": 0, "flush_operations": 0, "idle_time_ns": 1},
             "parent": {"stats": {"rd_bytes": 99, "wr_bytes": 99}}},
            {"device": "", "qdev": "/machine/peripheral/data/virtio-backend",
             "stats": {"rd_bytes": 0, "wr_bytes": 4096, "rd_operations": 0,
                       "wr_operations": 1}},
            {"device": "", "node-name": "\u0023block123", "stats": {"rd_bytes": 5, "wr_bytes": 5}},
            {"device": "", "qdev": "/machine/unattached/device[23]",
             "stats": {"rd_bytes": 0, "wr_bytes": 0}},
            {"device": "ide2-cd0", "qdev": "/machine/unattached/device[24]"}
        ])");
        QList<BlockDevice> a = parseBlockstats(before);
        QCOMPARE(a.size(), 4);              // the drive without stats left out
        QCOMPARE(a[0].name, "disk0");
        QCOMPARE(a[0].readBytes, 1048576);
        QCOMPARE(a[0].reads, 10);
        QCOMPARE(a[1].name, "data");        // the -device's id
        QCOMPARE(a[1].writtenBytes, 4096);
        QCOMPARE(a[2].name, "#block123");
        QCOMPARE(a[3].name, "device[23]");

        const QJsonArray after = jsonArray(R"([
            {"device": "disk0", "stats": {"rd_bytes": 13631488, "wr_bytes": 3145728,
                                          "rd_operations": 240, "wr_operations": 80}},
            {"device": "", "qdev": "/machine/peripheral/data/virtio-backend",
             "stats": {"rd_bytes": 0, "wr_bytes": 0, "rd_operations": 0, "wr_operations": 0}},
            {"device": "new", "stats": {"rd_bytes": 777, "wr_bytes": 0}}
        ])");
        const Disk d = diskRates(a, parseBlockstats(after), 2.0);
        QCOMPARE(d.devices.size(), 2);      // "new" has no reading before
        QCOMPARE(d.devices[0].name, "disk0");
        QCOMPARE(d.devices[0].read, 6.0 * 1048576);
        QCOMPARE(d.devices[0].written, 1.5 * 1048576);
        QCOMPARE(d.devices[0].reads, 115.0);
        QCOMPARE(d.devices[0].writes, 40.0);
        QCOMPARE(d.devices[0].readTotal, 13631488);
        QCOMPARE(d.devices[0].writtenTotal, 3145728);
        /* counters that went back count nothing */
        QCOMPARE(d.devices[1].written, 0.0);
        QCOMPARE(d.read, 6.0 * 1048576);
        QCOMPARE(d.written, 1.5 * 1048576);
        QVERIFY(diskRates(a, a, 0).devices.isEmpty());
    }

    void guestCounters()
    {
        const QJsonObject first = QJsonDocument::fromJson(R"({
            "time": 100.5,
            "net": [{"name": "enp0s3", "rxBytes": 1000, "txBytes": 500,
                     "rxPackets": 10, "txPackets": 5},
                    {"name": "", "rxBytes": 1}],
            "memory": {"total": 4096000000, "available": 3000000000}})").object();
        const GuestCounters a = parseGuestCounters(first);
        QCOMPARE(a.time, 100.5);
        QCOMPARE(a.interfaces.size(), 1);
        QCOMPARE(a.interfaces[0].name, "enp0s3");
        QCOMPARE(a.interfaces[0].rxBytes, 1000);
        QCOMPARE(a.interfaces[0].txPackets, 5);
        QCOMPARE(a.memoryTotal, 4096000000);
        QCOMPARE(a.memoryAvailable, 3000000000);

        GuestCounters b = a;
        b.time = 102.5;
        b.interfaces[0].rxBytes = 3001000;
        b.interfaces[0].txBytes = 400500;
        b.interfaces << Interface{"wlan0", 50, 50, 1, 1};
        Network n = networkRates(a, b);
        QCOMPARE(n.rx, 1500000.0);
        QCOMPARE(n.tx, 200000.0);
        QCOMPARE(n.interfaces.size(), 2);
        QCOMPARE(n.interfaces[0].rxTotal, 3001000);
        QCOMPARE(n.interfaces[1].name, "wlan0");    // new: listed, no rate yet
        QCOMPARE(n.interfaces[1].rx, 0.0);

        /* the guest restarted: its counters went back */
        GuestCounters c = b;
        c.time = 103.5;
        c.interfaces[0].rxBytes = 10;
        n = networkRates(b, c);
        QCOMPARE(n.interfaces[0].rx, 0.0);
        /* no earlier answer, or not later */
        QCOMPARE(networkRates(GuestCounters(), b).interfaces.size(), 0);
        QCOMPARE(networkRates(b, b).interfaces.size(), 0);

        /* an agent without memory or cards */
        const GuestCounters bare = parseGuestCounters(QJsonObject{{"time", 1}});
        QVERIFY(bare.interfaces.isEmpty());
        QCOMPARE(bare.memoryTotal, -1);
    }

    void fdinfoAmdgpu()
    {
        DrmClient c;
        QVERIFY(parseDrmFdinfo(kAmdgpu, &c));
        QCOMPARE(c.driver, "amdgpu");
        QCOMPARE(c.pdev, "0000:65:00.0");
        QCOMPARE(c.id, 7518);
        QCOMPARE(c.engines.size(), 2);
        QCOMPARE(c.engines["gfx"].busyNs, 10781351614);
        QCOMPARE(c.engines["gfx"].capacity, 1);
        QCOMPARE(c.engines["compute"].busyNs, 99508474);
        QCOMPARE(c.engines["gfx"].cycles, -1);
        QCOMPARE(c.regions["vram"].resident, qint64(310488) * 1024);
        QCOMPARE(c.regions["vram"].total, qint64(310488) * 1024);
        QCOMPARE(c.regions["vram"].shared, qint64(91096) * 1024);
        QCOMPARE(c.regions["vram"].purgeable, qint64(768) * 1024);
        QCOMPARE(c.regions["gtt"].resident, qint64(34880) * 1024);
        QCOMPARE(c.regions["cpu"].resident, 0);
        QCOMPARE(c.regions["gds"].total, 0);
        /* amd-* keys are the driver's own */
        QVERIFY(!c.regions.contains("evicted-vram"));

        /* before drm-resident-: drm-memory- as resident */
        QVERIFY(parseDrmFdinfo(kAmdgpuOld, &c));
        QCOMPARE(c.id, 12);
        QCOMPARE(c.regions["vram"].resident, 1024 * 1024);
        QCOMPARE(c.regions["vram"].total, -1);
        QCOMPARE(c.regions["gtt"].resident, 2048 * 1024);
        QCOMPARE(c.engines["gfx"].busyNs, 1000);
    }

    void fdinfoXe()
    {
        const DrmClient c = client(kXe);
        QCOMPARE(c.driver, "xe");
        QCOMPARE(c.pdev, "0000:03:00.0");
        QCOMPARE(c.id, 3);
        QCOMPARE(c.engines.size(), 5);
        QCOMPARE(c.engines["rcs"].cycles, 28257900);
        QCOMPARE(c.engines["rcs"].totalCycles, 7655183225);
        QCOMPARE(c.engines["rcs"].busyNs, -1);
        QCOMPARE(c.engines["rcs"].capacity, 1);
        QCOMPARE(c.engines["vcs"].capacity, 2);
        QCOMPARE(c.engines["ccs"].capacity, 4);
        /* drm-total-cycles- is no memory region */
        QVERIFY(!c.regions.contains("cycles-rcs"));
        QCOMPARE(c.regions["vram0"].resident, qint64(23992) * 1024);
        QCOMPARE(c.regions["vram0"].shared, qint64(16) << 20);
        QCOMPARE(c.regions["vram0"].active, 0);
        QCOMPARE(c.regions["gtt"].total, 192 * 1024);
        QCOMPARE(c.regions["system"].purgeable, 0);
        QCOMPARE(c.regions["stolen"].purgeable, -1);
    }

    void fdinfoI915()
    {
        const DrmClient c = client(kI915);
        QCOMPARE(c.driver, "i915");
        QCOMPARE(c.pdev, "0000:00:02.0");
        QCOMPARE(c.id, 7);
        QCOMPARE(c.engines.size(), 4);
        QCOMPARE(c.engines["render"].busyNs, 9288864723);
        QCOMPARE(c.engines["copy"].busyNs, 2035071108);
        QCOMPARE(c.engines["video"].capacity, 2);
        QCOMPARE(c.engines["video-enhance"].busyNs, 0);
        QVERIFY(c.regions.isEmpty());

        /* not a DRM client: a file's or a socket's fdinfo */
        DrmClient none;
        QVERIFY(!parseDrmFdinfo("pos:\t0\nflags:\t02\nmnt_id:\t15\nino:\t42\n", &none));
        QVERIFY(!parseDrmFdinfo("", &none));
        QCOMPARE(none.id, -1);
    }

    void gpuRates()
    {
        /* two clients of the amdgpu: gfx 370 ms and compute 20 ms busy in a second */
        const QByteArray other = QByteArray(kAmdgpu).replace("7518", "7600");
        const QList<DrmClient> before{client(kAmdgpu), client(other)};
        const QList<DrmClient> after{
            client(bump(bump(kAmdgpu, "drm-engine-gfx", 300000000), "drm-engine-compute",
                        20000000)),
            client(bump(other, "drm-engine-gfx", 70000000))};
        QList<GpuUse> use = gpuUse(before, after, 1000000000);
        QCOMPARE(use.size(), 1);
        const GpuUse &amd = use[0];
        QCOMPARE(amd.driver, "amdgpu");
        QCOMPARE(amd.pdev, "0000:65:00.0");
        QCOMPARE(amd.clients, 2);
        QCOMPARE(amd.engines.size(), 2);
        QCOMPARE(amd.engines[0].name, "compute");   // by name
        QCOMPARE(engine(amd, "gfx")->busy, 37.0);
        QCOMPARE(engine(amd, "compute")->busy, 2.0);
        QCOMPARE(amd.busy, 37.0);
        /* both clients' memory, regions with none left out, what they share
           (91096 KiB of VRAM each) once */
        QCOMPARE(amd.regions.size(), 2);
        QCOMPARE(amd.regions[1].name, "vram");
        QCOMPARE(amd.regions[1].resident, (2 * (qint64(310488) - 91096) + 91096) * 1024);
        QCOMPARE(amd.regions[0].resident, 2 * qint64(34880) * 1024);
        QCOMPARE(amd.memory, amd.regions[0].resident + amd.regions[1].resident);
        QVERIFY(amd.shared);

        /* an engine whose time went back counts nothing, and its higher time stays */
        QList<DrmClient> later{client(bump(kAmdgpu, "drm-engine-gfx", -5000))};
        QCOMPARE(engine(gpuUse(after, later, 1000000000)[0], "gfx")->busy, 0.0);
        const QList<DrmClient> kept = mergeDrmClients(after, later);
        QCOMPARE(kept[0].engines["gfx"].busyNs, after[0].engines["gfx"].busyNs);
        /* a client new since the last reading: its memory, no time */
        use = gpuUse({}, after, 1000000000);
        QCOMPARE(engine(use[0], "gfx")->busy, 0.0);
        QCOMPARE(use[0].memory, amd.memory);
        /* more than the time there was (overlapping jobs): at most 100 % */
        use = gpuUse(before, {client(bump(kAmdgpu, "drm-engine-gfx", 3000000000))}, 1000000000);
        QCOMPARE(use[0].busy, 100.0);
    }

    void gpuRatesXeI915()
    {
        /* xe: busy cycles out of the GPU's, over the engines of the class */
        const QList<DrmClient> before{client(kXe)};
        QByteArray xe = bump(QByteArray(kXe), "drm-total-cycles-rcs", 1000000);
        xe = bump(xe, "drm-cycles-rcs", 250000);
        xe = bump(xe, "drm-total-cycles-vcs", 1000000);
        xe = bump(xe, "drm-cycles-vcs", 1000000);   // one of two video engines
        xe = bump(xe, "drm-total-cycles-ccs", 1000000);
        QList<GpuUse> use = gpuUse(before, {client(xe)}, 2000000000);
        QCOMPARE(use.size(), 1);
        QCOMPARE(engine(use[0], "rcs")->busy, 25.0);
        QCOMPARE(engine(use[0], "vcs")->busy, 50.0);
        QCOMPARE(engine(use[0], "ccs")->busy, 0.0);
        /* bcs and vecs without a reading of their total since: idle */
        QCOMPARE(engine(use[0], "bcs")->busy, 0.0);
        QCOMPARE(use[0].busy, 50.0);
        /* 16 MiB of its VRAM shared: one client, all counted */
        QCOMPARE(use[0].memory, qint64(192 + 23992) * 1024);
        QVERIFY(use[0].shared);

        /* i915: time over the engines of the class */
        QByteArray i915 = bump(QByteArray(kI915), "drm-engine-video", 500000000);
        i915 = bump(i915, "drm-engine-render", 100000000);
        use = gpuUse({client(kI915)}, {client(i915)}, 1000000000);
        QCOMPARE(engine(use[0], "video")->busy, 25.0);
        QCOMPARE(engine(use[0], "render")->busy, 10.0);
        QCOMPARE(use[0].busy, 25.0);
        QCOMPARE(use[0].memory, 0);
        QVERIFY(!use[0].shared);

        /* two GPUs: the busier first */
        use = gpuUse({client(kI915), client(kAmdgpu)},
                     {client(i915), client(bump(kAmdgpu, "drm-engine-gfx", 900000000))},
                     1000000000);
        QCOMPARE(use.size(), 2);
        QCOMPARE(use[0].driver, "amdgpu");
        QCOMPARE(use[1].driver, "i915");
    }

    void drmClients()
    {
        QTemporaryDir proc;
        const QString fd = proc.filePath("300/fd/");
        const QString info = proc.filePath("300/fdinfo/");
        QDir().mkpath(fd);
        QVERIFY(QFile::link("/dev/dri/renderD128", fd + "3"));
        QVERIFY(QFile::link("/dev/dri/renderD128", fd + "4"));      // a dup of 3
        QVERIFY(QFile::link("socket:[1234]", fd + "5"));
        QVERIFY(QFile::link("/dev/dri/renderD128", fd + "6"));      // another context
        QVERIFY(QFile::link("/dmabuf:", fd + "7"));
        QVERIFY(QFile::link("/dev/dri/card1", fd + "8"));           // no fdinfo: gone meanwhile
        writeFile(info + "3", kAmdgpu);
        writeFile(info + "4", kAmdgpu);
        writeFile(info + "5", "pos:\t0\nflags:\t02\n");
        writeFile(info + "6", QByteArray(kAmdgpu).replace("7518", "7519"));
        writeFile(info + "7", "drm-driver:\tnot-a-drm-fd\n");

        QString error = "stale";
        QList<DrmClient> clients = readDrmClients(300, proc.path(), &error);
        QVERIFY(error.isEmpty());
        QCOMPARE(clients.size(), 2);
        QSet<qint64> ids{clients[0].id, clients[1].id};
        QCOMPARE(ids, QSet<qint64>({7518, 7519}));

        clients = readDrmClients(301, proc.path(), &error);
        QVERIFY(clients.isEmpty());
        QVERIFY(error.contains("cannot read"));
        /* this process has no GPU */
        QVERIFY(readDrmClients(QCoreApplication::applicationPid(), "/proc", &error).isEmpty());
        QVERIFY(error.isEmpty());
    }

    void text()
    {
        QCOMPARE(formatBytes(0), "0 B");
        QCOMPARE(formatBytes(512), "512 B");
        QCOMPARE(formatBytes(999), "999 B");
        QCOMPARE(formatBytes(1000), "0.98 KiB");
        QCOMPARE(formatBytes(1536), "1.5 KiB");
        QCOMPARE(formatBytes(12 * 1024), "12 KiB");
        QCOMPARE(formatBytes(1258291), "1.2 MiB");
        QCOMPARE(formatBytes(qint64(345) << 20), "345 MiB");
        QCOMPARE(formatBytes(qint64(1000) << 20), "0.98 GiB");
        QCOMPARE(formatBytes(qint64(3328599654)), "3.1 GiB");
        QCOMPARE(formatBytes(-5), "0 B");
        QCOMPARE(formatRate(0), "0 B/s");
        QCOMPARE(formatRate(0.4), "0 B/s");
        QCOMPARE(formatRate(46080), "45 KiB/s");
        QCOMPARE(formatRate(-1), "0 B/s");
        QCOMPARE(formatWholePercent(41.6), "42 %");
        QCOMPARE(formatWholePercent(0.2), "0 %");
        QCOMPARE(formatWholePercent(-3), "0 %");
    }

    /* The sampler on a VM found running: what it reads, and only that */
    void sampler()
    {
        Running vm(2);
        QVERIFY(vm.waitRunning());
        Sampler sampler;
        sampler.setInterval(100);
        QSignalSpy changed(&sampler, &Sampler::changed);

        /* nothing asked: nothing read */
        sampler.setVm(vm.vm);
        QTest::qWait(300);
        QVERIFY(!sampler.hasData());
        QCOMPARE(vm.qmp->asked.value("query-blockstats"), 0);

        sampler.setSources(Sampler::Cpu | Sampler::Memory | Sampler::Disk | Sampler::Gpu |
                           Sampler::Network);
        QTRY_VERIFY(sampler.snapshot().perf.threads);
        const Snapshot &s = sampler.snapshot();
        /* the vCPUs by their names */
        QCOMPARE(s.perf.vcpus.threads, 2);
        QCOMPARE(s.perf.others.threads, 1);     // "worker"
        QVERIFY(s.hostCpus > 0);
        QVERIFY(s.memory);
        QVERIFY(s.qemuMemory.resident > 0);
        QCOMPARE(s.qemuMemory.proportional, -1);    // not asked
        /* the disk: a MiB read and 256 KiB written per query, one query per poll */
        QTRY_VERIFY(s.disk);
        QCOMPARE(s.diskUse.devices.size(), 2);
        QCOMPARE(s.diskUse.devices[0].name, "disk0");
        QVERIFY(s.diskUse.read > 0);
        QCOMPARE(s.diskUse.written * 4, s.diskUse.read);
        /* the network from the agent: 10 MB/s and 1 MB/s by the guest's clock */
        QTRY_VERIFY(s.network);
        QCOMPARE(sampler.guestSource(), GuestSource::Agent);
        QCOMPARE(s.net.rx, 10000000.0);
        QCOMPARE(s.net.tx, 1000000.0);
        QCOMPARE(s.net.interfaces.value(0).name, "enp0s3");
        /* the guest's memory with the memory's details only */
        QVERIFY(!s.guestMemory);
        /* no GPU in the stand-in */
        QCOMPARE(s.drmClients, 0);
        QVERIFY(!s.gpu);
        QVERIFY(s.gpuError.isEmpty());
        /* neither asked: no display or KVM query */
        QCOMPARE(vm.qmp->asked.value("x-query-display-stats"), 0);
        QCOMPARE(vm.qmp->asked.value("query-stats"), 0);
        QVERIFY(!changed.isEmpty());

        /* KVM, PSS and the guest's memory while their details show */
        sampler.setSources(sampler.sources() | Sampler::Kvm | Sampler::MemoryDetails);
        QTRY_VERIFY(s.qemuMemory.proportional > 0);
        QTRY_VERIFY(s.guestMemory);
        QCOMPARE(s.guestTotal, qint64(4) << 30);
        QCOMPARE(s.guestAvailable, qint64(3) << 30);
        QTRY_VERIFY(vm.qmp->asked.value("query-stats") >= 2);
        QVERIFY(vm.qmp->asked.value("query-commands") == 1);

        /* the details hidden: the network's rates go on from the same counters */
        sampler.setSources(sampler.sources() & ~Sampler::MemoryDetails);
        QVERIFY(s.network);
        QVERIFY(!s.guestMemory);
        QCOMPARE(s.qemuMemory.proportional, -1);
        int asked = vm.agent->stats;
        QTRY_VERIFY(vm.agent->stats > asked);
        QVERIFY(s.network);
        QCOMPARE(s.net.rx, 10000000.0);

        /* the agent's socket dropped: still the agent it was, no "older agent" */
        vm.agent->drop();
        QTest::qWait(300);
        QCOMPARE(sampler.guestSource(), GuestSource::Agent);
        QTRY_VERIFY_WITH_TIMEOUT(vm.agent->connections == 2, 8000);
        asked = vm.agent->stats;
        QTRY_VERIFY(vm.agent->stats > asked + 1);
        QVERIFY(s.network);

        /* no more answers (the guest paused or restarting): no counters soon */
        vm.agent->silent = true;
        QTRY_VERIFY_WITH_TIMEOUT(!s.network, 6000);
        QCOMPARE(sampler.guestSource(), GuestSource::Agent);
        vm.agent->silent = false;
        QTRY_VERIFY_WITH_TIMEOUT(s.network, 8000);

        /* hidden again: no more queries, the parts forgotten */
        sampler.setSources({});
        QVERIFY(!s.disk);
        QVERIFY(!s.network);
        QVERIFY(!s.perf.threads);
        const int disk = vm.qmp->asked.value("query-blockstats");
        const int stats = vm.agent->stats;
        QTest::qWait(400);
        QCOMPARE(vm.qmp->asked.value("query-blockstats"), disk);
        QCOMPARE(vm.agent->stats, stats);

        /* the VM stops: nothing left */
        sampler.setSources(Sampler::Cpu);
        QTRY_VERIFY(sampler.hasData());
        vm.stop();
        QTRY_COMPARE_WITH_TIMEOUT(vm.vm->runner()->state(), VmRunner::State::Stopped, 10000);
        QVERIFY(!sampler.hasData());
        QVERIFY(!sampler.snapshot().perf.threads);
        QCOMPARE(sampler.guestSource(), GuestSource::None);
    }

    /* An agent of the guest tools before the stats command: never asked */
    void olderAgent()
    {
        Running vm(1);
        QVERIFY(vm.waitRunning());
        Sampler sampler;
        sampler.setInterval(100);
        sampler.setVm(vm.vm);
        sampler.setSources(Sampler::Network | Sampler::Memory);
        QTRY_COMPARE(sampler.guestSource(), GuestSource::OldAgent);
        QTRY_VERIFY(sampler.snapshot().memory);
        QTest::qWait(300);
        QCOMPARE(vm.agent->stats, 0);
        QVERIFY(!sampler.snapshot().network);
        QVERIFY(!sampler.snapshot().guestMemory);
    }

    /* No agent in the guest: QEMU's memory, no network */
    void noAgent()
    {
        Running vm;
        QVERIFY(vm.waitRunning());
        Sampler sampler;
        sampler.setInterval(100);
        QCOMPARE(sampler.guestSource(), GuestSource::None);
        sampler.setVm(vm.vm);
        sampler.setSources(Sampler::Network | Sampler::Memory);
        QCOMPARE(sampler.guestSource(), GuestSource::NoAgent);
        QTRY_VERIFY(sampler.snapshot().memory);
        QTest::qWait(300);
        QVERIFY(!sampler.snapshot().network);
        QCOMPARE(sampler.guestSource(), GuestSource::NoAgent);
    }
};

QTEST_GUILESS_MAIN(TestVmStats)
#include "test_vmstats.moc"
