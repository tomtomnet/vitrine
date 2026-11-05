// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QString>

struct QemuInfo;

/*
 * The QEMU reference of the GTK app's Arguments page, in words: what the
 * Qt app's ReferencePanel finds for a search, how it ranks it, the line an
 * entry adds to the arguments, and its documentation, here as Pango markup
 * rather than HTML.  Qt Core only, so that tests read it.
 */
namespace QemuReference {

enum Kind {
    Option, Device, Machine, Cpu, Object, Netdev, Chardev, Audiodev, Display, Accel, Property,
};

struct Hit {
    Kind kind;
    /* What it is known by: a property as "virtio-vga-gl,drm_native_context" */
    QString name;
    /* What the list shows: "-smp", a property's own name */
    QString label;
    QString desc;
};

struct Group {
    Kind kind;
    /* "Options", "Devices"... */
    QString title;
    /* All it found, the hits being at most a few hundred of them */
    int total = 0;
    QList<Hit> hits;
};

/*
 * The entries of @info for @query (words, any case, _ and - as spaces),
 * by kind, the best first: names first, then descriptions.  Device
 * properties, thousands of them, only for a query, and found by their
 * own name.  No query: everything but the properties.
 */
QList<Group> search(const QemuInfo &info, const QString &query);

/* The line an entry adds to the arguments: "-device virtio-vga-gl", "-smp " */
QString lineFor(const QemuInfo *info, Kind kind, const QString &name);

/* A part of an entry's documentation, in Pango markup */
struct Block {
    enum Type { Heading, Text, Code };
    Type type = Text;
    QString markup;
    /* A level of a definition list or a nested block, in steps */
    int indent = 0;
};

/*
 * What the entry @name of @kind is: its title, description, synopsis,
 * properties, the line to add.  @loading is set for a device whose
 * properties are not loaded yet (QemuDocs::loadProperties()).
 */
QList<Block> describe(const QemuInfo &info, Kind kind, const QString &name, bool *loading = nullptr);

/* QemuInfo::rstToHtml()'s HTML, the subset it writes, as Pango markup blocks */
QList<Block> fromHtml(const QString &html);

}
