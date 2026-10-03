// SPDX-License-Identifier: GPL-2.0-or-later
#include <QStandardPaths>
#include <QTest>

#include "core/hostdevices.h"
#include "core/qemuinfo.h"

#include "core/vmconfig.h"
#include "core/vmhardware.h"
#include "core/vmtemplate.h"

using namespace VmTemplate;

static Options fedora(const QString &arch)
{
    Options o;
    o.name = "Fedora";
    o.memoryMiB = 8192;
    o.cpus = 4;
    o.disk = "disk.qcow2";
    o.iso = "/isos/Fedora.iso";
    o.arch = arch;
    return o;
}

static void pflash(ArgsFile &args)
{
    ArgsFile::Line line;
    line.kind = ArgsFile::Line::Option;
    line.name = "drive";
    line.value = "if=pflash,format=qcow2,unit=0,readonly=on,file=CODE.qcow2";
    args.lines << line;
}

class TestVmTemplate : public QObject
{
    Q_OBJECT

private slots:
    /* Linux on a PC: the research launcher's machine (host/run-vm.sh) */
    void x86()
    {
        Options o = fedora("x86_64");
        o.desktop = "kde";
        o.threadsPerCore = 2;
        o.sshPort = 2222;
        o.passt = true;
        const ArgsFile args = build(o, pflash);
        /* AMD */
        const QString cpu = HostDevices::cpuHasFlag("topoext") ? "-cpu host,topoext=on\n"
                                                               : "-cpu host\n";

        QCOMPARE(args.toText(),
                 QString("# The QEMU command line of this VM, one option per line.\n"
                 "# Lines starting with # are comments; #share lines are shared folders.\n"
                 "\n"
                 "-name Fedora,debug-threads=on\n"
                 "#guest linux,desktop=kde\n"
                 "\n"
                 "# System\n"
                 "-machine q35,memory-backend=mem,dump-guest-core=off\n"
                 "-accel kvm,honor-guest-pat=on\n") + cpu +
                 "-smp 4,sockets=1,cores=2,threads=2\n"
                 "-object memory-backend-memfd,id=mem,size=8G,share=on\n"
                 "\n"
                 "# Firmware\n"
                 "-drive if=pflash,format=qcow2,unit=0,readonly=on,file=CODE.qcow2\n"
                 "\n"
                 "# Display\n"
                 "-vga none\n"
                 "-device virtio-gpu-gl-pci,hostmem=4G,blob=on,drm_native_context=on,venus=off,"
                 "x-host-vblank=on,x-vblank-lead=3000,x-vblank-lead-auto=on\n"
                 "-display dbus,p2p=yes,gl=on\n"
                 "\n"
                 "# Input\n"
                 "-device virtio-keyboard-pci\n"
                 "-device virtio-tablet-pci\n"
                 "\n"
                 "# Storage\n"
                 "-object iothread,id=iodisk\n"
                 "-drive file=disk.qcow2,format=qcow2,if=none,id=disk0,discard=unmap\n"
                 "-device virtio-blk-pci,drive=disk0,iothread=iodisk\n"
                 "-drive file=/isos/Fedora.iso,media=cdrom,readonly=on\n"
                 "\n"
                 "# Network\n"
                 "-netdev passt,id=net0,vhost-user=on,tcp-ports=127.0.0.1/2222:22\n"
                 "-device virtio-net-pci,netdev=net0\n"
                 "\n"
                 "# Sound\n"
                 "-audio driver=pipewire,model=virtio\n"
                 "\n"
                 "# USB\n"
                 "-device qemu-xhci\n"
                 "\n"
                 "# Clipboard sharing, with spice-vdagent in the guest\n"
                 "-device virtio-serial-pci\n"
                 "-chardev qemu-vdagent,id=vdagent0,name=vdagent,clipboard=on,mouse=off\n"
                 "-device virtserialport,chardev=vdagent0,name=com.redhat.spice.0\n"
                 "\n"
                 "# Random numbers for the guest's kernel\n"
                 "-device virtio-rng-pci\n");
        QVERIFY(hasBios("x86_64"));
        QVERIFY(hasVga("x86_64"));

        /* the settings pages read it back */
        QCOMPARE(VmConfig::name(args), "Fedora");
        QCOMPARE(VmConfig::guest(args).os, "linux");
        QCOMPARE(VmConfig::guest(args).desktop, "kde");
        QCOMPARE(VmConfig::memoryMiB(args), 8192);
        QVERIFY(VmConfig::hasSharedMemory(args));
        const VmConfig::Cpus cpus = VmConfig::cpus(args);
        QCOMPARE(cpus.count, 4);
        QCOMPARE(cpus.threads, 2);
        const VmConfig::Graphics g = VmConfig::graphics(args);
        QCOMPARE(g.kind, VmConfig::Graphics::Accelerated);
        QCOMPARE(g.device, "virtio-gpu-gl-pci");
        QVERIFY(g.nativeContext);
        QCOMPARE(g.hostmemMiB, 4096);
        QVERIFY(VmConfig::screen(args) == VmConfig::Screen::Embedded);
        QCOMPARE(VmConfig::accelProperty(args, "honor-guest-pat"), "on");
        const VmConfig::Network net = VmConfig::network(args);
        QCOMPARE(net.kind, VmConfig::Network::Nat);
        QCOMPARE(net.backend, "passt");
        QCOMPARE(net.sshPort, 2222);
        const QList<VmConfig::Disk> disks = VmConfig::disks(args);
        QCOMPARE(disks.size(), 2);
        QCOMPARE(disks[0].bus, VmConfig::Disk::Virtio);
        QVERIFY(disks[0].editable);
        QVERIFY(disks[1].cdrom);
        QVERIFY(VmConfig::hasUsbController(args));
    }

