// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

#include "optionvalue.h"

/*
 * The configuration of a VM: its QEMU command line, one option per line.
 *
 *   -option value
 *
 * The value is the rest of the line, used as is: no quoting, spaces and
 * all.  Lines starting with '#' are comments, except for the manager's
 * directives, which QEMU never sees:
 *
 *   #share tag=...,path=...[,cache=...]   a folder shared with virtiofs
 *   #qemu /path/to/qemu-system-x86_64     the QEMU of this VM, if not the
 *                                         one in the preferences
 *
 * Blank lines, comments and the order of the lines survive editing.
 */
class ArgsFile
{
public:
    struct Line {
        enum Kind { Blank, Comment, Option, Directive };
        Kind kind = Blank;
        QString name;       // option or directive, without dashes or '#'
        QString value;      // the rest of the line
        QString text;       // a comment as written
    };

    static ArgsFile parse(const QString &text);
    /*
     * From a command line split into arguments.  @takesValue tells which
     * options take a value; the first argument, the QEMU binary, and
     * arguments that are no option (a disk image) are skipped.
     */
    static ArgsFile fromArgv(const QStringList &argv,
                             const std::function<bool(const QString &)> &takesValue);
    QString toText() const;
    /* The arguments for QEMU, without the binary and the directives */
    QStringList argv() const;

    /* Lines of option or directive @name, "M" standing for "machine" */
    QList<int> indexesOf(const QString &name,
                         Line::Kind kind = Line::Option) const;
    int indexOf(const QString &name, Line::Kind kind = Line::Option) const;
    /* The first -device line whose driver @match accepts, or -1 */
    int indexOfDevice(const std::function<bool(const QString &)> &match) const;

    OptionValue valueAt(int index) const;
    void setValueAt(int index, const OptionValue &value);
    void setValueAt(int index, const QString &value);
    /* After the last line of the same option, else at the end */
    int add(const QString &name, const QString &value = {},
            Line::Kind kind = Line::Option);
    void removeAt(int index);
    void removeAll(const QString &name, Line::Kind kind = Line::Option);

    QList<Line> lines;

    static QString canonicalName(const QString &option);
};
