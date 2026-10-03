// SPDX-License-Identifier: GPL-2.0-or-later
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include "core/vmhardware.h"

using namespace VmConfig;

/* The display lines of the user's launch script */
static const char kScript[] = "-accel kvm,honor-guest-pat=on\n"
                              "-machine q35,memory-backend=mem\n"
                              "-device virtio-gpu-gl,hostmem=8G,blob=on,drm_native_context=on\n"
                              "-vga none\n"
                              "-display sdl,gl=on\n";

/* What a new VM gets */
static const char kTemplate[] = "-machine q35,memory-backend=mem\n"
                                "-accel kvm\n"
                                "-device virtio-vga-gl\n"
                                "-display sdl,gl=on\n";

class TestVmHardware : public QObject
{
    Q_OBJECT

    static QString text(const ArgsFile &args) { return args.toText(); }

private slots:
    void accelerator()
    {
        ArgsFile a = ArgsFile::parse("-machine q35,accel=kvm\n-m 1G\n");

        QCOMPARE(accel(a), "kvm");
        setAccelProperty(a, "honor-guest-pat", "on");
        QCOMPARE(text(a), "-machine q35\n-accel kvm,honor-guest-pat=on\n-m 1G\n");
        QCOMPARE(accelProperty(a, "honor-guest-pat"), "on");
        setAccelProperty(a, "honor-guest-pat", {});
        QCOMPARE(text(a), "-machine q35\n-accel kvm\n-m 1G\n");

        a = ArgsFile::parse("-enable-kvm\n-machine q35\n");
        setAccelProperty(a, "honor-guest-pat", "on");
        QCOMPARE(text(a), "-machine q35\n-accel kvm,honor-guest-pat=on\n");

        a = ArgsFile::parse("-machine q35,accel=kvm:tcg\n");
        setAccelProperty(a, "honor-guest-pat", "on");
        QCOMPARE(text(a), "-machine q35\n-accel kvm,honor-guest-pat=on\n-accel tcg\n");

        /* TCG, QEMU's default: nothing to put it on */
        a = ArgsFile::parse("-machine q35\n");
        setAccelProperty(a, "honor-guest-pat", "on");
        QCOMPARE(text(a), "-machine q35\n");

        a = ArgsFile::parse("# top\n-m 1G\n");
        setMachineType(a, "q35");
        setAccel(a, "kvm");
        QCOMPARE(text(a), "# top\n-machine q35\n-accel kvm\n-m 1G\n");
        QCOMPARE(machineType(a), "q35");
    }

    void readGraphics()
    {
        Graphics g = graphics(ArgsFile::parse(kScript));
        QCOMPARE(g.kind, Graphics::Accelerated);
        QCOMPARE(g.device, "virtio-gpu-gl");
        QVERIFY(g.nativeContext);
        QVERIFY(!g.venus);
        QCOMPARE(g.hostmemMiB, 8192);
        QCOMPARE(g.display, "sdl");

        g = graphics(ArgsFile::parse(kTemplate));
        QCOMPARE(g.kind, Graphics::Accelerated);
        QVERIFY(!g.nativeContext);
        QCOMPARE(g.hostmemMiB, 0);

        const std::pair<const char *, Graphics::Kind> cases[] = {
            {"-device virtio-vga\n", Graphics::Virtio},
            {"-device virtio-gpu-pci\n-vga none\n", Graphics::Virtio},
            {"-device VGA\n", Graphics::Standard},
            {"-m 1G\n", Graphics::Standard},
            {"-vga std\n", Graphics::Standard},
            {"-vga virtio\n", Graphics::Virtio},
            {"-vga none\n", Graphics::None},
            {"-nodefaults\n", Graphics::None},
            {"-machine virt\n", Graphics::None},
            {"-vga qxl\n", Graphics::Custom},
            {"-device qxl-vga\n", Graphics::Custom},
            {"-device virtio-vga\n-device secondary-vga\n", Graphics::Custom},
            {"-vga std\n-device virtio-gpu-pci\n", Graphics::Custom},
            {"-nographic\n", Graphics::Custom},
        };
        for (const auto &[args, kind] : cases) {
            QVERIFY2(graphics(ArgsFile::parse(args)).kind == kind, args);
        }
    }

