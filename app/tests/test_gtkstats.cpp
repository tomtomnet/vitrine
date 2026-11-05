// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef Q_MOC_RUN
#include "gtk/gtkbase.h"
#endif

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

#include "gtkdisplay.h"

#ifndef Q_MOC_RUN
#include "core/paths.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "gtk/gtkloop.h"
#include "gtk/richtext.h"
#include "gtk/statsbutton.h"
#include "ui/statstext.h"
#endif

using Sampler = VmStats::Sampler;

/*
 * The GTK app's statistics, on a display no one sees: the button while
 * the VM runs, the numbers read only while its popover shows (and the
 * details' extra ones while they show), each row's value, a VM found
 * running (the stand-in QEMU and a monitor answering as told); the
 * details' rich text as rows.
 */

/* Waits on the stand-in and on the sampler's polls, a second apart: long, for a busy host */
static const int kSlow = 20000;

static bool writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

/* QEMU's end of QMP: running, its disk busier at each query-blockstats */
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

private:
    QByteArray answer(const QString &command)
    {
        if (command == "query-status") {
            return R"({"running": true, "status": "running"})";
        }
        if (command == "query-blockstats") {
            const qint64 n = ++m_disk;
            /* and the empty drive the guest looked at once */
            return QString(R"([{"device": "disk0", "stats": {"rd_bytes": %1, "wr_bytes": %2,)"
                           R"( "rd_operations": %3, "wr_operations": %3}},)"
                           R"( {"device": "ide2-cd0", "stats": {"rd_bytes": 528, "wr_bytes": 0,)"
                           R"( "rd_operations": 1, "wr_operations": 0}}])")
                .arg(n * 12 << 20)
                .arg(n << 20)
                .arg(n)
                .toUtf8();
        }
        if (command == "query-commands") {
            return R"([{"name": "query-blockstats"}, {"name": "query-stats"}])";
        }
        return "{}";
    }

    QLocalServer m_server;
    QByteArray m_buffer;
    qint64 m_disk = 0;
};

/* A VM found running: the stand-in QEMU (threads "CPU 0/KVM", "CPU 1/KVM") and its monitor */
struct Running {
    QTemporaryDir vms;
    QProcess qemu;
    std::unique_ptr<FakeQmp> qmp;
    std::unique_ptr<VmStore> store;
    Vm *vm = nullptr;

    Running()
    {
        writeFile(vms.path() + "/busy/vm.args", "-name Busy\n-m 1G\n-display none\n");
        const QString run = Paths::vmRuntimeDir("busy");
        const QString qmpArg = QString("unix:%1/qmp.sock,server=on,wait=off").arg(run);
        qemu.start(FAKE_QEMU, {"-qmp", qmpArg});
        qemu.waitForStarted();
        /* the runner knows its QEMU by its command line: once exec is done */
        (void)QTest::qWaitFor([&]() {
            QFile f(QString("/proc/%1/cmdline").arg(qemu.processId()));
            return f.open(QIODevice::ReadOnly) && f.readAll().contains(qmpArg.toLocal8Bit());
        }, kSlow);
        writeFile(run + "/qemu.pid", QByteArray::number(qemu.processId()) + "\n");
        qmp = std::make_unique<FakeQmp>(run + "/qmp.sock");
        store = std::make_unique<VmStore>(vms.path());
        vm = store->find("busy");
    }
    ~Running() { stop(); }
    void stop()
    {
        qmp.reset();
        if (qemu.state() != QProcess::NotRunning) {
            qemu.kill();
            qemu.waitForFinished();
        }
    }
};

class TestGtkStats : public QObject
{
    Q_OBJECT

    QProcess m_broadway;
    GTestDBus *m_bus = nullptr;
    GtkLoop *m_loop = nullptr;

private slots:
    void initTestCase()
    {
        QString error;

        QStandardPaths::setTestModeEnabled(true);
        m_bus = GtkDisplay::up(m_broadway);
        if (!m_bus) {
            QSKIP("gtk4-broadwayd is not installed: no display to test on but the desktop's");
        }
        m_loop = new GtkLoop("org.vitrine.Vitrine");
        QCOMPARE(m_loop->start(&error), GtkLoop::Start::Primary);
    }

    void cleanupTestCase()
    {
        delete m_loop;
        GtkDisplay::down(m_broadway, m_bus);
    }

