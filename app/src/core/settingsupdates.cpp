// SPDX-License-Identifier: GPL-2.0-or-later
#include "settingsupdates.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QSettings>
#include <QStringDecoder>

#include <algorithm>
#include <vector>

#include "core/guesttools.h"
#include "core/hostdevices.h"
#include "core/nesteddesktop.h"
#include "core/paths.h"
#include "core/vmconfig.h"
#include "core/vmhardware.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"

namespace SettingsUpdates {

using Line = ArgsFile::Line;
using Item = OptionValue::Item;

static QString tr(const char *text)
{
    return QCoreApplication::translate("SettingsUpdates", text);
}

/* A line as vm.args has it written */
static QString lineText(const Line &line)
{
    ArgsFile one;
    one.lines << line;
    QString text = one.toText();
    text.chop(1);
    return text;
}

static bool sameLine(const Line &a, const Line &b)
{
    return a.kind == b.kind && a.name == b.name && a.value == b.value && a.text == b.text;
}

/* The lines @a and @b have in common, as (index in @a, index in @b), in order */
static QList<std::pair<int, int>> common(const QList<Line> &a, const QList<Line> &b)
{
    const int n = int(a.size());
    const int m = int(b.size());
    std::vector<int> length(size_t(n + 1) * size_t(m + 1), 0);
    const auto at = [m](int i, int j) { return size_t(i) * size_t(m + 1) + size_t(j); };
    QList<std::pair<int, int>> out;

    for (int i = n - 1; i >= 0; i--) {
        for (int j = m - 1; j >= 0; j--) {
            length[at(i, j)] = sameLine(a[i], b[j])
                                   ? length[at(i + 1, j + 1)] + 1
                                   : std::max(length[at(i + 1, j)], length[at(i, j + 1)]);
        }
    }
    for (int i = 0, j = 0; i < n && j < m;) {
        if (sameLine(a[i], b[j])) {
            out.append({i, j});
            i++;
            j++;
        } else if (length[at(i + 1, j)] >= length[at(i, j + 1)]) {
            i++;
        } else {
            j++;
        }
    }
    return out;
}

/*
 * @text with the lines of @after: those it has in common with @text stay
 * as @text has them, byte for byte, the others are written as ArgsFile
 * writes them
 */
static QString editedText(const QString &text, const ArgsFile &after)
{
    const ArgsFile before = ArgsFile::parse(text);
    const QStringList raw = text.split('\n');
    /* a file of Windows' line ends keeps them */
    const QString end = !raw.isEmpty() && raw.first().endsWith('\r') ? QString("\r") : QString();
    QStringList out;
    int next = 0;

    for (const auto &[b, a] : common(before.lines, after.lines)) {
        for (; next < a; next++) {
            out << lineText(after.lines[next]) + end;
        }
        out << raw[b];
        next = a + 1;
    }
    for (; next < after.lines.size(); next++) {
        out << lineText(after.lines[next]) + end;
    }
    /* the final line end, and the blank lines before it */
    for (qsizetype i = before.lines.size(); i < raw.size(); i++) {
        out << raw[i];
    }
    return out.join('\n');
}

/* The items of @v with the key @key */
static QList<Item> itemsOf(const OptionValue &v, const QString &key)
{
    QList<Item> out;

    for (const Item &item : v.items()) {
        if (item.key == key) {
            out << item;
        }
    }
    return out;
}

static bool sameItems(const QList<Item> &a, const QList<Item> &b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (qsizetype i = 0; i < a.size(); i++) {
        if (a[i].key != b[i].key || a[i].value != b[i].value || a[i].bare != b[i].bare) {
            return false;
        }
    }
    return true;
}

/* @key on @v once, as KEY=VALUE, with that value; or not there at all for none */
static bool hasValue(const OptionValue &v, const QString &key, const std::optional<QString> &value)
{
    const QList<Item> items = itemsOf(v, key);

    if (!value) {
        return items.isEmpty();
    }
    return items.size() == 1 && !items.first().bare && items.first().value == *value;
}

/* The values a key or a line's first value had and has, as edits; false where they cannot say it */
static bool lineEdits(const Line &before, const Line &after, int index, QList<Edit> *edits)
{
    const OptionValue b(before.value);
    const OptionValue a(after.value);
    QList<Edit> out;
    QStringList keys;
    Edit line;

    line.lineKind = after.kind;
    line.name = ArgsFile::canonicalName(after.name);
    line.implied = a.implied();
    line.index = index;
    if (b.implied() != a.implied()) {
        /* a first value that comes or goes is no change of it */
        if (b.implied().isEmpty() || a.implied().isEmpty()) {
            return false;
        }
        Edit e = line;
        e.kind = Edit::Implied;
        e.before = b.implied();
        e.after = a.implied();
        out << e;
    }
    for (const OptionValue *v : {&b, &a}) {
        for (const Item &item : v->items()) {
            if (!item.key.isEmpty() && !keys.contains(item.key)) {
                keys << item.key;
            }
        }
    }
    for (const QString &key : std::as_const(keys)) {
        const QList<Item> was = itemsOf(b, key);
        const QList<Item> is = itemsOf(a, key);

        if (sameItems(was, is)) {
            continue;
        }
        /* a bare word or a key given twice: no value to put back */
        if (was.size() > 1 || is.size() > 1 || (!was.isEmpty() && was.first().bare) ||
            (!is.isEmpty() && is.first().bare)) {
            return false;
        }
        Edit e = line;
        e.kind = Edit::Key;
        e.key = key;
        if (!was.isEmpty()) {
            e.before = was.first().value;
        }
        if (!is.isEmpty()) {
            e.after = is.first().value;
        }
        out << e;
    }
    *edits << out;
    return true;
}

/*
 * The edits that make @after of @before; @was and @becomes get the lines
 * they change, as they were and as they become
 */
static QList<Edit> diff(const ArgsFile &before, const ArgsFile &after, QStringList *was = nullptr,
                        QStringList *becomes = nullptr)
{
    QList<Edit> edits;
    int b0 = 0;
    int a0 = 0;

    /* the lines between two in common: those that went, and those that came */
    const auto gap = [&](int b1, int a1) {
        QHash<int, int> pairs;      // a line that went: the one that came in its place
        QSet<int> taken;

        /* the same option and first value, then the same option */
        for (int pass = 0; pass < 2; pass++) {
            for (int r = b0; r < b1; r++) {
                const Line &lb = before.lines[r];
                if (pairs.contains(r) || (lb.kind != Line::Option && lb.kind != Line::Directive)) {
                    continue;
                }
                for (int s = a0; s < a1; s++) {
                    const Line &la = after.lines[s];
                    if (taken.contains(s) || la.kind != lb.kind ||
                        ArgsFile::canonicalName(la.name) != ArgsFile::canonicalName(lb.name) ||
                        (pass == 0 &&
                         OptionValue(la.value).implied() != OptionValue(lb.value).implied())) {
                        continue;
                    }
                    pairs.insert(r, s);
                    taken.insert(s);
                    break;
                }
            }
        }
        for (int r = b0; r < b1; r++) {
            if (pairs.contains(r)) {
                const int s = pairs.value(r);
                if (lineEdits(before.lines[r], after.lines[s], s, &edits)) {
                    if (was) {
                        *was << lineText(before.lines[r]);
                    }
                    if (becomes) {
                        *becomes << lineText(after.lines[s]);
                    }
                    continue;
                }
                /* not as keys: the line goes, and the new one comes */
                taken.remove(s);
            }
            Edit e;
            e.kind = Edit::Remove;
            e.text = lineText(before.lines[r]);
            e.anchor = r > 0 ? lineText(before.lines[r - 1]) : QString();
            e.index = r;
            edits << e;
            if (was) {
                *was << e.text;
            }
        }
        for (int s = a0; s < a1; s++) {
            if (taken.contains(s)) {
                continue;
            }
            Edit e;
            e.kind = Edit::Insert;
            e.text = lineText(after.lines[s]);
            e.anchor = s > 0 ? lineText(after.lines[s - 1]) : QString();
            e.index = s;
            edits << e;
            if (becomes) {
                *becomes << e.text;
            }
        }
    };
    for (const auto &[b, a] : common(before.lines, after.lines)) {
        gap(b, a);
        b0 = b + 1;
        a0 = a + 1;
    }
    gap(int(before.lines.size()), int(after.lines.size()));
    return edits;
}

/*
 * The line of @args an edit was made on, as it is now: of its option,
 * with @matches, the one with the first value it had, then the nearest
 * to where it was; -1 for none
 */
static int findLine(const ArgsFile &args, const Edit &e,
                    const std::function<bool(const OptionValue &)> &matches)
{
    int best = -1;
    int bestScore = 0;

    for (int i = 0; i < args.lines.size(); i++) {
        const Line &line = args.lines[i];
        if (line.kind != e.lineKind || ArgsFile::canonicalName(line.name) != e.name) {
            continue;
        }
        const OptionValue v(line.value);
        if (!matches(v)) {
            continue;
        }
        const int score = (v.implied() == e.implied ? 0 : 1 << 20) + qAbs(i - e.index);
        if (best < 0 || score < bestScore) {
            best = i;
            bestScore = score;
        }
    }
    return best;
}

/* A line of @args as written, the nearest to @index; -1 for none */
static int findText(const ArgsFile &args, const QString &text, int index)
{
    int best = -1;

    for (int i = 0; i < args.lines.size(); i++) {
        if (lineText(args.lines[i]) == text && (best < 0 || qAbs(i - index) < qAbs(best - index))) {
            best = i;
        }
    }
    return best;
}

static bool revertKey(ArgsFile &args, const Edit &e)
{
    const int i =
        findLine(args, e, [&e](const OptionValue &v) { return hasValue(v, e.key, e.after); });

    if (i < 0) {
        /* put back by hand already */
        return e.before && findLine(args, e, [&e](const OptionValue &v) {
                               return hasValue(v, e.key, e.before);
                           }) >= 0;
    }
    OptionValue v = args.valueAt(i);
    if (e.before) {
        v.set(e.key, *e.before);
    } else {
        v.remove(e.key);
    }
    args.setValueAt(i, v);
    return true;
}

static bool revertImplied(ArgsFile &args, const Edit &e)
{
    Edit line = e;
    line.implied = e.after.value_or(QString());
    const int i = findLine(args, line, [&e](const OptionValue &v) {
        return v.implied() == e.after.value_or(QString());
    });

    if (i < 0) {
        return false;
    }
    OptionValue v = args.valueAt(i);
    v.setImplied(e.before.value_or(QString()));
    args.setValueAt(i, v);
    return true;
}

static bool revertInsert(ArgsFile &args, const Edit &e)
{
    const int exact = findText(args, e.text, e.index);

    if (exact >= 0) {
        args.removeAt(exact);
        return true;
    }
    /*
     * A line of keys alone (-machine hpet=off) that another change or the
     * user added to: its keys go, and the line if nothing is left
     */
    const QList<Line> parsed = ArgsFile::parse(e.text).lines;
    if (parsed.isEmpty() || (parsed.first().kind != Line::Option &&
                             parsed.first().kind != Line::Directive)) {
        return false;
    }
    const OptionValue added(parsed.first().value);
    if (!added.implied().isEmpty() || added.isEmpty()) {
        return false;
    }
    Edit line = e;
    line.lineKind = parsed.first().kind;
    line.name = ArgsFile::canonicalName(parsed.first().name);
    const int i = findLine(args, line, [&added](const OptionValue &v) {
        for (const Item &item : added.items()) {
            if (item.bare || !hasValue(v, item.key, item.value)) {
                return false;
            }
        }
        return true;
    });
    if (i < 0) {
        return false;
    }
    OptionValue v = args.valueAt(i);
    for (const Item &item : added.items()) {
        v.remove(item.key);
    }
    if (v.isEmpty()) {
        args.removeAt(i);
    } else {
        args.setValueAt(i, v);
    }
    return true;
}

static bool revertRemove(ArgsFile &args, const Edit &e)
{
    /* back already */
    if (findText(args, e.text, e.index) >= 0) {
        return true;
    }
    const QList<Line> parsed = ArgsFile::parse(e.text).lines;
    const Line line = parsed.isEmpty() ? Line() : parsed.first();
    int at = qMin(qMax(e.index, 0), int(args.lines.size()));

    if (e.anchor.isEmpty()) {
        at = 0;
    } else if (const int anchor = findText(args, e.anchor, e.index - 1); anchor >= 0) {
        at = anchor + 1;
    }
    args.lines.insert(at, line);
    return true;
}

/* The rules */

/* KEY on @v in any form: KEY=..., a bare KEY, or noKEY */
static bool hasKey(const OptionValue &v, const QString &key)
{
    for (const Item &item : v.items()) {
        if (item.key == key || (item.bare && item.key == "no" + key)) {
            return true;
        }
    }
    return false;
}

/* A -global sets @property, for any driver */
static bool globalSets(const ArgsFile &args, const QString &property)
{
    for (int i : args.indexesOf("global")) {
        const OptionValue g = args.valueAt(i);
        if (g.has("property") ? g.get("property") == property
                              : !g.items().isEmpty() &&
                                    g.items().first().key.section('.', 1) == property) {
            return true;
        }
    }
    return false;
}

/* A boolean KEY on: KEY=on (or yes, true, y), or a bare KEY */
static bool isOnFlag(const OptionValue &v, const QString &key)
{
    for (const Item &item : v.items()) {
        if (item.bare && item.key == key) {
            return true;
        }
    }
    return v.flag(key);
}

/* What #guest says, else what an agent said: Linux */
static bool isLinux(const ArgsFile &args, const Context &c)
{
    if (args.indexOf("guest", Line::Directive) >= 0) {
        const VmConfig::Guest guest = VmConfig::guest(args);
        if (!guest.os.isEmpty()) {
            return guest.os == "linux";
        }
        /* a desktop alone is a Linux one's */
        if (!guest.desktop.isEmpty()) {
            return true;
        }
    }
    return c.guestLinux;
}

/* A PC of QEMU's, its default machine included */
static bool isPc(const ArgsFile &args, const Context &c)
{
    const QString type = VmConfig::machineType(args);

    return c.hostArch == "x86_64" &&
           (type.isEmpty() || type == "q35" || type == "pc" || type == "isapc" ||
            type.startsWith("pc-"));
}

static bool anyMachineHas(const ArgsFile &args, const QString &key)
{
    for (int i : args.indexesOf("machine")) {
        if (hasKey(args.valueAt(i), key)) {
            return true;
        }
    }
    return false;
}

/* @key=@value on the last -machine, as QEMU merges them; a new one after -name without */
static void setMachineKey(ArgsFile &args, const QString &key, const QString &value)
{
    const QList<int> machines = args.indexesOf("machine");

    if (!machines.isEmpty()) {
        OptionValue v = args.valueAt(machines.last());
        v.set(key, value);
        args.setValueAt(machines.last(), v);
        return;
    }
    Line line;
    line.kind = Line::Option;
    line.name = "machine";
    line.value = key + '=' + value;
    const int name = args.indexOf("name");
    args.lines.insert(name < 0 ? int(args.lines.size()) : name + 1, line);
}

/* The one 3D card, with OpenGL or the nested desktop's; -1 for none, or several */
static int cardLine(const ArgsFile &args)
{
    int found = -1;

    for (int i : args.indexesOf("device")) {
        const QString driver = args.valueAt(i).implied();
        if (VmConfig::isAccelerated(driver) || NestedDesktop::isRutabagaCard(driver)) {
            if (found >= 0) {
                return -1;
            }
            found = i;
        }
    }
    return found;
}

/* The line of the 3D card the Display page's settings are for: @key on it, or with -global */
static bool cardSets(const ArgsFile &args, const QString &key)
{
    const int line = cardLine(args);

    return (line >= 0 && hasKey(args.valueAt(line), key)) || globalSets(args, key);
}

static bool nativeContextOn(const ArgsFile &args)
{
    if (CardSettings::isOn(args, CardSettings::Feature::NativeContext)) {
        return true;
    }
    const int line = args.indexOfDevice(NestedDesktop::isRutabagaCard);
    return line >= 0 && isOnFlag(args.valueAt(line), "x-drm");
}

/* A feature on -cpu in any form: topoext, +topoext, -topoext, topoext=... */
static bool hasCpuFeature(const OptionValue &cpu, const QString &feature)
{
    for (const Item &item : cpu.items()) {
        if (item.key.isEmpty()) {
            continue;
        }
        if (item.key == feature || item.key.mid(1) == feature) {
            return true;
        }
    }
    return false;
}

/*
 * The swap target VmRunner computes for a 3D card with OpenGL, which the
 * guest's desktop decides, applies: such a card with no target of its own,
 * in a QEMU that has the property (VmRunner::withComputedProperties())
 */
static bool computesSwapTarget(const ArgsFile &args, const Context &c)
{
    if (!c.offers || globalSets(args, "x-vblank-swap-target")) {
        return false;
    }
    for (int i : args.indexesOf("device")) {
        const OptionValue v = args.valueAt(i);
        if (VmConfig::isAccelerated(v.implied()) && !v.has("x-vblank-swap-target")) {
            return !c.offers->card || c.offers->card->contains("x-vblank-swap-target");
        }
    }
    return false;
}

struct Rule {
    const char *id;
    const char *title;
    const char *reason;
    QDate since;
    bool (*found)(const ArgsFile &args, const Context &c);
    void (*apply)(ArgsFile &args, const Context &c);
};

static const QList<Rule> &rules()
{
    static const QList<Rule> list = {
        {"hpet-off", QT_TRANSLATE_NOOP("SettingsUpdates", "No HPET timer"),
         QT_TRANSLATE_NOOP("SettingsUpdates",
                           "QEMU's HPET timer wakes this computer about a thousand times a "
                           "second, though Linux guests keep time without it: an idle VM drew "
                           "about 3 W more on a laptop, and kept its fans on."),
         QDate(2026, 10, 22),
         [](const ArgsFile &args, const Context &c) {
             return isPc(args, c) && isLinux(args, c) && args.indexOf("no-hpet") < 0 &&
                    !anyMachineHas(args, "hpet");
         },
         [](ArgsFile &args, const Context &) { setMachineKey(args, "hpet", "off"); }},

        {"hostmem-16g", QT_TRANSLATE_NOOP("SettingsUpdates", "A 16 GiB window for GPU memory"),
         QT_TRANSLATE_NOOP("SettingsUpdates",
                           "With the 4 GiB window of this VM's 3D card, apps in the guest can use "
                           "at most about 3.3 GB of the GPU's memory. The window is address "
                           "space only: nothing is allocated for it."),
         QDate(2026, 10, 26),
         [](const ArgsFile &args, const Context &) {
             const int line = cardLine(args);
             if (line < 0) {
                 return false;
             }
             const QList<Item> items = itemsOf(args.valueAt(line), "hostmem");
             return items.size() == 1 && !items.first().bare &&
                    VmConfig::parseSize(items.first().value, 1) == 4LL << 30;
         },
         [](ArgsFile &args, const Context &) {
             const int line = cardLine(args);
             OptionValue v = args.valueAt(line);
             v.set("hostmem", "16G");
             args.setValueAt(line, v);
         }},

        {"guest-kde", QT_TRANSLATE_NOOP("SettingsUpdates", "KDE Plasma as the guest's desktop"),
         QT_TRANSLATE_NOOP("SettingsUpdates",
                           "The guest said it runs KDE Plasma, which this VM's settings do not "
                           "say. Vitrine times a KDE guest's frames of its own: they show 1.5 ms "
                           "sooner, and in QEMU's own window without a copy. Also the General "
                           "page's Desktop."),
         QDate(2026, 10, 3),
         [](const ArgsFile &args, const Context &c) {
             if (!isLinux(args, c) || VmConfig::guest(args).desktop == "kde" ||
                 (c.guestDesktop != "kde" &&
                  NestedDesktop::mode(args) == NestedDesktop::Mode::Off)) {
                 return false;
             }
             /* what the desktop decides at start: the swap target, QEMU's window */
             const bool ownWindow = VmConfig::screen(args) == VmConfig::Screen::OwnWindow &&
                                    VmConfig::graphics(args).display != "gtk";
             return computesSwapTarget(args, c) || ownWindow;
         },
         [](ArgsFile &args, const Context &) { VmConfig::setGuest(args, {"linux", "kde"}); }},

        {"honor-guest-pat",
         QT_TRANSLATE_NOOP("SettingsUpdates", "GPU memory mapped as the guest asks"),
         QT_TRANSLATE_NOOP("SettingsUpdates",
                           "With DRM native context the guest maps the GPU's memory "
                           "write-combined, which KVM ignores without this setting, and Intel "
                           "GPUs need. Auto turns it on where this computer's kernel can."),
         QDate(2026, 10, 3),
         [](const ArgsFile &args, const Context &c) {
             if (c.hostArch != "x86_64" || !c.offers ||
                 VmConfig::accel(args).section(':', 0, 0) != "kvm" || !nativeContextOn(args)) {
                 return false;
             }
             const int accel = args.indexOf("accel");
             if (accel >= 0 && hasKey(args.valueAt(accel), "honor-guest-pat")) {
                 return false;
             }
             return !c.offers->accel || c.offers->accel->contains("honor-guest-pat");
         },
         [](ArgsFile &args, const Context &) {
             VmConfig::setAccelProperty(args, "honor-guest-pat", "auto");
         }},

        {"frame-timing",
         QT_TRANSLATE_NOOP("SettingsUpdates", "Frames in step with this computer's screen"),
         QT_TRANSLATE_NOOP("SettingsUpdates",
                           "The guest's refresh comes 3 ms before each refresh of this "
                           "computer's screen, as its desktop needs, so that each frame makes the "
                           "next one: without it frames show a refresh late. Also on the Display "
                           "page."),
         QDate(2026, 10, 3),
         [](const ArgsFile &args, const Context &c) {
             return c.offers && CardSettings::hasCard(args) && isLinux(args, c) &&
                    !cardSets(args, "x-host-vblank") &&
                    CardSettings::isOffered(*c.offers, CardSettings::Feature::FrameTiming);
         },
         [](ArgsFile &args, const Context &c) {
             CardSettings::set(args, CardSettings::Feature::FrameTiming, true, *c.offers);
         }},

        {"debug-threads", QT_TRANSLATE_NOOP("SettingsUpdates", "Named processor threads"),
         QT_TRANSLATE_NOOP("SettingsUpdates",
                           "Vitrine's host tuning finds the VM's processor threads by their "
                           "names: without them, while another VM is in front, this one's "
                           "processors lose the priority they get over other programs."),
         QDate(2026, 10, 3),
         [](const ArgsFile &args, const Context &c) {
             const int name = args.indexOf("name");
             return isLinux(args, c) && name >= 0 && !hasKey(args.valueAt(name), "debug-threads");
         },
         [](ArgsFile &args, const Context &) {
             const int name = args.indexOf("name");
             OptionValue v = args.valueAt(name);
             v.set("debug-threads", "on");
             args.setValueAt(name, v);
         }},

        {"vga-card", QT_TRANSLATE_NOOP("SettingsUpdates", "The 3D card with VGA"),
         QT_TRANSLATE_NOOP("SettingsUpdates",
                           "With its VGA part the card shows the firmware, the boot menu and the "
                           "boot messages, which UEFI draws quickly on it; without it the screen "
                           "stays blank until the guest's driver starts. Also on the Display "
                           "page."),
         QDate(2026, 10, 5),
         [](const ArgsFile &args, const Context &c) {
             const int line = cardLine(args);
             return isPc(args, c) && isLinux(args, c) && c.vgaCardOffered && line >= 0 &&
                    args.valueAt(line).implied() == "virtio-gpu-gl-pci" &&
                    VmConfig::graphics(args).kind == VmConfig::Graphics::Accelerated;
         },
         [](ArgsFile &args, const Context &) {
             /* as the Display page goes from one 3D card to the other: every key stays */
             const int line = cardLine(args);
             OptionValue v = args.valueAt(line);
             v.setImplied("virtio-vga-gl");
             args.setValueAt(line, v);
         }},

        {"native-context", QT_TRANSLATE_NOOP("SettingsUpdates", "DRM native context"),
         QT_TRANSLATE_NOOP("SettingsUpdates",
                           "The guest's 3D goes through the driver of this computer's GPU, much "
                           "faster than through virgl, which it uses without it. Also on the "
                           "Display page."),
         QDate(2026, 10, 3),
         [](const ArgsFile &args, const Context &c) {
             /* never set: the Display page's off keeps blob and hostmem */
             return c.offers && CardSettings::hasCard(args) && isLinux(args, c) &&
                    !cardSets(args, "drm_native_context") && !cardSets(args, "blob") &&
                    !cardSets(args, "hostmem") && VmConfig::hasSharedMemory(args) &&
                    CardSettings::isOffered(*c.offers, CardSettings::Feature::NativeContext);
         },
         [](ArgsFile &args, const Context &c) {
             CardSettings::set(args, CardSettings::Feature::NativeContext, true, *c.offers);
         }},

        {"topoext", QT_TRANSLATE_NOOP("SettingsUpdates", "Processor cores with two threads"),
         QT_TRANSLATE_NOOP("SettingsUpdates",
                           "The guest sees which of its processors share a core, and its caches, "
                           "as this computer's AMD processor has them; without it each thread "
                           "looks like a core of its own, and QEMU warns at every start."),
         QDate(2026, 10, 3),
         [](const ArgsFile &args, const Context &c) {
             const int cpu = args.indexOf("cpu");
             return c.hostArch == "x86_64" && c.hostTopoext && isLinux(args, c) && cpu >= 0 &&
                    args.valueAt(cpu).implied() == "host" && VmConfig::cpus(args).threads > 1 &&
                    !hasCpuFeature(args.valueAt(cpu), "topoext");
         },
         [](ArgsFile &args, const Context &) { VmConfig::enableCpuFeature(args, "topoext"); }},

        {"dump-guest-core", QT_TRANSLATE_NOOP("SettingsUpdates", "No guest memory in crash dumps"),
         QT_TRANSLATE_NOOP("SettingsUpdates",
                           "Should QEMU crash, its core dump leaves out the guest's memory, which "
                           "would otherwise be written to disk in full: gigabytes, and a long "
                           "wait."),
         QDate(2026, 10, 3),
         [](const ArgsFile &args, const Context &c) {
             return isPc(args, c) && isLinux(args, c) && !anyMachineHas(args, "dump-guest-core");
         },
         [](ArgsFile &args, const Context &) { setMachineKey(args, "dump-guest-core", "off"); }},
    };
    return list;
}

QStringList ids()
{
    QStringList out;

    for (const Rule &rule : rules()) {
        out << rule.id;
    }
    return out;
}

QString title(const QString &id)
{
    for (const Rule &rule : rules()) {
        if (id == rule.id) {
            return tr(rule.title);
        }
    }
    return {};
}

/* The options the rules read, which they cannot read in JSON */
static const QStringList kJsonOptions = {"machine", "device", "accel", "cpu",
                                         "name",    "smp",    "object", "global"};

QString problem(const QString &text)
{
    const QStringList raw = text.split('\n');

    for (qsizetype i = 0; i < raw.size(); i++) {
        const QString line = raw[i].trimmed();
        if (!line.isEmpty() && !line.startsWith('#') && !line.startsWith('-')) {
            return tr("its line %1 is no option: vitrine reads vm.args as one option per line")
                .arg(i + 1);
        }
    }
    const ArgsFile args = ArgsFile::parse(text);
    for (qsizetype i = 0; i < args.lines.size(); i++) {
        const Line &line = args.lines[i];
        if (line.kind != Line::Option) {
            continue;
        }
        const QString name = ArgsFile::canonicalName(line.name);
        if (name == "readconfig") {
            return tr("it reads settings from another file (-readconfig)");
        }
        if (name == "set") {
            return tr("it changes devices with -set, which vitrine does not follow");
        }
        if (kJsonOptions.contains(name) && line.value.trimmed().startsWith('{')) {
            return tr("its line %1 is written in JSON, which vitrine does not edit").arg(i + 1);
        }
    }
    return {};
}

Check check(const QString &text, const Context &context)
{
    Check out;

    out.problem = problem(text);
    if (!out.problem.isEmpty()) {
        return out;
    }
    const ArgsFile args = ArgsFile::parse(text);
    for (const Rule &rule : rules()) {
        if (!rule.found(args, context)) {
            continue;
        }
        ArgsFile after = args;
        rule.apply(after, context);
        Change change;
        change.id = rule.id;
        change.title = tr(rule.title);
        change.reason = tr(rule.reason);
        change.since = rule.since;
        diff(args, after, &change.before, &change.after);
        if (!change.before.isEmpty() || !change.after.isEmpty()) {
            out.changes << change;
        }
    }
    return out;
}

QString apply(const QString &text, const QStringList &ids, const Context &context, Record *record)
{
    if (!problem(text).isEmpty()) {
        return text;
    }
    ArgsFile args = ArgsFile::parse(text);
    for (const Rule &rule : rules()) {
        if (!ids.contains(rule.id) || !rule.found(args, context)) {
            continue;
        }
        ArgsFile after = args;
        rule.apply(after, context);
        const QList<Edit> edits = diff(args, after);
        if (edits.isEmpty()) {
            continue;
        }
        if (record) {
            record->changes << Record::Applied{rule.id, edits};
        }
        args = after;
    }
    return editedText(text, args);
}

QString revert(const QString &text, const Record &record, QStringList *failed)
{
    ArgsFile args = ArgsFile::parse(text);

    /* the latest change first; in each the keys, then the lines it added, then those it took */
    for (auto c = record.changes.crbegin(); c != record.changes.crend(); ++c) {
        bool ok = true;
        for (auto e = c->edits.crbegin(); e != c->edits.crend(); ++e) {
            if (e->kind == Edit::Key) {
                ok = revertKey(args, *e) && ok;
            } else if (e->kind == Edit::Implied) {
                ok = revertImplied(args, *e) && ok;
            }
        }
        for (auto e = c->edits.crbegin(); e != c->edits.crend(); ++e) {
            if (e->kind == Edit::Insert) {
                ok = revertInsert(args, *e) && ok;
            }
        }
        for (const Edit &e : c->edits) {
            if (e.kind == Edit::Remove) {
                ok = revertRemove(args, e) && ok;
            }
        }
        if (!ok && failed) {
            *failed << c->id;
        }
    }
    return editedText(text, args);
}

/* Records */

static const char *const kKinds[] = {"key", "implied", "insert", "remove"};

static QJsonValue optionalJson(const std::optional<QString> &value)
{
    return value ? QJsonValue(*value) : QJsonValue(QJsonValue::Null);
}

static std::optional<QString> optionalOf(const QJsonValue &value)
{
    return value.isString() ? std::optional<QString>(value.toString()) : std::nullopt;
}

QJsonObject Edit::toJson() const
{
    return {
        {"kind", kKinds[kind]},
        {"line", lineKind == ArgsFile::Line::Directive ? "directive" : "option"},
        {"name", name},
        {"implied", implied},
        {"key", key},
        {"before", optionalJson(before)},
        {"after", optionalJson(after)},
        {"text", text},
        {"anchor", anchor},
        {"index", index},
    };
}

Edit Edit::fromJson(const QJsonObject &json)
{
    Edit e;
    const QString kind = json["kind"].toString();

    for (int k = 0; k < int(std::size(kKinds)); k++) {
        if (kind == kKinds[k]) {
            e.kind = Kind(k);
        }
    }
    e.lineKind = json["line"].toString() == "directive" ? ArgsFile::Line::Directive
                                                        : ArgsFile::Line::Option;
    e.name = json["name"].toString();
    e.implied = json["implied"].toString();
    e.key = json["key"].toString();
    e.before = optionalOf(json["before"]);
    e.after = optionalOf(json["after"]);
    e.text = json["text"].toString();
    e.anchor = json["anchor"].toString();
    e.index = json["index"].toInt(-1);
    return e;
}

QJsonObject Record::toJson() const
{
    QJsonArray list;

    for (const Applied &applied : changes) {
        QJsonArray edits;
        for (const Edit &e : applied.edits) {
            edits << e.toJson();
        }
        list << QJsonObject{{"id", applied.id}, {"edits", edits}};
    }
    return {{"time", time.toString(Qt::ISODate)}, {"backup", backup}, {"changes", list}};
}

Record Record::fromJson(const QJsonObject &json)
{
    Record r;

    r.time = QDateTime::fromString(json["time"].toString(), Qt::ISODate);
    r.backup = json["backup"].toString();
    for (const QJsonValue &value : json["changes"].toArray()) {
        Applied applied;
        applied.id = value["id"].toString();
        for (const QJsonValue &e : value["edits"].toArray()) {
            applied.edits << Edit::fromJson(e.toObject());
        }
        r.changes << applied;
    }
    return r;
}

/* Per VM, in vitrine's settings: never in vm.args, which declining leaves alone */

static QString key(const QString &vmId, const char *name)
{
    return QString("settingsupdates/%1/%2").arg(vmId, name);
}

QStringList declined(const QString &vmId)
{
    return QSettings(Paths::settingsPath(), QSettings::IniFormat)
        .value(key(vmId, "declined"))
        .toStringList();
}

void setDeclined(const QString &vmId, const QString &id, bool declined)
{
    QSettings s(Paths::settingsPath(), QSettings::IniFormat);
    QStringList list = s.value(key(vmId, "declined")).toStringList();

    list.removeAll(id);
    if (declined) {
        list << id;
    }
    if (list.isEmpty()) {
        s.remove(key(vmId, "declined"));
    } else {
        s.setValue(key(vmId, "declined"), list);
    }
}

static QSet<QString> &notNowVms()
{
    static QSet<QString> vms;
    return vms;
}

bool notNow(const QString &vmId)
{
    return notNowVms().contains(vmId);
}

void setNotNow(const QString &vmId)
{
    notNowVms().insert(vmId);
}

QString guestDesktop(const QString &vmId)
{
    return QSettings(Paths::settingsPath(), QSettings::IniFormat)
        .value(key(vmId, "desktop"))
        .toString();
}

void setGuestDesktop(const QString &vmId, const QString &desktop)
{
    QSettings s(Paths::settingsPath(), QSettings::IniFormat);

    if (desktop.isEmpty()) {
        s.remove(key(vmId, "desktop"));
    } else {
        s.setValue(key(vmId, "desktop"), desktop);
    }
}

QList<Record> records(const QString &vmId)
{
    const QString json = QSettings(Paths::settingsPath(), QSettings::IniFormat)
                             .value(key(vmId, "records"))
                             .toString();
    QList<Record> out;

    for (const QJsonValue &value : QJsonDocument::fromJson(json.toUtf8()).array()) {
        out << Record::fromJson(value.toObject());
    }
    return out;
}

void setRecords(const QString &vmId, const QList<Record> &records)
{
    QSettings s(Paths::settingsPath(), QSettings::IniFormat);
    QJsonArray list;

    for (const Record &r : records) {
        list << r.toJson();
    }
    if (list.isEmpty()) {
        s.remove(key(vmId, "records"));
    } else {
        s.setValue(key(vmId, "records"),
                   QString::fromUtf8(QJsonDocument(list).toJson(QJsonDocument::Compact)));
    }
}

void forget(const QString &vmId)
{
    QSettings(Paths::settingsPath(), QSettings::IniFormat).remove("settingsupdates/" + vmId);
    notNowVms().remove(vmId);
}

bool readText(const QString &path, QString *text, QString *error)
{
    QFile f(path);

    if (!f.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = f.errorString();
        }
        return false;
    }
    const QByteArray bytes = f.readAll();
    QStringDecoder utf8(QStringDecoder::Utf8);
    const QString decoded = utf8(bytes);
    if (utf8.hasError()) {
        if (error) {
            *error = tr("it is not UTF-8 text");
        }
        return false;
    }
    *text = decoded;
    return true;
}

