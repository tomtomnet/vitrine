// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The udmabuf warning of the status bar: its text, the explanation with
 * the fix, and the explanation opening by itself once per run.  A VM found
 * running (a stand-in QEMU and its monitor), no host tuning; offscreen.
 */
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

#include <cerrno>
#include <memory>

#include <unistd.h>

#include "core/hostsettings.h"
#include "core/paths.h"
#include "core/udmabuf.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "ui/udmabufnotifier.h"

static bool writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
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
            connect(peer, &QLocalSocket::readyRead, this, [this, peer]() {
                m_buffer += peer->readAll();
                qsizetype nl;
                while ((nl = m_buffer.indexOf('\n')) >= 0) {
                    const QJsonObject c = QJsonDocument::fromJson(m_buffer.left(nl)).object();
                    m_buffer.remove(0, nl + 1);
                    const QByteArray result = c["execute"] == "query-status"
                                                  ? R"({"running": true, "status": "running"})"
                                                  : "{}";
                    peer->write("{\"return\": " + result + ", \"id\": " +
                                QByteArray::number(c["id"].toInteger()) + "}\n");
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

/* The message boxes shown now */
static QList<QMessageBox *> boxes()
{
    QList<QMessageBox *> list;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (auto *box = qobject_cast<QMessageBox *>(w); box && box->isVisible()) {
            list << box;
        }
    }
    return list;
}

/* A VM found running with the stand-in QEMU, its GPU with native context */
struct Running {
    QTemporaryDir runtime{QDir::tempPath() + "/vn-XXXXXX"};
    QTemporaryDir vms;
    QProcess qemu;
    std::unique_ptr<FakeQmp> qmp;
    std::unique_ptr<VmStore> store;
    Vm *vm = nullptr;

    Running()
    {
        qputenv("XDG_RUNTIME_DIR", runtime.path().toLocal8Bit());
        QDir(vms.path()).mkpath("native");
        writeFile(vms.path() + "/native/vm.args",
                  "-name Native\n-device virtio-vga-gl,blob=on,drm_native_context=on\n"
                  "-display dbus,p2p=yes\n");
        const QString run = Paths::vmRuntimeDir("native");
        qemu.start(FAKE_QEMU,
                   {"-qmp", QString("unix:%1/qmp.sock,server=on,wait=off").arg(run)});
        qemu.waitForStarted();
        writeFile(run + "/qemu.pid", QByteArray::number(qemu.processId()) + "\n");
        qmp = std::make_unique<FakeQmp>(run + "/qmp.sock");
        store = std::make_unique<VmStore>(vms.path());
        vm = store->find("native");
    }
    ~Running() { stop(); }
    void attach() { vm->runner()->attach(vm->args()); }
    void log(const QByteArray &lines)
    {
        QFile f(vm->runner()->logPath());
        if (f.open(QIODevice::WriteOnly | QIODevice::Append)) {
            f.write(lines);
        }
    }
    void stop()
    {
        qmp.reset();
        if (qemu.state() != QProcess::NotRunning) {
            qemu.kill();
            qemu.waitForFinished();
        }
    }
};

class TestUdmabufNotifier : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_tmp;

    static UdmabufWatch::Issue issue(const QString &name)
    {
        UdmabufWatch::Issue i;
        i.vmId = name.toLower();
        i.vmName = name;
        i.limitsLow = true;
        i.limits.listLimit = 1024;
        i.limits.sizeLimitMb = 64;
        return i;
    }

    void setLimits(qint64 list, qint64 sizeMb) const
    {
        writeFile(sys() + "/module/udmabuf/parameters/list_limit", QByteArray::number(list) + '\n');
        writeFile(sys() + "/module/udmabuf/parameters/size_limit_mb",
                  QByteArray::number(sizeMb) + '\n');
    }
    QString sys() const { return m_tmp.path() + "/" + QTest::currentTestFunction() + "/sys"; }
    QString devicePath() const
    {
        return m_tmp.path() + "/" + QTest::currentTestFunction() + "/udmabuf";
    }
    static bool offers(QMessageBox *box, const QString &text)
    {
        for (QAbstractButton *b : box->buttons()) {
            if (b->text() == text) {
                return true;
            }
        }
        return false;
    }

private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    void init() { QSettings(Paths::settingsPath(), QSettings::IniFormat).remove("host"); }

    void explanation()
    {
        Udmabuf::Limits now;
        now.listLimit = 1024;
        now.sizeLimitMb = 64;
        UdmabufWatch::Issue one = issue("Fedora KDE");
        one.notRaised = "you are not in the vitrine group";

        QString text = UdmabufNotifier::explanation({one}, now);
        QVERIFY(text.contains("<p>Fedora KDE: Guest windows drawn by the CPU will be copied: the "
                              "host's udmabuf limits are 1024 entries and 64 MB, below what "
                              "native context needs (16384 and 128 MB).</p>"));
        QVERIFY(text.contains("They are 1024 entries and 64 MB now. Host tuning raises them to "
                              "65536 entries and 2048 MB while native-context VMs run, but not "
                              "here: you are not in the vitrine group."));
        /* the persistent ways, exactly as they are to be typed */
        QVERIFY(text.contains("<pre>" + Udmabuf::grubbyCommand().toHtmlEscaped() + "</pre>"));
        QVERIFY(text.contains("<pre>w /sys/module/udmabuf/parameters/list_limit - - - - 65536\n"
                              "w /sys/module/udmabuf/parameters/size_limit_mb - - - - 2048</pre>"));
        QVERIFY(text.contains("/etc/tmpfiles.d/udmabuf.conf"));

        HostSettings::setEnabled(false);
        qputenv("VITRINE_HELPER", QCoreApplication::applicationFilePath().toLocal8Bit());
        text = UdmabufNotifier::explanation({one}, now);
        QVERIFY(text.contains("while native-context VMs run: it is off.</p>"));
        /* off, and no helper: turning it on would not do */
        qputenv("VITRINE_HELPER", "/nonexistent/vitrine-helper");
        text = UdmabufNotifier::explanation({one}, now);
        QVERIFY(text.contains("while native-context VMs run: it is off, and vitrine-helper is not "
                              "installed (see Installing in <a href=\""));
        QVERIFY(text.contains("docs/host-tuning.md#installing"));
        qunsetenv("VITRINE_HELPER");

        /* raised since: said so */
        Udmabuf::Limits raised = now;
        raised.listLimit = 65536;
        raised.sizeLimitMb = 2048;
        text = UdmabufNotifier::explanation({one}, raised);
        QVERIFY(text.contains("They are 65536 entries and 2048 MB now, which is enough: the "
                              "windows the guest makes from now on are not copied."));
        QVERIFY(!text.contains("Host tuning raises them"));

        /* no device: host tuning and the limits are no help, the device
           said once */
        now.deviceErrno = ENOENT;
        text = UdmabufNotifier::explanation({one}, now);
        QCOMPARE(text.count("/dev/udmabuf does not exist"), 1);
        QVERIFY(!text.contains("grubby"));
        QVERIFY(!text.contains("beyond its limits"));
        UdmabufWatch::Issue noDevice = one;
        noDevice.limits.deviceErrno = EACCES;
        now.deviceErrno = EACCES;
        text = UdmabufNotifier::explanation({noDevice}, now);
        QCOMPARE(text.count("/dev/udmabuf is not open to you: it is open to the kvm group and to "
                            "the user of the local desktop session"),
                 1);
        QVERIFY(text.contains("made through /dev/udmabuf: without it, the guest copies those "
                              "windows at each change."));
        QVERIFY(!text.contains("beyond its limits"));
        QVERIFY(!text.contains("Host tuning raises them"));

        /* refused for another reason: the limits are not blamed */
        UdmabufWatch::Issue refused;
        refused.vmId = "other";
        refused.vmName = "Other";
        refused.log.blobRefused = 3;
        text = UdmabufNotifier::explanation({refused}, raised);
        QVERIFY(text.contains("Other: Some guest windows are copied: QEMU refused their buffers, "
                              "qemu.log says why (3 refused).</p>"));
        QVERIFY(text.contains("The udmabuf limits, 65536 entries and 2048 MB, are not the reason"));
        QVERIFY(!text.contains("grubby"));
        QVERIFY(!text.contains("Host tuning raises them"));
    }

    /*
     * The warning shows while a VM has an issue, says "copied" once the
     * log shows refusals, and goes with the VM's run.  Host tuning off by
     * choice: no explanation by itself; Turn On offered where the helper is
     * installed.
     */
    void warning()
    {
        HostSettings::setEnabled(false);
        qputenv("VITRINE_HELPER", QCoreApplication::applicationFilePath().toLocal8Bit());
        setLimits(1024, 64);
        writeFile(devicePath(), QByteArray());
        Running vm;
        UdmabufWatch watch(vm.store.get(), nullptr);
        watch.setSysRoot(sys());
        watch.setDevice(devicePath());
        watch.setPollInterval(100000);
        QWidget window;
        UdmabufNotifier notifier(&watch, nullptr, &window);
        QToolButton *button = notifier.button();
        QVERIFY(button->isHidden());

        vm.attach();
        QTRY_VERIFY(!button->isHidden());
        QCOMPARE(button->text(), QString("udmabuf limits low"));
        QVERIFY(button->toolTip().startsWith("Native: Guest windows drawn by the CPU will be "
                                             "copied: the host's udmabuf limits are 1024 entries"));
        QVERIFY(button->toolTip().endsWith("\nClick for what to do."));
        /* off by choice: the button says it, nothing opens by itself */
        QTest::qWait(50);
        QVERIFY(boxes().isEmpty());
        button->click();
        QTRY_COMPARE(boxes().size(), 1);
        QMessageBox *box = boxes().first();
        QCOMPARE(box->windowTitle(), QString("udmabuf Limits Too Low"));
        QVERIFY(box->text().contains("grubby"));
        /* the limits as the watch reads them, not the host's */
        QVERIFY(box->text().contains("They are 1024 entries and 64 MB now."));
        QVERIFY(offers(box, "&Turn On Host Tuning"));
        /* Enter only closes it */
        QCOMPARE(static_cast<QAbstractButton *>(box->defaultButton()),
                 box->button(QMessageBox::Close));
        box->button(QMessageBox::Close)->click();
        QTRY_VERIFY(boxes().isEmpty());

        /* no helper: Turn On would not raise them, not offered */
        qputenv("VITRINE_HELPER", "/nonexistent/vitrine-helper");
        button->click();
        QTRY_COMPARE(boxes().size(), 1);
        QVERIFY(!offers(boxes().first(), "&Turn On Host Tuning"));
        QVERIFY(boxes().first()->text().contains("vitrine-helper is not installed"));
        boxes().first()->button(QMessageBox::Close)->click();
        QTRY_VERIFY(boxes().isEmpty());
        qunsetenv("VITRINE_HELPER");

        /* refusals: "copied" */
        vm.log("virtio_gpu_virgl_process_cmd: ctrl 0x10c, error 0x1201\n");
        watch.poll();
        QCOMPARE(button->text(), QString("Guest windows copied"));
        QVERIFY(button->toolTip().contains("Some guest windows are copied: QEMU refused their "
                                           "buffers, qemu.log says why (1 refused)."));
        vm.log("qemu-system-x86_64: warning: virtio_gpu_create_udmabuf_fd: UDMABUF_CREATE_LIST: "
               "Invalid argument (7473 entries, 32043008 bytes)\n"
               "virtio_gpu_virgl_process_cmd: ctrl 0x10c, error 0x1201\n");
        watch.poll();
        QVERIFY(button->toolTip().contains("Some guest windows are copied: the host's udmabuf "
                                           "limits are too low (1 refused)."));
        QTest::qWait(50);
        QVERIFY(boxes().isEmpty());
        /* a click: the explanation again */
        button->click();
        QTRY_COMPARE(boxes().size(), 1);
        QCOMPARE(boxes().first()->windowTitle(), QString("Guest Windows Copied"));
        boxes().first()->button(QMessageBox::Close)->click();

        /* the run ends: the warning goes */
        vm.stop();
        QTRY_VERIFY(button->isHidden());
    }

    /* Host tuning on and the limits in the way: the explanation opens by
       itself, once per run of vitrine */
    void opensByItself()
    {
        setLimits(1024, 64);
        writeFile(devicePath(), QByteArray());
        Running vm;
        UdmabufWatch watch(vm.store.get(), nullptr);
        watch.setSysRoot(sys());
        watch.setDevice(devicePath());
        watch.setPollInterval(100000);
        QWidget window;
        UdmabufNotifier notifier(&watch, nullptr, &window);
        vm.attach();
        QTRY_COMPARE(boxes().size(), 1);
        QMessageBox *box = boxes().first();
        QCOMPARE(box->windowTitle(), QString("udmabuf Limits Too Low"));
        QVERIFY(!offers(box, "&Turn On Host Tuning"));
        box->button(QMessageBox::Close)->click();
        QTRY_VERIFY(boxes().isEmpty());
        /* refusals later: not again */
        vm.log("qemu-system-x86_64: warning: virtio_gpu_create_udmabuf_fd: UDMABUF_CREATE_LIST: "
               "Invalid argument (7473 entries, 32043008 bytes)\n"
               "virtio_gpu_virgl_process_cmd: ctrl 0x10c, error 0x1201\n");
        watch.poll();
        QCOMPARE(notifier.button()->text(), QString("Guest windows copied"));
        QTest::qWait(50);
        QVERIFY(boxes().isEmpty());
    }

    /*
     * Refusals the limits did not cause, or that they no longer would
     * (in the log of a VM found running, raised since): the warning, not
     * the explanation by itself
     */
    void quietWhenLimitsFine()
    {
        setLimits(65536, 2048);
        writeFile(devicePath(), QByteArray());
        Running vm;
        vm.log("qemu-system-x86_64: warning: virtio_gpu_create_udmabuf_fd: UDMABUF_CREATE_LIST: "
               "Invalid argument (7473 entries, 32043008 bytes)\n"
               "virtio_gpu_virgl_process_cmd: ctrl 0x10c, error 0x1201\n");
        UdmabufWatch watch(vm.store.get(), nullptr);
        watch.setSysRoot(sys());
        watch.setDevice(devicePath());
        watch.setPollInterval(100000);
        QWidget window;
        UdmabufNotifier notifier(&watch, nullptr, &window);
        vm.attach();
        QTRY_VERIFY(!notifier.button()->isHidden());
        QCOMPARE(notifier.button()->text(), QString("Guest windows copied"));
        QTest::qWait(50);
        QVERIFY(boxes().isEmpty());
        notifier.button()->click();
        QTRY_COMPARE(boxes().size(), 1);
        QVERIFY(boxes().first()->text().contains("are not the reason"));
        QVERIFY(!offers(boxes().first(), "&Turn On Host Tuning"));
        boxes().first()->button(QMessageBox::Close)->click();
        QTRY_VERIFY(boxes().isEmpty());
    }

    /* /dev/udmabuf not open to the user: said as such, not as limits */
    void deviceUnavailable()
    {
        if (geteuid() == 0) {
            QSKIP("root opens it anyway");
        }
        setLimits(65536, 2048);
        writeFile(devicePath(), QByteArray());
        QFile::setPermissions(devicePath(), QFileDevice::ReadOwner);
        Running vm;
        UdmabufWatch watch(vm.store.get(), nullptr);
        watch.setSysRoot(sys());
        watch.setDevice(devicePath());
        watch.setPollInterval(100000);
        QWidget window;
        UdmabufNotifier notifier(&watch, nullptr, &window);
        vm.attach();
        QTRY_COMPARE(boxes().size(), 1);
        QCOMPARE(notifier.button()->text(), QString("udmabuf unavailable"));
        QMessageBox *box = boxes().first();
        QCOMPARE(box->windowTitle(), QString("udmabuf Not Available"));
        QCOMPARE(box->text().count("/dev/udmabuf is not open to you: it is open to the kvm group"),
                 1);
        QVERIFY(!box->text().contains("limits"));
        QVERIFY(!offers(box, "&Turn On Host Tuning"));
        box->button(QMessageBox::Close)->click();
        QTRY_VERIFY(boxes().isEmpty());
    }
};

QTEST_MAIN(TestUdmabufNotifier)
#include "test_udmabufnotifier.moc"
