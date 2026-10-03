// SPDX-License-Identifier: GPL-2.0-or-later
#include <QTest>

#include "core/vmconfig.h"

using namespace VmConfig;

class TestVmConfig : public QObject
{
    Q_OBJECT

private slots:
    void sizes()
    {
        QCOMPARE(parseSize("16G", 1), 16LL << 30);
        QCOMPARE(parseSize("4096", 1LL << 20), 4096LL << 20);
        QCOMPARE(parseSize("512M", 1), 512LL << 20);
        QCOMPARE(parseSize("1.5G", 1), 3LL << 29);
        QCOMPARE(parseSize("junk", 1), -1);
        QCOMPARE(formatMiB(16384), "16G");
        QCOMPARE(formatMiB(1536), "1536M");
    }

    void name()
    {
        ArgsFile a = ArgsFile::parse("# top\n-m 1G\n");

        QCOMPARE(VmConfig::name(a), "");
        setName(a, "win,11");
        QCOMPARE(a.toText(), "# top\n-name win,,11\n-m 1G\n");
        QCOMPARE(VmConfig::name(a), "win,11");
        a = ArgsFile::parse("-name guest=x,process=y\n");
        setName(a, "z");
        QCOMPARE(a.toText(), "-name guest=z,process=y\n");
    }

    void memoryFromM()
    {
        ArgsFile a = ArgsFile::parse("-m 4096\n");

        QCOMPARE(memoryMiB(a), 4096);
        setMemoryMiB(a, 8192);
        QCOMPARE(a.toText(), "-m 8G\n");
        a = ArgsFile::parse("-m size=2G,maxmem=8G\n");
        QCOMPARE(memoryMiB(a), 2048);
        setMemoryMiB(a, 3072);
        QCOMPARE(a.toText(), "-m size=3G,maxmem=8G\n");
    }

    void memoryFromBackend()
    {
        ArgsFile a = ArgsFile::parse(
            "-object memory-backend-memfd,id=ram0,size=16G\n"
            "-machine q35,memory-backend=ram0\n");

        QCOMPARE(memoryMiB(a), 16384);
        QVERIFY(hasSharedMemory(a));     /* memfd shares by default */
        setMemoryMiB(a, 8192);
        QCOMPARE(a.lines[0].value, "memory-backend-memfd,id=ram0,size=8G");
        QCOMPARE(a.indexOf("m"), -1);    /* the backend sets the size */
    }

    void useSharedMemoryAddsBackend()
    {
        ArgsFile a = ArgsFile::parse("-machine q35\n-m 4G\n");

        QVERIFY(!hasSharedMemory(a));
        useSharedMemory(a);
        QVERIFY(hasSharedMemory(a));
        QCOMPARE(memoryMiB(a), 4096);
        QCOMPARE(a.toText(), "-machine q35,memory-backend=mem\n-m 4G\n"
                             "-object memory-backend-memfd,id=mem,size=4G,share=on\n");
    }

    void useSharedMemoryConvertsRam()
    {
        ArgsFile a = ArgsFile::parse(
            "-object memory-backend-ram,id=mem,size=2G\n"
            "-M q35,memory-backend=mem\n");

        QVERIFY(!hasSharedMemory(a));
        useSharedMemory(a);
        QCOMPARE(a.lines[0].value, "memory-backend-memfd,id=mem,size=2G,share=on");
    }

    void cpus()
    {
        ArgsFile a = ArgsFile::parse("-smp 16,sockets=1,cores=8,threads=2\n"
                                     "-cpu host,topoext=on\n");
        Cpus c = VmConfig::cpus(a);

        QCOMPARE(c.count, 16);
        QCOMPARE(c.cores, 8);
        QCOMPARE(c.model, "host");
        c.count = 8;
        c.cores = 4;
        c.model = "max";
        setCpus(a, c);
        QCOMPARE(a.toText(), "-smp 8,sockets=1,cores=4,threads=2\n"
                             "-cpu max,topoext=on\n");
        c.model.clear();
        c.sockets = c.threads = 0;
        setCpus(a, c);
        QCOMPARE(a.toText(), "-smp 8,cores=4\n");

        a = ArgsFile::parse("-smp cores=4,threads=2\n");
        QCOMPARE(VmConfig::cpus(a).count, 8);
    }

    void cpuFeature()
    {
        ArgsFile a = ArgsFile::parse("-cpu host\n");

        enableCpuFeature(a, "topoext");
        QCOMPARE(a.toText(), "-cpu host,topoext=on\n");
        for (const char *line : {"-cpu host,topoext=off\n", "-cpu host,-topoext\n",
                                 "-cpu host,+topoext\n"}) {
            a = ArgsFile::parse(line);
            enableCpuFeature(a, "topoext");
            QCOMPARE(a.toText(), QString(line));
        }
        a = ArgsFile::parse("-m 1G\n");
        enableCpuFeature(a, "topoext");
        QCOMPARE(a.toText(), "-m 1G\n");
    }