QString backupName(const QString &dir, const QDateTime &time)
{
    const QString base = "vm.args." + time.toString("yyyy-MM-dd-HHmmss");
    QString name = base + ".bak";

    for (int n = 2; QFileInfo::exists(QDir(dir).filePath(name)); n++) {
        name = QString("%1-%2.bak").arg(base).arg(n);
    }
    return name;
}

}

using namespace SettingsUpdates;

SettingsUpdatesMonitor *SettingsUpdatesMonitor::of(Vm *vm)
{
    if (auto *m = vm->findChild<SettingsUpdatesMonitor *>(QString(), Qt::FindDirectChildrenOnly)) {
        return m;
    }
    return new SettingsUpdatesMonitor(vm);
}

SettingsUpdatesMonitor::SettingsUpdatesMonitor(Vm *vm) : QObject(vm), m_vm(vm)
{
    connect(vm, &Vm::changed, this, &SettingsUpdatesMonitor::refresh);
    /* off or on: whether they can be applied */
    connect(vm->runner(), &VmRunner::stateChanged, this, &SettingsUpdatesMonitor::refresh);
    connect(GuestToolsMonitor::of(vm), &GuestToolsMonitor::changed, this,
            &SettingsUpdatesMonitor::guestReported);
    refresh();
}

QList<Change> SettingsUpdatesMonitor::offered() const
{
    const QStringList no = SettingsUpdates::declined(m_vm->id());
    QList<Change> out;

    for (const Change &c : m_check.changes) {
        if (!no.contains(c.id)) {
            out << c;
        }
    }
    return out;
}

