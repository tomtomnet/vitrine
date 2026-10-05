// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The USB Devices menu, offscreen: VMs found running (stand-in QEMUs whose
 * monitor answers as QEMU does, fakeusbqemu.h), the host's devices a fake
 * sysfs and /dev on bus 99, and pkexec and setfacl stand-ins as the only
 * programs in PATH: no device of this computer is ever attached, and no
 * polkit dialog can come up.
 */
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>

#include <memory>

#include <unistd.h>

#include "core/paths.h"
#include "core/usbhotplug.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "fakeusbqemu.h"
#include "ui/usbaccess.h"
#include "ui/usbmenu.h"

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

    std::unique_ptr<FakeUsbQemu> qemu;

private:
    QProcess m_process;
};

class TestUsbMenu : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_tmp;
    QString m_sys, m_dev, m_bin;
    std::unique_ptr<VmStore> m_store;
    QHash<QString, std::shared_ptr<StandIn>> m_qemus;
    std::unique_ptr<QWidget> m_window;
    UsbMenu *m_menu = nullptr;

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
        writeFile(node(bus, address), {});
    }
    void unplug(const QString &name)
    {
        QFile::remove(m_sys + "/bus/usb/devices/" + name);
        QFile::remove(m_sys + "/bus/usb/devices/" + name + ":1.0");
    }
    QString node(int bus, int address) const
    {
        return m_dev + QString::asprintf("/bus/usb/%03d/%03d", bus, address);
    }
    /* A VM of the store, found running on a stand-in, or stopped */
    void addVm(const QString &id, const QByteArray &args, bool running)
    {
        writeFile(m_tmp.filePath("vms/" + id + "/vm.args"), "-name " + id.toLatin1() + "\n" + args);
        if (running) {
            m_qemus.insert(id, std::make_shared<StandIn>(id));
        }
    }
    FakeUsbQemu *qemu(const QString &id) const { return m_qemus.value(id)->qemu.get(); }
    Vm *vm(const QString &id) const { return m_store->find(id); }
    /* Whether the VM @id has the host's device at @port, as last read */
    bool has(const QString &id, const QString &port) const
    {
        for (const UsbDevice &d : UsbHotplug::hostDevices()) {
            if (d.port == port) {
                return UsbHotplug::of(vm(id)->runner())->has(d);
            }
        }
        return false;
    }

    /* The menu shown for @id, once it knows what the VM has */
    void open(const QString &id)
    {
        m_menu->hide();
        m_menu->setVm(vm(id));
        m_menu->popup(QPoint(0, 0));
        QVERIFY(QTest::qWaitFor([this]() {
            const QList<QAction *> list = m_menu->actions();
            return !list.isEmpty() && !list.first()->text().startsWith("Reading");
        }, 5000));
    }
    QAction *entry(const QString &start) const
    {
        for (QAction *a : m_menu->actions()) {
            if (a->text().startsWith(start)) {
                return a;
            }
        }
        return nullptr;
    }
    QStringList texts() const
    {
        QStringList list;
        for (QAction *a : m_menu->actions()) {
            list << (a->isSeparator() ? "--" : a->text());
        }
        return list;
    }
    /* Clicks @button of the next message box to come up */
    void answer(const QString &button)
    {
        QTimer::singleShot(0, this, [this, button]() { clickWhenShown(button, 50); });
    }
    void clickWhenShown(const QString &button, int tries)
    {
        for (QWidget *w : QApplication::topLevelWidgets()) {
            auto *box = qobject_cast<QMessageBox *>(w);
            if (!box || !box->isVisible()) {
                continue;
            }
            for (QAbstractButton *b : box->buttons()) {
                if (b->text().remove('&') == button) {
                    b->click();
                    return;
                }
            }
        }
        if (tries > 0) {
            QTimer::singleShot(50, this, [this, button, tries]() { clickWhenShown(button, tries - 1); });
        }
    }
    /* The text of the message box shown, closed; empty if none came */
    QString takeMessageBox()
    {
        QString text;
        const bool shown = QTest::qWaitFor([&text]() {
            for (QWidget *w : QApplication::topLevelWidgets()) {
                auto *box = qobject_cast<QMessageBox *>(w);
                if (box && box->isVisible() && box->windowTitle() == "USB Devices") {
                    text = box->text() + '\n' + box->informativeText();
                    box->close();
                    return true;
                }
            }
            return false;
        }, 5000);
        return shown ? text : QString();
    }
    /* A shell script in the PATH of the test */
    void program(const QString &name, const QByteArray &body)
    {
        const QString path = m_bin + '/' + name;
        writeFile(path, "#!/bin/sh\n" + body + "\n");
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                        QFileDevice::ExeOwner);
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        m_sys = m_tmp.filePath("sys");
        m_dev = m_tmp.filePath("dev");
        m_bin = m_tmp.filePath("bin");
        /*
         * pkexec and setfacl: stand-ins, alone in PATH, so that nothing
         * here can reach polkit, whose dialog would come up on the desktop
         */
        QDir().mkpath(m_bin);
        qputenv("PATH", QFile::encodeName(m_bin));
        QVERIFY(QStandardPaths::findExecutable("pkexec").isEmpty());
        UsbHotplug::setRoots(m_sys, m_dev);
        UsbHotplug::setWatchInterval(100);
        UsbMenu::setRefreshInterval(100);

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
        addVm("delta", "-device qemu-xhci\n", false);
        qemu("alpha")->plugged = {"99/2", "99/3"};
        FakeUsbQemu::Device byIds;
        byIds.path = "/machine/peripheral-anon/device[1]";
        byIds.vendor = 0xdead;
        byIds.product = 0xbeef;
        byIds.attached = true;
        qemu("beta")->devices = {byIds};
        qemu("gamma")->controller = false;

        m_store = std::make_unique<VmStore>(m_tmp.filePath("vms"));
        QCOMPARE(m_store->vms().size(), 4);
        m_window = std::make_unique<QWidget>();
        m_menu = new UsbMenu(m_store.get(), m_window.get());
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
        m_menu = nullptr;
        m_window.reset();
        m_store.reset();
        m_qemus.clear();
        UsbHotplug::setRoots("/sys", "/dev");
    }

    /* For a VM that runs, or is paused */
    void enabled()
    {
        m_menu->setVm(nullptr);
        QVERIFY(!m_menu->menuAction()->isEnabled());
        m_menu->setVm(vm("delta"));
        QVERIFY(!m_menu->menuAction()->isEnabled());
        m_menu->setVm(vm("alpha"));
        QVERIFY(m_menu->menuAction()->isEnabled());

        /* a button on a toolbar that opens it */
        QToolBar toolbar;
        QToolButton *button = m_menu->addTo(&toolbar);
        QVERIFY(button);
        QCOMPARE(button->popupMode(), QToolButton::InstantPopup);
        QCOMPARE(button->defaultAction(), m_menu->menuAction());
        QCOMPARE(button->defaultAction()->menu(), m_menu);
    }

    void entries()
    {
        open("alpha");
        QCOMPARE(texts(), QStringList({"SanDisk Cruzer Blade\t0781:5567",
                                       "Logitech Keyboard K120 && Co\t046d:c31c",
                                       "dead:beef\tin beta",
                                       "Intel Bluetooth\t8087:0026",
                                       "--",
                                       "&Keep for the Next Starts",
                                       "--",
                                       "USB &Settings…"}));
        QAction *stick = entry("SanDisk");
        QVERIFY(stick->isCheckable());
        QVERIFY(!stick->isChecked());
        QVERIFY(stick->isEnabled());
        QVERIFY(stick->toolTip().contains("Bus 99, address 2, port 99-1"));
        QVERIFY(entry("Logitech")->toolTip().contains("input device"));
        QVERIFY(entry("Intel")->toolTip().contains("wireless adapter"));
        /* another VM has it */
        QAction *other = entry("dead:beef");
        QVERIFY(!other->isEnabled());
        QVERIFY(other->toolTip().contains("beta has it"));
        /* the next starts get what the VM has now */
        QVERIFY(!entry("&Keep")->isEnabled());
        QVERIFY(entry("USB &Settings")->isEnabled());

        /* in beta's menu, checked */
        open("beta");
        QVERIFY(entry("dead:beef")->isChecked());
        QVERIFY(entry("dead:beef")->isEnabled());
        /* no name: its IDs, once */
        QCOMPARE(entry("dead:beef")->text(), "dead:beef");

        Vm *asked = nullptr;
        const QMetaObject::Connection settings =
            connect(m_menu, &UsbMenu::settingsRequested, this, [&asked](Vm *v) { asked = v; });
        entry("USB &Settings")->trigger();
        QTRY_COMPARE(asked, vm("beta"));
        disconnect(settings);
    }

    void noController()
    {
        open("gamma");
        QAction *first = m_menu->actions().first();
        QCOMPARE(first->text(), "This VM has no USB controller");
        QVERIFY(!first->isEnabled());
        QVERIFY(first->toolTip().contains("USB Devices settings"));
        QVERIFY(!entry("SanDisk")->isEnabled());
    }

    void attachDetachKeep()
    {
        QSignalSpy messages(m_menu, &UsbMenu::message);
        open("alpha");
        entry("SanDisk")->trigger();
        QTRY_COMPARE(messages.size(), 1);
        QCOMPARE(messages.first().first().toString(), "SanDisk Cruzer Blade attached to alpha");
        QCOMPARE(qemu("alpha")->executed("device_add").size(), 1);
        QCOMPARE(qemu("alpha")->executed("device_add").first()["arguments"].toObject(),
                 QJsonObject({{"driver", "usb-host"}, {"id", "vitrine-usb-99-2-0781-5567"},
                              {"hostbus", 99}, {"hostaddr", 2}}));
        QTRY_VERIFY(has("alpha", "99-1"));
        /* a checkable action checks itself as it is triggered: a menu shown anew */
        open("alpha");
        QVERIFY(entry("SanDisk")->isChecked());
        QVERIFY(entry("SanDisk")->isEnabled());

        /* kept: its arguments say so, for the next starts */
        QVERIFY(entry("&Keep")->isEnabled());
        entry("&Keep")->trigger();
        QTRY_COMPARE(messages.size(), 2);
        QCOMPARE(messages.last().first().toString(),
                 "alpha gets these USB devices at its next starts too");
        QVERIFY(readFile(vm("alpha")->argsPath())
                    .contains("-device usb-host,vendorid=0x0781,productid=0x5567"));
        open("alpha");
        QVERIFY(!entry("&Keep")->isEnabled());

        /* taken back */
        entry("SanDisk")->trigger();
        QTRY_COMPARE(messages.size(), 3);
        QCOMPARE(messages.last().first().toString(), "SanDisk Cruzer Blade is back on this computer");
        QCOMPARE(qemu("alpha")->executed("device_del").size(), 1);
        QTRY_VERIFY(!has("alpha", "99-1"));
        open("alpha");
        QVERIFY(!entry("SanDisk")->isChecked());
        /* and no longer kept: the arguments say otherwise now */
        QVERIFY(entry("&Keep")->isEnabled());
        entry("&Keep")->trigger();
        QTRY_COMPARE(messages.size(), 4);
        QVERIFY(!readFile(vm("alpha")->argsPath()).contains("usb-host"));
        QVERIFY(readFile(vm("alpha")->argsPath()).contains("qemu-xhci"));
    }

    /* A keyboard, a mouse, a Bluetooth adapter: not without asking */
    void inputAsks()
    {
        open("alpha");
        answer("Cancel");
        entry("Logitech")->trigger();
        QTest::qWait(300);
        QVERIFY(qemu("alpha")->executed("device_add").size() == 1);

        answer("Attach");
        entry("Logitech")->trigger();
        QTRY_COMPARE(qemu("alpha")->executed("device_add").size(), 2);
        QTRY_VERIFY(has("alpha", "99-2"));
        open("alpha");
        QVERIFY(entry("Logitech")->isChecked());
        /* given back without asking */
        entry("Logitech")->trigger();
        QTRY_VERIFY(!has("alpha", "99-2"));
    }

    /* QEMU's error, said plainly: here, the adapter is not on its bus */
    void attachFails()
    {
        open("alpha");
        answer("Attach");
        entry("Intel")->trigger();
        const QString text = takeMessageBox();
        QVERIFY2(text.contains("Intel Bluetooth was not given to alpha."), qPrintable(text));
        QVERIFY2(text.contains("It is no longer plugged in."), qPrintable(text));
        QVERIFY(!has("alpha", "99-4"));
    }

    /* No access to its node yet: asked for (pkexec), then attached */
    void access()
    {
        QFile::setPermissions(node(99, 2), QFileDevice::Permissions());
        if (::access(QFile::encodeName(node(99, 2)).constData(), R_OK | W_OK) == 0) {
            QSKIP("this user opens anything (root)");
        }
        /* refused: the dialog dismissed */
        program("pkexec", "exit 126");
        program("setfacl", "exit 0");
        open("alpha");
        QVERIFY(entry("SanDisk")->toolTip().contains("you are asked for the access first"));
        const qsizetype before = qemu("alpha")->executed("device_add").size();
        entry("SanDisk")->trigger();
        QString text = takeMessageBox();
        QVERIFY2(text.contains("the access was not given"), qPrintable(text));
        QVERIFY(text.contains(node(99, 2)));
        QCOMPARE(qemu("alpha")->executed("device_add").size(), before);

        /* given: setfacl's stand-in lets this user open the nodes it gets */
        program("pkexec", "exec \"$@\"");
        program("setfacl", "shift 2\nfor node; do /usr/bin/chmod u+rw \"$node\"; done");
        open("alpha");
        entry("SanDisk")->trigger();
        QTRY_COMPARE(qemu("alpha")->executed("device_add").size(), before + 1);
        QVERIFY(::access(QFile::encodeName(node(99, 2)).constData(), R_OK | W_OK) == 0);
        QTRY_VERIFY(has("alpha", "99-1"));
        open("alpha");
        entry("SanDisk")->trigger();
        QTRY_VERIFY(!has("alpha", "99-1"));
    }

    /* Before a start: the devices of its arguments whose node this user may not open */
    void startAccess()
    {
        Vm *delta = vm("delta");
        ArgsFile args = delta->args();
        args.add("device", "usb-host,vendorid=0x0781,productid=0x5567");
        QVERIFY(delta->save(args));
        QFile::setPermissions(node(99, 2), QFileDevice::Permissions());
        if (::access(QFile::encodeName(node(99, 2)).constData(), R_OK | W_OK) == 0) {
            QSKIP("this user opens anything (root)");
        }
        QString warning = "none yet";
        auto request = [&]() {
            warning = "none yet";
            UsbAccess::request(delta, this, [&warning](const QString &w) { warning = w; },
                               m_sys, m_dev);
            QTRY_VERIFY(warning != "none yet");
        };

        program("pkexec", "exit 126");
        program("setfacl", "exit 0");
        request();
        QVERIFY2(warning.contains("SanDisk Cruzer Blade (0781:5567)"), qPrintable(warning));
        QVERIFY(warning.contains("the access was not given"));

        program("pkexec", "exec \"$@\"");
        program("setfacl", "shift 2\nfor node; do /usr/bin/chmod u+rw \"$node\"; done");
        request();
        QCOMPARE(warning, QString());
        /* nothing to ask for now */
        program("pkexec", "exit 1");
        request();
        QCOMPARE(warning, QString());
    }

    /* Plugged out while the menu shows: it goes */
    void unplugged()
    {
        open("alpha");
        QVERIFY(entry("Intel"));
        unplug("99-4");
        QTRY_VERIFY(!entry("Intel"));
        QVERIFY(entry("SanDisk"));
    }

    /* The VM stops while the menu shows */
    void stops()
    {
        open("gamma");
        m_qemus.remove("gamma");
        QTRY_COMPARE(vm("gamma")->runner()->state(), VmRunner::State::Stopped);
        QTRY_COMPARE(m_menu->actions().first()->text(), "The VM is not running");
        QVERIFY(!m_menu->menuAction()->isEnabled());
    }
};

QTEST_MAIN(TestUsbMenu)
#include "test_usbmenu.moc"
