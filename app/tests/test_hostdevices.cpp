// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include "core/hostdevices.h"

#include <unistd.h>

class TestHostDevices : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir tmp;
    QString sys;

    void write(const QString &path, const QByteArray &data)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(data);
    }

    void link(const QString &target, const QString &path)
    {
        QDir().mkpath(target);
        QFile::remove(path);
        QVERIFY(QFile::link(target, path));
    }

    /* A PCI device the way sysfs shows it, in IOMMU group @group */
    void pci(const QString &address, const QByteArray &vendor, const QByteArray &device,
             const QByteArray &cls, const QString &driver, int group)
    {
        const QString dir = sys + "/devices/pci0000:00/" + address;
        write(dir + "/vendor", vendor + "\n");
        write(dir + "/device", device + "\n");
        write(dir + "/class", cls + "\n");
        if (!driver.isEmpty()) {
            link(sys + "/bus/pci/drivers/" + driver, dir + "/driver");
        }
        if (group >= 0) {
            const QString groupDir = sys + "/kernel/iommu_groups/" + QString::number(group);
            link(groupDir, dir + "/iommu_group");
            QDir().mkpath(groupDir + "/devices");
            link(dir, groupDir + "/devices/" + address);
        }
        QDir().mkpath(sys + "/bus/pci/devices");
        link(dir, sys + "/bus/pci/devices/" + address);
    }

    void usb(const QString &name, const QByteArray &bus, const QByteArray &dev,
             const QByteArray &vendor, const QByteArray &product, const QByteArray &cls,
             const QByteArray &manufacturer = {}, const QByteArray &productName = {})
    {
        const QString dir = sys + "/devices/usb/" + name;
        write(dir + "/busnum", bus + "\n");
        write(dir + "/devnum", dev + "\n");
        write(dir + "/idVendor", vendor + "\n");
        write(dir + "/idProduct", product + "\n");
        write(dir + "/bDeviceClass", cls + "\n");
        if (!manufacturer.isEmpty()) {
            write(dir + "/manufacturer", manufacturer + "\n");
            write(dir + "/product", productName + "\n");
        }
        QDir().mkpath(sys + "/bus/usb/devices");
        link(dir, sys + "/bus/usb/devices/" + name);
    }

    PciDevice find(const QString &address)
    {
        for (const PciDevice &d : HostDevices::pciDevices(sys)) {
            if (d.address == address) {
                return d;
            }
        }
        return {};
    }