    void nativeContext()
    {
        ArgsFile a = ArgsFile::parse(kTemplate);
        Graphics g = graphics(a);

        g.nativeContext = true;
        setGraphics(a, g);
        QCOMPARE(text(a), "-machine q35,memory-backend=mem\n"
                          "-accel kvm,honor-guest-pat=on\n"
                          "-device virtio-vga-gl,drm_native_context=on,blob=on,hostmem=4G\n"
                          "-display sdl,gl=on\n");

        /* off: blob and hostmem stay, 2D blobs use them too */
        a = ArgsFile::parse(kScript);
        g = graphics(a);
        g.nativeContext = false;
        setGraphics(a, g);
        QCOMPARE(text(a), QString(kScript).replace(",drm_native_context=on", ""));

        /* a bigger window */
        a = ArgsFile::parse(kScript);
        g = graphics(a);
        g.hostmemMiB = 16384;
        setGraphics(a, g);
        QVERIFY(text(a).contains("-device virtio-gpu-gl,hostmem=16G,blob=on,"
                                 "drm_native_context=on\n"));

        a = ArgsFile::parse(kTemplate);
        g = graphics(a);
        g.venus = true;
        setGraphics(a, g);
        QVERIFY(text(a).contains("-device virtio-vga-gl,venus=on,blob=on,hostmem=4G\n"));
        QVERIFY(text(a).contains("-accel kvm\n"));      /* only native context needs it */

        /* no KVM, no honor-guest-pat */
        a = ArgsFile::parse("-machine q35\n-device virtio-vga-gl\n");
        g = graphics(a);
        g.nativeContext = true;
        setGraphics(a, g);
        QCOMPARE(text(a), "-machine q35\n"
                          "-device virtio-vga-gl,drm_native_context=on,blob=on,hostmem=4G\n");
    }

    void changeCard()
    {
        /* 3D to 2D: the same card without OpenGL */
        ArgsFile a = ArgsFile::parse(kScript);
        Graphics g = graphics(a);
        g.kind = Graphics::Virtio;
        setGraphics(a, g);
        QCOMPARE(text(a), "-accel kvm,honor-guest-pat=on\n"
                          "-machine q35,memory-backend=mem\n"
                          "-device virtio-gpu,hostmem=8G,blob=on\n"
                          "-vga none\n"
                          "-display sdl\n");

        a = ArgsFile::parse("-device virtio-vga\n-display sdl\n");
        g = graphics(a);
        g.kind = Graphics::Accelerated;
        setGraphics(a, g);
        QCOMPARE(text(a), "-device virtio-vga-gl\n-display sdl,gl=on\n");

        /* from QEMU's default VGA */
        a = ArgsFile::parse("-machine q35\n-display sdl\n");
        g = graphics(a);
        g.kind = Graphics::Accelerated;
        setGraphics(a, g);
        QCOMPARE(text(a), "-machine q35\n-device virtio-vga-gl\n-display sdl,gl=on\n");

        /* a card without VGA: QEMU's own VGA goes */
        a = ArgsFile::parse("-machine q35\n-display sdl\n");
        g = graphics(a);
        g.kind = Graphics::Virtio;
        g.device = "virtio-gpu-pci";
        setGraphics(a, g);
        QCOMPARE(text(a), "-machine q35\n-device virtio-gpu-pci\n-vga none\n-display sdl\n");

        /* virtio to VGA: the virtio properties go */
        a = ArgsFile::parse("-device virtio-vga,max_outputs=2,id=gpu\n-display gtk,gl=on\n");
        g = graphics(a);
        g.kind = Graphics::Standard;
        setGraphics(a, g);
        QCOMPARE(text(a), "-device VGA,id=gpu\n-display gtk\n");

        a = ArgsFile::parse(kTemplate);
        g = graphics(a);
        g.kind = Graphics::None;
        setGraphics(a, g);
        QCOMPARE(text(a), "-machine q35,memory-backend=mem\n-accel kvm\n-vga none\n"
                          "-display sdl\n");
        QCOMPARE(graphics(a).kind, Graphics::None);

        /* ARM's virt: PCI cards, no VGA to turn off */
        a = ArgsFile::parse("-machine virt,gic-version=max\n-display sdl\n");
        g = graphics(a);
        QCOMPARE(g.kind, Graphics::None);
        g.kind = Graphics::Accelerated;
        setGraphics(a, g);
        QCOMPARE(text(a), "-machine virt,gic-version=max\n-device virtio-gpu-gl-pci\n"
                          "-display sdl,gl=on\n");
    }

