// SPDX-License-Identifier: GPL-2.0-or-later
#include "nesteddesktop.h"

#include "core/paths.h"
#include "core/qemuinfo.h"
#include "core/vmconfig.h"
#include "core/vmhardware.h"

namespace NestedDesktop {

static const char kCard[] = "virtio-vga-rutabaga";
/* Any rutabaga card with cross-domain on connects to a socket */
static const QStringList kRutabagaCards = {"virtio-gpu-rutabaga-pci", "virtio-gpu-rutabaga",
                                           "virtio-gpu-rutabaga-device", "virtio-vga-rutabaga"};
/* The guest's crosvm-desktop-fwcfg.service merges it with the kernel's command line */
static const char kFwCfgName[] = "opt/org.drm-native-context/cmdline";

/*
 * The research launcher's card (run-qemu-rutabaga.sh): the DRM native
 * context and virgl through rutabaga (x-drm, x-virgl2), cross-domain for
 * the guest's Wayland, blobs in a 4 GiB window, the guest's notifications
 * without a trip through QEMU's main loop (ioeventfd: one per message of
 * the guest's proxy, ~20 a frame), the blobs entered in KVM's page tables at
 * once rather than one fault per 4 KiB at the guest's first touch
 * (x-prefault-blobs), and the guest compositor's own buffers handed to the
 * native context as they are (x-guest-dmabuf)
 */
static const std::pair<const char *, const char *> kProperties[] = {
    {"x-drm", "on"},        {"x-virgl2", "on"},         {"cross-domain", "on"},
    {"hostmem", "4G"},      {"ioeventfd", "on"},        {"x-prefault-blobs", "on"},
    {"x-guest-dmabuf", "on"},
};

/* KEY=on, or KEY alone, which QEMU takes for on */
static bool isOn(const OptionValue &v, const QString &key)
{
    for (const OptionValue::Item &item : v.items()) {
        if (item.bare && item.key == key) {
            return true;
        }
    }
    return v.flag(key);
}

static int cardLine(const ArgsFile &args)
{
    return args.indexOfDevice([](const QString &driver) { return kRutabagaCards.contains(driver); });
}

Mode mode(const ArgsFile &args)
{
    const int line = cardLine(args);

    if (line < 0 || !isOn(args.valueAt(line), "cross-domain")) {
        return Mode::Off;
    }
    return VmConfig::screen(args) == VmConfig::Screen::Embedded ? Mode::Integrated
                                                                : Mode::OwnWindow;
}

Offer offer(const ArgsFile &args)
{
    QString binary = VmConfig::qemuBinary(args);
    Offer o;

    if (binary.isEmpty()) {
        binary = Paths::customQemuBinary();
    }
    if (binary.isEmpty()) {
        binary = Paths::stackQemu();
    }
    /* vitrine's, not built yet: built with rutabaga, it has the card */
    if (binary.isEmpty()) {
        o.offered = true;
        return o;
    }
    QString error;
    const QStringList known = QemuInfo::probeProperties(binary, kCard, &error);
    if (!error.isEmpty()) {
        return o;
    }
    o.answered = true;
    /* none for a QEMU without the card */
    o.offered = known.contains("cross-domain");
    o.properties = known;
    return o;
}

QString cardDevice(const std::optional<QStringList> &properties)
{
    OptionValue v;

    v.setImplied(kCard);
    for (const auto &[key, value] : kProperties) {
        if (!properties || properties->contains(key) || QString(key) == "cross-domain") {
            v.set(key, value);
        }
    }
    return v.toString();
}

/* -global DRIVER.PROPERTY=... or -global driver=DRIVER,property=PROPERTY,... */
static bool hasGlobal(const ArgsFile &args, const QString &driver, const QString &property)
{
    for (int i : args.indexesOf("global")) {
        const OptionValue g = args.valueAt(i);
        if (g.has("property") ? g.get("driver") == driver && g.get("property") == property
                              : !g.items().isEmpty() &&
                                    g.items().first().key == driver + '.' + property) {
            return true;
        }
    }
    return false;
}

/*
 * A PC (QEMU's default machine when the VM names none) whose HPET the VM
 * leaves as QEMU has it: no hpet= on any -machine, no -no-hpet
 */
static bool hasDefaultHpet(const ArgsFile &args)
{
    const QString type = VmConfig::machineType(args);

    if (!type.isEmpty() && type != "q35" && type != "pc" && type != "isapc" &&
        !type.startsWith("pc-")) {
        return false;
    }
    if (args.indexOf("no-hpet") >= 0) {
        return false;
    }
    for (int i : args.indexesOf("machine")) {
        if (args.valueAt(i).has("hpet")) {
            return false;
        }
    }
    return true;
}

/* After the last line of the first of @after the VM has, else at the end */
static void insertAfter(ArgsFile &args, const QStringList &after, const QString &name,
                        const QString &value)
{
    ArgsFile::Line line;
    line.kind = ArgsFile::Line::Option;
    line.name = name;
    line.value = value;
    for (const QString &option : after) {
        const QList<int> found = args.indexesOf(option);
        if (!found.isEmpty()) {
            args.lines.insert(found.last() + 1, line);
            return;
        }
    }
    args.lines.append(line);
}

void set(ArgsFile &args, bool on, const std::optional<QStringList> &properties)
{
    if (!on) {
        const VmConfig::Graphics now = VmConfig::graphics(args);
        if (now.kind != VmConfig::Graphics::Nested) {
            return;
        }
        VmConfig::Graphics g;
        g.kind = VmConfig::Graphics::Accelerated;
        g.display = now.display;
        VmConfig::setGraphics(args, g);
        /* a 3D card without a window takes egl-headless */
        if (now.display == "none") {
            VmConfig::setScreen(args, VmConfig::Screen::None);
        }
        return;
    }
    VmConfig::setNestedCard(args, cardDevice(properties));
    if (!VmConfig::hasSharedMemory(args)) {
        VmConfig::useSharedMemory(args);
    }
    if (!hasGlobal(args, "kvm-pit", "lost_tick_policy")) {
        insertAfter(args, {"smp", "cpu", "accel", "machine"}, "global",
                    "kvm-pit.lost_tick_policy=discard");
    }
    /* on the last -machine, as QEMU merges them */
    if (hasDefaultHpet(args)) {
        const QList<int> machines = args.indexesOf("machine");
        if (machines.isEmpty()) {
            insertAfter(args, {"name"}, "machine", "hpet=off");
        } else {
            OptionValue machine = args.valueAt(machines.last());
            machine.set("hpet", "off");
            args.setValueAt(machines.last(), machine);
        }
    }
    if (args.indexOfDevice([](const QString &driver) { return driver == "pvpanic"; }) < 0) {
        const int card = cardLine(args);
        ArgsFile::Line line;
        line.kind = ArgsFile::Line::Option;
        line.name = "device";
        line.value = "pvpanic";
        args.lines.insert(card + 1, line);
    }
}

bool moveToOneCard(ArgsFile &args)
{
    const VmConfig::Graphics g = VmConfig::graphics(args);

    if (g.kind != VmConfig::Graphics::Nested || g.device == kCard) {
        return false;
    }
    /* its properties as they are: the card inside is the same device */
    OptionValue card = args.valueAt(cardLine(args));
    card.setImplied(kCard);
    VmConfig::setNestedCard(args, card.toString());
    return true;
}

int screenConsole(const ArgsFile &args)
{
    const VmConfig::Graphics g = VmConfig::graphics(args);

    return g.kind == VmConfig::Graphics::Nested && g.device != kCard ? 1 : 0;
}

QString bootWords(const ArgsFile &args, const QString &label)
{
    QStringList words{"crosvm_desktop=1"};
    const int line = cardLine(args);
    QString name = label.simplified();

    if (line >= 0 && isOn(args.valueAt(line), "x-guest-dmabuf")) {
        words << "crosvm_desktop_guest_dmabuf=1";
    }
    /* the guest's session takes the word up to the next space */
    name.replace(' ', QChar(0x00a0));
    if (!name.isEmpty()) {
        words << "crosvm_desktop_label=" + name;
    }
    return words.join(' ');
}

ArgsFile withRunArgs(const ArgsFile &args, const QString &socket, const QString &label)
{
    ArgsFile out = args;
    const int line = cardLine(out);

    if (line < 0 || mode(args) == Mode::Off) {
        return out;
    }
    OptionValue card = out.valueAt(line);
    if (!card.has("wayland-socket-path") && !socket.isEmpty()) {
        card.set("wayland-socket-path", socket);
        out.setValueAt(line, card);
    }
    for (int i : out.indexesOf("fw_cfg")) {
        if (out.valueAt(i).get("name") == kFwCfgName) {
            return out;
        }
    }
    OptionValue words;
    words.set("name", kFwCfgName);
    words.set("string", bootWords(args, label));
    ArgsFile::Line fwCfg;
    fwCfg.kind = ArgsFile::Line::Option;
    fwCfg.name = "fw_cfg";
    fwCfg.value = words.toString();
    out.lines.insert(line + 1, fwCfg);
    return out;
}

QStringList environment(const ArgsFile &args)
{
    if (mode(args) == Mode::Off || VmConfig::graphics(args).display != "sdl") {
        return {};
    }
    return {"SDL_RENDER_DRIVER=vulkan"};
}

QString compositorSocket()
{
    const QString display = qEnvironmentVariable("WAYLAND_DISPLAY");
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");

    if (display.isEmpty()) {
        return {};
    }
    if (display.startsWith('/')) {
        return display;
    }
    return runtime.isEmpty() ? QString() : runtime + '/' + display;
}

}
