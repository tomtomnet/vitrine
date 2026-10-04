// SPDX-License-Identifier: GPL-2.0-or-later
#include "cardupdate.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QHash>
#include <QSettings>

#include "core/paths.h"
#include "core/qemuinfo.h"
#include "core/vmconfig.h"
#include "core/vmhardware.h"
#include "core/vmtemplate.h"

namespace CardUpdate {

/* The cards with OpenGL that take the template's properties, on a PC */
static const QStringList kCards = {"virtio-gpu-gl-pci", "virtio-vga-gl", "virtio-gpu-gl"};

static QString tr(const char *text)
{
    return QCoreApplication::translate("CardUpdate", text);
}

/* The user chose vitrine's QEMU by its path, which may not answer, say out of memory */
static bool isStackQemu(const QString &binary)
{
    const QString stack = Paths::stackQemu();
    return !stack.isEmpty() && QFileInfo(binary).canonicalFilePath() == stack;
}

/*
 * Linux, as the #guest directive says (a desktop alone is a Linux one);
 * without it, as GuestToolsBanner guesses: not Windows, whose VMs have
 * Hyper-V enlightenments on -cpu
 */
static bool isLinux(const ArgsFile &args)
{
    if (args.indexOf("guest", ArgsFile::Line::Directive) >= 0) {
        const VmConfig::Guest guest = VmConfig::guest(args);
        return guest.os == "linux" || (guest.os.isEmpty() && !guest.desktop.isEmpty());
    }
    for (int i : args.indexesOf("cpu")) {
        const OptionValue cpu = args.valueAt(i);
        for (const OptionValue::Item &item : cpu.items()) {
            const QString name = item.key.isEmpty() ? item.value : item.key;
            if (name.startsWith("hv-") || name.startsWith("hv_")) {
                return false;
            }
        }
    }
    return true;
}

/*
 * The line of the card the update is for, or -1: a Linux guest on a PC
 * (the template's card is for Linux on a PC; virt on ARM has its own)
 * with one 3D card, of those taking the template's properties
 */
static int cardLine(const ArgsFile &args)
{
    const VmConfig::Graphics g = VmConfig::graphics(args);

    if (!isLinux(args) || g.kind != VmConfig::Graphics::Accelerated ||
        !kCards.contains(g.device) || VmConfig::machineType(args).startsWith("virt")) {
        return -1;
    }
    return args.indexOfDevice([](const QString &driver) { return kCards.contains(driver); });
}

std::optional<Offers> offers(const ArgsFile &args)
{
    QString chosen = VmConfig::qemuBinary(args);
    const int line = cardLine(args);
    QString error;
    Offers o;

    if (chosen.isEmpty()) {
        chosen = Paths::customQemuBinary();
    }
    /* vitrine's, built or not: what the template writes for it */
    if (chosen.isEmpty()) {
        return o;
    }
    if (line < 0) {
        return Offers{QStringList(), QStringList()};
    }
    o.card = QemuInfo::probeProperties(chosen, args.valueAt(line).implied(), &error);
    if (!error.isEmpty()) {
        return isStackQemu(chosen) ? std::optional<Offers>(Offers()) : std::nullopt;
    }
    o.accel = QemuInfo::probeObjectProperties(chosen, "kvm-accel", &error);
    if (!error.isEmpty()) {
        o.accel = QStringList();
    }
    return o;
}

/* KEY written in any form: KEY=..., a bare KEY (on) or noKEY (off) */
static bool hasKey(const OptionValue &v, const QString &key)
{
    for (const OptionValue::Item &item : v.items()) {
        if (item.key == key || (item.bare && item.key == "no" + key)) {
            return true;
        }
    }
    return false;
}

/* The items that turn Venus off: venus=off (or no, false, n), novenus */
static bool isVenusOff(const OptionValue::Item &item)
{
    static const QStringList off = {"off", "no", "false", "n"};

    return (item.key == "venus" && !item.bare && off.contains(item.value.toLower())) ||
           (item.key == "novenus" && item.bare);
}

/* The properties -global sets, for any driver: the card's types take them too */
static QStringList globalProperties(const ArgsFile &args)
{
    QStringList out;

    for (int i : args.indexesOf("global")) {
        const OptionValue g = args.valueAt(i);
        if (g.has("property")) {
            out << g.get("property");
        } else if (!g.items().isEmpty()) {
            /* DRIVER.PROPERTY=VALUE, split at the first dot as QEMU does */
            out << g.items().first().key.section('.', 1);
        }
    }
    return out;
}

/* The -object line of guest RAM's backend, or -1 */
static int ramBackend(const ArgsFile &args)
{
    QString id;

    for (int i : args.indexesOf("machine")) {
        const OptionValue v = args.valueAt(i);
        if (v.has("memory-backend")) {
            id = v.get("memory-backend");
        }
    }
    if (id.isEmpty()) {
        return -1;
    }
    for (int i : args.indexesOf("object")) {
        const OptionValue v = args.valueAt(i);
        if (v.implied().startsWith("memory-backend-") && v.get("id") == id) {
            return i;
        }
    }
    return -1;
}

/*
 * Guest RAM to move to a shared memfd: QEMU's own (-m), or a RAM backend.
 * A memfd or file backend set up otherwise is left as written (share=off
 * on purpose, huge pages), as are NUMA nodes, which take backends of their
 * own, -mem-path, and a size left to QEMU, which the backend would change.
 */
static bool offersSharedMemory(const ArgsFile &args)
{
    const int backend = ramBackend(args);

    if (VmConfig::hasSharedMemory(args) || VmConfig::memoryMiB(args) <= 0 ||
        args.indexOf("numa") >= 0 || args.indexOf("mem-path") >= 0) {
        return false;
    }
    return backend < 0 || args.valueAt(backend).implied() == "memory-backend-ram";
}

QList<Change> changes(const ArgsFile &args, const Offers &offers)
{
    const int line = cardLine(args);
    QList<Change> out;

    if (line < 0) {
        return {};
    }
    const OptionValue card = args.valueAt(line);
    const QStringList global = globalProperties(args);
    const auto known = [&offers](const QString &key) {
        return !offers.card || offers.card->contains(key);
    };
    QString lead;

    for (const auto &[key, value] : VmTemplate::cardProperties()) {
        if (key == "x-vblank-lead") {
            lead = value;
        }
    }
    for (const auto &[key, value] : VmTemplate::cardProperties()) {
        if (hasKey(card, key) || global.contains(key) || !known(key)) {
            continue;
        }
        if (key.startsWith("x-vblank-")) {
            /* the lead of the host's vblank: with it on, and not off by hand */
            const bool hostVblank =
                card.flag("x-host-vblank") ||
                out.contains(Change{Change::AddProperty, "x-host-vblank", "on"});
            if (!hostVblank) {
                continue;
            }
            /* a lead of its own, fixed by hand, stays fixed */
            if (key == "x-vblank-lead-auto" && hasKey(card, "x-vblank-lead") &&
                card.get("x-vblank-lead") != lead) {
                continue;
            }
        }
        out << Change{Change::AddProperty, key, value};
    }

    /* the guest's caching of GPU mappings: an x86 KVM property */
    const QString accel = VmConfig::accel(args).section(':', 0, 0);
    const int accelLine = args.indexOf("accel");
    const bool pat = offers.accel ? offers.accel->contains("honor-guest-pat")
                                  : Paths::hostArch() == "x86_64";
    if (accel == "kvm" && pat &&
        (accelLine < 0 || !hasKey(args.valueAt(accelLine), "honor-guest-pat"))) {
        out << Change{Change::HonorGuestPat, "honor-guest-pat", "on"};
    }
    if (offersSharedMemory(args)) {
        out << Change{Change::SharedMemory, {}, {}};
    }

    /* tidying alone is not worth asking for */
    if (!out.isEmpty()) {
        for (const OptionValue::Item &item : card.items()) {
            if (isVenusOff(item)) {
                qsizetype at = 0;
                while (at < out.size() && out[at].kind == Change::AddProperty) {
                    at++;
                }
                out.insert(at, Change{Change::RemoveVenus, "venus", "off"});
                break;
            }
        }
    }
    return out;
}

QList<Change> changes(const ArgsFile &args)
{
    if (cardLine(args) < 0) {
        return {};
    }
    const std::optional<Offers> o = offers(args);
    return o ? changes(args, *o) : QList<Change>();
}

/*
 * @key=@value where the template writes it: after the last of the
 * template's properties before it that the card has, else before the
 * first after it, else at the end
 */
static OptionValue insertProperty(const OptionValue &card, const QString &key,
                                  const QString &value)
{
    QStringList order;
    QStringList items;
    qsizetype at = -1;

    for (const auto &p : VmTemplate::cardProperties()) {
        order << p.first;
    }
    const qsizetype rank = order.indexOf(key);
    for (qsizetype i = 0; i < card.items().size(); i++) {
        const OptionValue::Item &item = card.items()[i];
        const qsizetype r = order.indexOf(item.key);
        items << OptionValue::itemText(item);
        if (r >= 0 && r < rank) {
            at = i + 1;
        }
    }
    if (at < 0) {
        for (qsizetype i = 0; i < card.items().size(); i++) {
            if (order.indexOf(card.items()[i].key) > rank) {
                at = i;
                break;
            }
        }
    }
    if (at < 0) {
        at = items.size();
    }
    items.insert(at, OptionValue::escape(key) + '=' + OptionValue::escape(value));
    return OptionValue(items.join(','));
}

ArgsFile apply(const ArgsFile &args, const QList<Change> &changes)
{
    ArgsFile out = args;
    const int line = cardLine(out);

    /* the card first: the other changes may add lines before it */
    if (line >= 0) {
        OptionValue card = out.valueAt(line);
        for (const Change &c : changes) {
            if (c.kind == Change::AddProperty && !hasKey(card, c.key)) {
                card = insertProperty(card, c.key, c.value);
            } else if (c.kind == Change::RemoveVenus) {
                QStringList items;
                for (const OptionValue::Item &item : card.items()) {
                    if (!isVenusOff(item)) {
                        items << OptionValue::itemText(item);
                    }
                }
                card = OptionValue(items.join(','));
            }
        }
        if (card.toString() != out.valueAt(line).toString()) {
            out.setValueAt(line, card);
        }
    }
    /* changes found for other arguments do only what still applies */
    for (const Change &c : changes) {
        if (c.kind == Change::HonorGuestPat) {
            const int a = out.indexOf("accel");
            if (VmConfig::accel(out).section(':', 0, 0) == "kvm" &&
                (a < 0 || !hasKey(out.valueAt(a), c.key))) {
                VmConfig::setAccelProperty(out, c.key, c.value);
            }
        } else if (c.kind == Change::SharedMemory && offersSharedMemory(out)) {
            VmConfig::useSharedMemory(out);
        }
    }
    return out;
}

QString describe(const Change &change)
{
    static const QHash<QString, const char *> properties = {
        {"hostmem", QT_TRANSLATE_NOOP("CardUpdate",
                                      "A 4 GiB window for the memory the guest shares with "
                                      "this computer's GPU")},
        {"blob", QT_TRANSLATE_NOOP("CardUpdate",
                                   "Blob resources, which native context needs")},
        {"drm_native_context",
         QT_TRANSLATE_NOOP("CardUpdate",
                           "DRM native context: the guest's 3D goes through the driver of "
                           "this computer's GPU")},
        {"x-host-vblank", QT_TRANSLATE_NOOP("CardUpdate",
                                            "The guest's frames keep time with this "
                                            "computer's screen")},
        {"x-vblank-lead", QT_TRANSLATE_NOOP("CardUpdate",
                                            "The guest starts its frames 3 ms before the "
                                            "screen's refresh")},
        {"x-vblank-lead-auto", QT_TRANSLATE_NOOP("CardUpdate",
                                                 "That lead follows what this computer's "
                                                 "desktop needs")},
    };
    QString what;
    QString argument;

    switch (change.kind) {
    case Change::AddProperty:
        what = properties.contains(change.key) ? tr(properties.value(change.key)) : QString();
        argument = QString("%1=%2").arg(change.key, change.value);
        break;
    case Change::RemoveVenus:
        what = tr("No venus=off, which is QEMU's default anyway");
        break;
    case Change::HonorGuestPat:
        what = tr("KVM honors how the guest caches its GPU mappings, which Intel GPUs need");
        argument = tr("honor-guest-pat=on on -accel");
        break;
    case Change::SharedMemory:
        what = tr("Guest memory in a shared memfd, which the 3D card needs for resources "
                  "in guest memory");
        argument = "memory-backend-memfd,share=on";
        break;
    }
    if (what.isEmpty()) {
        return argument;
    }
    return argument.isEmpty() ? what : QString("%1 (%2)").arg(what, argument);
}

LineDiff diff(const ArgsFile &before, const ArgsFile &after)
{
    QStringList a = before.toText().split('\n');
    QStringList b = after.toText().split('\n');
    LineDiff out;

    /* the longest common subsequence of the lines: a few dozen of them */
    const qsizetype n = a.size(), m = b.size();
    QList<QList<int>> lcs(n + 1, QList<int>(m + 1, 0));
    for (qsizetype i = n - 1; i >= 0; i--) {
        for (qsizetype j = m - 1; j >= 0; j--) {
            lcs[i][j] = a[i] == b[j] ? lcs[i + 1][j + 1] + 1
                                     : qMax(lcs[i + 1][j], lcs[i][j + 1]);
        }
    }
    qsizetype i = 0, j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && a[i] == b[j]) {
            i++;
            j++;
        } else if (j < m && (i == n || lcs[i][j + 1] >= lcs[i + 1][j])) {
            out.after << b[j++];
        } else {
            out.before << a[i++];
        }
    }
    return out;
}

static QString declinedKey(const QString &vmId)
{
    return "cardupdate/declined/" + vmId;
}

bool isDeclined(const QString &vmId)
{
    return QSettings(Paths::settingsPath(), QSettings::IniFormat)
        .value(declinedKey(vmId), false)
        .toBool();
}

void setDeclined(const QString &vmId, bool declined)
{
    QSettings s(Paths::settingsPath(), QSettings::IniFormat);

    if (declined) {
        s.setValue(declinedKey(vmId), true);
    } else {
        s.remove(declinedKey(vmId));
    }
}

}
