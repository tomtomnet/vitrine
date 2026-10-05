// SPDX-License-Identifier: GPL-2.0-or-later
#include "cardsettings.h"

#include <QDeadlineTimer>
#include <QFileInfo>
#include <QHash>

#include <functional>

#include "core/paths.h"
#include "core/qemuinfo.h"
#include "core/vmconfig.h"
#include "core/vmhardware.h"
#include "core/vmtemplate.h"

namespace CardSettings {

/* The virtio-gpu cards with OpenGL, which take these settings */
static const QStringList kCards = {"virtio-vga-gl", "virtio-gpu-gl-pci", "virtio-gpu-gl",
                                   "virtio-gpu-gl-device"};

/* The user chose vitrine's QEMU by its path, which may not answer, say out of memory */
static bool isStackQemu(const QString &binary)
{
    const QString stack = Paths::stackQemu();
    return !stack.isEmpty() && QFileInfo(binary).canonicalFilePath() == stack;
}

/* The line of the VM's 3D card, or -1 */
static int cardLine(const ArgsFile &args)
{
    if (VmConfig::graphics(args).kind != VmConfig::Graphics::Accelerated) {
        return -1;
    }
    return args.indexOfDevice([](const QString &driver) { return kCards.contains(driver); });
}

bool hasCard(const ArgsFile &args)
{
    return cardLine(args) >= 0;
}

/*
 * What a QEMU offers, until it changes: the Display page asks each time it
 * shows a VM, and a QEMU that does not answer keeps it waiting 5 s.  One
 * that failed is asked again a minute later, in case it was only short of
 * memory.
 */
struct Probed {
    std::optional<Offers> offers;
    QDeadlineTimer expiry;
};

static std::optional<Offers> probed(const QString &binary, const QString &driver,
                                    const std::function<std::optional<Offers>()> &ask)
{
    static QHash<QString, Probed> cache;
    const QFileInfo fi(binary);
    const QString key =
        QStringList{binary, fi.canonicalFilePath(),
                    QString::number(fi.lastModified().toMSecsSinceEpoch()),
                    QString::number(fi.size()), driver, Paths::stackQemu()}
            .join('\n');

    if (auto it = cache.constFind(key); it != cache.constEnd() && !it->expiry.hasExpired()) {
        return it->offers;
    }
    const std::optional<Offers> out = ask();
    /* only what a QEMU that answered said is kept for good: it said its displays */
    cache.insert(key, {out, out && out->displays ? QDeadlineTimer(QDeadlineTimer::Forever)
                                                 : QDeadlineTimer(60 * 1000)});
    return out;
}

std::optional<Offers> offers(const ArgsFile &args, const QString &driver)
{
    QString chosen = VmConfig::qemuBinary(args);
    QString card = driver;

    if (card.isEmpty()) {
        const int line = cardLine(args);
        card = line < 0 ? QString() : args.valueAt(line).implied();
    }
    if (chosen.isEmpty()) {
        chosen = Paths::customQemuBinary();
    }
    /*
     * Vitrine's, built or not: every property the template writes.  Its
     * displays are those of the build there is, which may be older than
     * the one that brought a display (GTK), until it is built again.
     */
    if (chosen.isEmpty()) {
        const QString stack = Paths::stackQemu();
        if (stack.isEmpty()) {
            return Offers();
        }
        return probed(stack, {}, [&stack]() {
            QString error;
            Offers o;
            const QStringList displays = QemuInfo::probeList(stack, "display", &error);
            if (error.isEmpty()) {
                o.displays = displays;
            }
            return std::optional<Offers>(o);
        });
    }
    return probed(chosen, card, [&chosen, &card]() -> std::optional<Offers> {
        QString error;
        Offers o;

        o.displays = QemuInfo::probeList(chosen, "display", &error);
        if (!error.isEmpty()) {
            return isStackQemu(chosen) ? std::optional<Offers>(Offers()) : std::nullopt;
        }
        /* none for a card it lacks, or for a VM without a 3D card */
        o.card = card.isEmpty() ? QStringList() : QemuInfo::probeProperties(chosen, card, &error);
        if (!error.isEmpty()) {
            o.card = QStringList();
        }
        /* vitrine's, chosen by its path: without Venus all the same */
        if (isStackQemu(chosen)) {
            o.card->removeAll("venus");
        }
        error.clear();
        o.accel = QemuInfo::probeObjectProperties(chosen, "kvm-accel", &error);
        if (!error.isEmpty()) {
            o.accel = QStringList();
        }
        return o;
    });
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

/* A boolean's off, as QEMU's qapi_bool_parse() takes it */
static bool isOff(const QString &value)
{
    static const QStringList off = {"off", "no", "false", "n"};

    return off.contains(value.toLower());
}

/*
 * The properties -global sets, for any driver (the card's types take them
 * too), with their values: the last one counts, as QEMU applies them in order
 */
static QHash<QString, QString> globalProperties(const ArgsFile &args)
{
    QHash<QString, QString> out;

    for (int i : args.indexesOf("global")) {
        const OptionValue g = args.valueAt(i);
        if (g.has("property")) {
            out.insert(g.get("property"), g.get("value"));
        } else if (!g.items().isEmpty()) {
            /* DRIVER.PROPERTY=VALUE, split at the first dot as QEMU does */
            const OptionValue::Item &item = g.items().first();
            out.insert(item.key.section('.', 1), item.value);
        }
    }
    return out;
}

/*
 * The value @key is set to: on the card, where a bare KEY is on and noKEY
 * off and the last one counts, else with -global (the card's line wins
 * over it in QEMU); none when neither sets it
 */
static std::optional<QString> valueOf(const OptionValue &card,
                                      const QHash<QString, QString> &global,
                                      const QString &key)
{
    std::optional<QString> value;

    for (const OptionValue::Item &item : card.items()) {
        if (item.key == key) {
            value = item.bare ? QString("on") : item.value;
        } else if (item.bare && item.key == "no" + key) {
            value = QString("off");
        }
    }
    if (!value && global.contains(key)) {
        value = global.value(key);
    }
    return value;
}

/* @card without @key, in any form */
static OptionValue withoutKey(const OptionValue &card, const QString &key)
{
    QStringList items;

    for (const OptionValue::Item &item : card.items()) {
        if (item.key != key && !(item.bare && item.key == "no" + key)) {
            items << OptionValue::itemText(item);
        }
    }
    return OptionValue(items.join(','));
}

/*
 * @card with @key=@value: where @key was written, in whatever form, else
 * where the template writes it, after the last of the template's
 * properties before it that the card has, else before the first after it;
 * at the end for a key the template does not write
 */
static OptionValue withKey(const OptionValue &card, const QString &key, const QString &value)
{
    const QString text = OptionValue::escape(key) + '=' + OptionValue::escape(value);
    QStringList order;
    QStringList items;
    qsizetype at = -1;

    for (qsizetype i = 0; i < card.items().size(); i++) {
        const OptionValue::Item &item = card.items()[i];
        if (item.key == key || (item.bare && item.key == "no" + key)) {
            if (at < 0) {
                at = items.size();
                items << text;
            }
            continue;
        }
        items << OptionValue::itemText(item);
    }
    if (at >= 0) {
        return OptionValue(items.join(','));
    }

    for (const auto &p : VmTemplate::cardProperties()) {
        order << p.first;
    }
    const qsizetype rank = order.indexOf(key);
    if (rank >= 0) {
        for (qsizetype i = 0; i < card.items().size(); i++) {
            const qsizetype r = order.indexOf(card.items()[i].key);
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
    }
    if (at < 0) {
        at = items.size();
    }
    items.insert(at, text);
    return OptionValue(items.join(','));
}

/* The value the template gives @key */
static QString templateValue(const QString &key)
{
    for (const auto &[k, value] : VmTemplate::cardProperties()) {
        if (k == key) {
            return value;
        }
    }
    return {};
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
 * Guest RAM to move to a shared memfd: QEMU's own (-m), or a RAM backend
 * that leaves share out.  One with share set either way, or a memfd or
 * file backend set up otherwise, is left as written (share=off on purpose,
 * huge pages), as are NUMA nodes, which take backends of their own,
 * -mem-path, and a size left to QEMU, which the backend would change.
 */
static bool offersSharedMemory(const ArgsFile &args)
{
    const int backend = ramBackend(args);

    if (VmConfig::hasSharedMemory(args) || VmConfig::memoryMiB(args) <= 0 ||
        args.indexOf("numa") >= 0 || args.indexOf("mem-path") >= 0) {
        return false;
    }
    if (backend < 0) {
        return true;
    }
    const OptionValue v = args.valueAt(backend);
    return v.implied() == "memory-backend-ram" && !hasKey(v, "share");
}

/* The 3D features all keep their blob resources in a window of host memory */
static OptionValue withBlobs(OptionValue card, const QHash<QString, QString> &global,
                             const std::function<bool(const QString &)> &known)
{
    const std::optional<QString> blob = valueOf(card, global, "blob");

    if (known("blob") && (!blob || isOff(*blob))) {
        card = withKey(card, "blob", "on");
    }
    if (known("hostmem") && !valueOf(card, global, "hostmem")) {
        card = withKey(card, "hostmem", templateValue("hostmem"));
    }
    return card;
}

bool isOn(const ArgsFile &args, Feature feature)
{
    const int line = cardLine(args);

    if (line < 0) {
        return false;
    }
    const OptionValue card = args.valueAt(line);
    const QHash<QString, QString> global = globalProperties(args);
    const auto on = [&](const QString &key) {
        const std::optional<QString> value = valueOf(card, global, key);
        return value && !isOff(*value);
    };
    switch (feature) {
    case Feature::NativeContext:
        return on("drm_native_context");
    case Feature::Venus:
        return on("venus");
    case Feature::FrameTiming:
        return on("x-host-vblank");
    }
    return false;
}

bool isOffered(const Offers &offers, Feature feature)
{
    static const QHash<Feature, QString> keys = {
        {Feature::NativeContext, "drm_native_context"},
        {Feature::Venus, "venus"},
        {Feature::FrameTiming, "x-host-vblank"},
    };
    if (feature == Feature::Venus) {
        return offers.card && offers.card->contains(keys.value(feature));
    }
    return !offers.card || offers.card->contains(keys.value(feature));
}

void set(ArgsFile &args, Feature feature, bool on, const Offers &offers)
{
    const int line = cardLine(args);

    if (line < 0 || !isOffered(offers, feature)) {
        return;
    }
    const QHash<QString, QString> global = globalProperties(args);
    const auto known = [&offers](const QString &key) {
        return !offers.card || offers.card->contains(key);
    };
    OptionValue card = args.valueAt(line);
    bool nativeContextOn = false;

    switch (feature) {
    case Feature::NativeContext:
        if (on) {
            card = withKey(withBlobs(card, global, known), "drm_native_context", "on");
            nativeContextOn = true;
        } else {
            /* blob and hostmem stay: Venus and 2D resources use them too */
            card = withoutKey(card, "drm_native_context");
        }
        break;
    case Feature::Venus:
        card = on ? withKey(withBlobs(card, global, known), "venus", "on")
                  : withoutKey(card, "venus");
        break;
    case Feature::FrameTiming:
        if (on) {
            const std::optional<QString> host = valueOf(card, global, "x-host-vblank");
            const std::optional<QString> lead = valueOf(card, global, "x-vblank-lead");
            const QString templateLead = templateValue("x-vblank-lead");

            if (!host || isOff(*host)) {
                card = withKey(card, "x-host-vblank", "on");
            }
            if (known("x-vblank-lead") && !lead) {
                card = withKey(card, "x-vblank-lead", templateLead);
            }
            /* a lead of its own, fixed by hand, stays fixed: auto would move it */
            if (known("x-vblank-lead-auto") && !valueOf(card, global, "x-vblank-lead-auto") &&
                (!lead || *lead == templateLead)) {
                card = withKey(card, "x-vblank-lead-auto", templateValue("x-vblank-lead-auto"));
            }
        } else {
            /* off, as QEMU's default is on; the lead goes with it */
            card = withKey(card, "x-host-vblank", "off");
            card = withoutKey(withoutKey(card, "x-vblank-lead"), "x-vblank-lead-auto");
        }
        break;
    }
    if (card.toString() != args.valueAt(line).toString()) {
        args.setValueAt(line, card);
    }

    if (!nativeContextOn) {
        return;
    }
    /*
     * The guest maps the GPU's memory write-combined, which KVM ignores
     * without honor-guest-pat (Intel GPUs need it); auto, as on makes QEMU
     * refuse to start where KVM cannot (before Linux 6.16).  An x86 KVM
     * property: vitrine's QEMU has it on a PC.
     */
    const int accelLine = args.indexOf("accel");
    const bool pat = offers.accel ? offers.accel->contains("honor-guest-pat")
                                  : Paths::hostArch() == "x86_64";
    if (VmConfig::accel(args).section(':', 0, 0) == "kvm" && pat &&
        (accelLine < 0 || !hasKey(args.valueAt(accelLine), "honor-guest-pat"))) {
        VmConfig::setAccelProperty(args, "honor-guest-pat", "auto");
    }
    /* resources in guest memory reach the host's GPU as udmabufs, of memfd RAM */
    if (offersSharedMemory(args)) {
        VmConfig::useSharedMemory(args);
    }
}

}