QList<Change> SettingsUpdatesMonitor::declined() const
{
    const QStringList no = SettingsUpdates::declined(m_vm->id());
    QList<Change> out;

    for (const Change &c : m_check.changes) {
        if (no.contains(c.id)) {
            out << c;
        }
    }
    return out;
}

QList<Record> SettingsUpdatesMonitor::records() const
{
    return SettingsUpdates::records(m_vm->id());
}

QString SettingsUpdatesMonitor::blocked() const
{
    if (m_vm->runner()->isActive()) {
        return tr("%1 is running").arg(m_vm->name());
    }
    return m_vm->runner()->busy();
}

bool SettingsUpdatesMonitor::notNow() const
{
    return SettingsUpdates::notNow(m_vm->id());
}

void SettingsUpdatesMonitor::setNotNow()
{
    SettingsUpdates::setNotNow(m_vm->id());
    refresh();
}

void SettingsUpdatesMonitor::setDeclined(const QString &id, bool declined)
{
    SettingsUpdates::setDeclined(m_vm->id(), id, declined);
    refresh();
}

Context SettingsUpdatesMonitor::context(const ArgsFile &args) const
{
    /* the host's CPU stays the same */
    static const bool topoext = HostDevices::cpuHasFlag("topoext");
    const GuestTools::System system = GuestToolsMonitor::of(m_vm)->system();
    Context c;

    c.hostArch = Paths::hostArch();
    c.hostTopoext = topoext;
    c.offers = CardSettings::offers(args);
    /* asked only of a VM with the card without VGA, which may move to it */
    if (VmConfig::graphics(args).device == "virtio-gpu-gl-pci") {
        const std::optional<CardSettings::Offers> vga = CardSettings::offers(args, "virtio-vga-gl");
        c.vgaCardOffered = vga && (!vga->card || !vga->card->isEmpty());
    }
    /* qemu-ga names Windows mswindows; os-release, Linux */
    c.guestLinux = system.isKnown() && system.id != "mswindows";
    c.guestDesktop = SettingsUpdates::guestDesktop(m_vm->id());
    if (c.guestDesktop.isEmpty() &&
        (system.id == "kde-linux" || system.name.contains("KDE", Qt::CaseSensitive))) {
        c.guestDesktop = "kde";
    }
    return c;
}