private slots:
    void initTestCase()
    {
        sys = tmp.filePath("sys");
        /* a GPU and its audio function, behind a root port, in group 14 */
        pci("0000:00:01.0", "0x8086", "0x1901", "0x060400", "pcieport", 14);
        pci("0000:03:00.0", "0x10de", "0x2684", "0x030000", "vfio-pci", 14);
        pci("0000:03:00.1", "0x10de", "0x22ba", "0x040300", "snd_hda_intel", 14);
        pci("0000:00:02.0", "0x8086", "0x3e92", "0x030000", "i915", 2);
        write(tmp.filePath("dev/vfio/14"), "");

        usb("usb1", "1", "1", "1d6b", "0002", "09", "Linux xhci-hcd", "xHCI Host Controller");
        usb("1-1", "1", "2", "046d", "c52b", "00", "Logitech", "USB Receiver");
        usb("1-2", "1", "3", "046d", "c52b", "00");
        usb("1-1:1.0", "1", "2", "046d", "c52b", "00");
    }

    void pciDevices()
    {
        const QList<PciDevice> list = HostDevices::pciDevices(sys);
        QCOMPARE(list.size(), 4);
        QCOMPARE(list[0].address, "0000:00:01.0");

        const PciDevice gpu = find("0000:03:00.0");
        QCOMPARE(gpu.vendorId, 0x10de);
        QCOMPARE(gpu.deviceId, 0x2684);
        QCOMPARE(gpu.classCode, 0x030000u);
        QCOMPARE(gpu.driver, "vfio-pci");
        QCOMPARE(gpu.iommuGroup, 14);
        QCOMPARE(gpu.groupPeers, QStringList({"0000:00:01.0", "0000:03:00.1"}));
        QVERIFY(HostDevices::iommuEnabled(sys));
        QVERIFY(!HostDevices::iommuEnabled(tmp.filePath("nothing")));

        if (QFileInfo::exists("/usr/share/hwdata/pci.ids")) {
            QCOMPARE(gpu.vendorName, "NVIDIA Corporation");
            QCOMPARE(gpu.className, "VGA compatible controller");
            QVERIFY(gpu.displayName().startsWith("03:00.0 NVIDIA Corporation "));
        }
    }

    void problems()
    {
        PciDevice gpu = find("0000:03:00.0");
        QStringList problems = HostDevices::pciProblems(gpu, 0, sys);

        /* the audio function is still with the host; the bridge is fine */
        QCOMPARE(problems.size(), HostDevices::memlockLimit() >= 0 ? 2 : 1);
        QVERIFY(problems[0].contains("snd_hda_intel"));
        QVERIFY(problems[0].contains("03:00.1"));

        link(sys + "/bus/pci/drivers/vfio-pci", sys + "/devices/pci0000:00/0000:03:00.1/driver");
        problems = HostDevices::pciProblems(gpu, 0, sys);
        QCOMPARE(problems.size(), HostDevices::memlockLimit() >= 0 ? 1 : 0);

        const PciDevice audio = find("0000:03:00.1");
        QCOMPARE(audio.driver, "vfio-pci");

        /* the group device of the user */
        QFile::setPermissions(tmp.filePath("dev/vfio/14"), QFileDevice::Permissions());
        problems = HostDevices::pciProblems(gpu, 0, sys);
        QVERIFY(problems.first().contains(tmp.filePath("dev/vfio/14")));
        QFile::setPermissions(tmp.filePath("dev/vfio/14"),
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner);

        const PciDevice igpu = find("0000:00:02.0");
        problems = HostDevices::pciProblems(igpu, 0, sys);
        QVERIFY(problems.first().contains("i915"));

        PciDevice noIommu = igpu;
        noIommu.iommuGroup = -1;
        problems = HostDevices::pciProblems(noIommu, 0, sys);
        QCOMPARE(problems.size(), 1);
        QVERIFY(problems[0].contains("IOMMU"));
    }

    void memlock()
    {
        const qint64 limit = HostDevices::memlockLimit();
        if (limit < 0) {
            QSKIP("unlimited locked memory");
        }
        link(sys + "/bus/pci/drivers/vfio-pci", sys + "/devices/pci0000:00/0000:03:00.1/driver");
        const QStringList problems =
            HostDevices::pciProblems(find("0000:03:00.0"), 16384, sys);
        QCOMPARE(problems.size(), 1);
        QVERIFY(problems[0].contains("17.0 GiB"));
    }

    void cpuFlags()
    {
        const QString info = tmp.filePath("cpuinfo");
        QFile f(info);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("processor\t: 0\nmodel name\t: AMD Ryzen 9 9950X 16-Core Processor\n"
                "flags\t\t: fpu vme de pse topoext perfctr_core\n");
        f.close();
        QVERIFY(HostDevices::cpuHasFlag("topoext", info));
        QVERIFY(!HostDevices::cpuHasFlag("topo", info));
        QVERIFY(!HostDevices::cpuHasFlag("topoext", tmp.filePath("none")));
    }

    void usbWithoutAccess()
    {
        if (getuid() == 0) {
            QSKIP("root can open anything");
        }
        const QString dev = tmp.filePath("dev");
        QDir().mkpath(dev + "/bus/usb/001");
        for (const char *node : {"002", "003"}) {
            QFile f(dev + "/bus/usb/001/" + node);
            QVERIFY(f.open(QIODevice::WriteOnly));
        }
        /* 1-2, 046d:c52b too, on 001/003: this user cannot open it */
        QFile::setPermissions(dev + "/bus/usb/001/003", QFileDevice::Permissions());

        const QList<UsbDevice> list =
            HostDevices::usbWithoutAccess({{0x046d, 0xc52b}}, sys, dev);
        QCOMPARE(list.size(), 1);
        QCOMPARE(list[0].devNode(), "/dev/bus/usb/001/003");
        QVERIFY(HostDevices::usbWithoutAccess({{0x1234, 0x5678}}, sys, dev).isEmpty());
    }

    void usbDevices()
    {
        const QList<UsbDevice> list = HostDevices::usbDevices(sys);

        QCOMPARE(list.size(), 3);
        QCOMPARE(list[0].port, "usb1");
        QVERIFY(list[0].isHub);
        QCOMPARE(list[1].port, "1-1");
        QVERIFY(!list[1].isHub);
        QCOMPARE(list[1].vendorId, 0x046d);
        QCOMPARE(list[1].productId, 0xc52b);
        QCOMPARE(list[1].devNode(), "/dev/bus/usb/001/002");
        QCOMPARE(list[1].displayName(), "Logitech USB Receiver (046d:c52b)");
        QCOMPARE(list[2].port, "1-2");
        if (QFileInfo::exists("/usr/share/hwdata/usb.ids")) {
            QCOMPARE(list[2].manufacturer, "Logitech, Inc.");
            QCOMPARE(list[2].product, "Unifying Receiver");
        }
    }

    void system()
    {
        if (!QFileInfo::exists("/sys/bus/pci/devices")) {
            QSKIP("no sysfs");
        }
        const QList<PciDevice> list = HostDevices::pciDevices();
        QVERIFY(!list.isEmpty());
        for (const PciDevice &d : list) {
            QVERIFY(!d.vendorName.isEmpty());
            QVERIFY(d.classCode != 0 || d.vendorId != 0);
        }
        HostDevices::usbDevices();
    }
};

QTEST_APPLESS_MAIN(TestHostDevices)
#include "test_hostdevices.moc"
