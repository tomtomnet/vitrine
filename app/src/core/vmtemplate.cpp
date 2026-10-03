// SPDX-License-Identifier: GPL-2.0-or-later
#include "vmtemplate.h"

#include "core/paths.h"
#include "core/vmconfig.h"
#include "core/vmhardware.h"

namespace VmTemplate {

Defaults defaults(Os os)
{
    switch (os) {
    case Os::Linux:
        return {4096, 4, 64, Firmware::Uefi, Graphics::Accelerated};
    case Os::Windows11:
        return {8192, 4, 128, Firmware::UefiSecureBoot, Graphics::Standard};
    case Os::Windows:
        return {4096, 2, 64, Firmware::Uefi, Graphics::Standard};
    case Os::Other:
        break;
    }
    return {2048, 2, 32, Firmware::Uefi, Graphics::Compatible};
}

static bool isArm(const QString &arch)
{
    return (arch.isEmpty() ? Paths::hostArch() : arch) == "aarch64";
}

bool hasBios(const QString &arch)
{
    return !isArm(arch);
}

bool hasVga(const QString &arch)
{
    return !isArm(arch);
}

ArgsFile build(const Options &o, const std::function<void(ArgsFile &)> &addFirmware)
{
    const bool arm = isArm(o.arch);
    const bool windows = o.os == Os::Windows11 || o.os == Os::Windows;
    /* virt has no IDE: virtio for all */
    const bool virtio = o.os == Os::Linux || arm;
    ArgsFile args;
    qsizetype sectionStart = 0;

    auto comment = [&](const QString &text) {
        ArgsFile::Line line;
        line.kind = ArgsFile::Line::Comment;
        line.text = text;
        args.lines << line;
    };
    auto option = [&](const QString &name, const QString &value = {}) {
        ArgsFile::Line line;
        line.kind = ArgsFile::Line::Option;
        line.name = name;
        line.value = value;
        args.lines << line;
    };
    auto section = [&](const QString &title) {
        args.lines << ArgsFile::Line();
        sectionStart = args.lines.size();
        comment("# " + title);
    };

    comment("# The QEMU command line of this VM, one option per line.");
    comment("# Lines starting with # are comments; #share lines are shared folders.");
    args.lines << ArgsFile::Line();
    VmConfig::setName(args, o.name);

    section("System");
    option("machine", arm ? "virt,gic-version=max,memory-backend=mem"
                          : "q35,memory-backend=mem");
    option("accel", "kvm");
    option("cpu", windows && !arm
                      ? "host,hv-relaxed,hv-vapic,hv-spinlocks=0x1fff,hv-vpindex,"
                        "hv-runtime,hv-time,hv-synic,hv-stimer,hv-frequencies,"
                        "hv-tlbflush,hv-ipi"
                      : "host");
    option("smp", QString::number(o.cpus));
    /* memfd RAM, which virtiofsd can map to share folders */
    option("object", QString("memory-backend-memfd,id=mem,size=%1,share=on")
                         .arg(VmConfig::formatMiB(o.memoryMiB)));
    if (windows) {
        option("rtc", "base=localtime");
    }

    section("Firmware");
    if (addFirmware) {
        addFirmware(args);
    }
    if (args.lines.size() == sectionStart + 1) {
        /* BIOS: nothing to say */
        args.lines.resize(sectionStart - 1);
    }

    section("Display");
    switch (o.graphics) {
    case Graphics::Accelerated:
        option("device", arm ? "virtio-gpu-gl-pci" : "virtio-vga-gl");
        break;
    case Graphics::Standard:
        option("device", arm ? "virtio-gpu-pci" : "virtio-vga");
        break;
    case Graphics::Compatible:
        /* virt has no VGA */
        option("device", arm ? "virtio-gpu-pci" : "VGA");
        break;
    }
    option("display", "sdl,gl=on");
    if (o.nativeContext && o.graphics == Graphics::Accelerated) {
        VmConfig::Graphics g = VmConfig::graphics(args);
        g.nativeContext = true;
        VmConfig::setGraphics(args, g);
    }

    if (!o.disk.isEmpty() || !o.iso.isEmpty()) {
        section("Storage");
        if (!o.disk.isEmpty()) {
            const QString format = VmConfig::diskFormat(o.disk);
            QString value = "file=" + OptionValue::escape(o.disk);

            if (!format.isEmpty()) {
                value += ",format=" + format;
            }
            value += virtio ? ",if=virtio" : ",if=ide";
            value += ",discard=unmap";
            option("drive", value);
        }
        if (!o.iso.isEmpty()) {
            const QString iso = "file=" + OptionValue::escape(o.iso) + ",media=cdrom,readonly=on";
            if (arm) {
                option("device", "virtio-scsi-pci,id=scsi0");
                option("drive", iso + ",if=none,id=cd0");
                option("device", "scsi-cd,drive=cd0,bus=scsi0.0");
            } else {
                option("drive", iso);
            }
        }
    }

    section("Network");
    option("netdev", "user,id=net0");
    option("device", QString(virtio ? "virtio-net-pci" : "e1000e") + ",netdev=net0");

    section("Sound");
    option("audiodev", "pipewire,id=audio0");
    if (arm) {
        option("device", "virtio-sound-pci,audiodev=audio0");
    } else {
        option("device", "ich9-intel-hda");
        option("device", "hda-duplex,audiodev=audio0");
    }

    section("USB");
    option("device", "qemu-xhci");
    if (arm) {
        /* no PS/2 either */
        option("device", "usb-kbd");
    }
    option("device", "usb-tablet");

    section("Clipboard sharing, with spice-vdagent in the guest");
    option("device", "virtio-serial-pci");
    option("chardev", "qemu-vdagent,id=vdagent0,name=vdagent,clipboard=on,mouse=off");
    option("device", "virtserialport,chardev=vdagent0,name=com.redhat.spice.0");
    return args;
}

}