    void window()
    {
        ArgsFile a = ArgsFile::parse("-device virtio-vga-gl\n-display sdl,gl=on,grab-mod=rctrl\n");
        Graphics g = graphics(a);

        g.display = "gtk";
        setGraphics(a, g);
        QCOMPARE(text(a), "-device virtio-vga-gl\n-display gtk,gl=on\n");
        g.display = "none";
        setGraphics(a, g);
        QCOMPARE(text(a), "-device virtio-vga-gl\n-display none\n");

        a = ArgsFile::parse("-device virtio-vga-gl\n");
        g = graphics(a);
        g.display = "sdl";
        setGraphics(a, g);
        QCOMPARE(text(a), "-device virtio-vga-gl\n-display sdl,gl=on\n");

        /* a card set by hand stays as it is, the window may change */
        a = ArgsFile::parse("-device qxl-vga,vgamem_mb=64\n-display sdl\n");
        g = graphics(a);
        QCOMPARE(g.kind, Graphics::Custom);
        g.display = "gtk";
        setGraphics(a, g);
        QCOMPARE(text(a), "-device qxl-vga,vgamem_mb=64\n-display gtk\n");
    }

    void unchanged()
    {
        for (const char *args :
             {kScript, kTemplate, "-vga none\n-display none\n", "-device qxl-vga\n",
              "-machine virt\n-device virtio-gpu-pci\n-display gtk,gl=on\n",
              "-device virtio-gpu-gl-pci,venus=on,blob=on,hostmem=4G\n"}) {
            ArgsFile a = ArgsFile::parse(args);
            setGraphics(a, graphics(a));
            QCOMPARE(text(a), QString(args));
        }
    }

    void readDisks()
    {
        const ArgsFile a = ArgsFile::parse(
            "-drive if=pflash,format=raw,readonly=on,file=OVMF_CODE.fd\n"
            "-hda cachyos.qcow2\n"
            "-cdrom /isos/x.iso\n"
            "-drive file=disk.qcow2,format=qcow2,if=virtio,discard=unmap\n"
            "-drive file=win.iso,media=cdrom,readonly=on\n"
            "-drive file=d.qcow2,if=none,id=d0\n"
            "-device virtio-blk-pci,drive=d0,bootindex=2\n"
            "-blockdev driver=file,filename=/b.raw,node-name=f0\n");
        const QList<Disk> list = disks(a);

        QCOMPARE(list.size(), 6);
        QCOMPARE(list[0].file, "cachyos.qcow2");
        QCOMPARE(list[0].bus, Disk::Sata);
        QVERIFY(!list[0].cdrom);
        QVERIFY(list[1].cdrom);
        QCOMPARE(list[2].bus, Disk::Virtio);
        QCOMPARE(list[2].format, "qcow2");
        QVERIFY(list[3].cdrom);
        QCOMPARE(list[3].bus, Disk::Sata);
        QCOMPARE(list[4].deviceLine, 6);
        QCOMPARE(list[4].bootindex, 2);
        QCOMPARE(list[4].bus, Disk::Virtio);
        QVERIFY(!list[5].editable);
    }

