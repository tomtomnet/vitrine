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
        text = UdmabufNotifier::explanation({one}, now);
        QVERIFY(text.contains("while native-context VMs run: it is off."));

        /* raised since: said so */
        Udmabuf::Limits raised = now;
        raised.listLimit = 65536;
        raised.sizeLimitMb = 2048;
        text = UdmabufNotifier::explanation({one}, raised);
        QVERIFY(text.contains("They are 65536 entries and 2048 MB now, which is enough: the "
                              "windows the guest makes from now on are not copied."));
        QVERIFY(!text.contains("Host tuning raises them"));

        /* no device: host tuning and the limits are no help */
        now.deviceErrno = ENOENT;
        text = UdmabufNotifier::explanation({one}, now);
        QVERIFY(text.contains("/dev/udmabuf does not exist"));
        QVERIFY(!text.contains("grubby"));
    }

    /*
     * The warning shows while a VM has an issue, opens its explanation by
     * itself once per run, says "copied" once the log shows refusals, and
     * goes with the VM's run
     */
    void warning()
    {
        HostSettings::setEnabled(false);
        QTemporaryDir runtime(QDir::tempPath() + "/vn-XXXXXX");
        QTemporaryDir vms;
        QVERIFY(runtime.isValid() && vms.isValid());
        qputenv("XDG_RUNTIME_DIR", runtime.path().toLocal8Bit());
        const QString sys = m_tmp.path() + "/sys", device = m_tmp.path() + "/udmabuf";
        writeFile(sys + "/module/udmabuf/parameters/list_limit", "1024\n");
        writeFile(sys + "/module/udmabuf/parameters/size_limit_mb", "64\n");
        writeFile(device, QByteArray());
        QDir(vms.path()).mkpath("native");
        writeFile(vms.path() + "/native/vm.args",
                  "-name Native\n-device virtio-vga-gl,blob=on,drm_native_context=on\n"
                  "-display dbus,p2p=yes\n");
        const QString run = Paths::vmRuntimeDir("native");
        QProcess qemu;
        qemu.start(FAKE_QEMU, {"-qmp", QString("unix:%1/qmp.sock,server=on,wait=off").arg(run)});
        QVERIFY(qemu.waitForStarted());
        writeFile(run + "/qemu.pid", QByteArray::number(qemu.processId()) + "\n");
        auto qmp = std::make_unique<FakeQmp>(run + "/qmp.sock");

        VmStore store(vms.path());
        UdmabufWatch watch(&store, nullptr);
        watch.setSysRoot(sys);
        watch.setDevice(device);
        watch.setPollInterval(100000);
        QWidget window;
        UdmabufNotifier notifier(&watch, nullptr, &window);
        QToolButton *button = notifier.button();
        QVERIFY(button->isHidden());

        Vm *vm = store.find("native");
        vm->runner()->attach(vm->args());
        QTRY_VERIFY(!button->isHidden());
        QCOMPARE(button->text(), QString("udmabuf limits low"));
        QVERIFY(button->toolTip().startsWith("Native: Guest windows drawn by the CPU will be "
                                             "copied: the host's udmabuf limits are 1024 entries"));
        QVERIFY(button->toolTip().endsWith("\nClick for what to do."));
        /* the first issue of the run: its explanation, by itself */
        QTRY_COMPARE(boxes().size(), 1);
        QMessageBox *box = boxes().first();
        QCOMPARE(box->windowTitle(), QString("udmabuf Limits Too Low"));
        QVERIFY(box->text().contains("grubby"));
        /* the limits as the watch reads them, not the host's */
        QVERIFY(box->text().contains("They are 1024 entries and 64 MB now."));
        /* tuning is off here: the box offers to turn it on */
        bool turnOn = false;
        for (QAbstractButton *b : box->buttons()) {
            turnOn |= b->text() == "&Turn On Host Tuning";
        }
        QVERIFY(turnOn);
        /* Enter only closes it */
        QCOMPARE(static_cast<QAbstractButton *>(box->defaultButton()),
                 box->button(QMessageBox::Close));
        box->button(QMessageBox::Close)->click();
        QTRY_VERIFY(boxes().isEmpty());

        /* refusals: "copied", and no box by itself again */
        QFile log(vm->runner()->logPath());
        QVERIFY(log.open(QIODevice::WriteOnly | QIODevice::Append));
        log.write("virtio_gpu_virgl_process_cmd: ctrl 0x10c, error 0x1201\n");
        log.close();
        watch.poll();
        QCOMPARE(button->text(), QString("Guest windows copied"));
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
        qmp.reset();
        qemu.kill();
        qemu.waitForFinished();
        QTRY_VERIFY(button->isHidden());
    }
};

QTEST_MAIN(TestUdmabufNotifier)
#include "test_udmabufnotifier.moc"
