// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include "core/guesttools.h"
#include "core/paths.h"
#include "core/qmpclient.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"

using namespace GuestTools;

static QString testQemu()
{
    const QString env = qEnvironmentVariable("VITRINE_TEST_QEMU");
    return env.isEmpty() ? QStandardPaths::findExecutable("qemu-system-x86_64") : env;
}

/* The values of the options @name in @command */
static QStringList valuesOf(const QStringList &command, const QString &name)
{
    QStringList values;

    for (qsizetype i = 1; i < command.size(); i++) {
        if (command[i - 1] == name) {
            values << command[i];
        }
    }
    return values;
}

/* A status as the guest's agent writes it (guest/tools/agent/vitrine-agent) */
static const char kHello[] =
    R"j({"type":"hello","protocol":1,"agent":"0.1.0","status":{"tools":"0.1.0-1.fc44",)j"
    R"j("agent":"0.1.0","os":{"id":"fedora","version":"44","name":"Fedora Linux 44 (KDE Plasma)"},)j"
    R"j("kernel":"7.2.7-200.fc44.x86_64","kernels":[{"version":"7.2.7-200.fc44.x86_64",)j"
    R"j("headers":true,"driver":"installed"},{"version":"7.2.5-200.fc44.x86_64","headers":false,)j"
    R"j("driver":"no-headers"}],"secureBoot":false,"desktops":{"kde":"6.7.5"},)j"
    R"j("sessions":[{"desktop":"KDE","type":"wayland","active":true}],)j"
    R"j("driver":{"loaded":true,"patched":true,"taint":"OE","params":{"blob_flush_fence":"3",)j"
    R"j("tiled_scanout":"1"},"vblankoffdelay":"0"},"gpu":{"capsets":["virgl","virgl2","drm"],)j"
    R"j("contextInit":true},"packages":{"vitrine-guest-tools":"0.1.0-1.fc44",)j"
    R"j("mesa-dri-drivers":"26.2.3-1.xe.fc44","kwin":"6.7.5-1.21.fc44"},"kwinPatched":true,)j"
    R"j("preempt":"full","rebootNeeded":false,"installed":{"medium":"32fe6c83b34e87e8",)j"
    R"j("tools":"0.1.0-1.fc44"},"installing":false}})j";

/* QEMU's end of QMP, for a QEMU found running: answers every command,
   query-status with running */
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
                for (qsizetype nl; (nl = m_buffer.indexOf('\n')) >= 0;) {
                    const QJsonObject c = QJsonDocument::fromJson(m_buffer.left(nl)).object();
                    m_buffer.remove(0, nl + 1);
                    QJsonObject reply{{"return", QJsonObject()}, {"id", c["id"]}};
                    if (c["execute"] == "query-status") {
                        reply["return"] = QJsonObject{{"running", true}, {"status", "running"}};
                    }
                    peer->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
                }
            });
            peer->write(R"({"QMP": {"version": {"qemu": {"major": 11}}, "capabilities": []}})"
                        "\n");
        });
    }

private:
    QLocalServer m_server;
    QByteArray m_buffer;
};

