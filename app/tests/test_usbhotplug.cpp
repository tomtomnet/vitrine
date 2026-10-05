// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Host USB devices given to a running VM and taken back over QMP.  No
 * device of this computer is ever attached: the host's devices are a fake
 * sysfs and /dev, on bus 99, which no computer numbers its USB buses up
 * to.  QEMU is a stand-in (fake-qemu) whose monitor answers as QEMU 11.1
 * does, and a real QEMU (VITRINE_TEST_QEMU, else qemu-system-x86_64 from
 * PATH; 128 MiB, no disk) checks those answers: usb-host on bus 99 fails
 * as it finds nothing there, and the one by vendor and product ID
 * (dead:beef) matches nothing here.
 */
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <utility>

#include "core/paths.h"
#include "core/qmpclient.h"
#include "core/usbhotplug.h"
#include "core/vmrunner.h"
#include "fakeusbqemu.h"

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

/* Runs $VITRINE_TEST_QEMU, else the qemu-system-x86_64 in PATH */
static QString testQemu()
{
    const QString env = qEnvironmentVariable("VITRINE_TEST_QEMU");
    return env.isEmpty() ? QStandardPaths::findExecutable("qemu-system-x86_64") : env;
}

/* A VM found running: the stand-in QEMU with the runner's -qmp, its pid file, its monitor */
class FakeVm
{
public:
    FakeVm(const QString &dir, const QString &id) : runner(id, dir + '/' + id)
    {
        const QString run = Paths::vmRuntimeDir(id);
        QDir().mkpath(dir + '/' + id);
        QDir().mkpath(run);
        m_qemu.start(FAKE_QEMU, {"-qmp", QString("unix:%1/qmp.sock,server=on,wait=off").arg(run)});
        m_qemu.waitForStarted();
        /* its arguments readable, as a runner checks them */
        QElapsedTimer clock;
        clock.start();
        while (!readFile(QString("/proc/%1/cmdline").arg(m_qemu.processId())).contains("-qmp") &&
               clock.elapsed() < 5000) {
            QTest::qWait(5);
        }
        writeFile(run + "/qemu.pid", QByteArray::number(m_qemu.processId()) + "\n");
        qemu = std::make_unique<FakeUsbQemu>(run + "/qmp.sock");
    }
    ~FakeVm() { quit(); }
    /* QEMU ends: its monitor goes, the process too */
    void quit()
    {
        qemu.reset();
        if (m_qemu.state() != QProcess::NotRunning) {
            m_qemu.kill();
            m_qemu.waitForFinished();
        }
    }
    void attach() { runner.attach(ArgsFile::parse("-machine q35\n-device qemu-xhci\n")); }

    VmRunner runner;
    std::unique_ptr<FakeUsbQemu> qemu;

private:
    QProcess m_qemu;
};