void SettingsUpdatesMonitor::guestReported()
{
    const QHash<QString, QString> &desktops = GuestToolsMonitor::of(m_vm)->report().desktops;
    QString desktop;

    if (desktops.contains("kde") && !desktops.contains("gnome")) {
        desktop = "kde";
    } else if (desktops.contains("gnome") && !desktops.contains("kde")) {
        desktop = "gnome";
    }
    if (!desktop.isEmpty() && desktop != SettingsUpdates::guestDesktop(m_vm->id())) {
        SettingsUpdates::setGuestDesktop(m_vm->id(), desktop);
    }
    refresh();
}

void SettingsUpdatesMonitor::refresh()
{
    QString text;
    QString error;
    Check check;

    if (!SettingsUpdates::readText(m_vm->argsPath(), &text, &error)) {
        check.problem = error;
    } else {
        check = SettingsUpdates::check(text, context(ArgsFile::parse(text)));
    }
    m_check = check;

    /* what the pages show of it, to say only when that changes */
    QStringList state{m_check.problem, blocked(), notNow() ? "not now" : ""};
    for (const Change &c : m_check.changes) {
        state << c.id << c.before << c.after;
    }
    state << SettingsUpdates::declined(m_vm->id());
    for (const Record &r : records()) {
        state << r.time.toString(Qt::ISODate);
    }
    const QString key = state.join('\n');
    if (key != m_state) {
        m_state = key;
        emit changed();
    }
}