    void editDisks()
    {
        ArgsFile a = ArgsFile::parse("-drive if=pflash,unit=0,file=CODE.fd\n"
                                     "-drive file=disk.qcow2,format=qcow2,if=virtio,discard=unmap\n"
                                     "-netdev user,id=net0\n");

        addDisk(a, "data,2.qcow2", Disk::Virtio);
        addDisk(a, "/imgs/win.img", Disk::Sata);
        addCdrom(a, {});
        QCOMPARE(text(a), "-drive if=pflash,unit=0,file=CODE.fd\n"
                          "-drive file=disk.qcow2,format=qcow2,if=virtio,discard=unmap\n"
                          "-drive file=data,,2.qcow2,format=qcow2,if=virtio,discard=unmap\n"
                          "-drive file=/imgs/win.img,format=raw,if=ide,discard=unmap\n"
                          "-drive media=cdrom,readonly=on\n"
                          "-netdev user,id=net0\n");

        QList<Disk> list = disks(a);
        QCOMPARE(list.size(), 4);
        setDisc(a, list[3], "/isos/fedora.iso");
        QVERIFY(text(a).contains("-drive media=cdrom,readonly=on,file=/isos/fedora.iso\n"));
        setDisc(a, disks(a)[3], {});
        QVERIFY(text(a).contains("-drive media=cdrom,readonly=on\n"));
        removeDisk(a, disks(a)[1]);
        QCOMPARE(disks(a).size(), 3);
        QVERIFY(!text(a).contains("data,,2"));

        /* -cdrom ejected is an empty drive in its place */
        a = ArgsFile::parse("-cdrom x.iso\n");
        setDisc(a, disks(a)[0], {});
        QCOMPARE(text(a), "-drive index=2,media=cdrom,readonly=on\n");
        QVERIFY(disks(a)[0].cdrom);

        /* the drive and its device go together */
        a = ArgsFile::parse("-drive file=d.qcow2,if=none,id=d0\n-m 1G\n"
                            "-device virtio-blk-pci,drive=d0\n");
        removeDisk(a, disks(a)[0]);
        QCOMPARE(text(a), "-m 1G\n");
    }

    void armDisks()
    {
        /* virt has no IDE: the CD goes on virtio-scsi */
        ArgsFile a = ArgsFile::parse("-machine virt\n"
                                     "-drive file=disk.qcow2,format=qcow2,if=virtio\n");
        addCdrom(a, "/isos/fedora.iso");
        addCdrom(a, {});
        QCOMPARE(text(a), "-machine virt\n"
                          "-drive file=disk.qcow2,format=qcow2,if=virtio\n"
                          "-device virtio-scsi-pci,id=scsi0\n"
                          "-drive file=/isos/fedora.iso,media=cdrom,readonly=on,if=none,id=cd0\n"
                          "-device scsi-cd,drive=cd0,bus=scsi0.0\n"
                          "-drive media=cdrom,readonly=on,if=none,id=cd1\n"
                          "-device scsi-cd,drive=cd1,bus=scsi0.0\n");
        const QList<Disk> list = disks(a);
        QCOMPARE(list.size(), 3);
        QCOMPARE(list[1].bus, Disk::Scsi);
        QVERIFY(list[1].cdrom);
        QCOMPARE(list[1].file, "/isos/fedora.iso");
    }

    void firmware()
    {
        QTemporaryDir none;
        const QStringList dirs{none.path()};

        QCOMPARE(firmwareKind(ArgsFile::parse("-m 1G\n"), dirs), FirmwareKind::Bios);
        QCOMPARE(firmwareKind(ArgsFile::parse("-bios my.fd\n"), dirs), FirmwareKind::Custom);
        QCOMPARE(firmwareKind(ArgsFile::parse("-machine q35,pflash0=code\n"), dirs),
                 FirmwareKind::Custom);
        QCOMPARE(firmwareKind(ArgsFile::parse(
                                  "-drive if=pflash,format=qcow2,unit=0,readonly=on,"
                                  "file=OVMF_CODE_4M.qcow2\n"
                                  "-drive if=pflash,format=qcow2,unit=1,file=OVMF_VARS_4M.qcow2\n"),
                              dirs),
                 FirmwareKind::Uefi);
        QCOMPARE(firmwareKind(ArgsFile::parse("-drive if=pflash,unit=0,readonly=on,"
                                              "file=OVMF_CODE_4M.secboot.qcow2\n"),
                              dirs),
                 FirmwareKind::UefiSecureBoot);

        /* by the descriptors, when they know the file */
        QFile json(none.filePath("50-custom.json"));
        QVERIFY(json.open(QIODevice::WriteOnly));
        json.write(R"({"interface-types": ["uefi"], "features": ["secure-boot"],
            "mapping": {"device": "flash", "mode": "split",
                        "executable": {"filename": "/fw/MY_CODE.fd", "format": "raw"},
                        "nvram-template": {"filename": "/fw/MY_VARS.fd", "format": "raw"}},
            "targets": [{"architecture": "x86_64", "machines": ["pc-q35-*"]}]})");
        json.close();
        QCOMPARE(firmwareKind(ArgsFile::parse("-drive if=pflash,unit=0,readonly=on,"
                                              "file=MY_CODE.fd\n"),
                              dirs),
                 FirmwareKind::UefiSecureBoot);