class TestUsbHotplug : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_tmp;
    QString m_sys;
    QString m_dev;
    int m_count = 0;

    QString vmId()
    {
        return QString("usb-%1-%2").arg(QCoreApplication::applicationPid()).arg(++m_count);
    }

    /* A host USB device in the fake sysfs, its node in the fake /dev */
    void plug(const QString &name, int bus, int address, const QByteArray &vendor,
              const QByteArray &product, const QByteArray &manufacturer = {},
              const QByteArray &productName = {})
    {
        const QString dir = m_sys + "/devices/" + name;
        writeFile(dir + "/busnum", QByteArray::number(bus) + "\n");
        writeFile(dir + "/devnum", QByteArray::number(address) + "\n");
        writeFile(dir + "/idVendor", vendor + "\n");
        writeFile(dir + "/idProduct", product + "\n");
        writeFile(dir + "/bDeviceClass", "00\n");
        if (!manufacturer.isEmpty()) {
            writeFile(dir + "/manufacturer", manufacturer + "\n");
            writeFile(dir + "/product", productName + "\n");
        }
        QDir().mkpath(m_sys + "/bus/usb/devices");
        QFile::link(dir, m_sys + "/bus/usb/devices/" + name);
        writeFile(m_dev + QString::asprintf("/bus/usb/%03d/%03d", bus, address), {});
    }
    void unplug(const QString &name)
    {
        QFile::remove(m_sys + "/bus/usb/devices/" + name);
        QDir(m_sys + "/devices/" + name).removeRecursively();
    }
    UsbDevice host(const QString &port)
    {
        for (const UsbDevice &d : UsbHotplug::hostDevices()) {
            if (d.port == port) {
                return d;
            }
        }
        return {};
    }

    /* @hotplug's attach() or detach(), waited for: its error */
    QString wait(const std::function<void(const UsbHotplug::Done &)> &call)
    {
        bool done = false;
        QString error;
        call([&](const QString &e) {
            error = e;
            done = true;
        });
        if (!QTest::qWaitFor([&]() { return done; }, 5000)) {
            return "timed out";
        }
        return error;
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        m_sys = m_tmp.filePath("sys");
        m_dev = m_tmp.filePath("dev");
        UsbHotplug::setRoots(m_sys, m_dev);
        UsbHotplug::setWatchInterval(100);
        plug("99-1", 99, 2, "0781", "5567", "SanDisk", "Cruzer Blade");
        plug("99-2", 99, 3, "dead", "beef");
        /* a hub: no device for a VM */
        plug("99-3", 99, 4, "05e3", "0610");
        writeFile(m_sys + "/devices/99-3/bDeviceClass", "09\n");
    }

    void cleanupTestCase()
    {
        UsbHotplug::setRoots("/sys", "/dev");
    }

    void ids()
    {
        UsbDevice d;
        d.bus = 3;
        d.device = 17;
        d.vendorId = 0x046d;
        d.productId = 0xc52b;
        QCOMPARE(UsbHotplug::idFor(d), "vitrine-usb-3-17-046d-c52b");

        UsbHotplug::Device dev;
        dev.id = UsbHotplug::idFor(d);
        QVERIFY(dev.isOwn());
        for (const char *other : {"", "sdl-usb-3-17", "vitrine-usb-3-17", "vitrine-usb-a-1-046d-c52b"}) {
            dev.id = other;
            QVERIFY2(!dev.isOwn(), other);
        }
    }

    /* As QEMU's autoscan matches host devices */
    void matches()
    {
        UsbDevice host;
        host.bus = 1;
        host.device = 5;
        host.port = "1-2.3";
        host.vendorId = 0x0781;
        host.productId = 0x5567;

        UsbHotplug::Device d;
        /* no filter: QEMU takes whatever it can open first, no device here */
        QVERIFY(!d.matches(host));
        d.bus = 1;
        d.address = 5;
        QVERIFY(d.matches(host));
        d.address = 6;
        QVERIFY(!d.matches(host));

        d = {};
        d.vendorId = 0x0781;
        d.productId = 0x5567;
        QVERIFY(d.matches(host));
        d.productId = 0x5568;
        QVERIFY(!d.matches(host));

        /* QEMU's port has no bus */
        d = {};
        d.bus = 1;
        d.port = "2.3";
        QVERIFY(d.matches(host));
        d.port = "1-2.3";
        QVERIFY(!d.matches(host));
        d.port = "2";
        QVERIFY(!d.matches(host));

        /* the node alone */
        d = {};
        d.node = "/dev/bus/usb/001/005";
        d.vendorId = 0x1234;
        QVERIFY(d.matches(host));
        d.node = "/dev/bus/usb/001/006";
        QVERIFY(!d.matches(host));
    }

    void explain()
    {
        QCOMPARE(UsbHotplug::explain("failed to find host usb device 99:7"),
                 "It is no longer plugged in.");
        QVERIFY(UsbHotplug::explain("failed to open host usb device 1:5")
                    .startsWith("QEMU could not open it"));
        QVERIFY(UsbHotplug::explain("No 'usb-bus' bus found for device 'usb-host'")
                    .startsWith("This VM has no USB controller."));
        QVERIFY(UsbHotplug::explain("'usb-host' is not a valid device model name")
                    .contains("without libusb"));
        QCOMPARE(UsbHotplug::explain("something else"), "something else");
    }

    void hostDevices()
    {
        const QList<UsbDevice> list = UsbHotplug::hostDevices();
        QCOMPARE(list.size(), 2);
        QCOMPARE(list[0].port, "99-1");
        QCOMPARE(list[1].port, "99-2");
        QCOMPARE(UsbHotplug::nodeOf(list[0]), m_dev + "/bus/usb/099/002");
    }

    /* What the VM has, from QOM: with an id or without, usb-host only */
    void reads()
    {
        FakeVm vm(m_tmp.path(), vmId());
        FakeUsbQemu::Device anon;
        anon.path = "/machine/peripheral-anon/device[1]";
        anon.vendor = 0xdead;
        anon.product = 0xbeef;
        FakeUsbQemu::Device sdl;
        sdl.path = "/machine/peripheral/sdl-usb-99-2";
        sdl.bus = 99;
        sdl.address = 2;
        sdl.attached = true;
        FakeUsbQemu::Device tablet;
        tablet.path = "/machine/peripheral/tablet";
        tablet.type = "usb-tablet";
        vm.qemu->devices = {anon, sdl, tablet};

        UsbHotplug *hotplug = UsbHotplug::of(&vm.runner);
        QCOMPARE(UsbHotplug::of(&vm.runner), hotplug);
        QSignalSpy changed(hotplug, &UsbHotplug::changed);
        QVERIFY(!hotplug->isKnown());
        vm.attach();
        /* read once it runs, without being asked */
        QTRY_VERIFY(hotplug->isKnown());
        QCOMPARE(changed.size(), 1);
        QCOMPARE(hotplug->unavailable(), QString());

        const QList<UsbHotplug::Device> list = hotplug->devices();
        QCOMPARE(list.size(), 2);
        QCOMPARE(list[0].path, anon.path);
        QCOMPARE(list[0].id, QString());
        QCOMPARE(list[0].vendorId, 0xdead);
        QCOMPARE(list[0].productId, 0xbeef);
        QVERIFY(!list[0].attached);
        QCOMPARE(list[1].id, "sdl-usb-99-2");
        QCOMPARE(list[1].bus, 99);
        QCOMPARE(list[1].address, 2);
        QVERIFY(list[1].attached);
        QVERIFY(!list[1].isOwn());

        QVERIFY(hotplug->has(host("99-1")));
        QVERIFY(hotplug->has(host("99-2")));
        QCOMPARE(hotplug->devicesFor(host("99-2")).first().path, anon.path);

        /* asked again, the same: no change */
        hotplug->refresh();
        QTRY_COMPARE(vm.qemu->executed("x-query-usb").size(), 2);
        QTest::qWait(50);
        QCOMPARE(changed.size(), 1);
        /* usb-host asked for once a run */
        QCOMPARE(vm.qemu->executed("device-list-properties").size(), 1);
    }

    void attachDetach()
    {
        FakeVm vm(m_tmp.path(), vmId());
        vm.qemu->plugged = {"99/2", "99/3"};
        UsbHotplug *hotplug = UsbHotplug::of(&vm.runner);
        vm.attach();
        QTRY_VERIFY(hotplug->isKnown());
        QVERIFY(hotplug->devices().isEmpty());

        const UsbDevice stick = host("99-1");
        QCOMPARE(wait([&](const UsbHotplug::Done &done) { hotplug->attach(stick, done); }),
                 QString());
        const QList<QJsonObject> added = vm.qemu->executed("device_add");
        QCOMPARE(added.size(), 1);
        QCOMPARE(added[0]["arguments"].toObject(),
                 QJsonObject({{"driver", "usb-host"}, {"id", "vitrine-usb-99-2-0781-5567"},
                              {"hostbus", 99}, {"hostaddr", 2}}));
        QTRY_VERIFY(hotplug->has(stick));
        const UsbHotplug::Device own = hotplug->devicesFor(stick).first();
        QVERIFY(own.isOwn());
        /* the device it was added for, from its id */
        QCOMPARE(own.vendorId, 0x0781);
        QCOMPARE(own.productId, 0x5567);
        QVERIFY(!hotplug->has(host("99-2")));
        QVERIFY(readFile(vm.runner.logPath()).contains("vitrine: USB: attached SanDisk Cruzer Blade"));

        /* attached already: nothing sent */
        QCOMPARE(wait([&](const UsbHotplug::Done &done) { hotplug->attach(stick, done); }),
                 QString());
        QCOMPARE(vm.qemu->executed("device_add").size(), 1);

        QCOMPARE(wait([&](const UsbHotplug::Done &done) { hotplug->detach(stick, done); }),
                 QString());
        QCOMPARE(vm.qemu->executed("device_del").last()["arguments"].toObject(),
                 QJsonObject({{"id", "vitrine-usb-99-2-0781-5567"}}));
        QTRY_VERIFY(!hotplug->has(stick));
        QVERIFY(hotplug->devices().isEmpty());

        /* not there: nothing to take back */
        QCOMPARE(wait([&](const UsbHotplug::Done &done) { hotplug->detach(stick, done); }),
                 QString());
        QCOMPARE(vm.qemu->executed("device_del").size(), 1);
    }

    /* QEMU's errors, said plainly */
    void attachErrors()
    {
        FakeVm vm(m_tmp.path(), vmId());
        UsbHotplug *hotplug = UsbHotplug::of(&vm.runner);
        vm.attach();
        QTRY_VERIFY(hotplug->isKnown());

        /* unplugged between the menu and the click */
        const UsbDevice stick = host("99-1");
        QCOMPARE(wait([&](const UsbHotplug::Done &done) { hotplug->attach(stick, done); }),
                 "It is no longer plugged in.");
        vm.qemu->plugged = {"99/2"};
        vm.qemu->busy = {"99/2"};
        QVERIFY(wait([&](const UsbHotplug::Done &done) { hotplug->attach(stick, done); })
                    .startsWith("QEMU could not open it"));
        QVERIFY(readFile(vm.runner.logPath()).contains("failed to open host usb device 99:2"));
        QTRY_VERIFY(!hotplug->has(stick));
    }

    /* A VM without a USB controller, a QEMU without usb-host: said before trying */
    void unavailable()
    {
        FakeVm vm(m_tmp.path(), vmId());
        vm.qemu->controller = false;
        vm.qemu->plugged = {"99/2"};
        UsbHotplug *hotplug = UsbHotplug::of(&vm.runner);
        vm.attach();
        QTRY_VERIFY(hotplug->isKnown());
        QVERIFY(hotplug->unavailable().startsWith("This VM has no USB controller."));
        const UsbDevice stick = host("99-1");
        QCOMPARE(wait([&](const UsbHotplug::Done &done) { hotplug->attach(stick, done); }),
                 hotplug->unavailable());
        QVERIFY(vm.qemu->executed("device_add").isEmpty());

        FakeVm other(m_tmp.path(), vmId());
        other.qemu->libusb = false;
        UsbHotplug *without = UsbHotplug::of(&other.runner);
        other.attach();
        QTRY_VERIFY(without->isKnown());
        QVERIFY(without->unavailable().contains("without libusb"));
    }

    /* A device the VM has by its arguments (no id), taken back by its path */
    void detachAnonymous()
    {
        FakeVm vm(m_tmp.path(), vmId());
        FakeUsbQemu::Device anon;
        anon.path = "/machine/peripheral-anon/device[2]";
        anon.vendor = 0x0781;
        anon.product = 0x5567;
        anon.attached = true;
        vm.qemu->devices = {anon};
        UsbHotplug *hotplug = UsbHotplug::of(&vm.runner);
        vm.attach();
        QTRY_VERIFY(hotplug->isKnown());
        const UsbDevice stick = host("99-1");
        QVERIFY(hotplug->has(stick));
        QCOMPARE(wait([&](const UsbHotplug::Done &done) { hotplug->detach(stick, done); }),
                 QString());
        QCOMPARE(vm.qemu->executed("device_del").last()["arguments"].toObject(),
                 QJsonObject({{"id", anon.path}}));
        QTRY_VERIFY(!hotplug->has(stick));
    }

    /* Taken back elsewhere (QEMU's monitor, the SDL menu): DEVICE_DELETED */
    void deletedElsewhere()
    {
        FakeVm vm(m_tmp.path(), vmId());
        FakeUsbQemu::Device sdl;
        sdl.path = "/machine/peripheral/sdl-usb-99-2";
        sdl.bus = 99;
        sdl.address = 2;
        sdl.attached = true;
        vm.qemu->devices = {sdl};
        UsbHotplug *hotplug = UsbHotplug::of(&vm.runner);
        vm.attach();
        QTRY_VERIFY(hotplug->isKnown());
        QVERIFY(hotplug->has(host("99-1")));
        QSignalSpy changed(hotplug, &UsbHotplug::changed);
        vm.qemu->remove(sdl.path);
        QTRY_COMPARE(changed.size(), 1);
        QVERIFY(!hotplug->has(host("99-1")));
    }

    /*
     * vitrine's own, whose device left the host: QEMU would give the next
     * device at that address to the guest, so it is taken back, also when
     * found so as the VM is found running
     */
    void unpluggedOwn()
    {
        plug("99-4", 99, 9, "1234", "5678", "Some", "Stick");
        FakeVm vm(m_tmp.path(), vmId());
        vm.qemu->plugged = {"99/9"};
        UsbHotplug *hotplug = UsbHotplug::of(&vm.runner);
        vm.attach();
        QTRY_VERIFY(hotplug->isKnown());
        const UsbDevice stick = host("99-4");
        QCOMPARE(wait([&](const UsbHotplug::Done &done) { hotplug->attach(stick, done); }),
                 QString());
        QTRY_VERIFY(hotplug->has(stick));
        /* still there: left alone */
        QTest::qWait(300);
        QVERIFY(vm.qemu->executed("device_del").isEmpty());

        unplug("99-4");
        QTRY_COMPARE(vm.qemu->executed("device_del").size(), 1);
        QCOMPARE(vm.qemu->executed("device_del").first()["arguments"].toObject(),
                 QJsonObject({{"id", "vitrine-usb-99-9-1234-5678"}}));
        QTRY_VERIFY(hotplug->devices().isEmpty());
        QVERIFY(readFile(vm.runner.logPath()).contains("left this computer"));

        /* another device at that address since, found as the VM is found running */
        plug("99-4", 99, 9, "1111", "2222");
        FakeVm found(m_tmp.path(), vmId());
        FakeUsbQemu::Device left;
        left.path = "/machine/peripheral/vitrine-usb-99-9-1234-5678";
        left.bus = 99;
        left.address = 9;
        left.attached = true;
        found.qemu->devices = {left};
        UsbHotplug *again = UsbHotplug::of(&found.runner);
        found.attach();
        QTRY_COMPARE(found.qemu->executed("device_del").size(), 1);
        QTRY_VERIFY(again->devices().isEmpty());
        unplug("99-4");
    }

    /* The run ends: nothing known of the next */
    void stops()
    {
        auto vm = std::make_unique<FakeVm>(m_tmp.path(), vmId());
        FakeUsbQemu::Device anon;
        anon.path = "/machine/peripheral-anon/device[1]";
        anon.vendor = 0x0781;
        anon.product = 0x5567;
        vm->qemu->devices = {anon};
        UsbHotplug *hotplug = UsbHotplug::of(&vm->runner);
        vm->attach();
        QTRY_VERIFY(hotplug->isKnown());
        QVERIFY(hotplug->has(host("99-1")));
        QSignalSpy changed(hotplug, &UsbHotplug::changed);
        /* QEMU goes: its monitor closes, then the process */
        vm->quit();
        QTRY_COMPARE(vm->runner.state(), VmRunner::State::Stopped);
        QVERIFY(!hotplug->isKnown());
        QVERIFY(hotplug->devices().isEmpty());
        QVERIFY(changed.size() >= 1);
        QCOMPARE(wait([&](const UsbHotplug::Done &done) { hotplug->attach(host("99-1"), done); }),
                 "The VM is not running.");
    }

    /*
     * The same with a real QEMU: a usb-host on bus 99, which nothing has,
     * and one by IDs nothing has (dead:beef), are all it ever looks for
     */
    void realQemu()
    {
        const QString qemu = testQemu();
        if (!QFileInfo(qemu).isExecutable()) {
            QSKIP("no QEMU, set VITRINE_TEST_QEMU");
        }
        QProcess probe;
        probe.start(qemu, {"-device", "help"});
        probe.waitForFinished(20000);
        if (!probe.readAllStandardOutput().contains("\"usb-host\"")) {
            QSKIP("this QEMU has no usb-host");
        }
        Paths::setQemuBinary(qemu);

        const QString id = vmId();
        const QString bareId = vmId();
        VmRunner runner(id, m_tmp.filePath(id));
        VmRunner bare(bareId, m_tmp.filePath(bareId));
        QDir().mkpath(m_tmp.filePath(id));
        QDir().mkpath(m_tmp.filePath(bareId));
        /* a failed check leaves no QEMU running: a VM outlives its runner */
        const auto stop = qScopeGuard([&runner, &bare]() {
            for (VmRunner *r : {&runner, &bare}) {
                r->forceOff();
            }
            if (!QTest::qWaitFor([&]() { return !runner.isActive() && !bare.isActive(); }, 20000)) {
                qWarning("a test QEMU did not quit");
            }
            Paths::setQemuBinary({});
        });
        UsbHotplug *hotplug = UsbHotplug::of(&runner);
        QSignalSpy failed(&runner, &VmRunner::failed);
        runner.start(ArgsFile::parse("-machine q35\n-m 128\n-nodefaults\n-display none\n"
                                     "-device qemu-xhci\n"
                                     "-device usb-host,vendorid=0xdead,productid=0xbeef\n"));
        QTRY_VERIFY_WITH_TIMEOUT(runner.state() == VmRunner::State::Running, 20000);
        QTRY_VERIFY(hotplug->isKnown());
        QCOMPARE(hotplug->unavailable(), QString());

        /* by its IDs, from the arguments: not attached, as no device has them */
        QCOMPARE(hotplug->devices().size(), 1);
        const UsbHotplug::Device anon = hotplug->devices().first();
        QVERIFY(anon.path.startsWith("/machine/peripheral-anon/device["));
        QCOMPARE(anon.vendorId, 0xdead);
        QCOMPARE(anon.productId, 0xbeef);
        QCOMPARE(anon.bus, 0);
        QVERIFY(!anon.attached);
        const UsbDevice deadbeef = host("99-2");
        QVERIFY(hotplug->has(deadbeef));

        /* taken back by its path, gone at once; DEVICE_DELETED comes too, as
           QEMU frees it (through RCU: before the reply or after) */
        QStringList order;
        QObject::connect(runner.qmp(), &QmpClient::qmpEvent, hotplug,
                         [&order](const QString &name) { order << name; });
        bool done = false;
        hotplug->detach(deadbeef, [&](const QString &error) {
            QCOMPARE(error, QString());
            order << "reply";
            done = true;
        });
        QTRY_VERIFY(done);
        QTRY_VERIFY(hotplug->devices().isEmpty());
        QTRY_COMPARE(order.size(), 2);
        QVERIFY(order.contains("reply") && order.contains("DEVICE_DELETED"));

        /* bus 99 has nothing: QEMU's error, plainly */
        const UsbDevice stick = host("99-1");
        QCOMPARE(wait([&](const UsbHotplug::Done &d) { hotplug->attach(stick, d); }),
                 "It is no longer plugged in.");
        QVERIFY(readFile(runner.logPath()).contains("failed to find host usb device 99:2"));
        QVERIFY(hotplug->devices().isEmpty());

        /* an emulated USB device, added and taken back the same way */
        QString error = "pending";
        runner.qmp()->execute("device_add", {{"driver", "usb-tablet"}, {"id", "vitrine-usb-test"}},
                              [&](const QJsonValue &, const QString &e) { error = e; });
        QTRY_VERIFY(error != "pending");
        QCOMPARE(error, QString());
        hotplug->refresh();
        QTest::qWait(200);
        /* no usb-host: not one of the host's devices */
        QVERIFY(hotplug->devices().isEmpty());
        error = "pending";
        runner.qmp()->execute("device_del", {{"id", "vitrine-usb-test"}},
                              [&](const QJsonValue &, const QString &e) { error = e; });
        QTRY_VERIFY(error != "pending");
        QCOMPARE(error, QString());
        QTRY_VERIFY(order.count("DEVICE_DELETED") == 2);
        QVERIFY(failed.isEmpty());

        /* no USB controller: x-query-usb says so; device_add would refuse */
        UsbHotplug *none = UsbHotplug::of(&bare);
        bare.start(ArgsFile::parse("-machine q35\n-m 128\n-nodefaults\n-display none\n"));
        QTRY_VERIFY_WITH_TIMEOUT(bare.state() == VmRunner::State::Running, 20000);
        QTRY_VERIFY(none->isKnown());
        QVERIFY(none->unavailable().startsWith("This VM has no USB controller."));
        error = "pending";
        bare.qmp()->execute("device_add", {{"driver", "usb-host"}, {"id", "vitrine-usb-x"},
                                          {"hostbus", 99}, {"hostaddr", 2}},
                            [&](const QJsonValue &, const QString &e) { error = e; });
        QTRY_VERIFY(error != "pending");
        QVERIFY2(UsbHotplug::explain(error).startsWith("This VM has no USB controller."),
                 qPrintable(error));

        for (VmRunner *r : {&runner, &bare}) {
            r->forceOff();
        }
        QTRY_VERIFY_WITH_TIMEOUT(runner.state() == VmRunner::State::Stopped &&
                                     bare.state() == VmRunner::State::Stopped,
                                 20000);
    }
};

QTEST_GUILESS_MAIN(TestUsbHotplug)
#include "test_usbhotplug.moc"
