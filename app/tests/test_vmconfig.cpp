// SPDX-License-Identifier: GPL-2.0-or-later
#include <QTest>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

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

        /* the model alone: -smp and the flags of -cpu stay */
        a = ArgsFile::parse("-smp 8,cores=4\n-cpu host,+avx\n");
        setCpuModel(a, "max");
        QCOMPARE(a.toText(), "-smp 8,cores=4\n-cpu max,+avx\n");
        setCpuModel(a, {});
        QCOMPARE(a.toText(), "-smp 8,cores=4\n");
        setCpuModel(a, "host");
        QCOMPARE(a.toText(), "-smp 8,cores=4\n-cpu host\n");
    }

    /* What QEMU makes of the topology keys -smp leaves out */
    void derivedTopologies()
    {
        const auto derived = [](const char *smp) {
            const Cpus d = derivedTopology(VmConfig::cpus(ArgsFile::parse(QString("-smp %1\n").arg(smp))));
            return QString("%1x%2x%3").arg(d.sockets).arg(d.cores).arg(d.threads);
        };

        /* the cores take what the others leave */
        QCOMPARE(derived("8"), "1x8x1");
        QCOMPARE(derived("8,threads=2"), "1x4x2");
        QCOMPARE(derived("8,sockets=2"), "2x4x1");
        /* then the sockets */
        QCOMPARE(derived("8,cores=4"), "2x4x1");
        QCOMPARE(derived("cpus=16,cores=4,threads=2"), "2x4x2");
        /* then the threads */
        QCOMPARE(derived("8,sockets=1,cores=4"), "1x4x2");
        QCOMPARE(derived("8,sockets=2,cores=2"), "2x2x2");
        /* all given, or none with no count */
        QCOMPARE(derived("16,sockets=1,cores=8,threads=2"), "1x8x2");
        QCOMPARE(derived("cores=4,threads=2"), "1x4x2");
        /* a count that does not divide, which QEMU refuses: as given */
        QCOMPARE(derived("6,cores=4"), "1x4x1");
        QCOMPARE(derived("3,sockets=2,threads=2"), "2x1x2");
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

    void environment()
    {
        const ArgsFile a = ArgsFile::parse(
            "#env QEMU_SDL_ZERO_COPY=0\n-m 1G\n#env EMPTY=\n#env not an assignment\n"
            "#env WITH_EQUALS=a=b\n# env COMMENT=1\n");
        const QList<EnvVar> vars = VmConfig::environment(a);

        QCOMPARE(a.argv(), QStringList({"-m", "1G"}));
        QCOMPARE(vars.size(), 3);
        QCOMPARE(vars[0].name, "QEMU_SDL_ZERO_COPY");
        QCOMPARE(vars[0].value, "0");
        QCOMPARE(vars[1].name, "EMPTY");
        QCOMPARE(vars[1].value, "");
        QCOMPARE(vars[2].name, "WITH_EQUALS");
        QCOMPARE(vars[2].value, "a=b");
        QVERIFY(isEnvAssignment("A_1=x"));
        QVERIFY(!isEnvAssignment("1A=x"));
        QVERIFY(!isEnvAssignment("A"));
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

    /* #guest: what runs in the VM, which QEMU never sees */
    void guestDirective()
    {
        ArgsFile a = ArgsFile::parse("# top\n-name Fedora\n-m 1G\n");

        QCOMPARE(guest(a).os, "");
        setGuest(a, {"linux", "kde"});
        QCOMPARE(a.toText(), "# top\n-name Fedora\n#guest linux,desktop=kde\n-m 1G\n");
        QCOMPARE(a.argv(), QStringList({"-name", "Fedora", "-m", "1G"}));
        QCOMPARE(guest(a).os, "linux");
        QCOMPARE(guest(a).desktop, "kde");

        /* other keys stay */
        a = ArgsFile::parse("-m 1G\n#guest Linux,desktop=GNOME,tools=1\n");
        QCOMPARE(guest(a).os, "linux");
        QCOMPARE(guest(a).desktop, "gnome");
        setGuest(a, {"linux", ""});
        QCOMPARE(a.toText(), "-m 1G\n#guest linux,tools=1\n");
        setGuest(a, {"", "kde"});
        QCOMPARE(a.toText(), "-m 1G\n#guest desktop=kde,tools=1\n");
        QCOMPARE(guest(a).os, "");
        setGuest(a, {});
        QCOMPARE(a.toText(), "-m 1G\n");

        /* no -name: first after the comments */
        a = ArgsFile::parse("# top\n\n-m 1G\n");
        setGuest(a, {"windows", ""});
        QCOMPARE(a.toText(), "# top\n\n#guest windows\n-m 1G\n");
        a = ArgsFile();
        setGuest(a, {"other", ""});
        QCOMPARE(a.toText(), "#guest other\n");

        /* the system, after the family; files written before it have none */
        a = ArgsFile::parse("-name F\n#guest linux,desktop=kde\n");
        QCOMPARE(guest(a).id, "");
        setGuest(a, {"linux", "kde", "fedora44"});
        QCOMPARE(a.toText(), "-name F\n#guest linux,id=fedora44,desktop=kde\n");
        QCOMPARE(guest(a).os, "linux");
        QCOMPARE(guest(a).desktop, "kde");
        QCOMPARE(guest(a).id, "fedora44");
        setGuest(a, {"linux", "kde"});
        QCOMPARE(a.toText(), "-name F\n#guest linux,desktop=kde\n");
        a = ArgsFile::parse("#guest windows,id=Win11,tools=1\n");
        QCOMPARE(guest(a).id, "win11");
        setGuest(a, {"windows", "", "win10"});
        QCOMPARE(a.toText(), "#guest windows,id=win10,tools=1\n");
        setGuest(a, {"", "", "win10"});
        QCOMPARE(a.toText(), "#guest id=win10,tools=1\n");
        QCOMPARE(guest(a).os, "");
        setGuest(a, {});
        QCOMPARE(a.toText(), "");
        /* ids with the characters options escape */
        a = ArgsFile();
        setGuest(a, {"linux", "", "nixos-25.05"});
        QCOMPARE(a.toText(), "#guest linux,id=nixos-25.05\n");
        QCOMPARE(guest(a).id, "nixos-25.05");
    }

    /* The Processors of the Hardware page: the topology keeps its shape */
    void cpuCount()
    {
        ArgsFile a = ArgsFile::parse("-smp 8,sockets=1,cores=4,threads=2\n-cpu host,+avx\n");

        setCpuCount(a, 4);
        QCOMPARE(a.toText(), "-smp 4,sockets=1,cores=2,threads=2\n-cpu host,+avx\n");
        /* odd: one thread per core */
        setCpuCount(a, 3);
        QCOMPARE(a.toText(), "-smp 3,sockets=1,cores=3,threads=1\n-cpu host,+avx\n");
        QCOMPARE(VmConfig::cpus(a).count, 3);

        /* the sockets stay if they divide it */
        a = ArgsFile::parse("-smp 8,sockets=2,cores=2,threads=2\n");
        setCpuCount(a, 12);
        QCOMPARE(a.toText(), "-smp 12,sockets=2,cores=3,threads=2\n");
        setCpuCount(a, 6);
        QCOMPARE(a.toText(), "-smp 6,sockets=1,cores=3,threads=2\n");

        /* keys not given stay out */
        a = ArgsFile::parse("-smp cpus=4,cores=4\n");
        setCpuCount(a, 6);
        QCOMPARE(a.toText(), "-smp cpus=6,cores=6\n");

        /* no topology, no -cpu touched */
        a = ArgsFile::parse("-cpu ,+avx\n-smp 2\n");
        setCpuCount(a, 6);
        QCOMPARE(a.toText(), "-cpu ,+avx\n-smp 6\n");
        a = ArgsFile::parse("-m 1G\n");
        setCpuCount(a, 2);
        QCOMPARE(a.toText(), "-m 1G\n-smp 2\n");

        /* maxcpus= or levels the topology above leaves out: as written,
           QEMU checks their product */
        for (const char *smp : {"-smp 4,maxcpus=8,sockets=1,cores=8,threads=1\n",
                                "-smp 4,maxcpus=8\n",
                                "-smp 8,sockets=1,dies=2,cores=4,threads=1\n",
                                "-smp 8,modules=2,cores=4\n"}) {
            a = ArgsFile::parse(smp);
            QVERIFY(VmConfig::cpus(a).custom);
            setCpuCount(a, 6);
            QCOMPARE(a.toText(), smp);
        }
        /* a level of 1 changes nothing */
        a = ArgsFile::parse("-smp 8,sockets=1,dies=1,cores=8\n");
        QVERIFY(!VmConfig::cpus(a).custom);
        setCpuCount(a, 4);
        QCOMPARE(a.toText(), "-smp 4,sockets=1,dies=1,cores=4\n");
    }

    /* The count QEMU runs when -smp gives none */
    void cpuCountRead()
    {
        QCOMPARE(VmConfig::cpus(ArgsFile::parse("-smp maxcpus=8\n")).count, 8);
        QCOMPARE(VmConfig::cpus(ArgsFile::parse("-smp 4,maxcpus=8\n")).count, 4);
        QCOMPARE(VmConfig::cpus(ArgsFile::parse("-smp sockets=1,dies=2,cores=4\n")).count, 8);
        QCOMPARE(VmConfig::cpus(ArgsFile::parse("-smp sockets=2\n")).count, 2);
        QVERIFY(!VmConfig::cpus(ArgsFile::parse("-smp 8,sockets=2\n")).custom);
        QVERIFY(!VmConfig::cpus(ArgsFile::parse("-m 1G\n")).custom);
    }

    void freePorts()
    {
        const int first = freePort({});
        QVERIFY(first >= 10022);
        QVERIFY(freePort({first}) > first);
        QCOMPARE(freePort({}, 100) >= 1024, true);

        /* one in use here */
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        QVERIFY(::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0);
        QVERIFY(::listen(fd, 1) == 0);
        socklen_t size = sizeof addr;
        ::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &size);
        const int busy = ntohs(addr.sin_port);
        QVERIFY(freePort({}, busy) != busy);
        ::close(fd);
    }
};

QTEST_APPLESS_MAIN(TestVmConfig)
#include "test_vmconfig.moc"
