// SPDX-License-Identifier: GPL-2.0-or-later
#include "vmhardware.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QHash>
#include <QSet>

#include <climits>

#include "core/firmware.h"
#include "core/vmconfig.h"

namespace VmConfig {

static QString tr(const char *text)
{
    return QCoreApplication::translate("VmConfig", text);
}

static int lastIndex(const ArgsFile &args, const QString &name)
{
    const QList<int> found = args.indexesOf(name);
    return found.isEmpty() ? -1 : found.last();
}

static ArgsFile::Line optionLine(const QString &name, const QString &value)
{
    ArgsFile::Line line;
    line.kind = ArgsFile::Line::Option;
    line.name = name;
    line.value = value;
    return line;
}

/*
 * Inserts -@name after the last line of the options @after, else at the
 * top (after the comments there) if @top, else at the end
 */
static int insertAfter(ArgsFile &args, const QStringList &after, const QString &name,
                       const QString &value, bool top = false)
{
    int at = -1;

    for (const QString &a : after) {
        at = qMax(at, lastIndex(args, a));
    }
    if (at >= 0) {
        args.lines.insert(at + 1, optionLine(name, value));
        return at + 1;
    }
    if (top) {
        at = 0;
        while (at < args.lines.size() && args.lines[at].kind != ArgsFile::Line::Option) {
            at++;
        }
        args.lines.insert(at, optionLine(name, value));
        return at;
    }
    args.lines.append(optionLine(name, value));
    return int(args.lines.size()) - 1;
}

static void setKey(ArgsFile &args, int line, const QString &key, const QString &value)
{
    OptionValue v = args.valueAt(line);
    if (value.isEmpty()) {
        v.remove(key);
    } else {
        v.set(key, value);
    }
    args.setValueAt(line, v);
}

/* Machine and accelerator */

QString machineType(const ArgsFile &args)
{
    QString type;

    for (int i : args.indexesOf("machine")) {
        const OptionValue v = args.valueAt(i);
        if (!v.implied().isEmpty()) {
            type = v.implied();
        } else if (v.has("type")) {
            type = v.get("type");
        }
    }
    return type;
}

void setMachineType(ArgsFile &args, const QString &type)
{
    const QList<int> machines = args.indexesOf("machine");
    int target = -1;

    for (int i : machines) {
        const OptionValue v = args.valueAt(i);
        if (!v.implied().isEmpty() || v.has("type")) {
            target = i;
        }
    }
    if (target < 0 && !machines.isEmpty()) {
        target = machines.first();
    }
    if (target < 0) {
        if (!type.isEmpty()) {
            insertAfter(args, {"name"}, "machine", type, true);
        }
        return;
    }

    OptionValue v = args.valueAt(target);
    if (v.implied().isEmpty() && v.has("type")) {
        v.set("type", type);
    } else {
        v.setImplied(type);
    }
    args.setValueAt(target, v);
}

QString accel(const ArgsFile &args)
{
    const int a = args.indexOf("accel");
    QString value;

    if (a >= 0) {
        const OptionValue v = args.valueAt(a);
        return v.implied().isEmpty() ? v.get("accel") : v.implied();
    }
    for (int i : args.indexesOf("machine")) {
        const OptionValue v = args.valueAt(i);
        if (v.has("accel")) {
            value = v.get("accel");
        }
    }
    if (value.isEmpty() && args.indexOf("enable-kvm") >= 0) {
        value = "kvm";
    }
    return value;
}

void setAccel(ArgsFile &args, const QString &accel)
{
    const int a = args.indexOf("accel");

    if (a >= 0) {
        OptionValue v = args.valueAt(a);
        if (v.implied().isEmpty() && v.has("accel")) {
            v.set("accel", accel);
        } else {
            v.setImplied(accel);
        }
        args.setValueAt(a, v);
        return;
    }
    for (int i : args.indexesOf("machine")) {
        OptionValue v = args.valueAt(i);
        if (v.has("accel")) {
            v.set("accel", accel);
            args.setValueAt(i, v);
            return;
        }
    }
    if (args.indexOf("enable-kvm") >= 0) {
        if (accel == "kvm") {
            return;
        }
        args.removeAll("enable-kvm");
    }
    if (!accel.isEmpty()) {
        insertAfter(args, {"name", "machine"}, "accel", accel, true);
    }
}

QString accelProperty(const ArgsFile &args, const QString &key)
{
    const int a = args.indexOf("accel");
    return a < 0 ? QString() : args.valueAt(a).get(key);
}

void setAccelProperty(ArgsFile &args, const QString &key, const QString &value)
{
    if (args.indexOf("accel") < 0) {
        /* -accel takes over from -machine accel= and -enable-kvm */
        const QStringList accels = accel(args).split(':', Qt::SkipEmptyParts);
        int at = -1;

        if (accels.isEmpty() || value.isEmpty()) {
            return;
        }
        for (int i : args.indexesOf("machine")) {
            OptionValue v = args.valueAt(i);
            if (v.has("accel")) {
                v.remove("accel");
                args.setValueAt(i, v);
            }
        }
        args.removeAll("enable-kvm");
        /* kvm:tcg: -accel kvm, then -accel tcg */
        at = insertAfter(args, {"name", "machine"}, "accel", accels.first(), true);
        for (qsizetype i = 1; i < accels.size(); i++) {
            args.lines.insert(++at, optionLine("accel", accels[i]));
        }
    }
    setKey(args, args.indexOf("accel"), key, value);
}

/* Graphics */

static const QStringList kAccelerated = {
    "virtio-vga-gl", "virtio-gpu-gl-pci", "virtio-gpu-gl", "virtio-gpu-gl-device",
};
static const QStringList kVirtio = {
    "virtio-vga", "virtio-gpu-pci", "virtio-gpu", "virtio-gpu-device",
};
static const QStringList kStandard = {"VGA", "bochs-display"};
static const QStringList kOtherCards = {
    "qxl", "qxl-vga", "cirrus-vga", "ramfb", "vmware-svga", "secondary-vga", "ati-vga",
    "vhost-user-gpu", "vhost-user-gpu-pci", "vhost-user-vga", "virtio-vga-rutabaga",
    "virtio-gpu-rutabaga", "virtio-gpu-rutabaga-pci", "virtio-gpu-rutabaga-device",
};
static const QStringList kVgaDevices = {
    "VGA", "virtio-vga", "virtio-vga-gl", "qxl-vga", "cirrus-vga", "ati-vga",
    "vhost-user-vga", "virtio-vga-rutabaga", "vmware-svga",
};
/* The same card with and without OpenGL */
static const QHash<QString, QString> k3dTo2d = {
    {"virtio-vga-gl", "virtio-vga"},
    {"virtio-gpu-gl-pci", "virtio-gpu-pci"},
    {"virtio-gpu-gl", "virtio-gpu"},
    {"virtio-gpu-gl-device", "virtio-gpu-device"},
};
/* The keys each window takes, the others go when it changes */
static const QHash<QString, QStringList> kDisplayKeys = {
    {"sdl", {"gl", "grab-mod", "show-cursor", "window-close"}},
    {"gtk",
     {"gl", "full-screen", "grab-on-hover", "show-tabs", "show-cursor", "window-close",
      "show-menubar", "zoom-to-fit", "clipboard"}},
    {"egl-headless", {"rendernode"}},
};

bool isVgaDevice(const QString &device)
{
    return kVgaDevices.contains(device);
}

QString glCounterpart(const QString &device)
{
    return k3dTo2d.contains(device) ? k3dTo2d.value(device) : k3dTo2d.key(device);
}

/* The machines without a VGA by default, e.g. virt on ARM */
static bool hasDefaultVga(const ArgsFile &args)
{
    return !machineType(args).startsWith("virt") && args.indexOf("nodefaults") < 0;
}

static QList<int> cardLines(const ArgsFile &args)
{
    QList<int> lines;

    for (int i : args.indexesOf("device")) {
        const QString driver = args.valueAt(i).implied();
        if (kAccelerated.contains(driver) || kVirtio.contains(driver) ||
            kStandard.contains(driver) || kOtherCards.contains(driver)) {
            lines << i;
        }
    }
    return lines;
}

Graphics graphics(const ArgsFile &args)
{
    const QList<int> cards = cardLines(args);
    const int vgaLine = lastIndex(args, "vga");
    const QString vga = vgaLine < 0 ? QString() : args.lines[vgaLine].value.trimmed();
    const int displayLine = lastIndex(args, "display");
    Graphics g;

    if (displayLine >= 0) {
        g.display = args.valueAt(displayLine).implied();
    }
    if (args.indexOf("nographic") >= 0) {
        g.kind = Graphics::Custom;
        g.custom = "-nographic";
        return g;
    }
    if (cards.size() > 1) {
        g.kind = Graphics::Custom;
        g.custom = tr("%1 graphics cards").arg(cards.size());
        return g;
    }
    if (cards.size() == 1) {
        const OptionValue v = args.valueAt(cards.first());

        g.device = v.implied();
        if (!vga.isEmpty() && vga != "none" && !isVgaDevice(g.device)) {
            /* the card of -vga, and this one */
            g.kind = Graphics::Custom;
            g.custom = QString("-vga %1, %2").arg(vga, g.device);
            return g;
        }
        if (kAccelerated.contains(g.device)) {
            g.kind = Graphics::Accelerated;
            g.nativeContext = v.flag("drm_native_context");
            g.venus = v.flag("venus");
        } else if (kVirtio.contains(g.device)) {
            g.kind = Graphics::Virtio;
        } else if (kStandard.contains(g.device)) {
            g.kind = Graphics::Standard;
        } else {
            g.kind = Graphics::Custom;
            g.custom = g.device;
        }
        if (v.has("hostmem")) {
            g.hostmemMiB = qMax<qint64>(parseSize(v.get("hostmem"), 1) >> 20, 0);
        }
        return g;
    }

    if (vga == "virtio") {
        g.kind = Graphics::Virtio;
    } else if (vga == "none" || (vga.isEmpty() && !hasDefaultVga(args))) {
        g.kind = Graphics::None;
    } else if (vga.isEmpty() || vga == "std") {
        g.kind = Graphics::Standard;
    } else {
        g.kind = Graphics::Custom;
        g.custom = "-vga " + vga;
    }
    return g;
}

/* Inserts after line @after, or at the end if it is -1 */
static int insertLine(ArgsFile &args, int after, const QString &name, const QString &value)
{
    if (after < 0 || after >= args.lines.size()) {
        args.lines.append(optionLine(name, value));
        return int(args.lines.size()) - 1;
    }
    args.lines.insert(after + 1, optionLine(name, value));
    return after + 1;
}

/* No -vga card besides the one of -device, or none at all */
static void noVgaCard(ArgsFile &args, int after)
{
    const int vga = lastIndex(args, "vga");

    if (vga >= 0) {
        args.lines[vga].value = "none";
    } else if (hasDefaultVga(args)) {
        insertLine(args, after, "vga", "none");
    }
}

static void setCard(ArgsFile &args, const Graphics &now, const Graphics &g)
{
    const QList<int> cards = cardLines(args);
    int line = cards.isEmpty() ? -1 : cards.first();
    /* virt on ARM has no VGA */
    const bool pciOnly = machineType(args).startsWith("virt");
    QString driver;

    if (g.kind == Graphics::None) {
        int at = qMax(lastIndex(args, "accel"), lastIndex(args, "machine"));
        if (line >= 0) {
            /* where the card was */
            args.removeAt(line);
            at = line - 1;
        } else if (lastIndex(args, "display") >= 0) {
            at = lastIndex(args, "display") - 1;
        }
        noVgaCard(args, at);
        return;
    }
    switch (g.kind) {
    case Graphics::Accelerated:
        driver = kAccelerated.contains(g.device) ? g.device
                 : now.kind == Graphics::Accelerated ? now.device
                 : now.kind == Graphics::Virtio && !now.device.isEmpty() ? k3dTo2d.key(now.device)
                 : pciOnly ? "virtio-gpu-gl-pci" : "virtio-vga-gl";
        break;
    case Graphics::Virtio:
        driver = kVirtio.contains(g.device) ? g.device
                 : now.kind == Graphics::Virtio && !now.device.isEmpty() ? now.device
                 : now.kind == Graphics::Accelerated ? k3dTo2d.value(now.device)
                 : pciOnly ? "virtio-gpu-pci" : "virtio-vga";
        break;
    default:
        driver = kStandard.contains(g.device) ? g.device
                 : now.kind == Graphics::Standard && !now.device.isEmpty() ? now.device : "VGA";
        break;
    }
    if (driver.isEmpty()) {
        driver = pciOnly ? "virtio-gpu-pci" : "virtio-vga";
    }

    OptionValue v = line >= 0 ? args.valueAt(line) : OptionValue();
    const QString old = v.implied();
    const bool virtioBefore = kAccelerated.contains(old) || kVirtio.contains(old);
    const bool virtioAfter = kAccelerated.contains(driver) || kVirtio.contains(driver);

    if (line >= 0 && old != driver && virtioBefore != virtioAfter) {
        /* another kind of card: only the properties any device has */
        OptionValue fresh;
        fresh.setImplied(driver);
        for (const char *key : {"id", "bus", "addr"}) {
            if (v.has(key)) {
                fresh.set(key, v.get(key));
            }
        }
        v = fresh;
    }
    v.setImplied(driver);
    if (kAccelerated.contains(driver)) {
        v.remove("drm_native_context");
        v.remove("venus");
        if (g.nativeContext) {
            v.set("drm_native_context", "on");
        }
        if (g.venus) {
            v.set("venus", "on");
        }
        if (g.nativeContext || g.venus) {
            const qint64 mib = g.hostmemMiB > 0   ? g.hostmemMiB
                               : now.hostmemMiB > 0 ? now.hostmemMiB : 4096;
            if (!v.flag("blob")) {
                v.set("blob", "on");
            }
            if (!v.has("hostmem") || mib != now.hostmemMiB) {
                v.set("hostmem", formatMiB(mib));
            }
        }
    } else {
        v.remove("drm_native_context");
        v.remove("venus");
    }

    if (line >= 0) {
        args.setValueAt(line, v);
    } else {
        /* with the other display lines */
        const int display = lastIndex(args, "display");
        if (display >= 0) {
            args.lines.insert(display, optionLine("device", v.toString()));
            line = display;
        } else {
            line = insertAfter(args, {"vga", "accel", "machine"}, "device", v.toString());
        }
    }
    if (!isVgaDevice(driver)) {
        /* QEMU would add its VGA to the card */
        noVgaCard(args, line);
    } else if (const int vga = lastIndex(args, "vga"); vga >= 0 && args.lines[vga].value != "none") {
        args.lines[vga].value = "none";
    }

    /* Intel GPUs need it for the caching of the guest's mappings */
    if (g.nativeContext && accel(args).startsWith("kvm") &&
        accelProperty(args, "honor-guest-pat").isEmpty()) {
        setAccelProperty(args, "honor-guest-pat", "on");
    }
}

/* @gl: 1 on, 0 off, -1 as it is; a new window has it on for @accelerated */
static void setWindow(ArgsFile &args, const QString &display, int gl, bool accelerated)
{
    const int line = lastIndex(args, "display");

    if (line < 0) {
        if (display.isEmpty()) {
            return;
        }
        QString value = display;
        if ((gl == 1 || (gl == -1 && accelerated)) && (display == "sdl" || display == "gtk")) {
            value += ",gl=on";
        }
        const QList<int> cards = cardLines(args);
        if (!cards.isEmpty()) {
            args.lines.insert(qMax(cards.last(), lastIndex(args, "vga")) + 1,
                              optionLine("display", value));
        } else {
            insertAfter(args, {"vga", "accel", "machine"}, "display", value);
        }
        return;
    }

    OptionValue v = args.valueAt(line);
    if (!display.isEmpty() && display != v.implied()) {
        OptionValue fresh;
        fresh.setImplied(display);
        for (const OptionValue::Item &item : v.items()) {
            if (!item.key.isEmpty() && !item.bare &&
                kDisplayKeys.value(display).contains(item.key)) {
                fresh.set(item.key, item.value);
            }
        }
        v = fresh;
    }
    const QString type = v.implied();
    if (type == "sdl" || type == "gtk") {
        if (gl == 1 && v.get("gl", "off") == "off") {
            v.set("gl", "on");
        } else if (gl == 0) {
            v.remove("gl");
        }
    } else if (type == "none") {
        v.remove("gl");
    }
    args.setValueAt(line, v);
}

void setGraphics(ArgsFile &args, const Graphics &g)
{
    const Graphics now = graphics(args);
    const bool custom = now.kind == Graphics::Custom || g.kind == Graphics::Custom;
    const bool sameCard = g.kind == now.kind && (g.device.isEmpty() || g.device == now.device) &&
                          g.nativeContext == now.nativeContext && g.venus == now.venus &&
                          (g.hostmemMiB == 0 || g.hostmemMiB == now.hostmemMiB);

    if (!custom && !sameCard) {
        setCard(args, now, g);
    }
    /* OpenGL follows the card when it changes to or from 3D */
    const bool accelerated = !custom && g.kind == Graphics::Accelerated;
    const int gl = custom || g.kind == now.kind ? -1 : accelerated ? 1 : 0;
    setWindow(args, g.display, gl, accelerated);
}

/* Disks */

static const QStringList kHardDisks = {"hda", "hdb", "hdc", "hdd"};
static const QHash<QString, Disk::Bus> kDiskDevices = {
    {"virtio-blk-pci", Disk::Virtio},
    {"virtio-blk", Disk::Virtio},
    {"virtio-blk-pci-non-transitional", Disk::Virtio},
    {"ide-hd", Disk::Sata},
    {"ide-cd", Disk::Sata},
    {"ide-drive", Disk::Sata},
    {"scsi-hd", Disk::Scsi},
    {"scsi-cd", Disk::Scsi},
    {"scsi-block", Disk::Scsi},
    {"nvme", Disk::Nvme},
    {"usb-storage", Disk::Usb},
};

QList<Disk> disks(const ArgsFile &args)
{
    QList<Disk> list;

    for (int i = 0; i < args.lines.size(); i++) {
        const ArgsFile::Line &line = args.lines[i];
        Disk d;

        if (line.kind != ArgsFile::Line::Option) {
            continue;
        }
        d.line = i;
        if (line.name == "drive") {
            const OptionValue v(line.value);
            const QString iface = v.get("if", "ide");

            if (iface == "pflash" || iface == "mtd" || iface == "sd" || iface == "floppy") {
                continue;
            }
            d.file = v.get("file");
            d.format = v.get("format");
            d.cdrom = v.get("media") == "cdrom";
            if (iface == "virtio") {
                d.bus = Disk::Virtio;
            } else if (iface == "ide") {
                d.bus = Disk::Sata;
            } else if (iface == "scsi") {
                d.bus = Disk::Scsi;
            } else if (iface == "none") {
                const QString id = v.get("id");
                d.bus = Disk::Other;
                d.editable = false;
                for (int j : args.indexesOf("device")) {
                    const OptionValue dev = args.valueAt(j);
                    if (id.isEmpty() || dev.get("drive") != id) {
                        continue;
                    }
                    d.deviceLine = j;
                    d.bus = kDiskDevices.value(dev.implied(), Disk::Other);
                    d.cdrom |= dev.implied().endsWith("-cd");
                    d.editable = true;
                    if (dev.has("bootindex")) {
                        d.bootindex = dev.get("bootindex").toInt();
                    }
                }
            } else {
                d.bus = Disk::Other;
            }
        } else if (kHardDisks.contains(line.name)) {
            d.file = line.value;
            d.bus = Disk::Sata;
        } else if (line.name == "cdrom") {
            d.file = line.value;
            d.cdrom = true;
            d.bus = Disk::Sata;
        } else if (line.name == "blockdev") {
            const OptionValue v(line.value);
            if (!v.has("filename")) {
                continue;
            }
            d.file = v.get("filename");
            d.bus = Disk::Other;
            d.editable = false;
        } else {
            continue;
        }
        list << d;
    }
    return list;
}

QString diskFormat(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();

    if (suffix == "qcow2" || suffix == "vdi" || suffix == "vmdk" || suffix == "vhdx") {
        return suffix;
    }
    if (suffix == "vhd") {
        return "vpc";
    }
    if (suffix == "img" || suffix == "raw" || suffix == "iso") {
        return "raw";
    }
    return {};
}

/* An id no -drive, -device, -blockdev... has yet */
static QString uniqueId(const ArgsFile &args, const QString &prefix)
{
    QSet<QString> used;

    for (const ArgsFile::Line &line : args.lines) {
        if (line.kind == ArgsFile::Line::Option) {
            const OptionValue v(line.value);
            used << v.get("id") << v.get("node-name");
        }
    }
    for (int n = 0;; n++) {
        const QString id = prefix + QString::number(n);
        if (!used.contains(id)) {
            return id;
        }
    }
}

/* The line after which new disks go: the last disk, else the firmware;
   -1 for the end */
static int diskAnchor(const ArgsFile &args)
{
    int after = -1;

    for (const Disk &d : disks(args)) {
        after = qMax(after, qMax(d.line, d.deviceLine));
    }
    return after >= 0 ? after : lastIndex(args, "drive");
}

/* A drive on the virtio-scsi controller, which comes first if needed */
static void insertScsi(ArgsFile &args, const QString &drive, bool cdrom)
{
    int at = diskAnchor(args);
    QString bus;

    for (int i : args.indexesOf("device")) {
        const OptionValue v = args.valueAt(i);
        if (v.implied().startsWith("virtio-scsi") && v.has("id")) {
            bus = v.get("id") + ".0";
            at = qMax(at, i);
            break;
        }
    }
    if (bus.isEmpty()) {
        const QString controller = uniqueId(args, "scsi");
        at = insertLine(args, at, "device", "virtio-scsi-pci,id=" + controller);
        bus = controller + ".0";
    }
    const QString id = uniqueId(args, cdrom ? "cd" : "disk");
    at = insertLine(args, at, "drive", drive + ",if=none,id=" + id);
    insertLine(args, at, "device",
               QString("%1,drive=%2,bus=%3").arg(cdrom ? "scsi-cd" : "scsi-hd", id, bus));
}

void addDisk(ArgsFile &args, const QString &file, Disk::Bus bus)
{
    const QString format = diskFormat(file);
    QString value = "file=" + OptionValue::escape(file);

    if (!format.isEmpty()) {
        value += ",format=" + format;
    }
    if (bus == Disk::Scsi) {
        insertScsi(args, value + ",discard=unmap", false);
        return;
    }
    value += bus == Disk::Sata ? ",if=ide" : ",if=virtio";
    insertLine(args, diskAnchor(args), "drive", value + ",discard=unmap");
}

void addCdrom(ArgsFile &args, const QString &iso)
{
    QString value;

    if (!iso.isEmpty()) {
        value = "file=" + OptionValue::escape(iso) + ",";
    }
    value += "media=cdrom,readonly=on";
    /* virt has no IDE, SATA only with a controller */
    if (machineType(args).startsWith("virt")) {
        insertScsi(args, value, true);
    } else {
        insertLine(args, diskAnchor(args), "drive", value);
    }
}

void removeDisk(ArgsFile &args, const Disk &disk)
{
    const int first = qMax(disk.line, disk.deviceLine);
    const int second = qMin(disk.line, disk.deviceLine);

    args.removeAt(first);
    if (second >= 0) {
        args.removeAt(second);
    }
}

void setDisc(ArgsFile &args, const Disk &drive, const QString &iso)
{
    ArgsFile::Line &line = args.lines[drive.line];

    if (line.name == "cdrom") {
        if (!iso.isEmpty()) {
            line.value = iso;
            return;
        }
        /* -cdrom is the drive of index 2 */
        line.name = "drive";
        line.value = "index=2,media=cdrom,readonly=on";
        return;
    }
    setKey(args, drive.line, "file", iso);
}

/* Boot */

static bool isSecureFlash(const OptionValue &global)
{
    return (global.get("driver") == "cfi.pflash01" && global.get("property") == "secure") ||
           global.has("cfi.pflash01.secure");
}

FirmwareKind firmwareKind(const ArgsFile &args, const QStringList &dirs)
{
    QString code;
    bool pflash = false;

    if (args.indexOf("bios") >= 0) {
        return FirmwareKind::Custom;
    }
    for (int i : args.indexesOf("machine")) {
        if (args.valueAt(i).has("pflash0")) {
            return FirmwareKind::Custom;
        }
    }
    for (int i : args.indexesOf("drive")) {
        const OptionValue v = args.valueAt(i);
        if (v.get("if") != "pflash") {
            continue;
        }
        pflash = true;
        /* the code, not the variable store */
        if (code.isEmpty() || v.get("unit") == "0" || v.flag("readonly")) {
            code = QFileInfo(v.get("file")).fileName();
        }
    }
    for (int i : args.indexesOf("pflash")) {
        pflash = true;
        if (code.isEmpty()) {
            code = QFileInfo(args.lines[i].value).fileName();
        }
    }
    if (!pflash) {
        return FirmwareKind::Bios;
    }
    for (const Firmware &fw : FirmwareDb::list(dirs)) {
        if (QFileInfo(fw.code).fileName() == code) {
            return fw.features.contains("secure-boot") ? FirmwareKind::UefiSecureBoot
                                                       : FirmwareKind::Uefi;
        }
    }
    return code.contains("secboot", Qt::CaseInsensitive) ? FirmwareKind::UefiSecureBoot
                                                         : FirmwareKind::Uefi;
}

void useBios(ArgsFile &args)
{
    for (int i = int(args.lines.size()) - 1; i >= 0; i--) {
        const ArgsFile::Line &line = args.lines[i];

        if (line.kind != ArgsFile::Line::Option) {
            continue;
        }
        if ((line.name == "drive" && OptionValue(line.value).get("if") == "pflash") ||
            line.name == "pflash" ||
            (line.name == "global" && isSecureFlash(OptionValue(line.value)))) {
            args.removeAt(i);
        }
    }
}

bool bootMenu(const ArgsFile &args)
{
    const int line = lastIndex(args, "boot");
    return line >= 0 && args.valueAt(line).flag("menu");
}

void setBootMenu(ArgsFile &args, bool on)
{
    const int line = lastIndex(args, "boot");

    if (line < 0) {
        if (on) {
            insertAfter(args, {"accel", "machine"}, "boot", "menu=on", true);
        }
        return;
    }
    OptionValue v = args.valueAt(line);
    if (on) {
        v.set("menu", "on");
    } else {
        v.remove("menu");
    }
    if (v.isEmpty()) {
        args.removeAt(line);
    } else {
        args.setValueAt(line, v);
    }
}

/* The network cards: devices of a -netdev */
static QList<int> nicLines(const ArgsFile &args)
{
    QList<int> lines;

    for (int i : args.indexesOf("device")) {
        if (args.valueAt(i).has("netdev")) {
            lines << i;
        }
    }
    return lines;
}

BootDevice firstBootDevice(const ArgsFile &args)
{
    BootDevice device = BootDevice::Default;
    int best = INT_MAX;

    for (const Disk &d : disks(args)) {
        if (d.bootindex >= 0 && d.bootindex < best) {
            best = d.bootindex;
            device = d.cdrom ? BootDevice::Cdrom : BootDevice::Disk;
        }
    }
    for (int i : nicLines(args)) {
        const OptionValue v = args.valueAt(i);
        if (v.has("bootindex") && v.get("bootindex").toInt() < best) {
            best = v.get("bootindex").toInt();
            device = BootDevice::Network;
        }
    }
    if (device != BootDevice::Default) {
        return device;
    }

    const int line = lastIndex(args, "boot");
    if (line >= 0) {
        const OptionValue v = args.valueAt(line);
        const QString order = v.has("order") ? v.get("order") : v.implied();
        if (order.startsWith('c')) {
            return BootDevice::Disk;
        }
        if (order.startsWith('d')) {
            return BootDevice::Cdrom;
        }
        if (order.startsWith('n')) {
            return BootDevice::Network;
        }
    }
    return BootDevice::Default;
}

/* Makes @d an if=none drive with a -device; returns the device's line */
static int splitDrive(ArgsFile &args, const Disk &d)
{
    const ArgsFile::Line line = args.lines[d.line];
    const QString id = uniqueId(args, d.cdrom ? "cd" : "disk");
    OptionValue drive;
    QString driver;

    if (line.name == "drive") {
        const QString iface = OptionValue(line.value).get("if", "ide");
        drive = OptionValue(line.value);
        if (iface == "virtio") {
            driver = "virtio-blk-pci";
        } else if (iface == "ide") {
            driver = d.cdrom ? "ide-cd" : "ide-hd";
        } else {
            return -1;
        }
        for (const char *key : {"if", "index", "bus", "unit"}) {
            drive.remove(key);
        }
    } else if (kHardDisks.contains(line.name)) {
        drive.set("file", line.value);
        driver = "ide-hd";
    } else if (line.name == "cdrom") {
        drive.set("file", line.value);
        drive.set("media", "cdrom");
        driver = "ide-cd";
    } else {
        return -1;
    }
    drive.set("if", "none");
    drive.set("id", id);
    args.lines[d.line].name = "drive";
    args.setValueAt(d.line, drive);
    args.lines.insert(d.line + 1, optionLine("device", driver + ",drive=" + id));
    return d.line + 1;
}

bool setFirstBootDevice(ArgsFile &args, BootDevice device)
{
    for (const Disk &d : disks(args)) {
        if (d.deviceLine >= 0 && d.bootindex >= 0) {
            setKey(args, d.deviceLine, "bootindex", {});
        }
    }
    for (int i : nicLines(args)) {
        if (args.valueAt(i).has("bootindex")) {
            setKey(args, i, "bootindex", {});
        }
    }
    const int boot = lastIndex(args, "boot");
    if (boot >= 0) {
        OptionValue v = args.valueAt(boot);
        const QString implied = v.implied();
        v.remove("order");
        /* -boot d: the implied order */
        if (!implied.isEmpty()) {
            OptionValue rest;
            for (const OptionValue::Item &item : v.items()) {
                if (item.key.isEmpty()) {
                    continue;
                }
                if (item.bare) {
                    rest.setFlag(item.key, true);
                } else {
                    rest.set(item.key, item.value);
                }
            }
            v = rest;
        }
        if (v.isEmpty()) {
            args.removeAt(boot);
        } else {
            args.setValueAt(boot, v);
        }
    }

    if (device == BootDevice::Default) {
        return true;
    }
    if (device == BootDevice::Network) {
        const QList<int> nics = nicLines(args);
        if (nics.isEmpty()) {
            return false;
        }
        setKey(args, nics.first(), "bootindex", "1");
        return true;
    }
    for (const Disk &d : disks(args)) {
        if (d.cdrom != (device == BootDevice::Cdrom) || !d.editable) {
            continue;
        }
        const int dev = d.deviceLine >= 0 ? d.deviceLine : splitDrive(args, d);
        if (dev >= 0) {
            setKey(args, dev, "bootindex", "1");
            return true;
        }
    }
    return false;
}

}
