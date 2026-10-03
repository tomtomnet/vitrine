// SPDX-License-Identifier: GPL-2.0-or-later
#include "uiconfig.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QRegularExpression>

#include "core/qemuinfo.h"

namespace UiConfig {

static QString tr(const char *text)
{
    return QCoreApplication::translate("UiConfig", text);
}

QString busName(VmConfig::Disk::Bus bus)
{
    switch (bus) {
    case VmConfig::Disk::Virtio:
        return "virtio";
    case VmConfig::Disk::Sata:
        return tr("SATA");
    case VmConfig::Disk::Scsi:
        return tr("SCSI");
    case VmConfig::Disk::Nvme:
        return tr("NVMe");
    case VmConfig::Disk::Usb:
        return tr("USB");
    case VmConfig::Disk::Other:
        break;
    }
    return tr("other");
}

static bool secureFlash(const ArgsFile &args)
{
    for (int i : args.indexesOf("global")) {
        const OptionValue v = args.valueAt(i);
        if ((v.get("driver") == "cfi.pflash01" && v.get("property") == "secure" &&
             v.flag("value")) ||
            v.flag("cfi.pflash01.secure")) {
            return true;
        }
    }
    return false;
}

QString firmwareSummary(const ArgsFile &args)
{
    QString code;
    bool uefi = false;

    for (int i : args.indexesOf("drive")) {
        const OptionValue v = args.valueAt(i);
        if (v.get("if") != "pflash") {
            continue;
        }
        uefi = true;
        /* the code, not the variable store */
        if (code.isEmpty() || v.get("unit") == "0" || v.flag("readonly")) {
            code = QFileInfo(v.get("file")).fileName();
        }
    }
    for (int i : args.indexesOf("machine")) {
        uefi |= args.valueAt(i).has("pflash0");
    }
    if (uefi) {
        const QString name = secureFlash(args) ? tr("UEFI with Secure Boot") : tr("UEFI");
        return code.isEmpty() ? name : QString("%1 (%2)").arg(name, code);
    }

    const int bios = args.indexOf("bios");
    if (bios >= 0) {
        return tr("Firmware file %1").arg(QFileInfo(args.lines[bios].value).fileName());
    }
    return tr("BIOS (SeaBIOS)");
}

QStringList devicesOf(const ArgsFile &args, const QString &category,
                      const QemuInfo *info)
{
    static const QHash<QString, QStringList> known = {
        {"Display devices",
         {"VGA", "virtio-vga", "virtio-vga-gl", "virtio-gpu-pci", "virtio-gpu-gl-pci",
          "virtio-gpu", "virtio-gpu-gl", "qxl", "qxl-vga", "cirrus-vga", "bochs-display",
          "ramfb", "vmware-svga", "secondary-vga", "ati-vga", "vhost-user-gpu",
          "vhost-user-vga", "virtio-vga-rutabaga", "virtio-gpu-rutabaga-pci"}},
        {"Network devices",
         {"virtio-net-pci", "virtio-net", "e1000", "e1000e", "igb", "rtl8139", "vmxnet3",
          "ne2k_pci", "pcnet", "usb-net", "i82559er", "virtio-net-pci-non-transitional"}},
        {"Sound devices",
         {"intel-hda", "ich9-intel-hda", "hda-duplex", "hda-output", "hda-micro", "AC97",
          "ES1370", "sb16", "usb-audio", "virtio-sound-pci", "virtio-sound"}},
    };
    QStringList found;

    for (int i : args.indexesOf("device")) {
        const QString driver = args.valueAt(i).implied();
        const QemuDeviceDoc *doc = info ? info->device(driver) : nullptr;

        if (doc ? doc->category == category : known.value(category).contains(driver)) {
            found << driver;
        }
    }
    return found;
}

QString displaySummary(const ArgsFile &args, const QemuInfo *info)
{
    static const QHash<QString, const char *> outputs = {
        {"sdl", QT_TRANSLATE_NOOP("UiConfig", "SDL window")},
        {"gtk", QT_TRANSLATE_NOOP("UiConfig", "GTK window")},
        {"none", QT_TRANSLATE_NOOP("UiConfig", "no window")},
        {"egl-headless", QT_TRANSLATE_NOOP("UiConfig", "headless")},
        {"dbus", QT_TRANSLATE_NOOP("UiConfig", "D-Bus")},
        {"spice-app", QT_TRANSLATE_NOOP("UiConfig", "SPICE viewer")},
        {"curses", QT_TRANSLATE_NOOP("UiConfig", "text console")},
    };
    QStringList devices = devicesOf(args, "Display devices", info);
    const int vga = args.indexOf("vga");
    const int display = args.indexOf("display");
    QString output;

    if (vga >= 0 && args.lines[vga].value != "none") {
        devices << args.lines[vga].value;
    }
    if (args.indexOf("nographic") >= 0) {
        output = tr("no window (serial console)");
    } else if (display >= 0) {
        const OptionValue v = args.valueAt(display);
        const QString type = v.implied();
        const QString gl = v.get("gl", "off");

        output = outputs.contains(type) ? tr(outputs.value(type)) : type;
        if (gl != "off") {
            output = tr("%1 with OpenGL").arg(output);
        }
    } else {
        output = tr("default window");
    }
    if (devices.isEmpty()) {
        return output;
    }
    return QString("%1 · %2").arg(devices.join(", "), output);
}

QString networkSummary(const ArgsFile &args, const QemuInfo *info)
{
    static const QHash<QString, const char *> types = {
        {"user", QT_TRANSLATE_NOOP("UiConfig", "NAT (user networking)")},
        {"passt", QT_TRANSLATE_NOOP("UiConfig", "NAT (passt)")},
        {"tap", QT_TRANSLATE_NOOP("UiConfig", "TAP interface")},
        {"bridge", QT_TRANSLATE_NOOP("UiConfig", "bridge")},
    };
    auto typeName = [](const QString &type) {
        return types.contains(type) ? tr(types.value(type)) : type;
    };
    QStringList parts;

    for (int i : args.indexesOf("netdev")) {
        const OptionValue v = args.valueAt(i);
        QString model;

        for (int j : args.indexesOf("device")) {
            const OptionValue dev = args.valueAt(j);
            if (dev.get("netdev") == v.get("id") && !v.get("id").isEmpty()) {
                model = dev.implied();
            }
        }
        parts << (model.isEmpty() ? typeName(v.implied())
                                  : QString("%1 · %2").arg(typeName(v.implied()), model));
    }
    for (int i : args.indexesOf("nic")) {
        const OptionValue v = args.valueAt(i);
        if (v.implied() == "none") {
            parts << tr("not connected");
        } else {
            const QString model = v.get("model");
            parts << (model.isEmpty() ? typeName(v.implied())
                                      : QString("%1 · %2").arg(typeName(v.implied()), model));
        }
    }
    if (!args.indexesOf("net").isEmpty()) {
        parts << tr("legacy -net options");
    }
    if (parts.isEmpty()) {
        const QStringList cards = devicesOf(args, "Network devices", info);
        if (!cards.isEmpty()) {
            return cards.join(", ");
        }
        return args.indexOf("nodefaults") >= 0 ? tr("none")
                                               : tr("QEMU default (NAT)");
    }
    return parts.join("; ");
}

QString audioSummary(const ArgsFile &args, const QemuInfo *info)
{
    static const QHash<QString, QString> drivers = {
        {"pipewire", "PipeWire"}, {"pa", "PulseAudio"}, {"alsa", "ALSA"},
        {"jack", "JACK"},         {"sdl", "SDL"},       {"oss", "OSS"},
    };
    QStringList backends;
    QStringList devices = devicesOf(args, "Sound devices", info);

    for (int i : args.indexesOf("audiodev")) {
        const QString type = args.valueAt(i).implied();
        backends << drivers.value(type, type);
    }
    for (int i : args.indexesOf("audio")) {
        const OptionValue v = args.valueAt(i);
        const QString type = v.implied().isEmpty() ? v.get("driver") : v.implied();
        backends << drivers.value(type, type);
        if (v.has("model")) {
            devices << v.get("model");
        }
    }
    if (backends.isEmpty() && devices.isEmpty()) {
        return tr("none");
    }
    if (devices.isEmpty()) {
        return backends.join(", ");
    }
    return QString("%1 · %2").arg(backends.join(", "), devices.join(", "));
}

QString shellQuote(const QString &arg)
{
    static const QRegularExpression safe("^[A-Za-z0-9_@%+=:,./-]+$");

    if (safe.match(arg).hasMatch()) {
        return arg;
    }
    QString quoted = arg;
    return '\'' + quoted.replace('\'', "'\\''") + '\'';
}

QString commandText(const QStringList &command, const QString &dir)
{
    QStringList lines;
    QString current;

    for (qsizetype i = 0; i < command.size(); i++) {
        const QString &arg = command[i];
        if (i == 0) {
            current = shellQuote(arg);
        } else if (arg.startsWith('-') && arg.size() > 1) {
            lines << current;
            current = "    " + shellQuote(arg);
        } else {
            current += ' ' + shellQuote(arg);
        }
    }
    lines << current;
    return "cd " + shellQuote(dir) + "\n" + lines.join(" \\\n") + "\n";
}

}
