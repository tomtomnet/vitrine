// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * One USB device for one running VM at a time, and what else holds a
 * device a VM passes through.  Never this computer's devices nor its
 * processes: the host's devices are a fake sysfs and /dev on bus 99, its
 * processes a fake /proc (pid folders with a cmdline, a comm and fd
 * links); the VMs are found running, stand-in QEMUs whose monitors answer
 * as QEMU 11.1 does (fakeusbqemu.h), and nothing here starts a QEMU.
 */
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <memory>

#include <linux/netlink.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/paths.h"
#include "core/usbguard.h"
#include "core/usbhotplug.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "fakeusbqemu.h"
#include "ui/usbguardtext.h"

using Device = UsbHotplug::Device;
using Holder = UsbGuard::Holder;
using Kind = UsbGuard::Holder::Kind;

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

/* Whether this user may read the kernel's uevents here, as the guard does */
static bool ueventsReadable()
{
    const int fd = ::socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    sockaddr_nl address = {};
    address.nl_family = AF_NETLINK;
    address.nl_groups = 1;
    const bool ok = fd >= 0 &&
                    ::bind(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0;
    if (fd >= 0) {
        ::close(fd);
    }
    return ok;
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

/* VMs in a folder of their own: those running found so, their devices read */
class Fleet
{
public:
    explicit Fleet(int serial) : m_serial(serial) {}
    ~Fleet()
    {
        store.reset();
        m_qemus.clear();
    }

    /* A VM named @name with @args; running, its stand-in's monitor */
    FakeUsbQemu *add(const QString &name, const QByteArray &args, bool running)
    {
        const QString id = QString("%1-%2-%3")
                               .arg(name)
                               .arg(QCoreApplication::applicationPid())
                               .arg(m_serial);
        writeFile(m_dir.filePath(id + "/vm.args"), "-name " + name.toUtf8() + "\n" + args);
        if (!running) {
            return nullptr;
        }
        auto standIn = std::make_shared<StandIn>(id);
        m_qemus.insert(name, standIn);
        return standIn->qemu.get();
    }
    /* The store, its running VMs found and read */
    bool open()
    {
        store = std::make_unique<VmStore>(m_dir.path());
        for (Vm *each : store->vms()) {
            if (m_qemus.contains(each->name())) {
                each->runner()->attach(each->args());
            }
        }
        for (Vm *each : store->vms()) {
            if (m_qemus.contains(each->name()) &&
                !QTest::qWaitFor([each]() {
                    return each->runner()->state() == VmRunner::State::Running &&
                           UsbHotplug::of(each->runner())->isKnown();
                }, 10000)) {
                return false;
            }
        }
        return true;
    }
    Vm *vm(const QString &name) const
    {
        for (Vm *each : store->vms()) {
            if (each->name() == name) {
                return each;
            }
        }
        return nullptr;
    }
    qint64 pid(const QString &name) const { return m_qemus.value(name)->pid(); }
    /* Its QEMU ends: the run with it */
    void stop(const QString &name) { m_qemus.remove(name); }

    std::unique_ptr<VmStore> store;

private:
    QTemporaryDir m_dir;
    QHash<QString, std::shared_ptr<StandIn>> m_qemus;
    int m_serial;
};

/* A usb-host device by vendor and product ID, as QEMU's QOM shows it */
static FakeUsbQemu::Device byIds(int vendor, int product)
{
    FakeUsbQemu::Device d;
    d.path = "/machine/peripheral-anon/device[1]";
    d.vendor = vendor;
    d.product = product;
    d.attached = true;
    return d;
}

/* One vitrine gave while the VM ran, by bus and address */
static FakeUsbQemu::Device given(int bus, int address, const QString &ids)
{
    FakeUsbQemu::Device d;
    d.path = QString("/machine/peripheral/vitrine-usb-%1-%2-%3").arg(bus).arg(address).arg(ids);
    d.bus = bus;
    d.address = address;
    d.attached = true;
    return d;
}

class TestUsbGuard : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_tmp;
    QString m_sys, m_dev, m_proc;
    int m_count = 0;

    /* A host USB device in the fake sysfs, an interface with @driver, its node in the fake /dev */
    void plug(const QString &name, int bus, int address, const QByteArray &vendor,
              const QByteArray &product, const QByteArray &manufacturer,
              const QByteArray &productName)
    {
        const QString dir = m_sys + "/devices/" + name;
        writeFile(dir + "/busnum", QByteArray::number(bus) + "\n");
        writeFile(dir + "/devnum", QByteArray::number(address) + "\n");
        writeFile(dir + "/idVendor", vendor + "\n");
        writeFile(dir + "/idProduct", product + "\n");
        writeFile(dir + "/bDeviceClass", "00\n");
        writeFile(dir + "/manufacturer", manufacturer + "\n");
        writeFile(dir + "/product", productName + "\n");
        writeFile(dir + ":1.0/bInterfaceClass", "ff\n");
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
    /* Its interface claimed through usbfs, or let go */
    void claim(const QString &name, bool on)
    {
        const QString link = m_sys + "/devices/" + name + ":1.0/driver";
        QFile::remove(link);
        if (on) {
            QDir().mkpath(m_sys + "/bus/usb/drivers/usbfs");
            QVERIFY(QFile::link(m_sys + "/bus/usb/drivers/usbfs", link));
        }
    }
    QString node(int bus, int address) const
    {
        return m_dev + QString::asprintf("/bus/usb/%03d/%03d", bus, address);
    }
    UsbDevice host(const QString &port) const
    {
        for (const UsbDevice &d : UsbHotplug::hostDevices()) {
            if (d.port == port) {
                return d;
            }
        }
        return {};
    }
    /* A process of the fake /proc: its argv, its comm, the nodes it has open */
    void process(qint64 pid, const QStringList &argv, const QString &comm,
                 const QStringList &nodes)
    {
        const QString dir = QString("%1/%2").arg(m_proc).arg(pid);
        QByteArray cmdline;
        for (const QString &arg : argv) {
            cmdline += arg.toUtf8() + '\0';
        }
        QDir(dir).removeRecursively();
        QVERIFY(writeFile(dir + "/cmdline", cmdline));
        QVERIFY(writeFile(dir + "/comm", comm.toUtf8() + "\n"));
        QDir().mkpath(dir + "/fd");
        int fd = 3;
        for (const QString &target : nodes) {
            QVERIFY(QFile::link(target, QString("%1/fd/%2").arg(dir).arg(fd++)));
        }
    }
    void endProcess(qint64 pid) { QDir(QString("%1/%2").arg(m_proc).arg(pid)).removeRecursively(); }

private slots:
    void initTestCase()
    {
        m_sys = m_tmp.filePath("sys");
        m_dev = m_tmp.filePath("dev");
        m_proc = m_tmp.filePath("proc");
        QDir().mkpath(m_proc);
        UsbHotplug::setRoots(m_sys, m_dev);
        UsbHotplug::setWatchInterval(100);
        plug("99-1", 99, 2, "2d95", "6013", "vivo", "X200 Ultra");
        plug("99-2", 99, 3, "0781", "5567", "SanDisk", "Cruzer Blade");
    }

    void cleanupTestCase()
    {
        UsbHotplug::setRoots("/sys", "/dev");
    }

    /* The usb-host lines of a VM's arguments, as QEMU's filters */
    void filters()
    {
        const QList<Device> f = UsbGuard::filtersOf(ArgsFile::parse(
            "-device qemu-xhci\n"
            "-device usb-host,vendorid=0x2d95,productid=0x6013\n"
            "-device usb-host,id=stick,hostbus=3,hostaddr=0x0c\n"
            "-device usb-host,hostbus=1,hostport=2.3\n"
            "-device usb-host,hostdevice=/dev/bus/usb/003/012\n"
            "-device usb-host,vendorid=11669\n"
            "-device usb-tablet\n"));

        QCOMPARE(f.size(), 5);
        QCOMPARE(f[0].vendorId, 0x2d95);
        QCOMPARE(f[0].productId, 0x6013);
        QCOMPARE(f[0].bus, 0);
        QCOMPARE(f[1].id, QString("stick"));
        QCOMPARE(f[1].bus, 3);
        QCOMPARE(f[1].address, 12);
        QCOMPARE(f[2].bus, 1);
        QCOMPARE(f[2].port, QString("2.3"));
        QCOMPARE(f[3].node, QString("/dev/bus/usb/003/012"));
        QCOMPARE(f[4].vendorId, 11669);
        QCOMPARE(f[4].productId, 0);
        QVERIFY(UsbGuard::filtersOf(ArgsFile::parse("-device qemu-xhci\n")).isEmpty());
    }

    /* Whether a device could go to both: rules by IDs, devices in particular, ports */
    void overlaps()
    {
        const QList<UsbDevice> hosts = UsbHotplug::hostDevices();
        Device rule, other, vendor, port, none;
        rule.vendorId = 0x2d95;
        rule.productId = 0x6013;
        other = rule;
        other.productId = 0x6014;
        vendor.vendorId = 0x2d95;
        /* QEMU's hostport has no bus: port 1 of bus 99 is 99-1 */
        port.bus = 99;
        port.port = "1";
        Device phone;
        phone.id = "vitrine-usb-99-2-2d95-6013";
        phone.bus = 99;
        phone.address = 2;
        phone.vendorId = 0x2d95;
        phone.productId = 0x6013;
        Device stick = phone;
        stick.id = "vitrine-usb-99-3-0781-5567";
        stick.address = 3;
        stick.vendorId = 0x0781;
        stick.productId = 0x5567;

        QVERIFY(UsbGuard::overlap(rule, rule, hosts));
        QVERIFY(!UsbGuard::overlap(rule, other, hosts));
        QVERIFY(UsbGuard::overlap(rule, vendor, hosts));
        QVERIFY(UsbGuard::overlap(rule, phone, hosts));
        QVERIFY(UsbGuard::overlap(phone, rule, hosts));
        QVERIFY(UsbGuard::overlap(phone, phone, hosts));
        QVERIFY(!UsbGuard::overlap(phone, stick, hosts));
        QVERIFY(UsbGuard::overlap(port, phone, hosts));
        QVERIFY(!UsbGuard::overlap(port, stick, hosts));
        /* no field: no device in particular */
        QVERIFY(!UsbGuard::overlap(none, rule, hosts));
        QVERIFY(!UsbGuard::overlap(none, none, hosts));
        /* one in particular that is not plugged in: by the fields */
        Device gone = phone;
        gone.address = 9;
        QVERIFY(UsbGuard::overlap(gone, rule, hosts));
        QVERIFY(!UsbGuard::overlap(gone, other, hosts));
    }

    /* The start check through the runner: refused before anything runs, saying who has it */
    void startRefused()
    {
        Fleet fleet(++m_count);
        FakeUsbQemu *phone =
            fleet.add("phone", "-device qemu-xhci\n-device usb-host,vendorid=0x2d95,productid=0x6013\n",
                      true);
        phone->devices = {byIds(0x2d95, 0x6013)};
        fleet.add("other",
                  "#qemu " FAKE_QEMU "\n-device qemu-xhci\n"
                  "-device usb-host,vendorid=0x2d95,productid=0x6013\n",
                  false);
        fleet.add("free",
                  "#qemu " FAKE_QEMU "\n-device qemu-xhci\n"
                  "-device usb-host,vendorid=0x0781,productid=0x5567\n",
                  false);
        fleet.add("plain", "#qemu " FAKE_QEMU "\n-device qemu-xhci\n", false);
        QVERIFY(fleet.open());
        UsbGuard guard(fleet.store.get());
        Vm *other = fleet.vm("other");

        const QString why = UsbGuard::startConflict(fleet.store.get(), other, other->args());
        QCOMPARE(why, QString("USB passthrough: phone, which runs, passes vivo X200 Ultra "
                              "(2d95:6013) through too. Shut it down first, or take the device "
                              "out of the USB settings of phone or other."));
        QVERIFY(UsbGuard::startConflict(fleet.store.get(), fleet.vm("free"), fleet.vm("free")->args())
                    .isEmpty());
        QVERIFY(UsbGuard::startConflict(fleet.store.get(), fleet.vm("plain"),
                                        fleet.vm("plain")->args())
                    .isEmpty());
        /* the running VM itself: no other */
        QVERIFY(UsbGuard::startConflict(fleet.store.get(), fleet.vm("phone"),
                                        fleet.vm("phone")->args())
                    .isEmpty());

        QSignalSpy failed(other->runner(), &VmRunner::failed);
        QSignalSpy launched(other->runner(), &VmRunner::qemuLaunched);
        other->runner()->start(other->args());
        QCOMPARE(failed.size(), 1);
        QCOMPARE(failed[0][0].toString(), why);
        QCOMPARE(other->runner()->state(), VmRunner::State::Stopped);
        QVERIFY(other->runner()->refusedByCheck());
        QVERIFY(launched.isEmpty());
        QVERIFY(!QFileInfo::exists(other->runner()->logPath()));
        /* refused for another reason the next time: not by the check */
        other->runner()->setBusy("Its disks are being copied.");
        other->runner()->start(other->args());
        QCOMPARE(failed.size(), 2);
        QVERIFY(!other->runner()->refusedByCheck());
        other->runner()->setBusy({});

        /* phone stopped: free to go */
        fleet.stop("phone");
        QTRY_VERIFY(!fleet.vm("phone")->runner()->isActive());
        QVERIFY(UsbGuard::startConflict(fleet.store.get(), other, other->args()).isEmpty());
    }

    /* Against a device given while the VM runs; two identical devices plugged in */
    void startAgainstGiven()
    {
        Fleet fleet(++m_count);
        FakeUsbQemu *phone = fleet.add("phone", "-device qemu-xhci\n", true);
        phone->devices = {given(99, 2, "2d95-6013")};
        fleet.add("other", "-device usb-host,vendorid=0x2d95,productid=0x6013\n", false);
        /* a rule by port, the phone's: the same device */
        fleet.add("byport", "-device usb-host,hostbus=99,hostport=1\n", false);
        fleet.add("stick", "-device usb-host,hostbus=99,hostport=2\n", false);
        QVERIFY(fleet.open());
        Vm *other = fleet.vm("other");

        QCOMPARE(UsbGuard::startConflict(fleet.store.get(), other, other->args()),
                 QString("USB passthrough: phone has vivo X200 Ultra (2d95:6013), which other "
                         "would take too. Take it back from phone first (USB Devices), or shut "
                         "phone down."));
        QVERIFY(UsbGuard::startConflict(fleet.store.get(), fleet.vm("byport"),
                                        fleet.vm("byport")->args())
                    .startsWith("USB passthrough: phone has vivo X200 Ultra (2d95:6013)"));
        QVERIFY(UsbGuard::startConflict(fleet.store.get(), fleet.vm("stick"),
                                        fleet.vm("stick")->args())
                    .isEmpty());

        plug("99-3", 99, 4, "2d95", "6013", "vivo", "X200 Ultra");
        QVERIFY(UsbGuard::startConflict(fleet.store.get(), other, other->args())
                    .endsWith(" 2 devices with these IDs are plugged in, which USB settings cannot "
                              "tell apart: give one to each VM from USB Devices instead."));
        unplug("99-3");
    }

    /* A device another running VM has, or would take: not given, saying who */
    void attachRefused()
    {
        Fleet fleet(++m_count);
        FakeUsbQemu *phone =
            fleet.add("phone", "-device qemu-xhci\n-device usb-host,vendorid=0x2d95,productid=0x6013\n",
                      true);
        phone->devices = {byIds(0x2d95, 0x6013)};
        FakeUsbQemu *alpha = fleet.add("alpha", "-device qemu-xhci\n", true);
        alpha->plugged = {"99/2", "99/3", "99/4"};
        QVERIFY(fleet.open());
        UsbGuard guard(fleet.store.get());
        UsbHotplug *hotplug = UsbHotplug::of(fleet.vm("alpha")->runner());
        QString error = "unset";
        const auto done = [&error](const QString &e) { error = e; };

        hotplug->attach(host("99-1"), done);
        QCOMPARE(error, QString("phone has it: take it back there first."));
        QVERIFY(alpha->executed("device_add").isEmpty());
        QVERIFY(readFile(fleet.vm("alpha")->runner()->logPath())
                    .contains("USB: did not attach vivo X200 Ultra (2d95:6013) at bus 99, address 2: "
                              "phone has it"));
        QCOMPARE(UsbGuard::attachConflict(fleet.store.get(), fleet.vm("alpha"), host("99-1")),
                 fleet.vm("phone"));
        /* the phone's own VM may have it */
        QVERIFY(!UsbGuard::attachConflict(fleet.store.get(), fleet.vm("phone"), host("99-1")));

        /* a stick no one has: given */
        error = "unset";
        hotplug->attach(host("99-2"), done);
        QTRY_COMPARE(error, QString());
        QCOMPARE(alpha->executed("device_add").size(), 1);

        /* two identical phones: phone's rule may take either */
        plug("99-3", 99, 4, "2d95", "6013", "vivo", "X200 Ultra");
        error = "unset";
        hotplug->attach(host("99-3"), done);
        QCOMPARE(error, QString("phone passes devices with these IDs through: its QEMU may take "
                                "this one too."));
        QCOMPARE(alpha->executed("device_add").size(), 1);
        unplug("99-3");
    }

    /* Two identical devices given by bus and address: one to each VM */
    void identicalDevices()
    {
        plug("99-3", 99, 4, "2d95", "6013", "vivo", "X200 Ultra");
        Fleet fleet(++m_count);
        FakeUsbQemu *phone = fleet.add("phone", "-device qemu-xhci\n", true);
        phone->devices = {given(99, 2, "2d95-6013")};
        FakeUsbQemu *alpha = fleet.add("alpha", "-device qemu-xhci\n", true);
        alpha->plugged = {"99/2", "99/4"};
        QVERIFY(fleet.open());
        UsbGuard guard(fleet.store.get());
        UsbHotplug *hotplug = UsbHotplug::of(fleet.vm("alpha")->runner());
        QString error = "unset";
        const auto done = [&error](const QString &e) { error = e; };

        hotplug->attach(host("99-1"), done);
        QCOMPARE(error, QString("phone has it: take it back there first."));
        error = "unset";
        hotplug->attach(host("99-3"), done);
        QTRY_COMPARE(error, QString());
        QCOMPARE(alpha->executed("device_add").size(), 1);
        QCOMPARE(alpha->executed("device_add")[0]["arguments"].toObject()["hostaddr"].toInt(), 4);
        unplug("99-3");
    }

    /* Who holds the nodes, from a fake /proc: names, kinds, a QEMU's VM; what is not read */
    void scan()
    {
        const QString phone = node(99, 2), stick = node(99, 3);

        process(4190001, {"adb", "-L", "tcp:5037", "fork-server", "server", "--reply-fd", "4"},
                "adb", {phone});
        process(4190002, {"/usr/libexec/gvfsd-mtp", "--spawner", ":1.5", "/org/gtk/gvfs/exec_spaw/3"},
                "gvfsd-mtp", {phone, phone});
        process(4190003, {"/opt/qemu/bin/qemu-system-x86_64", "-name", "guest=handmade,debug-threads=on"},
                "qemu-system-x86", {stick});
        /* no cmdline (a zombie): its comm, cut at 15 characters */
        process(4190004, {}, "gvfs-mtp-volume", {phone});
        /* what it had of a device gone, and other files: none of the host's */
        process(4190005, {"cat"}, "cat", {phone + " (deleted)", "/dev/null", m_dev + "/null"});
        process(4190006, {"secret"}, "secret", {phone});
        QFile::link(m_proc + "/4190001", m_proc + "/self");
        /* another user's: its fds cannot be listed */
        QFile::setPermissions(m_proc + "/4190006/fd", QFileDevice::Permissions());

        const UsbGuard::Scan s = UsbGuard::scan(m_sys, m_dev, m_proc);
        QFile::setPermissions(m_proc + "/4190006/fd", QFileDevice::ReadOwner |
                                                          QFileDevice::WriteOwner |
                                                          QFileDevice::ExeOwner);
        QCOMPARE(s.hosts.size(), 2);
        QList<Holder> onPhone = s.held.value(phone);
        std::sort(onPhone.begin(), onPhone.end(),
                  [](const Holder &a, const Holder &b) { return a.pid < b.pid; });
        if (getuid() == 0) {
            /* root reads every process */
            onPhone.removeIf([](const Holder &h) { return h.pid == 4190006; });
        }
        QCOMPARE(onPhone.size(), 3);
        QCOMPARE(onPhone[0].pid, 4190001);
        QCOMPARE(onPhone[0].program, QString("adb"));
        QCOMPARE(onPhone[0].kind, Kind::Adb);
        QCOMPARE(onPhone[0].text(), QString("adb (pid 4190001)"));
        QCOMPARE(onPhone[1].program, QString("gvfsd-mtp"));
        QCOMPARE(onPhone[1].kind, Kind::Mtp);
        QCOMPARE(onPhone[2].program, QString("gvfs-mtp-volume"));
        QCOMPARE(onPhone[2].kind, Kind::Mtp);
        const QList<Holder> onStick = s.held.value(stick);
        QCOMPARE(onStick.size(), 1);
        QCOMPARE(onStick[0].program, QString("qemu-system-x86_64"));
        QCOMPARE(onStick[0].kind, Kind::Qemu);
        QCOMPARE(onStick[0].vmName, QString("handmade"));
        QVERIFY(!onStick[0].vitrine);
        QCOMPARE(onStick[0].text(), QString("handmade's QEMU (pid 4190003)"));
        QCOMPARE(s.held.size(), 2);

        QCOMPARE(UsbGuard::kindOf("gvfsd-gphoto2"), Kind::Gphoto2);
        QCOMPARE(UsbGuard::kindOf("gvfs-gphoto2-vo"), Kind::Gphoto2);
        QCOMPARE(UsbGuard::kindOf("gvfs-mtp-volume-monitor"), Kind::Mtp);
        QCOMPARE(UsbGuard::kindOf("qemu-kvm"), Kind::Qemu);
        QCOMPARE(UsbGuard::kindOf("steam"), Kind::Other);

        QFile::remove(m_proc + "/self");
        for (qint64 pid = 4190001; pid <= 4190006; pid++) {
            endProcess(pid);
        }
    }

    /* What a scan says of the running VMs: their own QEMU aside, identical devices, claims */
    void issues()
    {
        UsbDevice first, second;
        first.bus = 99;
        first.device = 2;
        first.port = "99-1";
        first.vendorId = 0x2d95;
        first.productId = 0x6013;
        first.manufacturer = "vivo";
        first.product = "X200 Ultra";
        second = first;
        second.device = 4;
        second.port = "99-3";
        Device rule;
        rule.vendorId = 0x2d95;
        rule.productId = 0x6013;
        const UsbGuard::Run run{"gnome", "fedora-gnome1", 100, {rule}};
        const Holder qemu{100, "qemu-system-x86_64", Kind::Qemu, {}, false};
        const Holder adb{200, "adb", Kind::Adb, {}, false};
        const Holder kde{300, "qemu-system-x86_64", Kind::Qemu, {}, false};
        UsbGuard::Scan s;

        s.hosts = {first};
        s.held[node(99, 2)] = {qemu, adb};
        QList<UsbGuard::Issue> found = UsbGuard::issuesOf({run}, s, m_dev);
        QCOMPARE(found.size(), 1);
        QCOMPARE(found[0].vmId, QString("gnome"));
        QCOMPARE(found[0].holders, QList<Holder>({adb}));
        QVERIFY(!found[0].claimedElsewhere);
        QCOMPARE(found[0].text(),
                 QString("adb (pid 200) holds vivo X200 Ultra, which fedora-gnome1 passes through"));
        /* its own QEMU alone: nothing */
        s.held[node(99, 2)] = {qemu};
        QVERIFY(UsbGuard::issuesOf({run}, s, m_dev).isEmpty());

        /* identical: its QEMU holds the first, adb has the second for this computer */
        s.hosts = {first, second};
        s.held[node(99, 4)] = {adb};
        QVERIFY(UsbGuard::issuesOf({run}, s, m_dev).isEmpty());
        /* holding neither, it would take either: adb's counts */
        s.held.remove(node(99, 2));
        found = UsbGuard::issuesOf({run}, s, m_dev);
        QCOMPARE(found.size(), 1);
        QCOMPARE(found[0].device.device, 4);

        /* another VM of the store's QEMU, named */
        s.hosts = {first};
        s.held.clear();
        s.held[node(99, 2)] = {qemu, kde, adb};
        found = UsbGuard::issuesOf({run}, s, m_dev, {{100, "fedora-gnome1"}, {300, "fedora-kde6e"}});
        QCOMPARE(found.size(), 1);
        QCOMPARE(found[0].holders.size(), 2);
        QVERIFY(found[0].holders[0].vitrine);
        QCOMPARE(found[0].text(), QString("fedora-kde6e's QEMU (pid 300) and adb (pid 200) hold vivo "
                                          "X200 Ultra, which fedora-gnome1 passes through"));

        /* none of this user's, an interface on usbfs, its QEMU not on it: claimed elsewhere */
        first.interfaceDrivers = {"usbfs", ""};
        s.hosts = {first};
        s.held.clear();
        found = UsbGuard::issuesOf({run}, s, m_dev);
        QCOMPARE(found.size(), 1);
        QVERIFY(found[0].claimedElsewhere);
        QVERIFY(found[0].holders.isEmpty());
        QCOMPARE(found[0].text(), QString("A program of another user, or of the system, claims vivo "
                                          "X200 Ultra, which fedora-gnome1 passes through"));
        /* its QEMU on it: its own claim */
        s.held[node(99, 2)] = {qemu};
        QVERIFY(UsbGuard::issuesOf({run}, s, m_dev).isEmpty());
        /* a VM that passes nothing through: nothing */
        QVERIFY(UsbGuard::issuesOf({UsbGuard::Run{"x", "x", 400, {}}}, s, m_dev).isEmpty());
    }

    /*
     * The watch: a running VM's device held by adb, told once (the notice,
     * the VM's log) however often it is looked at again; a second holder
     * after a USB event; the notice gone as they let go; the kernel's
     * events listened to only while a running VM has USB devices
     */
    void watch()
    {
        Fleet fleet(++m_count);
        FakeUsbQemu *gnome = fleet.add(
            "fedora-gnome1", "-device qemu-xhci\n-device usb-host,vendorid=0x2d95,productid=0x6013\n",
            true);
        gnome->devices = {byIds(0x2d95, 0x6013)};
        fleet.add("off", "-device qemu-xhci\n", false);
        QVERIFY(fleet.open());
        Vm *vm = fleet.vm("fedora-gnome1");
        const qint64 qemu = fleet.pid("fedora-gnome1");
        process(qemu, {FAKE_QEMU, "-qmp", "unix:qmp.sock"}, "qemu-system-x86", {node(99, 2)});
        process(4190011, {"adb", "fork-server", "server"}, "adb", {node(99, 2)});

        UsbGuard guard(fleet.store.get());
        QSignalSpy notices(&guard, &UsbGuard::notice);
        guard.setProcRoot(m_proc);
        guard.setDelays(20, 100);
        QCOMPARE(guard.listening(), ueventsReadable());
        guard.scanNow();
        QCOMPARE(guard.issues().size(), 1);
        QCOMPARE(guard.issues()[0].vmName, QString("fedora-gnome1"));
        QCOMPARE(guard.issues()[0].holders.size(), 1);
        QCOMPARE(guard.issues()[0].holders[0].pid, 4190011);
        QCOMPARE(notices.size(), 1);
        QCOMPARE(notices[0][0].toString(),
                 QString("adb (pid 4190011) holds vivo X200 Ultra, which fedora-gnome1 passes through."));

        /* looked at again while it holds: told once */
        QTest::qWait(400);
        QCOMPARE(guard.issues().size(), 1);
        QCOMPARE(notices.size(), 1);
        QCOMPARE(readFile(vm->runner()->logPath())
                     .count(QString("vitrine: USB: vivo X200 Ultra (2d95:6013), at bus 99, address 2, "
                                    "is held by adb (pid 4190011) too")),
                 1);

        /* gvfs mounts it at a USB event: told, both named */
        process(4190012, {"/usr/libexec/gvfsd-mtp"}, "gvfsd-mtp", {node(99, 2)});
        guard.hostChanged();
        QTRY_COMPARE(guard.issues().value(0).holders.size(), 2);
        QCOMPARE(notices.size(), 2);

        /* they let go: gone at the next look */
        endProcess(4190011);
        endProcess(4190012);
        QTRY_VERIFY(guard.issues().isEmpty());
        QCOMPARE(notices.size(), 2);

        /* the VM stops: nothing to look at, the socket closed */
        fleet.stop("fedora-gnome1");
        QTRY_VERIFY(!guard.listening());
        endProcess(qemu);
    }

    /* None of this user's, an interface claimed: only once two looks in a row find it */
    void claimedElsewhere()
    {
        Fleet fleet(++m_count);
        FakeUsbQemu *gnome = fleet.add(
            "fedora-gnome1", "-device qemu-xhci\n-device usb-host,vendorid=0x2d95,productid=0x6013\n",
            true);
        gnome->devices = {byIds(0x2d95, 0x6013)};
        QVERIFY(fleet.open());
        const qint64 qemu = fleet.pid("fedora-gnome1");
        process(qemu, {FAKE_QEMU}, "qemu-system-x86", {});
        claim("99-1", true);

        UsbGuard guard(fleet.store.get());
        guard.setProcRoot(m_proc);
        guard.setDelays(60000, 60000);
        guard.scanNow();
        QVERIFY(guard.issues().isEmpty());
        guard.scanNow();
        QCOMPARE(guard.issues().size(), 1);
        QVERIFY(guard.issues()[0].claimedElsewhere);
        /* its QEMU on it: its own claim */
        process(qemu, {FAKE_QEMU}, "qemu-system-x86", {node(99, 2)});
        guard.scanNow();
        QVERIFY(guard.issues().isEmpty());
        claim("99-1", false);
        endProcess(qemu);
    }

    /* The notice's words: what holds what, each with its fix */
    void texts()
    {
        UsbGuard::Issue issue;
        issue.vmId = "gnome";
        issue.vmName = "fedora-gnome1";
        issue.device = host("99-1");

        issue.holders = {Holder{4190001, "adb", Kind::Adb, {}, false}};
        QString text = UsbGuardText::explanation({issue});
        QVERIFY(text.startsWith("<p><b>adb (pid 4190001) holds vivo X200 Ultra, which "
                                "fedora-gnome1 passes through.</b></p>"));
        QVERIFY(text.contains("<nobr>adb kill-server</nobr>"));
        QVERIFY(text.contains("<nobr>kill 4190001</nobr>"));
        QVERIFY(text.contains("give it again there"));

        issue.holders = {Holder{4190002, "gvfsd-mtp", Kind::Mtp, {}, false},
                         Holder{4190003, "gvfsd-gphoto2", Kind::Gphoto2, {}, false}};
        text = UsbGuardText::explanation({issue});
        QCOMPARE(text.count(QString("<pre>systemctl --user mask --now gvfs-mtp-volume-monitor "
                                    "gvfs-gphoto2-volume-monitor</pre>")),
                 1);
        QCOMPARE(UsbGuardText::udevRule(0x2d95, 0x6013),
                 QString("SUBSYSTEM==\"usb\", ATTR{idVendor}==\"2d95\", ATTR{idProduct}==\"6013\", "
                         "ENV{MTP_NO_PROBE}=\"1\", ENV{ID_MTP_DEVICE}=\"\", ENV{ID_GPHOTO2}=\"\""));
        QVERIFY(text.contains("<pre>" + UsbGuardText::udevRule(0x2d95, 0x6013).toHtmlEscaped() +
                              "</pre>"));
        QVERIFY(text.contains("/etc/udev/rules.d/60-vitrine-usb.rules"));
        QVERIFY(text.contains("No data transfer"));

        issue.holders = {Holder{300, "qemu-system-x86_64", Kind::Qemu, "fedora-kde6e", true}};
        QVERIFY(UsbGuardText::explanation({issue}).contains("fedora-kde6e passes it through too"));
        issue.holders = {Holder{301, "qemu-system-x86_64", Kind::Qemu, "handmade", false}};
        QVERIFY(UsbGuardText::explanation({issue})
                    .contains("handmade, a VM this Vitrine does not run, has it too (pid 301)"));
        issue.holders = {Holder{302, "qemu-system-x86_64", Kind::Qemu, {}, false}};
        QVERIFY(UsbGuardText::explanation({issue})
                    .contains("A QEMU this Vitrine does not run has it too (pid 302)"));
        issue.holders = {Holder{555, "steam", Kind::Other, {}, false}};
        QVERIFY(UsbGuardText::explanation({issue}).contains("Quit steam, or end it with "
                                                           "<nobr>kill 555</nobr>."));
        issue.holders.clear();
        issue.claimedElsewhere = true;
        QVERIFY(UsbGuardText::explanation({issue}).contains("<nobr>sudo lsof /dev/bus/usb/099/002</nobr>"));

        UsbGuard::Issue stick = issue;
        stick.claimedElsewhere = false;
        stick.device = host("99-2");
        stick.holders = {Holder{4190001, "adb", Kind::Adb, {}, false}};
        QCOMPARE(UsbGuardText::lines({issue, stick}),
                 QString("A program of another user, or of the system, claims vivo X200 Ultra, "
                         "which fedora-gnome1 passes through.\n"
                         "adb (pid 4190001) holds SanDisk Cruzer Blade, which fedora-gnome1 passes "
                         "through."));
        QVERIFY(UsbGuardText::explanation({}).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestUsbGuard)
#include "test_usbguard.moc"
