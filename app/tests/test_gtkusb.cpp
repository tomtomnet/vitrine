// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef Q_MOC_RUN
#include "gtk/gtkbase.h"
#endif

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

#include "gtkdisplay.h"

#ifndef Q_MOC_RUN
#include "core/paths.h"
#include "core/usbguard.h"
#include "core/usbhotplug.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "fakeusbqemu.h"
#include "gtk/gtkloop.h"
#include "gtk/notices.h"
#include "gtk/usbdialog.h"
#include "ui/usbchoices.h"
#endif

/*
 * The GTK app's USB devices dialog, on a display no one sees, with a fake
 * host (its sysfs and /dev) and stand-in QEMUs answering QMP as QEMU does:
 * never this computer's devices.  The devices of a running VM with their
 * states, a VM without a USB controller, giving a device and taking it
 * back, Keep for the Next Starts, the question before a keyboard goes, a
 * device plugged out, a device another VM has, the notice of a program of
 * this computer holding one (a fake /proc), the VM stopping.
 */

static bool writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

static QString readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

/* The QEMU of a VM found running: a stand-in with the runner's -qmp, its pid file, its monitor */
class StandIn
{
public:
    explicit StandIn(const QString &id)
    {
        const QString run = Paths::vmRuntimeDir(id);
        QDir().mkpath(run);
        m_process.start(FAKE_QEMU,
                        {"-qmp", QString("unix:%1/qmp.sock,server=on,wait=off").arg(run)});
        m_process.waitForStarted();
        QElapsedTimer clock;
        clock.start();
        while (!readFile(QString("/proc/%1/cmdline").arg(m_process.processId())).contains("-qmp") &&
               clock.elapsed() < 5000) {
            QTest::qWait(5);
        }
        writeFile(run + "/qemu.pid", QByteArray::number(m_process.processId()) + "\n");
        qemu = std::make_unique<FakeUsbQemu>(run + "/qmp.sock");
    }
    ~StandIn()
    {
        qemu.reset();
        m_process.kill();
        m_process.waitForFinished();
    }
    qint64 pid() const { return m_process.processId(); }

    std::unique_ptr<FakeUsbQemu> qemu;

private:
    QProcess m_process;
};

class TestGtkUsb : public QObject
{
    Q_OBJECT

    QProcess m_broadway;
    GTestDBus *m_bus = nullptr;
    GtkLoop *m_loop = nullptr;
    QTemporaryDir m_tmp;
    QString m_sys, m_dev, m_bin;
    std::unique_ptr<VmStore> m_store;
    QHash<QString, std::shared_ptr<StandIn>> m_qemus;
    QList<AdwDialog *> m_presented;

    Gtk4::Dialog::Presenter presenter()
    {
        return [this](AdwDialog *dialog) { m_presented << ADW_DIALOG(g_object_ref_sink(dialog)); };
    }
    AdwAlertDialog *alert(const QString &heading) const
    {
        for (qsizetype i = m_presented.size() - 1; i >= 0; i--) {
            if (ADW_IS_ALERT_DIALOG(m_presented[i]) &&
                QString::fromUtf8(adw_alert_dialog_get_heading(ADW_ALERT_DIALOG(m_presented[i]))) ==
                    heading) {
                return ADW_ALERT_DIALOG(m_presented[i]);
            }
        }
        return nullptr;
    }