bool SettingsUpdatesMonitor::apply(const QList<Change> &shown, QString *error, bool *stale)
{
    const auto fail = [error](const QString &why) {
        if (error) {
            *error = why;
        }
        return false;
    };
    QString text;
    QString readError;

    if (stale) {
        *stale = false;
    }
    if (const QString why = blocked(); !why.isEmpty()) {
        return fail(why);
    }
    if (!SettingsUpdates::readText(m_vm->argsPath(), &text, &readError)) {
        return fail(readError);
    }
    const ArgsFile args = ArgsFile::parse(text);
    const Context c = context(args);
    const Check now = SettingsUpdates::check(text, c);
    QStringList ids;

    if (!now.problem.isEmpty()) {
        return fail(now.problem);
    }
    /* never what the user did not see */
    for (const Change &change : shown) {
        if (!now.changes.contains(change)) {
            refresh();
            if (stale) {
                *stale = true;
            }
            return fail(tr("vm.args changed meanwhile: the list is up to date again"));
        }
        ids << change.id;
    }
    Record record;
    const QString out = SettingsUpdates::apply(text, ids, c, &record);
    if (out == text || record.changes.isEmpty()) {
        refresh();
        return true;
    }
    record.time = QDateTime::currentDateTime();
    record.backup = SettingsUpdates::backupName(m_vm->dir(), record.time);
    if (!QFile::copy(m_vm->argsPath(), QDir(m_vm->dir()).filePath(record.backup))) {
        return fail(tr("Cannot save a copy of vm.args as %1").arg(record.backup));
    }
    QList<Record> list = SettingsUpdates::records(m_vm->id());
    list << record;
    SettingsUpdates::setRecords(m_vm->id(), list);
    QString saveError;
    if (!m_vm->saveText(out, &saveError)) {
        list.removeLast();
        SettingsUpdates::setRecords(m_vm->id(), list);
        refresh();
        return fail(saveError);
    }
    return true;
}