    void shares()
    {
        ArgsFile a = ArgsFile::parse(
            "-m 1G\n"
            "#share tag=pub,path=/home/b/Public,x-future=1\n"
            "#share tag=old,path=/old\n");
        QList<Share> list = VmConfig::shares(a);

        QCOMPARE(list.size(), 2);
        QCOMPARE(list[0].path, "/home/b/Public");
        QCOMPARE(list[0].cache, "auto");
        list[0].cache = "always";
        list[0].readonly = true;
        list.removeLast();
        list.append({"new", "/a,b", "never", false});
        setShares(a, list);
        QCOMPARE(a.toText(),
                 "-m 1G\n"
                 "#share tag=pub,path=/home/b/Public,x-future=1,cache=always,readonly=on\n"
                 "#share tag=new,path=/a,,b,cache=never\n");
        QCOMPARE(a.argv(), QStringList({"-m", "1G"}));

        /* where the guest mounts it */
        list = VmConfig::shares(a);
        QVERIFY(list[0].mount.isEmpty());
        list[0].mount = "/home/me/Public";
        setShares(a, list);
        QVERIFY(a.toText().contains("readonly=on,mount=/home/me/Public\n"));
        QCOMPARE(VmConfig::shares(a)[0].mount, "/home/me/Public");
        list[0].mount.clear();
        setShares(a, list);
        QVERIFY(!a.toText().contains("mount="));
    }

    void pci()
    {
        ArgsFile a = ArgsFile::parse(
            "-device vfio-pci,host=03:00.0,x-vga=on\n"
            "-device qemu-xhci\n"
            "-device vfio-pci,host=0000:04:00.0\n");

        QCOMPARE(pciPassthrough(a),
                 QStringList({"0000:03:00.0", "0000:04:00.0"}));
        setPciPassthrough(a, {"0000:03:00.0", "0000:05:00.1"});
        QCOMPARE(a.toText(),
                 "-device vfio-pci,host=03:00.0,x-vga=on\n"
                 "-device qemu-xhci\n"
                 "-device vfio-pci,host=0000:05:00.1\n");
    }

    void files()
    {
        ArgsFile a = ArgsFile::parse(
            "-drive if=pflash,format=raw,readonly=on,file=OVMF_CODE.fd\n"
            "-hda /vm/disk.qcow2\n"
            "-chardev socket,id=s,path=/run/sock\n"
            "-trace events=x,file=trace.log\n"
            "-object memory-backend-file,id=m,mem-path=/dev/hugepages,size=1G\n"
            "-object filter-dump,id=d,netdev=n,file=dump.pcap\n"
            "-netdev tap,id=n,script=no\n"
            "-drive file=nbd://host/disk\n"
            "#share tag=t,path=/pub\n");
        const QList<FileRef> list = VmConfig::files(a);

        QCOMPARE(list.size(), 3);
        QCOMPARE(list[0].line, 0);
        QCOMPARE(list[0].key, "file");
        QCOMPARE(list[0].path, "OVMF_CODE.fd");
        QCOMPARE(list[1].key, "");
        QCOMPARE(list[1].path, "/vm/disk.qcow2");
        QCOMPARE(list[2].key, "mem-path");
        setFile(a, list[0], "/fw/OVMF,CODE.fd");
        setFile(a, list[1], "/other/disk.qcow2");
        QCOMPARE(a.lines[0].value, "if=pflash,format=raw,readonly=on,file=/fw/OVMF,,CODE.fd");
        QCOMPARE(a.lines[1].value, "/other/disk.qcow2");
    }

    void usb()
    {
        ArgsFile a = ArgsFile::parse(
            "-device qemu-xhci\n"
            "-device usb-host,vendorid=0x046d,productid=0xc52b\n");

        QVERIFY(hasUsbController(a));
        QCOMPARE(usbPassthrough(a).size(), 1);
        QCOMPARE(usbPassthrough(a)[0].vendor, 0x046d);
        setUsbPassthrough(a, {{0x046d, 0xc52b}, {0x1234, 0x0001}});
        QCOMPARE(a.toText(),
                 "-device qemu-xhci\n"
                 "-device usb-host,vendorid=0x046d,productid=0xc52b\n"
                 "-device usb-host,vendorid=0x1234,productid=0x0001\n");
        setUsbPassthrough(a, {});
        QCOMPARE(a.toText(), "-device qemu-xhci\n");
        QVERIFY(!hasUsbController(ArgsFile::parse("-m 1G\n")));
    }
};

QTEST_APPLESS_MAIN(TestVmConfig)
#include "test_vmconfig.moc"