    void plug(const QString &name, int bus, int address, const QByteArray &vendor,
              const QByteArray &product, const QByteArray &manufacturer,
              const QByteArray &productName, const QByteArray &interfaceClass,
              const QByteArray &deviceClass = "00")
    {
        const QString dir = m_sys + "/devices/" + name;
        writeFile(dir + "/busnum", QByteArray::number(bus) + "\n");
        writeFile(dir + "/devnum", QByteArray::number(address) + "\n");
        writeFile(dir + "/idVendor", vendor + "\n");
        writeFile(dir + "/idProduct", product + "\n");
        writeFile(dir + "/bDeviceClass", deviceClass + "\n");
        if (!manufacturer.isEmpty()) {
            writeFile(dir + "/manufacturer", manufacturer + "\n");
            writeFile(dir + "/product", productName + "\n");
        }
        writeFile(dir + ":1.0/bInterfaceClass", interfaceClass + "\n");
        QDir().mkpath(m_sys + "/bus/usb/devices");
        QFile::link(dir, m_sys + "/bus/usb/devices/" + name);
        QFile::link(dir + ":1.0", m_sys + "/bus/usb/devices/" + name + ":1.0");
        writeFile(m_dev + QString::asprintf("/bus/usb/%03d/%03d", bus, address), {});
    }
    void unplug(const QString &name)
    {
        QFile::remove(m_sys + "/bus/usb/devices/" + name);
        QFile::remove(m_sys + "/bus/usb/devices/" + name + ":1.0");
    }
    void addVm(const QString &id, const QByteArray &args, bool running)
    {
        writeFile(m_tmp.filePath("vms/" + id + "/vm.args"), "-name " + id.toLatin1() + "\n" + args);
        if (running) {
            m_qemus.insert(id, std::make_shared<StandIn>(id));
        }
    }
    FakeUsbQemu *qemu(const QString &id) const { return m_qemus.value(id)->qemu.get(); }
    Vm *vm(const QString &id) const { return m_store->find(id); }
    bool has(const QString &id, const QString &port) const
    {
        for (const UsbDevice &d : UsbHotplug::hostDevices()) {
            if (d.port == port) {
                return UsbHotplug::of(vm(id)->runner())->has(d);
            }
        }
        return false;
    }
    /* An alert's body, and the explanation under it (a long one's) */
    static QString body(AdwAlertDialog *alert)
    {
        QString text = QString::fromUtf8(adw_alert_dialog_get_body(alert));
        if (GtkWidget *extra = adw_alert_dialog_get_extra_child(alert); extra && GTK_IS_LABEL(extra)) {
            text += QString::fromUtf8(gtk_label_get_label(GTK_LABEL(extra)));
        }
        return text;
    }
    /* The dialog of @id, once it knows what the VM has */
    std::unique_ptr<UsbDialog> open(const QString &id)
    {
        auto dialog = std::make_unique<UsbDialog>(m_store.get(), vm(id), presenter(), nullptr,
                                                  [](Vm *) {});
        [&]() {
            QVERIFY(QTest::qWaitFor([&dialog]() {
                const QList<UsbChoices::Entry> shown = dialog->shown();
                return !shown.isEmpty() && !shown.first().text.startsWith("Reading");
            }, 5000));
        }();
        return dialog;
    }
    /* The entry of @dialog whose text starts with @start */
    static int entry(const UsbDialog &dialog, const QString &start)
    {
        const QList<UsbChoices::Entry> shown = dialog.shown();
        for (qsizetype i = 0; i < shown.size(); i++) {
            if (shown[i].text.startsWith(start)) {
                return int(i);
            }
        }
        return -1;
    }

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
        m_sys = m_tmp.filePath("sys");
        m_dev = m_tmp.filePath("dev");
        m_bin = m_tmp.filePath("bin");
        /* pkexec and setfacl: none in PATH, nothing here can reach polkit */
        QDir().mkpath(m_bin);
        qputenv("PATH", QFile::encodeName(m_bin));
        QVERIFY(QStandardPaths::findExecutable("pkexec").isEmpty());
        UsbHotplug::setRoots(m_sys, m_dev);
        UsbHotplug::setWatchInterval(100);
        UsbDialog::setRefreshInterval(100);

        plug("99-1", 99, 2, "0781", "5567", "SanDisk", "Cruzer Blade", "08");
        plug("99-2", 99, 3, "046d", "c31c", "Logitech", "Keyboard K120 & Co", "03");
        plug("99-3", 99, 4, "dead", "beef", {}, {}, "ff");
        plug("99-4", 99, 5, "8087", "0026", "Intel", "Bluetooth", "e0", "e0");
        plug("99-5", 99, 6, "05e3", "0610", "Genesys", "Hub", "09", "09");