    /* QEMU's user-mode network, no forward, odd processors, no disk */
    void x86Variants()
    {
        Options o = fedora("x86_64");
        o.cpus = 3;
        o.threadsPerCore = 2;
        o.disk.clear();
        o.iso.clear();
        const QString text = build(o).toText();

        QVERIFY(text.contains("-smp 3,sockets=1,cores=3,threads=1\n"));
        /* one thread per core: no topoext */
        QVERIFY(text.contains("-cpu host\n"));
        QVERIFY(text.contains("# Network\n-netdev user,id=net0\n-device virtio-net-pci,netdev=net0\n"));
        QVERIFY(!text.contains("# Storage"));
        QVERIFY(!text.contains("iothread"));
        QVERIFY(!text.contains("# Firmware"));
        QVERIFY(text.contains("#guest linux\n"));

        o.sshPort = 2232;
        QVERIFY(build(o).toText().contains(
            "-netdev user,id=net0,hostfwd=tcp:127.0.0.1:2232-:22\n"));
        QCOMPARE(VmConfig::network(build(o)).sshPort, 2232);

        /* an existing raw image */
        o.disk = "/images/guest.img";
        QVERIFY(build(o).toText().contains(
            "-drive file=/images/guest.img,format=raw,if=none,id=disk0,discard=unmap\n"));
    }

    /* A QEMU without vitrine's patches refuses what it does not know */
    void plainQemu()
    {
        Options o = fedora("x86_64");
        /* virtio-gpu-gl-pci of QEMU 10.2 */
        o.gpuProperties = {"addr", "blob", "hostmem", "max_hostmem", "venus", "xres", "yres"};
        const ArgsFile args = build(o);
        const QString text = args.toText();

        QVERIFY(text.contains("-device virtio-gpu-gl-pci,hostmem=4G,blob=on,venus=off\n"));
        QVERIFY(text.contains("-accel kvm\n"));
        QVERIFY(!text.contains("x-"));
        QVERIFY(!text.contains("honor-guest-pat"));
        QVERIFY(!VmConfig::graphics(args).nativeContext);

        /* vitrine's */
        o.gpuProperties << "drm_native_context" << "x-host-vblank" << "x-vblank-lead"
                        << "x-vblank-lead-auto" << "x-vblank-swap-target";
        QCOMPARE(build(o).toText(), build(fedora("x86_64")).toText());
    }

    /* What the VM's QEMU tells the template and the Network page */
    void fromQemuInfo()
    {
        QemuInfo info;
        const bool installed = !QStandardPaths::findExecutable("passt").isEmpty();

        QCOMPARE(gpuProperties(nullptr), QStringList());
        QCOMPARE(gpuProperties(&info), QStringList());
        info.properties["virtio-gpu-gl-pci"] = {{"blob", "bool", {}, "off"},
                                                {"x-host-vblank", "bool", {}, "on"}};
        QCOMPARE(gpuProperties(&info), QStringList({"blob", "x-host-vblank"}));

        QCOMPARE(hasPasst(nullptr), installed);
        info.netdevs = {{"user", {}}, {"tap", {}}};
        QVERIFY(!hasPasst(&info));
        info.netdevs << QemuNamedDoc{"passt", {}};
        QCOMPARE(hasPasst(&info), installed);
    }