class TestGuestTools : public QObject
{
    Q_OBJECT

private:
    QString writeMedium(const QString &release = "44")
    {
        QDir().mkpath(dataDir());
        const QString image = QString("%1/vitrine-guest-tools-fc%2.img").arg(dataDir(), release);
        QFile img(image), json(QString(image).replace(".img", ".json"));
        if (img.open(QIODevice::WriteOnly)) {
            img.write(QByteArray(1 << 20, '\0'));
        }
        if (json.open(QIODevice::WriteOnly)) {
            json.write(R"({"label": "VITRINETOOL", "mediumId": "32fe6c83b34e87e8", "fedora": "44",
                          "tools": "0.1.0-1.fc44", "mesa": "26.2.3-1.xe.fc44",
                          "kwin": "6.7.5-1.21.fc44", "packages": [
                          {"name": "kwin", "evr": "6.7.5-1.21.fc44", "arch": "x86_64"},
                          {"name": "vitrine-guest-tools", "evr": "0.1.0-1.fc44", "arch": "noarch"}]})");
        }
        return image;
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QDir(dataDir()).removeRecursively();
    }

    void cleanupTestCase()
    {
        QDir(dataDir()).removeRecursively();
        setPending("vitrine-test-vm", Pending::None);
    }

    void agentPort()
    {
        QCOMPARE(agentPortArgs("/run/user/1000/vitrine/a,b/agent.sock"),
                 QStringList({"-chardev",
                              "socket,id=vitrine-agent,path=/run/user/1000/vitrine/a,,b/agent.sock,"
                              "server=on,wait=off",
                              "-device",
                              "virtserialport,bus=vitrine-serial.0,chardev=vitrine-agent,"
                              "name=org.vitrine.agent.0,id=vitrine-agent-port"}));

        const QString x86 = "/usr/bin/qemu-system-x86_64";
        QVERIFY(addsAgentPort(ArgsFile::parse("-machine q35\n"), x86));
        QVERIFY(addsAgentPort(ArgsFile::parse("-m 1G\n"), x86));
        QVERIFY(addsAgentPort(ArgsFile::parse("-machine virt\n"), "/opt/qemu-system-aarch64"));
        QVERIFY(!addsAgentPort(ArgsFile::parse("-machine isapc\n"), x86));
        QVERIFY(!addsAgentPort(ArgsFile::parse("-machine microvm\n"), x86));
        QVERIFY(!addsAgentPort(ArgsFile::parse(""), "/opt/qemu-system-ppc64"));
        /* its own */
        QVERIFY(!addsAgentPort(
            ArgsFile::parse("-device virtserialport,chardev=a,name=org.vitrine.agent.0\n"), x86));
    }

    void medium()
    {
        QCOMPARE(mediumArgs("/home/me/tools,1.img"),
                 QStringList({"-drive",
                              "if=none,id=vitrine-tools,format=raw,readonly=on,"
                              "file=/home/me/tools,,1.img",
                              "-device", "virtio-blk-pci,drive=vitrine-tools,id=vitrine-tools-disk"}));

        QVERIFY(!GuestTools::medium().isValid());
        const QString image = writeMedium();
        const Medium m = GuestTools::medium();
        QVERIFY(m.isValid());
        QCOMPARE(m.image, image);
        QCOMPARE(m.mediumId, "32fe6c83b34e87e8");
        QCOMPARE(m.fedora, "44");
        QCOMPARE(m.tools, "0.1.0-1.fc44");
        QCOMPARE(m.kwin, "6.7.5-1.21.fc44");
        QCOMPARE(m.packages, QStringList({"kwin-6.7.5-1.21.fc44.x86_64",
                                          "vitrine-guest-tools-0.1.0-1.fc44.noarch"}));
        QVERIFY(!parseManifest("{}", image).isValid());
        QVERIFY(!parseManifest("not json", image).isValid());
    }

    /* the unit the guest's systemd gets through SMBIOS, and how it is started */
    void bootstrapCredentials()
    {
        const QStringList args = bootstrapArgs();
        const QString unitPrefix = "type=11,value=io.systemd.credential.binary:"
                                   "systemd.extra-unit.vitrine-tools-bootstrap.service=";
        const QString dropInPrefix = "type=11,value=io.systemd.credential.binary:"
                                     "systemd.unit-dropin.multi-user.target~vitrine-tools=";

        QCOMPARE(args.size(), 4);
        QCOMPARE(args[0], "-smbios");
        QCOMPARE(args[2], "-smbios");
        QVERIFY(args[1].startsWith(unitPrefix));
        QVERIFY(args[3].startsWith(dropInPrefix));
        /* QEMU's option syntax: no comma in the values (base64 has none) */
        QCOMPARE(args[1].count(','), 1);
        QCOMPARE(args[3].count(','), 1);

        const QByteArray unit = QByteArray::fromBase64(args[1].mid(unitPrefix.size()).toLatin1());
        const QByteArray dropIn = QByteArray::fromBase64(args[3].mid(dropInPrefix.size()).toLatin1());
        QCOMPARE(unit, bootstrapUnit());
        QCOMPARE(dropIn, bootstrapDropIn());
        /* the unit: not in the initrd, mounts the medium by its label, runs its bootstrap */
        QVERIFY(unit.contains("\nConditionPathExists=!/etc/initrd-release\n"));
        QVERIFY(unit.contains("\nRequires=dev-disk-by\\x2dlabel-VITRINETOOL.device\n"));
        QVERIFY(unit.contains("/usr/bin/mount -o ro /dev/disk/by-label/VITRINETOOL "));
        QVERIFY(unit.contains("\nExecStart=/bin/sh /run/vitrine-tools-medium/bootstrap\n"));
        QVERIFY(unit.contains("\nType=oneshot\n"));
        QVERIFY(unit.contains("Before=display-manager.service"));
        QVERIFY(dropIn.contains("[Unit]\nWants=vitrine-tools-bootstrap.service\n"));
        QCOMPARE(QString(kLabel).size(), 11);    // FAT's limit

        QVERIFY(canBootstrap(ArgsFile::parse("-machine q35\n"), "qemu-system-x86_64"));
        QVERIFY(!canBootstrap(ArgsFile::parse("-machine virt\n"), "qemu-system-aarch64"));
        QVERIFY(!canBootstrap(ArgsFile::parse("-machine isapc\n"), "qemu-system-x86_64"));
    }

    void pendingSetting()
    {
        setPending("vitrine-test-vm", Pending::Bootstrap);
        QCOMPARE(pending("vitrine-test-vm"), Pending::Bootstrap);
        setPending("vitrine-test-vm", Pending::Medium);
        QCOMPARE(pending("vitrine-test-vm"), Pending::Medium);
        setPending("vitrine-test-vm", Pending::None);
        QCOMPARE(pending("vitrine-test-vm"), Pending::None);
        QCOMPARE(pending("other"), Pending::None);
    }

    /* the runner's command line: the agent's port always, the medium and the
       credentials while the tools are pending */
    void runnerCommandLine()
    {
        QTemporaryDir tmp;
        const QString id = "vitrine-test-vm";
        const VmRunner runner(id, tmp.path());
        const QString qemu = "#qemu /opt/q/bin/qemu-system-x86_64\n";
        auto command = [&](const QString &text) {
            return runner.commandLine(ArgsFile::parse(qemu + text));
        };

        QDir(dataDir()).removeRecursively();
        setPending(id, Pending::None);
        QStringList c = command("-machine q35\n");
        const QString runDir = QFileInfo(c.last()).absolutePath();
        /* then qemu-ga's port, on a controller of its own (test_vmrunner) */
        QCOMPARE(valuesOf(c, "-device"),
                 QStringList({"virtio-serial-pci,id=vitrine-serial",
                              "virtserialport,bus=vitrine-serial.0,chardev=vitrine-agent,"
                              "name=org.vitrine.agent.0,id=vitrine-agent-port",
                              "virtio-serial-pci,id=vitrine-ga-serial",
                              "virtserialport,bus=vitrine-ga-serial.0,chardev=vitrine-ga,"
                              "name=org.qemu.guest_agent.0,id=vitrine-ga-port"}));
        QCOMPARE(valuesOf(c, "-chardev"),
                 QStringList({"socket,id=vitrine-agent,path=" + runDir +
                                  "/agent.sock,server=on,wait=off",
                              "socket,id=vitrine-ga,path=" + runDir + "/qga.sock,server=on,wait=off"}));
        QVERIFY(valuesOf(c, "-smbios").isEmpty());

        /* with shares to mount: qemu-ga's port first on the one controller */
        c = command("-machine q35,memory-backend=m\n#share tag=t,path=/x,mount=/mnt/x\n");
        QCOMPARE(valuesOf(c, "-device").filter("virtio-serial-pci").size(), 1);
        QCOMPARE(valuesOf(c, "-device").filter("virtserialport,bus=vitrine-serial.0").size(), 2);
        /* a VM with qemu-ga's port of its own: the agent's alone on the controller */
        c = command("-machine q35\n-device virtio-serial\n"
                    "-device virtserialport,chardev=ga,name=org.qemu.guest_agent.0\n");
        QCOMPARE(valuesOf(c, "-device").filter("bus=vitrine-serial.0").size(), 1);
        QCOMPARE(valuesOf(c, "-device").filter("vitrine-serial").size(), 2);

        /* pending, but no medium built: nothing to attach */
        setPending(id, Pending::Bootstrap);
        QVERIFY(valuesOf(command("-machine q35\n"), "-smbios").isEmpty());

        const QString image = writeMedium();
        c = command("-machine q35\n");
        QCOMPARE(valuesOf(c, "-drive"),
                 QStringList({"if=none,id=vitrine-tools,format=raw,readonly=on,file=" + image}));
        QCOMPARE(valuesOf(c, "-smbios"), valuesOf(bootstrapArgs(), "-smbios"));
        /* the medium only */
        setPending(id, Pending::Medium);
        c = command("-machine q35\n");
        QCOMPARE(valuesOf(c, "-drive").size(), 1);
        QVERIFY(valuesOf(c, "-smbios").isEmpty());
        /* not for an ARM guest */
        setPending(id, Pending::Bootstrap);
        c = runner.commandLine(ArgsFile::parse("#qemu /opt/qemu-system-aarch64\n-machine virt\n"));
        QVERIFY(valuesOf(c, "-drive").isEmpty());
        QVERIFY(valuesOf(c, "-smbios").isEmpty());
        QCOMPARE(valuesOf(c, "-device").size(), 4);     // the agents' ports still
        setPending(id, Pending::None);
        QDir(dataDir()).removeRecursively();
    }

    void messages()
    {
        Message m = parseMessage(kHello);
        QCOMPARE(m.type, Message::Type::Hello);
        QCOMPARE(m.protocol, 1);
        QCOMPARE(m.id, -1);
        const Report &r = m.report;
        QCOMPARE(r.tools, "0.1.0-1.fc44");
        QCOMPARE(r.agent, "0.1.0");
        QCOMPARE(r.os, (System{"fedora", "44", "Fedora Linux 44 (KDE Plasma)"}));
        QCOMPARE(r.kernel, "7.2.7-200.fc44.x86_64");
        QCOMPARE(r.kernels.size(), 2);
        QCOMPARE(r.kernels[0].driver, "installed");
        QVERIFY(r.kernels[0].headers);
        QCOMPARE(r.kernels[1].driver, "no-headers");
        QCOMPARE(r.secureBoot, std::optional<bool>(false));
        QCOMPARE(r.desktops.value("kde"), "6.7.5");
        QVERIFY(r.driverPresent);           // not said: assumed
        QVERIFY(r.driverLoaded);
        QVERIFY(r.driverPatched);
        QCOMPARE(r.taint, "OE");
        QCOMPARE(r.driverParams.value("blob_flush_fence"), "3");
        QCOMPARE(r.capsets, QStringList({"virgl", "virgl2", "drm"}));
        QCOMPARE(r.packages.value("mesa-dri-drivers"), "26.2.3-1.xe.fc44");
        QVERIFY(r.kwinPatched);
        QCOMPARE(r.preempt, "full");
        QVERIFY(!r.rebootNeeded);
        QVERIFY(!r.installing);
        QCOMPARE(r.installedMedium, "32fe6c83b34e87e8");

        m = parseMessage(R"({"type":"status","id":7,"status":{"tools":null,"secureBoot":null}})");
        QCOMPARE(m.type, Message::Type::Status);
        QCOMPARE(m.id, 7);
        QVERIFY(m.report.tools.isEmpty());
        QVERIFY(!m.report.secureBoot.has_value());

        m = parseMessage(R"({"type":"progress","id":null,"step":"Installing the packages","current":3,"total":6})");
        QCOMPARE(m.type, Message::Type::Progress);
        QCOMPARE(m.id, -1);
        QCOMPARE(m.step, "Installing the packages");
        QCOMPARE(m.current, 3);
        QCOMPARE(m.total, 6);

        m = parseMessage(R"({"type":"result","id":2,"cmd":"install-from-medium","ok":false,)"
                         R"("error":"ERROR: dnf failed","rebootNeeded":false})");
        QCOMPARE(m.type, Message::Type::Result);
        QCOMPARE(m.command, "install-from-medium");
        QVERIFY(!m.ok);
        QCOMPARE(m.error, "ERROR: dnf failed");

        m = parseMessage(R"({"type":"error","id":3,"error":"unknown command 'x'"})");
        QCOMPARE(m.type, Message::Type::Error);
        QCOMPARE(m.error, "unknown command 'x'");

        QCOMPARE(parseMessage("{\"type\":\"other\"}").type, Message::Type::Invalid);
        QCOMPARE(parseMessage("[1]").type, Message::Type::Invalid);
        QCOMPARE(parseMessage("not json").type, Message::Type::Invalid);
        QCOMPARE(parseMessage("").type, Message::Type::Invalid);

        /* the guest's counters, for the status bar (protocol 2) */
        m = parseMessage(R"({"type":"stats","id":9,"stats":{"time":12.5,"net":[{"name":"enp0s3",)"
                         R"("rxBytes":100,"txBytes":50}],"memory":{"total":4,"available":3}}})");
        QCOMPARE(m.type, Message::Type::Stats);
        QCOMPARE(m.id, 9);
        QCOMPARE(m.stats["time"].toDouble(), 12.5);
        QCOMPARE(m.stats["net"].toArray().size(), 1);
        QCOMPARE(m.stats["memory"].toObject()["available"].toInt(), 3);
        QCOMPARE(parseMessage(R"({"type":"hello","protocol":2,"status":{}})").protocol, 2);
        QCOMPARE(kStatsProtocol, 2);

        QCOMPARE(commandLine("status", 3), QByteArray("{\"cmd\":\"status\",\"id\":3}\n"));
        QCOMPARE(commandLine("stats", 8), QByteArray("{\"cmd\":\"stats\",\"id\":8}\n"));
        QCOMPARE(commandLine("install-from-medium", 4),
                 QByteArray("{\"cmd\":\"install-from-medium\",\"id\":4}\n"));

        /* systemctl's refusal comes back as the error */
        m = parseMessage(R"({"type":"result","id":5,"cmd":"shutdown","ok":false,)"
                         R"("error":"Operation inhibited","rebootNeeded":false})");
        QCOMPARE(m.command, "shutdown");
        QVERIFY(!m.ok);
        QCOMPARE(m.error, "Operation inhibited");
    }

    void states()
    {
        const Medium medium = parseManifest(
            R"({"mediumId": "32fe6c83b34e87e8", "fedora": "44", "tools": "0.1.0-1.fc44"})",
            "/x.img");
        Inputs in;

        QCOMPARE(evaluate(in), State::Unknown);
        in.pending = Pending::Bootstrap;
        QCOMPARE(evaluate(in), State::Pending);

        /* running, waiting for an agent */
        in = Inputs{};
        in.running = true;
        in.medium = medium;
        QCOMPARE(evaluate(in), State::Unknown);
        in.waited = true;
        QCOMPARE(evaluate(in), State::NotInstalled);
        in.pending = Pending::Bootstrap;            // asked for, the VM not restarted yet
        QCOMPARE(evaluate(in), State::Pending);
        in.pending = Pending::None;
        in.bootstrapRun = true;
        in.waited = false;
        QCOMPARE(evaluate(in), State::Installing);  // the guest installs before its desktop
        in.waited = true;
        QCOMPARE(evaluate(in), State::NotInstalled);    // it did not (no systemd 256...)
        in.installing = true;
        QCOMPARE(evaluate(in), State::Installing);
        in.installing = false;
        in.failed = true;
        QCOMPARE(evaluate(in), State::Failed);

        /* the agent answered */
        in = Inputs{};
        in.running = true;
        in.medium = medium;
        in.agentSeen = true;
        in.report = parseMessage(kHello).report;
        QCOMPARE(evaluate(in), State::Installed);
        in.report.rebootNeeded = true;
        QCOMPARE(evaluate(in), State::RebootNeeded);
        in.report.rebootNeeded = false;
        in.report.driverPatched = false;
        QCOMPARE(evaluate(in), State::DriverNotActive);
        in.report.driverPresent = false;            // no virtio GPU: nothing to drive
        QCOMPARE(evaluate(in), State::Installed);
        in.report.driverPresent = true;
        in.report.driverPatched = true;
        in.report.installedMedium = "0000000000000000";
        QCOMPARE(evaluate(in), State::UpdateAvailable);
        in.report.installedMedium = medium.mediumId;
        in.report.tools = "0.0.9-1.fc44";
        QCOMPARE(evaluate(in), State::UpdateAvailable);
        in.medium = {};                             // no medium built: nothing to compare
        QCOMPARE(evaluate(in), State::Installed);
        in.medium = medium;
        in.report.tools.clear();
        QCOMPARE(evaluate(in), State::NotInstalled);
        in.report.tools = medium.tools;
        in.report.os.version = "45";
        QCOMPARE(evaluate(in), State::Unsupported);
        /* an agent that does not say what the guest runs: not another system */
        in.report.os = {};
        QCOMPARE(evaluate(in), State::Installed);
        in.report.os = {"fedora", "45", ""};
        in.running = false;
        QCOMPARE(evaluate(in), State::Unknown);

        /* off: what the last run showed */
        Inputs off;
        off.medium = medium;
        off.remembered = State::NotInstalled;
        QCOMPARE(evaluate(off), State::NotInstalled);
        off.remembered = State::Installed;
        off.rememberedTools = medium.tools;
        QCOMPARE(evaluate(off), State::Unknown);
        off.rememberedTools = "0.0.9-1.fc44";
        QCOMPARE(evaluate(off), State::UpdateAvailable);
        off.pending = Pending::Bootstrap;
        QCOMPARE(evaluate(off), State::Pending);

        /* the installer of this medium failed where the host did not see it */
        in.running = true;
        in.report.os.version = "44";
        in.report.lastMedium = medium.mediumId;
        in.report.lastOk = false;
        QCOMPARE(evaluate(in), State::Failed);
        in.report.lastMedium = "another medium";
        QCOMPARE(evaluate(in), State::Installed);
        in.report.lastMedium = medium.mediumId;
        in.report.lastOk = true;
        QCOMPARE(evaluate(in), State::Installed);

        /* the bootstrap of this run, which the agent of the tools in the
           guest answers before (the unit waits for the network) */
        in.report.bootstrap = "waiting";
        QCOMPARE(evaluate(in), State::Installing);
        in.report.bootstrap = "running";
        QCOMPARE(evaluate(in), State::Installing);
        in.report.bootstrap = "done";
        QCOMPARE(evaluate(in), State::Installed);
        /* an older agent does not say: this medium not installed yet is the sign */
        in.report.bootstrap.clear();
        in.bootstrapRun = true;
        in.report.lastMedium = "an older medium";
        QCOMPARE(evaluate(in), State::Installing);
        in.bootstrapExpired = true;
        QCOMPARE(evaluate(in), State::Installed);
    }

    /*
     * The systems the tools are for, in one place: Fedora at the release of
     * the medium, any while none is built; what a guest's agents said, and
     * how vitrine keeps it per VM
     */
    void systems_data()
    {
        QTest::addColumn<QString>("id");
        QTest::addColumn<QString>("version");
        QTest::addColumn<bool>("withMedium");      // the medium for 44 built
        QTest::addColumn<bool>("supported");

        QTest::newRow("fedora 44") << "fedora" << "44" << true << true;
        QTest::newRow("fedora 43") << "fedora" << "43" << true << false;
        QTest::newRow("fedora 45") << "fedora" << "45" << true << false;
        QTest::newRow("fedora 43, no medium") << "fedora" << "43" << false << true;
        QTest::newRow("arch") << "arch" << "" << true << false;
        QTest::newRow("arch, no medium") << "arch" << "" << false << false;
        QTest::newRow("kde linux") << "kde-linux" << "" << true << false;
        QTest::newRow("ubuntu") << "ubuntu" << "24.04" << false << false;
        QTest::newRow("windows") << "mswindows" << "10" << true << false;
        QTest::newRow("unknown") << "" << "" << true << false;
        QTest::newRow("unknown, no medium") << "" << "" << false << false;
    }

    void systems()
    {
        QFETCH(QString, id);
        QFETCH(QString, version);
        QFETCH(bool, withMedium);
        QFETCH(bool, supported);
        QDir(dataDir()).removeRecursively();
        if (withMedium) {
            writeMedium();
        }
        const Medium m = GuestTools::medium();
        QCOMPARE(m.isValid(), withMedium);

        QCOMPARE(supports(System{id, version, ""}, m), supported);
        /* the medium's release, or the one a build makes */
        QCOMPARE(supportedSystems(m), "Fedora 44");
        QDir(dataDir()).removeRecursively();
    }

    /* qemu-ga's guest-get-osinfo, as it answers for Linux and Windows */
    void osInfo()
    {
        const System fedora = parseOsInfo(QJsonObject{
            {"name", "Fedora Linux"}, {"kernel-release", "7.2.7-200.fc44.x86_64"},
            {"version", "44 (KDE Plasma Desktop Edition)"},
            {"variant", "KDE Plasma Desktop Edition"},
            {"pretty-name", "Fedora Linux 44 (KDE Plasma Desktop Edition)"},
            {"version-id", "44"}, {"variant-id", "kde"},
            {"kernel-version", "#1 SMP PREEMPT_DYNAMIC"}, {"machine", "x86_64"},
            {"id", "fedora"}});
        QCOMPARE(fedora, (System{"fedora", "44", "Fedora Linux 44 (KDE Plasma Desktop Edition)"}));
        QCOMPARE(fedora.displayName(), "Fedora Linux 44 (KDE Plasma Desktop Edition)");
        /* a rolling release: no VERSION_ID */
        const System arch = parseOsInfo(QJsonObject{
            {"name", "Arch Linux"}, {"pretty-name", "Arch Linux"}, {"id", "arch"},
            {"kernel-release", "6.17.1-arch1-1"}, {"machine", "x86_64"}});
        QCOMPARE(arch, (System{"arch", "", "Arch Linux"}));
        const System windows = parseOsInfo(QJsonObject{
            {"name", "Microsoft Windows"}, {"pretty-name", "Windows 10 Pro"},
            {"id", "mswindows"}, {"version", "Microsoft Windows 10"}, {"version-id", "10"},
            {"variant", "client"}, {"variant-id", "client"}, {"kernel-version", "10.0"},
            {"machine", "x86_64"}});
        QCOMPARE(windows, (System{"mswindows", "10", "Windows 10 Pro"}));
        /* no pretty name: the name; neither: the id and the version */
        QCOMPARE(parseOsInfo(QJsonObject{{"id", "x"}, {"name", "X OS"}}).displayName(), "X OS");
        QCOMPARE(parseOsInfo(QJsonObject{{"id", "x"}, {"version-id", "3"}}).displayName(), "x 3");
        QCOMPARE(parseOsInfo(QJsonObject{{"id", "x"}}).displayName(), "x");
        QVERIFY(!parseOsInfo(QJsonObject()).isKnown());
    }

    /* Kept per VM, as written, until another answer; an unknown one forgets */
    void systemSetting()
    {
        const QString id = "vitrine-test-system";
        const System kde{"kde-linux", "", "KDE Linux, \"rolling\" = 1; [x]"};
        QVERIFY(!system(id).isKnown());
        setSystem(id, kde);
        QCOMPARE(system(id), kde);
        QVERIFY(!GuestTools::system("vitrine-test-other").isKnown());
        setSystem(id, {"fedora", "44", "Fedora Linux 44 (KDE Plasma Desktop Edition)"});
        QCOMPARE(system(id).version, "44");
        setSystem(id, {});
        QVERIFY(!system(id).isKnown());
    }

    void failures()
    {
        const Report r = parseReport(QJsonDocument::fromJson(
            R"({"tools": "0.1.0-1.fc44", "lastInstall": {"medium": "c3b739935785acc2",
                "tools": "0.1.0-3.fc44", "ok": false, "code": 3, "error": "Secure Boot is on"}})")
            .object());
        QCOMPARE(r.lastMedium, "c3b739935785acc2");
        QCOMPARE(r.lastOk, std::optional<bool>(false));
        QCOMPARE(r.lastError, "Secure Boot is on");
        QVERIFY(!parseReport({}).lastOk.has_value());
        QCOMPARE(parseReport(QJsonDocument::fromJson(R"({"bootstrap": "running"})").object())
                     .bootstrap,
                 "running");
        QVERIFY(parseReport(QJsonDocument::fromJson(R"({"bootstrap": null})").object())
                    .bootstrap.isEmpty());

        QCOMPARE(failureReason("== [3/6] Installing\nNo match\nERROR: not installed: a b\n"),
                 "not installed: a b");
        QCOMPARE(failureReason("Secure Boot is on in this VM.\nTurn it off."),
                 "Secure Boot is on in this VM. Turn it off.");
        QCOMPARE(failureReason(""), "");
    }

    void driverProblems()
    {
        Report r = parseMessage(kHello).report;

        r.secureBoot = true;
        QVERIFY(driverProblem(r).contains("Secure Boot"));
        r.driverLoaded = false;
        QVERIFY(driverProblem(r).contains("no graphics driver"));
        r.driverLoaded = true;
        r.secureBoot = false;
        QVERIFY(driverProblem(r).contains("restart the guest"));
        r.kernel = "7.2.5-200.fc44.x86_64";
        QVERIFY(driverProblem(r).contains("no headers"));
        r.kernels[1].driver = "missing";
        QVERIFY(driverProblem(r).contains("did not build"));
        r.kernel = "7.3.0-100.fc44.x86_64";
        QCOMPARE(driverProblem(r), "the driver is not loaded");
    }

    /* QEMU takes the agent's port, the medium and the credentials */
    void realQemu()
    {
        if (!QFileInfo(testQemu()).isExecutable()) {
            QSKIP("no QEMU build, set VITRINE_TEST_QEMU");
        }
        QTemporaryDir tmp;
        const QString id = QString("vitrine-gt-test-%1").arg(QCoreApplication::applicationPid());
        Paths::setQemuBinary(testQemu());
        writeMedium();
        setPending(id, Pending::Bootstrap);
        VmRunner runner(id, tmp.path());
        QSignalSpy failed(&runner, &VmRunner::failed);

        runner.start(ArgsFile::parse("-machine q35\n-m 128\n-nodefaults\n-display none\n"));
        QTRY_COMPARE_WITH_TIMEOUT(runner.state(), VmRunner::State::Running, 20000);
        QVERIFY2(failed.isEmpty(), qPrintable(failed.isEmpty() ? QString() : failed[0][0].toString()));
        const QString socket = runner.agentSocket();
        QVERIFY(QFileInfo::exists(socket));

        /* the medium, read-only */
        bool readOnly = false, found = false;
        runner.qmp()->execute("query-block", {}, [&](const QJsonValue &result, const QString &) {
            for (const QJsonValue &b : result.toArray()) {
                if (b["device"].toString() == "vitrine-tools") {
                    readOnly = b["inserted"]["ro"].toBool();
                }
            }
            found = true;
        });
        QTRY_VERIFY(found);
        QVERIFY(readOnly);

        /* qemu-ga's port beside it, on the same controller */
        QVERIFY(QFileInfo::exists(runner.guestAgentSocket()));

        /* Shut Down asks the shutdown handler first; when it declines,
           qemu-ga, whose port no guest opened here, then the power button */
        using Way = GuestShutdown::Way;
        int asked = 0;
        bool takes = true;
        QList<Way> ways;
        connect(&runner, &VmRunner::shutdownWayChanged, this, [&ways](Way way) { ways << way; });
        runner.setShutdownHandler([&](const std::function<void(bool)> &answer) {
            asked++;
            if (takes) {
                QTimer::singleShot(0, this, [answer]() { answer(true); });
            }
            return takes;
        });
        runner.powerdown();
        QCOMPARE(asked, 1);
        QCOMPARE(runner.shutdownWay(), Way::ToolsAgent);
        QTest::qWait(50);
        takes = false;
        runner.powerdown();     // the power button: a VM without a guest stays up
        QCOMPARE(asked, 2);
        QTRY_COMPARE(runner.shutdownWay(), Way::PowerButton);
        QCOMPARE(ways, QList<Way>({Way::ToolsAgent, Way::PowerButton}));
        QTest::qWait(200);
        QCOMPARE(runner.state(), VmRunner::State::Running);

        runner.forceOff();
        QTRY_COMPARE_WITH_TIMEOUT(runner.state(), VmRunner::State::Stopped, 15000);
        QVERIFY(!QFileInfo::exists(socket));
        runner.powerdown();     // not running: the handler is not asked
        QCOMPARE(asked, 2);
        setPending(id, Pending::None);
        Paths::setQemuBinary({});
        QDir(dataDir()).removeRecursively();
    }

    /*
     * The agent's report of tools in the guest takes back an install pending
     * from before the run (they are in after all), not one asked for in the
     * run: an update or an install again, over the tools the guest has, for
     * which the guest shuts down to start again with the medium
     */
    void pendingAskedInTheRun()
    {
        QTemporaryDir tmp;
        const QString id = QString("vitrine-gt-pending-%1").arg(QCoreApplication::applicationPid());
        const QString dir = tmp.filePath(id);
        QVERIFY(QDir().mkpath(dir));
        QFile args(dir + "/vm.args");
        QVERIFY(args.open(QIODevice::WriteOnly));
        args.write("-machine q35\n");
        args.close();
        const QStringList command = VmRunner(id, dir).commandLine(ArgsFile::parse("-machine q35\n"));
        const QString runDir = QFileInfo(command.last()).absolutePath();

        /* a QEMU found running, and the agent behind the port's socket */
        FakeQmp qmp(runDir + "/qmp.sock");
        QLocalServer agent;
        QLocalServer::removeServer(runDir + "/agent.sock");
        QVERIFY(agent.listen(runDir + "/agent.sock"));
        QPointer<QLocalSocket> peer;
        connect(&agent, &QLocalServer::newConnection, this,
                [&]() { peer = agent.nextPendingConnection(); });
        QProcess qemu;
        qemu.start(FAKE_QEMU, {"-qmp", command[command.size() - 3]});
        QVERIFY(qemu.waitForStarted());
        /* running its program, not still the fork that execs it */
        QVERIFY(QTest::qWaitFor([&qemu]() {
            QFile f(QString("/proc/%1/cmdline").arg(qemu.processId()));
            return f.open(QIODevice::ReadOnly) && f.readAll().contains("-qmp");
        }, 5000));
        QFile pid(runDir + "/qemu.pid");
        QVERIFY(pid.open(QIODevice::WriteOnly));
        pid.write(QByteArray::number(qemu.processId()) + '\n');
        pid.close();
        const auto end = qScopeGuard([&]() {
            setPending(id, Pending::None);
            qemu.kill();
            qemu.waitForFinished();
        });

        setPending(id, Pending::Bootstrap);
        VmStore store(tmp.path());
        Vm *vm = store.find(id);
        QVERIFY(vm);
        GuestToolsMonitor *monitor = GuestToolsMonitor::of(vm);
        vm->runner()->attach(vm->args());
        QTRY_COMPARE(vm->runner()->state(), VmRunner::State::Running);
        QTRY_VERIFY(peer);
        auto report = [&]() { peer->write(QByteArray(kHello) + '\n'); };

        /* from before the run, the tools in: no install at the next start */
        report();
        QTRY_VERIFY(monitor->hasAgent());
        QCOMPARE(pending(id), Pending::None);

        /* asked for in the run, as Install Guest Tools does: kept */
        monitor->setPending(Pending::Bootstrap);
        QSignalSpy changed(monitor, &GuestToolsMonitor::changed);
        report();
        QTRY_VERIFY(!changed.isEmpty());
        QCOMPARE(pending(id), Pending::Bootstrap);
    }

    /*
     * What the guest runs, as its agents say: the guest tools' agent in its
     * report, qemu-ga through the runner; kept for the VM, which the next
     * monitor of it (the app's next start) knows from the start.  An agent
     * that does not say leaves what an earlier one said.
     */
    void systemFromAgents()
    {
        QTemporaryDir tmp;
        const QString id = QString("vitrine-gt-system-%1").arg(QCoreApplication::applicationPid());
        const QString dir = tmp.filePath(id);
        QVERIFY(QDir().mkpath(dir));
        QFile args(dir + "/vm.args");
        QVERIFY(args.open(QIODevice::WriteOnly));
        args.write("-machine q35\n");
        args.close();
        const QStringList command = VmRunner(id, dir).commandLine(ArgsFile::parse("-machine q35\n"));
        const QString runDir = QFileInfo(command.last()).absolutePath();
        const auto forget = qScopeGuard([&]() { setSystem(id, {}); });
        writeMedium();

        /* qemu-ga, at a boot of the guest */
        {
            VmStore store(tmp.path());
            Vm *vm = store.find(id);
            QVERIFY(vm);
            GuestToolsMonitor *monitor = GuestToolsMonitor::of(vm);
            QVERIFY(!monitor->system().isKnown());
            QVERIFY(!monitor->supported() && !monitor->unsupported());
            QSignalSpy changed(monitor, &GuestToolsMonitor::changed);
            emit vm->runner()->guestOsInfo(
                QJsonObject{{"id", "kde-linux"}, {"pretty-name", "KDE Linux"}});
            QCOMPARE(changed.size(), 1);
            QCOMPARE(monitor->system(), (System{"kde-linux", "", "KDE Linux"}));
            QVERIFY(!monitor->supported() && monitor->unsupported());
            /* the same again: no change */
            emit vm->runner()->guestOsInfo(
                QJsonObject{{"id", "kde-linux"}, {"pretty-name", "KDE Linux"}});
            QCOMPARE(changed.size(), 1);
            /* nothing said: kept */
            emit vm->runner()->guestOsInfo(QJsonObject());
            QCOMPARE(monitor->system().id, "kde-linux");
        }
        QCOMPARE(system(id).id, "kde-linux");

        /* the guest tools' agent, in a run found going */
        FakeQmp qmp(runDir + "/qmp.sock");
        QLocalServer agent;
        QLocalServer::removeServer(runDir + "/agent.sock");
        QVERIFY(agent.listen(runDir + "/agent.sock"));
        QPointer<QLocalSocket> peer;
        connect(&agent, &QLocalServer::newConnection, this,
                [&]() { peer = agent.nextPendingConnection(); });
        QProcess qemu;
        qemu.start(FAKE_QEMU, {"-qmp", command[command.size() - 3]});
        QVERIFY(qemu.waitForStarted());
        QVERIFY(QTest::qWaitFor([&qemu]() {
            QFile f(QString("/proc/%1/cmdline").arg(qemu.processId()));
            return f.open(QIODevice::ReadOnly) && f.readAll().contains("-qmp");
        }, 5000));
        QFile pid(runDir + "/qemu.pid");
        QVERIFY(pid.open(QIODevice::WriteOnly));
        pid.write(QByteArray::number(qemu.processId()) + '\n');
        pid.close();
        const auto end = qScopeGuard([&]() {
            qemu.kill();
            qemu.waitForFinished();
        });
        {
            VmStore store(tmp.path());
            Vm *vm = store.find(id);
            QVERIFY(vm);
            GuestToolsMonitor *monitor = GuestToolsMonitor::of(vm);
            QCOMPARE(monitor->system(), (System{"kde-linux", "", "KDE Linux"}));
            vm->runner()->attach(vm->args());
            QTRY_COMPARE(vm->runner()->state(), VmRunner::State::Running);
            QTRY_VERIFY(peer);
            peer->write(QByteArray(kHello) + '\n');
            QTRY_VERIFY(monitor->hasAgent());
            QCOMPARE(monitor->system(), (System{"fedora", "44", "Fedora Linux 44 (KDE Plasma)"}));
            QVERIFY(monitor->supported() && !monitor->unsupported());
            /* an older agent, which does not say */
            QByteArray older = kHello;
            older.replace(R"j("os":{"id":"fedora","version":"44","name":"Fedora Linux 44 (KDE Plasma)"},)j",
                          "");
            QVERIFY(older != kHello);
            peer->write(older.replace(R"j("type":"hello")j", R"j("type":"status")j") + '\n');
            QTest::qWait(200);
            QCOMPARE(monitor->system().id, "fedora");
        }
        QCOMPARE(system(id), (System{"fedora", "44", "Fedora Linux 44 (KDE Plasma)"}));
        QDir(dataDir()).removeRecursively();
    }

    /*
     * A real guest, end to end, through the app's own code: the runner
     * starts the VM of VITRINE_TEST_GUEST (a VM folder, its vm.args) with
     * the agent's port, and with VITRINE_TEST_MEDIUM (a built medium, its
     * .json beside it) the medium and the bootstrap; the monitor follows
     * the agent until the tools are in (VITRINE_TEST_GUEST_TIMEOUT
     * seconds, 1800 by default), then shuts the guest down through it.
     * Not run without these: it boots a guest for minutes.
     */
    void realGuest()
    {
        const QString dir = qEnvironmentVariable("VITRINE_TEST_GUEST");
        const QString image = qEnvironmentVariable("VITRINE_TEST_MEDIUM");
        const int timeout = qEnvironmentVariableIntValue("VITRINE_TEST_GUEST_TIMEOUT");

        if (dir.isEmpty()) {
            QSKIP("VITRINE_TEST_GUEST is not set");
        }
        Vm vm(dir);
        if (!image.isEmpty()) {
            QDir().mkpath(dataDir());
            const QString to = dataDir() + "/vitrine-guest-tools-fc44";
            QFile::remove(to + ".img");
            QFile::remove(to + ".json");
            QVERIFY(QFile::link(image, to + ".img"));
            QVERIFY(QFile::copy(QString(image).replace(".img", ".json"), to + ".json"));
            QVERIFY(GuestTools::medium().isValid());
            setPending(vm.id(), Pending::Bootstrap);
        }
        GuestToolsMonitor *monitor = GuestToolsMonitor::of(&vm);
        QString last;
        connect(monitor, &GuestToolsMonitor::changed, this, [&]() {
            const QString now = QString("state %1: %2").arg(int(monitor->state())).arg(monitor->text());
            if (now != last) {
                qInfo("%s", qPrintable(now));
                last = now;
            }
        });

        vm.runner()->start(vm.args());
        QTRY_COMPARE_WITH_TIMEOUT(vm.runner()->state(), VmRunner::State::Running, 30000);
        if (!image.isEmpty()) {
            QCOMPARE(pending(vm.id()), Pending::None);   // what the start brought
        }
        /* a failed install stays Failed once the medium's agent has gone */
        QTRY_VERIFY_WITH_TIMEOUT(monitor->state() == State::Failed ||
                                     (monitor->hasAgent() && !monitor->inputs().installing &&
                                      (monitor->state() == State::Installed ||
                                       monitor->state() == State::DriverNotActive)),
                                 (timeout > 0 ? timeout : 1800) * 1000);
        const Report r = monitor->report();
        qInfo("guest: tools %s, kernel %s, driver %s (taint %s), capsets %s, KWin %s%s",
              qPrintable(r.tools), qPrintable(r.kernel), r.driverPatched ? "patched" : "stock",
              qPrintable(r.taint), qPrintable(r.capsets.join(',')),
              qPrintable(r.packages.value("kwin")), r.kwinPatched ? " (vitrine's)" : "");
        /* VITRINE_TEST_GUEST_EXPECT=failed: a guest the installer refuses */
        if (qEnvironmentVariable("VITRINE_TEST_GUEST_EXPECT") == "failed") {
            qInfo("%s", qPrintable(monitor->text()));
            QCOMPARE(monitor->state(), State::Failed);
        } else {
            QCOMPARE(monitor->state(), State::Installed);
        }

        /* Shut Down: through the agent, which the monitor gave the runner */
        vm.runner()->powerdown();
        QTRY_COMPARE_WITH_TIMEOUT(vm.runner()->state(), VmRunner::State::Stopped, 60000);
        QDir(dataDir()).removeRecursively();
    }
};

int main(int argc, char **argv)
{
    /* realGuest() boots a guest for many minutes, past QtTest's watchdog
       (5 min, read once, before the first test function) */
    if (qEnvironmentVariableIsSet("VITRINE_TEST_GUEST") &&
        !qEnvironmentVariableIsSet("QTEST_FUNCTION_TIMEOUT")) {
        qputenv("QTEST_FUNCTION_TIMEOUT", "3600000");
    }
    QCoreApplication app(argc, argv);
    TestGuestTools test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}
#include "test_guesttools.moc"
