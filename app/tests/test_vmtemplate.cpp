// SPDX-License-Identifier: GPL-2.0-or-later
#include <QTest>

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
    /* As it has always been */
    void x86()
    {
        QCOMPARE(build(fedora("x86_64"), pflash).toText(),
                 "# The QEMU command line of this VM, one option per line.\n"
                 "# Lines starting with # are comments; #share lines are shared folders.\n"
                 "\n"
                 "-name Fedora\n"
                 "\n"
                 "# System\n"
                 "-machine q35,memory-backend=mem\n"
                 "-accel kvm\n"
                 "-cpu host\n"
                 "-smp 4\n"
                 "-object memory-backend-memfd,id=mem,size=8G,share=on\n"
                 "\n"
                 "# Firmware\n"
                 "-drive if=pflash,format=qcow2,unit=0,readonly=on,file=CODE.qcow2\n"
                 "\n"
                 "# Display\n"
                 "-device virtio-vga-gl\n"
                 "-display sdl,gl=on\n"
                 "\n"
                 "# Storage\n"
                 "-drive file=disk.qcow2,format=qcow2,if=virtio,discard=unmap\n"
                 "-drive file=/isos/Fedora.iso,media=cdrom,readonly=on\n"
                 "\n"
                 "# Network\n"
                 "-netdev user,id=net0\n"
                 "-device virtio-net-pci,netdev=net0\n"
                 "\n"
                 "# Sound\n"
                 "-audiodev pipewire,id=audio0\n"
                 "-device ich9-intel-hda\n"
                 "-device hda-duplex,audiodev=audio0\n"
                 "\n"
                 "# USB\n"
                 "-device qemu-xhci\n"
                 "-device usb-tablet\n"
                 "\n"
                 "# Clipboard sharing, with spice-vdagent in the guest\n"
                 "-device virtio-serial-pci\n"
                 "-chardev qemu-vdagent,id=vdagent0,name=vdagent,clipboard=on,mouse=off\n"
                 "-device virtserialport,chardev=vdagent0,name=com.redhat.spice.0\n");
        QVERIFY(hasBios("x86_64"));
        QVERIFY(hasVga("x86_64"));
    }

    void nativeContext()
    {
        Options o = fedora("x86_64");
        o.nativeContext = true;
        const QString text = build(o).toText();

        QVERIFY(text.contains("-accel kvm,honor-guest-pat=on\n"));
        QVERIFY(text.contains("-device virtio-vga-gl,drm_native_context=on,blob=on,"
                              "hostmem=4G\n-display sdl,gl=on\n"));
        QVERIFY(VmConfig::graphics(build(o)).nativeContext);

        /* only with 3D */
        o.graphics = Graphics::Standard;
        QVERIFY(!build(o).toText().contains("drm_native_context"));
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