    void windows()
    {
        Options o = fedora("x86_64");
        o.os = Os::Windows11;
        o.graphics = Graphics::Standard;
        const QString text = build(o).toText();

        QVERIFY(text.contains("-cpu host,hv-relaxed,"));
        QVERIFY(text.contains("-rtc base=localtime\n"));
        QVERIFY(text.contains("-drive file=disk.qcow2,format=qcow2,if=ide,discard=unmap\n"));
        QVERIFY(text.contains("-device e1000e,netdev=net0\n"));
        QVERIFY(text.contains("-device virtio-vga\n"));
        QVERIFY(text.contains("#guest windows\n"));
        /* as before: no native context, QEMU's own window */
        QVERIFY(!text.contains("drm_native_context"));
        QVERIFY(text.contains("-display sdl,gl=on\n"));
        QVERIFY(text.contains("-netdev user,id=net0\n"));

        o.os = Os::Other;
        o.graphics = Graphics::Compatible;
        QVERIFY(build(o).toText().contains("#guest other\n"));
        QVERIFY(build(o).toText().contains("-device VGA\n"));
    }

    /* Asahi Linux on Apple silicon */
    void aarch64()
    {
        Options o = fedora("aarch64");
        o.nativeContext = true;
        const ArgsFile args = build(o, pflash);
        const QString text = args.toText();

        QVERIFY(text.contains("-machine virt,gic-version=max,memory-backend=mem\n"));
        QVERIFY(text.contains("-cpu host\n"));
        QVERIFY(text.contains("-device virtio-gpu-gl-pci,drm_native_context=on,blob=on,"
                              "hostmem=4G\n"));
        QVERIFY(text.contains("-accel kvm,honor-guest-pat=on\n"));
        QVERIFY(!text.contains("-vga"));
        QVERIFY(!text.contains("VGA"));
        QVERIFY(!text.contains("virtio-vga"));
        QVERIFY(!text.contains("if=ide"));
        QVERIFY(!text.contains("hda"));
        QVERIFY(!text.contains("smm"));
        QVERIFY(text.contains("-drive file=disk.qcow2,format=qcow2,if=virtio,discard=unmap\n"));
        QVERIFY(text.contains("-device virtio-scsi-pci,id=scsi0\n"
                              "-drive file=/isos/Fedora.iso,media=cdrom,readonly=on,if=none,"
                              "id=cd0\n"
                              "-device scsi-cd,drive=cd0,bus=scsi0.0\n"));
        QVERIFY(text.contains("-device virtio-sound-pci,audiodev=audio0\n"));
        QVERIFY(text.contains("-device qemu-xhci\n-device usb-kbd\n-device usb-tablet\n"));
        QVERIFY(text.contains("#guest linux\n"));
        QVERIFY(text.contains("-display sdl,gl=on\n"));
        QVERIFY(!hasBios("aarch64"));
        QVERIFY(!hasVga("aarch64"));

        /* the accessors read it back */
        const VmConfig::Graphics g = VmConfig::graphics(args);
        QCOMPARE(g.kind, VmConfig::Graphics::Accelerated);
        QVERIFY(g.nativeContext);
        const QList<VmConfig::Disk> disks = VmConfig::disks(args);
        QCOMPARE(disks.size(), 2);
        QCOMPARE(disks[1].bus, VmConfig::Disk::Scsi);
        QVERIFY(disks[1].cdrom);

        /* Windows on ARM: no VGA, no IDE, no Hyper-V flags */
        o.os = Os::Windows11;
        o.graphics = Graphics::Compatible;
        const QString win = build(o).toText();
        QVERIFY(win.contains("-device virtio-gpu-pci\n"));
        QVERIFY(win.contains("-cpu host\n"));
        QVERIFY(win.contains("if=virtio"));
        QVERIFY(win.contains("-device virtio-net-pci,netdev=net0\n"));
        QVERIFY(!win.contains("VGA"));
    }
};

QTEST_APPLESS_MAIN(TestVmTemplate)
#include "test_vmtemplate.moc"
