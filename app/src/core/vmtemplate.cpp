// SPDX-License-Identifier: GPL-2.0-or-later
#include "vmtemplate.h"

#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include "core/hostdevices.h"
#include "core/paths.h"
#include "core/qemuinfo.h"
#include "core/vmconfig.h"
#include "core/vmhardware.h"

namespace VmTemplate {

Defaults defaults(Os os)
{
    switch (os) {
    case Os::Linux:
        /* as the research launcher runs its guests */
        return {8192, 8, 64, Firmware::Uefi, Graphics::Accelerated};
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

/* The user chose vitrine's QEMU by its path, which may not answer, say out of memory */
static bool isStackQemu(const QString &binary)
{
    const QString stack = Paths::stackQemu();
    return !stack.isEmpty() && QFileInfo(binary).canonicalFilePath() == stack;
}

bool hasPasst(const QemuInfo *info, const QString &chosen)
{
    QString error;

    if (QStandardPaths::findExecutable("passt").isEmpty()) {
        return false;
    }
    /* vitrine's, built with it (host/build.sh) */
    if (chosen.isEmpty()) {
        return true;
    }
    if (info) {
        for (const QemuNamedDoc &netdev : info->netdevs) {
            if (netdev.name == "passt") {
                return true;
            }
        }
        return false;
    }
    const QStringList netdevs = QemuInfo::probeList(chosen, "netdev", &error);
    return error.isEmpty() ? netdevs.contains("passt") : isStackQemu(chosen);
}

std::optional<QStringList> gpuProperties(const QemuInfo *info, const QString &chosen)
{
    static const QString card = "virtio-gpu-gl-pci";
    QStringList names;
    QString error;

    /*
     * Vitrine's, even before it is built: the system's QEMU found in the
     * meantime is not the one the VM is for, and what it lacks, native
     * context first, would stay out of the VM for good
     */
    if (chosen.isEmpty()) {
        return std::nullopt;
    }
    if (info && (info->properties.contains(card) || !info->device(card))) {
        for (const QemuPropertyDoc &p : info->properties.value(card)) {
            names << p.name;
        }
        return names;
    }
    names = QemuInfo::probeProperties(chosen, card, &error);
    if (!error.isEmpty() && isStackQemu(chosen)) {
        return std::nullopt;
    }
    return names;
}

/* With SMT on, two threads per core */
static int hostThreadsPerCore()
{
    QFile f("/sys/devices/system/cpu/smt/active");
    return f.open(QIODevice::ReadOnly) && f.readAll().trimmed() == "1" ? 2 : 1;
}

/* The arguments, written in sections */
struct Writer {
    ArgsFile args;
    qsizetype sectionStart = 0;

    void comment(const QString &text)
    {
        ArgsFile::Line line;
        line.kind = ArgsFile::Line::Comment;
        line.text = text;
        args.lines << line;
    }
    void option(const QString &name, const QString &value = {})
    {
        ArgsFile::Line line;
        line.kind = ArgsFile::Line::Option;
        line.name = name;
        line.value = value;
        args.lines << line;
    }
    void section(const QString &title)
    {
        args.lines << ArgsFile::Line();
        sectionStart = args.lines.size();
        comment("# " + title);
    }
    void firmware(const std::function<void(ArgsFile &)> &addFirmware)
    {
        section("Firmware");
        if (addFirmware) {
            addFirmware(args);
        }
        if (args.lines.size() == sectionStart + 1) {
            /* BIOS: nothing to say */
            args.lines.resize(sectionStart - 1);
        }
    }
};

/*
 * The research launcher's card: DRM native context, blob resources in a
 * 4 GiB window, and the guest's vblank ticked 3 ms before the host's,
 * following what the host's compositor needs.  No Venus, which is off by
 * default.
 */
const QList<std::pair<QString, QString>> &cardProperties()
{
    static const QList<std::pair<QString, QString>> properties = {
        {"hostmem", "4G"},
        {"blob", "on"},
        {"drm_native_context", "on"},
        {"x-host-vblank", "on"},
        {"x-vblank-lead", "3000"},
        {"x-vblank-lead-auto", "on"},
    };
    return properties;
}

/* Those of vitrine's QEMU that @known lacks stay out, as QEMU refuses unknown properties */
static QString gpuDevice(const std::optional<QStringList> &known)
{
    QString value = "virtio-gpu-gl-pci";

    for (const auto &[key, v] : cardProperties()) {
        if (!known || known->contains(key)) {
            value += QString(",%1=%2").arg(key, v);
        }
    }
    return value;
}

/* Linux on a PC: the research launcher's machine */
static void linuxPc(Writer &w, const Options &o,
                    const std::function<void(ArgsFile &)> &addFirmware)
{
    const bool nativeContext = !o.gpuProperties ||
                               o.gpuProperties->contains("drm_native_context");
    int threads = o.threadsPerCore > 0 ? o.threadsPerCore : hostThreadsPerCore();

    if (o.cpus % threads != 0) {
        threads = 1;
    }
    /* the threads named after what they run, e.g. CPU 0/KVM */
    OptionValue name = w.args.valueAt(w.args.indexOf("name"));
    name.set("debug-threads", "on");
    w.args.setValueAt(w.args.indexOf("name"), name);

    w.section("System");
    /* no guest RAM in QEMU's core dumps */
    w.option("machine", "q35,memory-backend=mem,dump-guest-core=off");
    /* the guest's caching of its GPU mappings, which Intel GPUs need */
    w.option("accel", nativeContext ? "kvm,honor-guest-pat=on" : "kvm");
    /* AMD: the guest sees the threads of a core, and the caches they share, with topoext */
    w.option("cpu", threads > 1 && HostDevices::cpuHasFlag("topoext") ? "host,topoext=on"
                                                                      : "host");
    w.option("smp", QString("%1,sockets=1,cores=%2,threads=%3")
                        .arg(o.cpus).arg(o.cpus / threads).arg(threads));
    /* memfd RAM, which virtiofsd and passt can map */
    w.option("object", QString("memory-backend-memfd,id=mem,size=%1,share=on")
                           .arg(VmConfig::formatMiB(o.memoryMiB)));

    w.firmware(addFirmware);

    w.section("Display");
    /* no VGA beside the card: UEFI shows the boot screens on it */
    w.option("vga", "none");
    w.option("device", gpuDevice(o.gpuProperties));
    w.option("display", "dbus,p2p=yes,gl=on");

    w.section("Input");
    w.option("device", "virtio-keyboard-pci");
    w.option("device", "virtio-tablet-pci");

    if (!o.disk.isEmpty() || !o.iso.isEmpty()) {
        w.section("Storage");
        if (!o.disk.isEmpty()) {
            const QString format = VmConfig::imageFormat(o.disk);
            QString drive = "file=" + OptionValue::escape(o.disk);

            if (!format.isEmpty()) {
                drive += ",format=" + format;
            }
            /* the disk's requests off QEMU's main loop, which runs the display */
            w.option("object", "iothread,id=iodisk");
            w.option("drive", drive + ",if=none,id=disk0,discard=unmap");
            w.option("device", "virtio-blk-pci,drive=disk0,iothread=iodisk");
        }
        if (!o.iso.isEmpty()) {
            w.option("drive", "file=" + OptionValue::escape(o.iso) + ",media=cdrom,readonly=on");
        }
    }

    w.section("Network");
    w.option("nic", "none");
    VmConfig::Network net;
    net.backend = o.passt ? "passt" : "user";
    net.card = "virtio-net-pci";
    net.sshPort = o.sshPort;
    VmConfig::setNetwork(w.args, net);

    w.section("Sound");
    w.option("audio", "driver=pipewire,model=virtio");

    w.section("USB");
    w.option("device", "qemu-xhci");

    w.section("Clipboard sharing, with spice-vdagent in the guest");
    w.option("device", "virtio-serial-pci");
    w.option("chardev", "qemu-vdagent,id=vdagent0,name=vdagent,clipboard=on,mouse=off");
    w.option("device", "virtserialport,chardev=vdagent0,name=com.redhat.spice.0");

    w.section("Random numbers for the guest's kernel");
    w.option("device", "virtio-rng-pci");
}

ArgsFile build(const Options &o, const std::function<void(ArgsFile &)> &addFirmware)
{
    const bool arm = isArm(o.arch);
    const bool windows = o.os == Os::Windows11 || o.os == Os::Windows;
    /* virt has no IDE: virtio for all */
    const bool virtio = o.os == Os::Linux || arm;
    Writer w;
    ArgsFile &args = w.args;
    auto option = [&w](const QString &name, const QString &value = {}) { w.option(name, value); };
    auto section = [&w](const QString &title) { w.section(title); };

    w.comment("# The QEMU command line of this VM, one option per line.");
    w.comment("# Lines starting with # are comments; #share lines are shared folders.");
    args.lines << ArgsFile::Line();
    VmConfig::setName(args, o.name);
    VmConfig::setGuest(args, {o.os == Os::Linux ? "linux" : windows ? "windows" : "other",
                              o.os == Os::Linux ? o.desktop : QString()});

    if (o.os == Os::Linux && !arm) {
        linuxPc(w, o, addFirmware);
        return args;
    }

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

    w.firmware(addFirmware);

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
            const QString format = VmConfig::imageFormat(o.disk);
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