        addVm("alpha", "-device qemu-xhci\n", true);
        /* it has dead:beef by its arguments */
        addVm("beta", "-device qemu-xhci\n-device usb-host,vendorid=0xdead,productid=0xbeef\n",
              true);
        addVm("gamma", "-machine q35\n", true);
        qemu("alpha")->plugged = {"99/2", "99/3"};
        FakeUsbQemu::Device byIds;
        byIds.path = "/machine/peripheral-anon/device[1]";
        byIds.vendor = 0xdead;
        byIds.product = 0xbeef;
        byIds.attached = true;
        qemu("beta")->devices = {byIds};
        qemu("gamma")->controller = false;

        m_store = std::make_unique<VmStore>(m_tmp.filePath("vms"));
        QCOMPARE(m_store->vms().size(), 3);
        for (Vm *each : m_store->vms()) {
            each->runner()->attach(each->args());
        }
        for (const char *id : {"alpha", "beta", "gamma"}) {
            QTRY_COMPARE(vm(id)->runner()->state(), VmRunner::State::Running);
            QTRY_VERIFY(UsbHotplug::of(vm(id)->runner())->isKnown());
        }
    }

    void cleanupTestCase()
    {
        m_store.reset();
        m_qemus.clear();
        UsbHotplug::setRoots("/sys", "/dev");
        delete m_loop;
        GtkDisplay::down(m_broadway, m_bus);
    }

    void cleanup()
    {
        for (AdwDialog *dialog : std::as_const(m_presented)) {
            g_object_unref(dialog);
        }
        m_presented.clear();
    }

    /* The devices of alpha: names, what they say, who may have them; Keep; Settings */
    void entries()
    {
        auto dialog = open("alpha");
        QStringList texts;
        for (const UsbChoices::Entry &e : dialog->shown()) {
            texts << e.text + (e.right.isEmpty() ? QString() : " | " + e.right);
        }
        QCOMPARE(texts, QStringList({"SanDisk Cruzer Blade | 0781:5567",
                                     "Logitech Keyboard K120 & Co | 046d:c31c",
                                     "dead:beef | in beta", "Intel Bluetooth | 8087:0026",
                                     "&Keep for the Next Starts", "USB &Settings…"}));
        /* the rows: a switch each, beta's insensitive, saying why */
        GtkWidget *stick = dialog->row(entry(*dialog, "SanDisk"));
        QVERIFY(ADW_IS_SWITCH_ROW(stick));
        QVERIFY(!adw_switch_row_get_active(ADW_SWITCH_ROW(stick)));
        QVERIFY(gtk_widget_get_sensitive(stick));
        QVERIFY(QString::fromUtf8(gtk_widget_get_tooltip_text(stick)).contains("Bus 99, address 2"));
        GtkWidget *keyboard = dialog->row(entry(*dialog, "Logitech"));
        QVERIFY(QString::fromUtf8(adw_action_row_get_subtitle(ADW_ACTION_ROW(keyboard)))
                    .contains("input device"));
        GtkWidget *other = dialog->row(entry(*dialog, "dead:beef"));
        QVERIFY(!gtk_widget_get_sensitive(other));
        QVERIFY(QString::fromUtf8(adw_action_row_get_subtitle(ADW_ACTION_ROW(other)))
                    .contains("beta has it"));
        GtkWidget *keep = dialog->row(entry(*dialog, "&Keep"));
        QVERIFY(gtk_widget_get_visible(keep));
        QVERIFY(!gtk_widget_get_sensitive(keep));
        QCOMPARE(QString::fromUtf8(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(keep))),
                 QString("_Keep for the Next Starts"));

        /* in beta's, on */
        auto beta = open("beta");
        QVERIFY(adw_switch_row_get_active(ADW_SWITCH_ROW(beta->row(entry(*beta, "dead:beef")))));
    }

    /* No USB controller: said, every device insensitive */
    void noController()
    {
        auto dialog = open("gamma");
        const QList<UsbChoices::Entry> shown = dialog->shown();
        QCOMPARE(shown.first().text, QString("This VM has no USB controller"));
        QVERIFY(QString::fromUtf8(adw_action_row_get_subtitle(ADW_ACTION_ROW(dialog->row(0))))
                    .contains("USB Devices settings"));
        QVERIFY(!gtk_widget_get_sensitive(dialog->row(entry(*dialog, "SanDisk"))));
    }

    /* Given, kept for the next starts, taken back, no longer kept */
    void attachDetachKeep()
    {
        auto dialog = open("alpha");
        const int stick = dialog->shown()[entry(*dialog, "SanDisk")].device;

        dialog->toggle(stick, true);
        QTRY_COMPARE(dialog->lastMessage(), QString("SanDisk Cruzer Blade attached to alpha"));
        QCOMPARE(qemu("alpha")->executed("device_add").size(), 1);
        QTRY_VERIFY(has("alpha", "99-1"));
        QTRY_VERIFY(adw_switch_row_get_active(ADW_SWITCH_ROW(dialog->row(entry(*dialog, "SanDisk")))));

        QTRY_VERIFY(gtk_widget_get_sensitive(dialog->row(entry(*dialog, "&Keep"))));
        dialog->keep();
        QCOMPARE(dialog->lastMessage(), QString("alpha gets these USB devices at its next starts too"));
        QVERIFY(readFile(vm("alpha")->argsPath())
                    .contains("-device usb-host,vendorid=0x0781,productid=0x5567"));
        QTRY_VERIFY(!gtk_widget_get_sensitive(dialog->row(entry(*dialog, "&Keep"))));

        dialog->toggle(stick, false);
        QTRY_COMPARE(dialog->lastMessage(), QString("SanDisk Cruzer Blade is back on this computer"));
        QCOMPARE(qemu("alpha")->executed("device_del").size(), 1);
        QTRY_VERIFY(!has("alpha", "99-1"));
        QTRY_VERIFY(gtk_widget_get_sensitive(dialog->row(entry(*dialog, "&Keep"))));
        dialog->keep();
        QVERIFY(!readFile(vm("alpha")->argsPath()).contains("usb-host"));
        QVERIFY(readFile(vm("alpha")->argsPath()).contains("qemu-xhci"));
    }

    /* A keyboard: not without asking; Cancel gives nothing, Attach gives it */
    void inputAsks()
    {
        auto dialog = open("alpha");
        const int keyboard = dialog->shown()[entry(*dialog, "Logitech")].device;
        const qsizetype before = qemu("alpha")->executed("device_add").size();

        dialog->toggle(keyboard, true);
        AdwAlertDialog *question = alert("Give Logitech Keyboard K120 & Co to alpha?");
        QVERIFY(question);
        QCOMPARE(QString::fromUtf8(adw_alert_dialog_get_default_response(question)), QString("cancel"));
        g_signal_emit_by_name(question, "response", "cancel");
        QTest::qWait(200);
        QCOMPARE(qemu("alpha")->executed("device_add").size(), before);
        QVERIFY(!adw_switch_row_get_active(ADW_SWITCH_ROW(dialog->row(entry(*dialog, "Logitech")))));

        dialog->toggle(keyboard, true);
        g_signal_emit_by_name(alert("Give Logitech Keyboard K120 & Co to alpha?"), "response", "attach");
        QTRY_COMPARE(qemu("alpha")->executed("device_add").size(), before + 1);
        QTRY_VERIFY(has("alpha", "99-2"));
        /* given back without asking */
        dialog->toggle(keyboard, false);
        QTRY_VERIFY(!has("alpha", "99-2"));
    }

    /* Plugged out while it shows: it goes */
    void unplugged()
    {
        auto dialog = open("alpha");
        QVERIFY(entry(*dialog, "Intel") >= 0);
        unplug("99-4");
        QTRY_VERIFY(entry(*dialog, "Intel") < 0);
        QVERIFY(entry(*dialog, "SanDisk") >= 0);
    }

    /* beta's by its rule, given to alpha all the same (the switch's own path): refused, naming beta */
    void attachRefused()
    {
        UsbGuard guard(m_store.get());
        auto dialog = open("alpha");
        const qsizetype before = qemu("alpha")->executed("device_add").size();

        dialog->toggle(dialog->shown()[entry(*dialog, "dead:beef")].device, true);
        QTRY_VERIFY(alert("dead:beef was not given to alpha."));
        QVERIFY(body(alert("dead:beef was not given to alpha."))
                    .contains("beta has it: take it back there first."));
        QCOMPARE(qemu("alpha")->executed("device_add").size(), before);
    }

    /* Two identical devices, which beta's rule takes either of: said so beside each */
    void identicalDevices()
    {
        plug("99-6", 99, 7, "dead", "beef", {}, {}, "ff");
        {
            auto dialog = open("alpha");
            int rows = 0;
            for (qsizetype i = 0; i < dialog->shown().size(); i++) {
                if (dialog->shown()[i].text != "dead:beef") {
                    continue;
                }
                rows++;
                GtkWidget *row = dialog->row(int(i));
                QVERIFY(!gtk_widget_get_sensitive(row));
                QVERIFY(QString::fromUtf8(adw_action_row_get_subtitle(ADW_ACTION_ROW(row)))
                            .contains("beta passes devices with these IDs through: its QEMU may "
                                      "take this one too."));
            }
            QCOMPARE(rows, 2);
        }
        unplug("99-6");
    }

    /* adb holds beta's device: the notice names it, its explanation has the fix; gone as it lets go */
    void heldNotice()
    {
        const QString proc = m_tmp.filePath("proc");
        QDir().mkpath(proc + "/4190021/fd");
        QVERIFY(writeFile(proc + "/4190021/cmdline", QByteArray("adb\0fork-server\0server\0", 23)));
        QVERIFY(writeFile(proc + "/4190021/comm", "adb\n"));
        QVERIFY(QFile::link(m_dev + "/bus/usb/099/004", proc + "/4190021/fd/5"));
        /* beta's QEMU holds none of it */
        QDir().mkpath(proc + QString("/%1/fd").arg(m_qemus.value("beta")->pid()));

        UsbGuard guard(m_store.get());
        guard.setProcRoot(proc);
        guard.setDelays(20, 100);
        GtkWidget *window = gtk_window_new();
        {
            Notices notices(nullptr, m_store.get(), GTK_WINDOW(window), presenter());
            guard.scanNow();
            QTRY_VERIFY(notices.shown().contains(Notices::Kind::UsbHeld));
            QCOMPARE(notices.title(Notices::Kind::UsbHeld),
                     QString("USB Device in Use on This Computer"));
            QCOMPARE(notices.text(Notices::Kind::UsbHeld),
                     QString("adb (pid 4190021) holds dead:beef, which beta passes through."));
            QVERIFY(gtk_widget_get_visible(notices.button()));
            QVERIFY(QString::fromUtf8(gtk_widget_get_tooltip_text(notices.button()))
                        .contains("USB Device in Use on This Computer"));

            notices.explain(Notices::Kind::UsbHeld);
            AdwAlertDialog *explained = alert("USB Device in Use on This Computer");
            QVERIFY(explained);
            QVERIFY(body(explained).contains("adb kill-server"));
            QVERIFY(body(explained).contains("kill 4190021"));

            QDir(proc + "/4190021").removeRecursively();
            QTRY_VERIFY(!notices.shown().contains(Notices::Kind::UsbHeld));
        }
        gtk_window_destroy(GTK_WINDOW(window));
    }

    /* The VM stops while it shows */
    void stops()
    {
        auto dialog = open("gamma");
        m_qemus.remove("gamma");
        QTRY_COMPARE(vm("gamma")->runner()->state(), VmRunner::State::Stopped);
        QTRY_COMPARE(dialog->shown().first().text, QString("The VM is not running"));
        /* nothing to keep from a VM that does not run */
        QCOMPARE(entry(*dialog, "&Keep"), -1);
    }
};

int main(int argc, char **argv)
{
    QTemporaryDir runtime(QDir::tempPath() + "/vitrine-gtkusb-XXXXXX");
    if (!GtkDisplay::prepare(runtime)) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    TestGtkUsb test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_gtkusb.moc"