bool SettingsUpdatesMonitor::undo(QStringList *failed, QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error) {
            *error = why;
        }
        return false;
    };
    QList<Record> list = SettingsUpdates::records(m_vm->id());
    QString text;
    QString readError;

    if (const QString why = blocked(); !why.isEmpty()) {
        return fail(why);
    }
    if (list.isEmpty()) {
        return fail(tr("There is no update to undo"));
    }
    if (!SettingsUpdates::readText(m_vm->argsPath(), &text, &readError)) {
        return fail(readError);
    }
    const Record last = list.last();
    QStringList failedIds;
    const QString out = SettingsUpdates::revert(text, last, &failedIds);
    if (out != text) {
        const QString backup =
            SettingsUpdates::backupName(m_vm->dir(), QDateTime::currentDateTime());
        QString saveError;
        if (!QFile::copy(m_vm->argsPath(), QDir(m_vm->dir()).filePath(backup))) {
            return fail(tr("Cannot save a copy of vm.args as %1").arg(backup));
        }
        list.removeLast();
        SettingsUpdates::setRecords(m_vm->id(), list);
        if (!m_vm->saveText(out, &saveError)) {
            list << last;
            SettingsUpdates::setRecords(m_vm->id(), list);
            refresh();
            return fail(saveError);
        }
    } else {
        list.removeLast();
        SettingsUpdates::setRecords(m_vm->id(), list);
        refresh();
    }
    if (failed) {
        for (const QString &id : std::as_const(failedIds)) {
            *failed << SettingsUpdates::title(id);
        }
    }
    return true;
}