        ArgsFile a = ArgsFile::parse("-machine q35,smm=on\n"
                                     "-drive if=pflash,unit=0,readonly=on,file=C.fd\n"
                                     "-drive if=pflash,unit=1,file=V.fd\n"
                                     "-global driver=cfi.pflash01,property=secure,value=on\n"
                                     "-m 1G\n");
        useBios(a);
        QCOMPARE(text(a), "-machine q35,smm=on\n-m 1G\n");
        QCOMPARE(firmwareKind(a, dirs), FirmwareKind::Bios);
    }

    void bootMenuOption()
    {
        ArgsFile a = ArgsFile::parse("-machine q35\n-accel kvm\n-m 1G\n");

        QVERIFY(!bootMenu(a));
        setBootMenu(a, true);
        QCOMPARE(text(a), "-machine q35\n-accel kvm\n-boot menu=on\n-m 1G\n");
        QVERIFY(bootMenu(a));
        setBootMenu(a, false);
        QCOMPARE(text(a), "-machine q35\n-accel kvm\n-m 1G\n");

        a = ArgsFile::parse("-boot order=d,menu=on\n");
        setBootMenu(a, false);
        QCOMPARE(text(a), "-boot order=d\n");
    }

    void bootDevice()
    {
        ArgsFile a = ArgsFile::parse("-drive file=disk.qcow2,format=qcow2,if=virtio,discard=unmap\n"
                                     "-drive file=x.iso,media=cdrom,readonly=on\n"
                                     "-netdev user,id=net0\n"
                                     "-device virtio-net-pci,netdev=net0\n");

        QCOMPARE(firstBootDevice(a), BootDevice::Default);
        QVERIFY(setFirstBootDevice(a, BootDevice::Disk));
        QCOMPARE(text(a), "-drive file=disk.qcow2,format=qcow2,discard=unmap,if=none,id=disk0\n"
                          "-device virtio-blk-pci,drive=disk0,bootindex=1\n"
                          "-drive file=x.iso,media=cdrom,readonly=on\n"
                          "-netdev user,id=net0\n"
                          "-device virtio-net-pci,netdev=net0\n");
        QCOMPARE(firstBootDevice(a), BootDevice::Disk);

        QVERIFY(setFirstBootDevice(a, BootDevice::Cdrom));
        QCOMPARE(text(a), "-drive file=disk.qcow2,format=qcow2,discard=unmap,if=none,id=disk0\n"
                          "-device virtio-blk-pci,drive=disk0\n"
                          "-drive file=x.iso,media=cdrom,readonly=on,if=none,id=cd0\n"
                          "-device ide-cd,drive=cd0,bootindex=1\n"
                          "-netdev user,id=net0\n"
                          "-device virtio-net-pci,netdev=net0\n");
        QCOMPARE(firstBootDevice(a), BootDevice::Cdrom);

        QVERIFY(setFirstBootDevice(a, BootDevice::Network));
        QVERIFY(text(a).contains("-device virtio-net-pci,netdev=net0,bootindex=1\n"));
        QVERIFY(text(a).contains("-device ide-cd,drive=cd0\n"));
        QCOMPARE(firstBootDevice(a), BootDevice::Network);

        QVERIFY(setFirstBootDevice(a, BootDevice::Default));
        QVERIFY(!text(a).contains("bootindex"));

        /* -hda, and -boot order, which OVMF does not follow */
        a = ArgsFile::parse("-hda cachyos.qcow2\n-boot order=d,menu=on\n");
        QCOMPARE(firstBootDevice(a), BootDevice::Cdrom);
        QVERIFY(setFirstBootDevice(a, BootDevice::Disk));
        QCOMPARE(text(a), "-drive file=cachyos.qcow2,if=none,id=disk0\n"
                          "-device ide-hd,drive=disk0,bootindex=1\n"
                          "-boot menu=on\n");

        /* nothing to boot from */
        a = ArgsFile::parse("-m 1G\n");
        QVERIFY(!setFirstBootDevice(a, BootDevice::Network));
        QVERIFY(!setFirstBootDevice(a, BootDevice::Cdrom));
    }
};

QTEST_APPLESS_MAIN(TestVmHardware)
#include "test_vmhardware.moc"