    /* The details' rich text: its lines and its tables' rows, as plain cells */
    void blocks()
    {
        const QString html =
            "<b>Disks</b>, over the last second" + QString("<table>") +
            PerfStats::tableRow("disk0", "R 12 MiB/s · W 1 MiB/s", "in all 24 MiB read, 2 < 3") +
            PerfStats::tableRow("cd", "< 0.1 %") + "</table><i>A note &amp; more.</i><br>" +
            "<b>KVM</b><table>" + PerfStats::tableRow("VM exits", "1.2 k/s") + "</table>";
        const QList<StatsButton::Block> list = StatsButton::blocks(html);

        QCOMPARE(list.size(), 5);
        QCOMPARE(list[0].markup, QString("<b>Disks</b>, over the last second"));
        QVERIFY(list[1].isTable());
        QCOMPARE(list[1].rows, QList<QStringList>({{"disk0", "R 12 MiB/s · W 1 MiB/s",
                                                    "in all 24 MiB read, 2 < 3"},
                                                   {"cd", "< 0.1 %", ""}}));
        QCOMPARE(list[2].markup, QString("<i>A note &amp; more.</i>"));
        QCOMPARE(list[3].markup, QString("<b>KVM</b>"));
        QCOMPARE(list[4].rows.size(), 1);

        /* what the details of a VM with no numbers yet say parses as GTK's markup */
        const VmStats::Snapshot none;
        for (int i = 0; i < StatsText::kStats; i++) {
            for (const StatsButton::Block &block :
                 StatsButton::blocks(StatsText::details(StatsText::Stat(i), none, {}))) {
                if (!block.isTable()) {
                    QVERIFY2(pango_parse_markup(block.markup.toUtf8().constData(), -1, 0, nullptr,
                                                nullptr, nullptr, nullptr),
                             qPrintable(block.markup));
                }
            }
        }
    }

    /*
     * A VM found running: the button shows; nothing is read until the
     * popover shows, then the list's numbers, the details' extra ones while
     * they show, nothing once it closes; it goes with the run
     */
    void running()
    {
        Running vm;
        StatsButton button([&vm]() { return vm.vm; }, []() { return nullptr; });
        GtkWidget *window = gtk_window_new();

        gtk_window_set_child(GTK_WINDOW(window), button.widget());
        gtk_window_present(GTK_WINDOW(window));
        button.update();
        QVERIFY(!gtk_widget_get_visible(button.widget()));
        vm.vm->runner()->attach(vm.vm->args());
        QTRY_VERIFY_WITH_TIMEOUT(vm.vm->runner()->state() == VmRunner::State::Running, kSlow);
        button.update();
        QVERIFY(gtk_widget_get_visible(button.widget()));
        QCOMPARE(button.sources(), Sampler::Sources());

        button.showPopover(true);
        QTRY_COMPARE(button.sources(), Sampler::Cpu | Sampler::Memory | Sampler::Disk |
                                           Sampler::Network | Sampler::Gpu | Sampler::Display);
        QTRY_VERIFY_WITH_TIMEOUT(button.value(StatsText::Cpu).endsWith(" %"), kSlow);
        QVERIFY(button.value(StatsText::Memory) != "—");
        QTRY_VERIFY_WITH_TIMEOUT(button.value(StatsText::Disk).startsWith("R "), kSlow);
        /* no guest tools in the stand-in */
        QTRY_COMPARE_WITH_TIMEOUT(button.note(StatsText::Network), QString("needs the guest tools"),
                                  kSlow);

        button.showDetails(StatsText::Disk);
        QTRY_VERIFY_WITH_TIMEOUT(button.detailsText().contains("disk0"), kSlow);
        QVERIFY(!button.detailsText().contains("ide2-cd0"));
        button.showDetails(StatsText::Cpu);
        QVERIFY(button.sources() & Sampler::Kvm);
        button.showDetails(StatsText::Memory);
        QVERIFY(button.sources() & Sampler::MemoryDetails);
        QVERIFY(!(button.sources() & Sampler::Kvm));
        button.showDetails(-1);
        QVERIFY(!(button.sources() & Sampler::MemoryDetails));

        button.showPopover(false);
        QTRY_COMPARE(button.sources(), Sampler::Sources());

        vm.stop();
        QTRY_VERIFY_WITH_TIMEOUT(vm.vm->runner()->state() == VmRunner::State::Stopped, kSlow);
        button.update();
        QVERIFY(!gtk_widget_get_visible(button.widget()));
        gtk_window_destroy(GTK_WINDOW(window));
    }
};

int main(int argc, char **argv)
{
    QTemporaryDir runtime(QDir::tempPath() + "/vitrine-gtkstats-XXXXXX");
    if (!GtkDisplay::prepare(runtime)) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    TestGtkStats test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_gtkstats.moc"
