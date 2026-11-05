// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/qemureference.h"

#include <QRegularExpression>

#include <algorithm>

#include "core/qemuinfo.h"

namespace QemuReference {

/* Lowercase, with _ and - as spaces: "drm native" finds drm_native_context */
static QString searchable(const QString &text)
{
    QString s = text.toLower();
    return s.replace('_', ' ').replace('-', ' ');
}

/* "virtio-vga-gl,drm_native_context": the device and the property */
static std::pair<QString, QString> splitProperty(const QString &name)
{
    return {name.section(',', 0, 0), name.section(',', 1)};
}

static const char *const kGroups[] = {
    "Options",          "Devices",         "Machines", "CPU models", "Objects",
    "Network backends", "Character devices", "Audio backends", "Displays", "Accelerators",
    "Device properties",
};

QList<Group> search(const QemuInfo &info, const QString &query)
{
    struct Entry {
        Kind kind;
        QString name;
        QString desc;
        QString text;       // everything else to search
        QString label = {}; // if not the name
    };
    /* past that, a search for properties is too vague */
    const int maxHits = 300;
    const QStringList words = searchable(query).split(' ', Qt::SkipEmptyParts);
    const QString joined = words.join(' ');
    QList<Entry> entries;

    for (const QemuOptionDoc &o : info.options) {
        entries << Entry{Option, o.name, o.help.section('\n', 0, 0),
                         o.synopsis + ' ' + o.help + ' ' + o.details};
    }
    for (const QemuDeviceDoc &d : info.devices) {
        entries << Entry{Device, d.name, d.desc.isEmpty() ? d.category : d.desc,
                         d.category + ' ' + d.bus + ' ' + d.aliases.join(' ')};
    }
    const std::pair<Kind, const QList<QemuNamedDoc> *> lists[] = {
        {Machine, &info.machines}, {Cpu, &info.cpus},         {Object, &info.objects},
        {Netdev, &info.netdevs},   {Chardev, &info.chardevs}, {Audiodev, &info.audiodevs},
        {Display, &info.displays}, {Accel, &info.accels},
    };
    for (const auto &[kind, list] : lists) {
        for (const QemuNamedDoc &n : *list) {
            entries << Entry{kind, n.name, n.desc, {}, {}};
        }
    }
    /* thousands of them: only when searching */
    if (!words.isEmpty()) {
        for (const QemuDeviceDoc &d : info.devices) {
            for (const QemuPropertyDoc &p : info.properties.value(d.name)) {
                QString desc = d.name + " · " + p.type;
                if (!p.defaultValue.isEmpty()) {
                    desc += QString(" (default %1)").arg(p.defaultValue);
                }
                if (!p.desc.isEmpty()) {
                    desc += " · " + p.desc;
                }
                entries << Entry{Property, d.name + ',' + p.name, desc, {}, p.name};
            }
        }
    }

    QList<std::pair<int, const Entry *>> hits[Property + 1];
    for (const Entry &e : entries) {
        const QString name = searchable(e.label.isEmpty() ? e.name : e.label);
        const QString text = searchable(e.desc + ' ' + e.text);
        int score = 0, nameHits = 0;
        bool all = true;

        for (const QString &w : words) {
            if (name == w) {
                score += 100;
            } else if (name.startsWith(w)) {
                score += 50;
            } else if (name.contains(w)) {
                score += 20;
            } else if (text.contains(w)) {
                score += 1;
                continue;
            } else {
                all = false;
                break;
            }
            nameHits++;
        }
        if (!words.isEmpty() && name == joined) {
            score += 200;
        }
        /* a property is found by its name, not by its device's */
        if (all && (e.kind != Property || nameHits > 0)) {
            hits[e.kind] << std::pair(score, &e);
        }
    }

    QList<Group> groups;
    for (int kind = Option; kind <= Property; kind++) {
        auto &list = hits[kind];
        if (list.isEmpty()) {
            continue;
        }
        std::stable_sort(list.begin(), list.end(), [](const auto &a, const auto &b) {
            if (a.first != b.first) {
                return a.first > b.first;
            }
            return a.second->name.compare(b.second->name, Qt::CaseInsensitive) < 0;
        });
        Group group{Kind(kind), kGroups[kind], int(list.size()), {}};
        for (qsizetype i = 0; i < list.size() && i < maxHits; i++) {
            const Entry *e = list[i].second;
            group.hits << Hit{Kind(kind), e->name,
                              kind == Option ? '-' + e->name
                              : e->label.isEmpty() ? e->name
                                                   : e->label,
                              e->desc};
        }
        groups << group;
    }
    return groups;
}

QString lineFor(const QemuInfo *info, Kind kind, const QString &name)
{
    switch (kind) {
    case Option: {
        const QemuOptionDoc *o = info ? info->option(name) : nullptr;
        return '-' + name + (o && o->takesValue ? " " : "");
    }
    case Device:
        return "-device " + name;
    case Machine:
        return "-machine " + name;
    case Cpu:
        return "-cpu " + name;
    case Object:
        return "-object " + name + ",id=";
    case Netdev:
        return "-netdev " + name + ",id=";
    case Chardev:
        return "-chardev " + name + ",id=";
    case Audiodev:
        return "-audiodev " + name + ",id=";
    case Display:
        return "-display " + name;
    case Property: {
        const auto [device, property] = splitProperty(name);
        QString value;
        for (const QemuPropertyDoc &p : info ? info->properties.value(device)
                                             : QList<QemuPropertyDoc>()) {
            if (p.name == property && p.type == "bool") {
                value = "on";
            }
        }
        return "-device " + device + ',' + property + '=' + value;
    }
    case Accel:
        break;
    }
    return "-accel " + name;
}

static QString escaped(const QString &text)
{
    return text.toHtmlEscaped();
}

/* "Type  bool": a key and its value */
static Block row(const QString &key, const QString &value)
{
    return {Block::Text, QString("<b>%1</b>  %2").arg(escaped(key), escaped(value))};
}

QList<Block> describe(const QemuInfo &info, Kind kind, const QString &name, bool *loading)
{
    QList<Block> blocks;

    if (loading) {
        *loading = false;
    }
    if (kind == Property) {
        const auto [device, property] = splitProperty(name);
        const QemuDeviceDoc *d = info.device(device);

        for (const QemuPropertyDoc &p : info.properties.value(device)) {
            if (p.name != property) {
                continue;
            }
            blocks << Block{Block::Heading, escaped(p.name)};
            blocks << Block{Block::Text,
                            QString("A property of the <b>%1</b> device%2.")
                                .arg(escaped(device),
                                     d && !d->desc.isEmpty() ? ": " + escaped(d->desc) : QString())};
            blocks << row("Type", p.type);
            if (!p.defaultValue.isEmpty()) {
                blocks << row("Default", p.defaultValue);
            }
            if (!p.desc.isEmpty()) {
                blocks << row("Description", p.desc);
            }
            blocks << Block{Block::Text, "Use it with:"};
            blocks << Block{Block::Code, escaped(lineFor(&info, kind, name))};
        }
        return blocks;
    }
    if (kind == Option) {
        const QemuOptionDoc *o = info.option(name);
        if (!o) {
            return blocks;
        }
        blocks << Block{Block::Heading, escaped('-' + name)};
        if (!o->section.isEmpty()) {
            blocks << Block{Block::Text, "<i>" + escaped(o->section) + "</i>"};
        }
        blocks << Block{Block::Code, escaped(o->synopsis)};
        if (!o->details.isEmpty()) {
            blocks << fromHtml(QemuInfo::rstToHtml(o->details));
        } else if (!o->help.isEmpty()) {
            blocks << Block{Block::Code, escaped(o->help)};
        }
        return blocks;
    }
    if (kind == Device) {
        const QemuDeviceDoc *d = info.device(name);
        if (!d) {
            return blocks;
        }
        blocks << Block{Block::Heading, escaped(name)};
        if (!d->desc.isEmpty()) {
            blocks << Block{Block::Text, escaped(d->desc)};
        }
        blocks << row("Category", d->category);
        if (!d->bus.isEmpty()) {
            blocks << row("Bus", d->bus);
        }
        if (!d->aliases.isEmpty()) {
            blocks << row("Also called", d->aliases.join(", "));
        }
        if (!d->userCreatable) {
            blocks << row("Note", "Built into machines, cannot be added with -device");
        }
        blocks << Block{Block::Heading, "Properties"};
        if (!info.properties.contains(name)) {
            blocks << Block{Block::Text, "Loading the properties…"};
            if (loading) {
                *loading = true;
            }
        } else if (info.properties.value(name).isEmpty()) {
            blocks << Block{Block::Text, "None."};
        } else {
            for (const QemuPropertyDoc &p : info.properties.value(name)) {
                QString line = QString("<tt>%1</tt>  %2").arg(escaped(p.name), escaped(p.type));
                if (!p.defaultValue.isEmpty()) {
                    line += QString(" (default %1)").arg(escaped(p.defaultValue));
                }
                if (!p.desc.isEmpty()) {
                    line += " · " + escaped(p.desc);
                }
                blocks << Block{Block::Text, line};
            }
        }
        return blocks;
    }
    const QList<QemuNamedDoc> *list = nullptr;
    switch (kind) {
    case Machine:
        list = &info.machines;
        break;
    case Cpu:
        list = &info.cpus;
        break;
    case Object:
        list = &info.objects;
        break;
    case Netdev:
        list = &info.netdevs;
        break;
    case Chardev:
        list = &info.chardevs;
        break;
    case Audiodev:
        list = &info.audiodevs;
        break;
    case Display:
        list = &info.displays;
        break;
    default:
        list = &info.accels;
        break;
    }
    blocks << Block{Block::Heading, escaped(name)};
    for (const QemuNamedDoc &n : *list) {
        if (n.name == name && !n.desc.isEmpty()) {
            blocks << Block{Block::Text, escaped(n.desc)};
        }
    }
    const QString line = lineFor(&info, kind, name);
    blocks << Block{Block::Text, "Use it with:"};
    blocks << Block{Block::Code, escaped(line)};
    const QString option = line.section(' ', 0, 0).mid(1);
    if (info.option(option)) {
        blocks << Block{Block::Text,
                        QString("See the <b>-%1</b> option for its settings.").arg(escaped(option))};
    }
    return blocks;
}

QList<Block> fromHtml(const QString &html)
{
    static const QRegularExpression block(
        "<(p|pre)(?: style=\"margin-left:(\\d+)px\")?>(.*?)</\\1>",
        QRegularExpression::DotMatchesEverythingOption);
    /* the inline tags Pango has, by their names there */
    static const QRegularExpression tag("<(/?)(\\w+)((?:\\s[^>]*)?)>");
    QList<Block> blocks;

    for (auto it = block.globalMatch(html); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        QString markup;
        qsizetype from = 0;
        const QString body = m.captured(3);

        for (auto t = tag.globalMatch(body); t.hasNext();) {
            const QRegularExpressionMatch tm = t.next();
            const QString closing = tm.captured(1);
            const QString name = tm.captured(2).toLower();

            markup += body.mid(from, tm.capturedStart() - from);
            from = tm.capturedEnd();
            if (name == "b" || name == "i") {
                markup += '<' + closing + name + '>';
            } else if (name == "code") {
                markup += '<' + closing + "tt>";
            } else if (name == "a") {
                /* a link: its target kept, as GTK's labels open them */
                markup += closing.isEmpty() ? "<a" + tm.captured(3) + '>' : QString("</a>");
            }
            /* any other tag goes, its text stays */
        }
        markup += body.mid(from);
        /* the entities QString::toHtmlEscaped() writes are Pango's; but &nbsp; */
        markup.replace("&nbsp;", "&#160;");
        blocks << Block{m.captured(1) == "pre" ? Block::Code : Block::Text, markup,
                        m.captured(2).toInt() / 5};
    }
    return blocks;
}

}
