// SPDX-License-Identifier: GPL-2.0-or-later
#include "argsfile.h"

#include <QRegularExpression>

static const QStringList kDirectives = {"share", "qemu"};

QString ArgsFile::canonicalName(const QString &option)
{
    if (option == "M") {
        return "machine";
    }
    return option;
}

ArgsFile ArgsFile::parse(const QString &text)
{
    ArgsFile file;

    for (const QString &raw : text.split('\n')) {
        const QString line = raw.trimmed();
        Line out;

        if (line.isEmpty()) {
            out.kind = Line::Blank;
        } else if (line.startsWith('#')) {
            const QString body = line.mid(1);
            const qsizetype space = body.indexOf(' ');
            const QString word = space < 0 ? body : body.left(space);

            if (kDirectives.contains(word)) {
                out.kind = Line::Directive;
                out.name = word;
                out.value = space < 0 ? QString() : body.mid(space + 1).trimmed();
            } else {
                out.kind = Line::Comment;
                out.text = line;
            }
        } else if (line.startsWith('-')) {
            QString option = line;
            const qsizetype space = option.indexOf(QRegularExpression("\\s"));

            out.kind = Line::Option;
            if (space >= 0) {
                out.value = option.mid(space + 1).trimmed();
                option.truncate(space);
            }
            out.name = option.mid(option.startsWith("--") ? 2 : 1);
        } else {
            /* not an option: keep it, as a comment QEMU never sees */
            out.kind = Line::Comment;
            out.text = "# " + line;
        }
        file.lines.append(out);
    }
    /* the final newline of the file */
    while (!file.lines.isEmpty() && file.lines.last().kind == Line::Blank) {
        file.lines.removeLast();
    }
    return file;
}

ArgsFile ArgsFile::fromArgv(const QStringList &argv,
                            const std::function<bool(const QString &)> &takesValue)
{
    ArgsFile file;

    for (qsizetype i = 1; i < argv.size(); i++) {
        const QString &arg = argv[i];
        Line line;

        if (!arg.startsWith('-') || arg.size() < 2) {
            continue;
        }
        line.kind = Line::Option;
        line.name = arg.mid(arg.startsWith("--") ? 2 : 1);
        if (takesValue(line.name) && i + 1 < argv.size()) {
            line.value = argv[++i];
        }
        file.lines.append(line);
    }
    return file;
}

QString ArgsFile::toText() const
{
    QString text;

    for (const Line &line : lines) {
        switch (line.kind) {
        case Line::Blank:
            break;
        case Line::Comment:
            text += line.text;
            break;
        case Line::Option:
            text += '-' + line.name;
            if (!line.value.isEmpty()) {
                text += ' ' + line.value;
            }
            break;
        case Line::Directive:
            text += '#' + line.name;
            if (!line.value.isEmpty()) {
                text += ' ' + line.value;
            }
            break;
        }
        text += '\n';
    }
    return text;
}

QStringList ArgsFile::argv() const
{
    QStringList args;

    for (const Line &line : lines) {
        if (line.kind == Line::Option) {
            args << '-' + line.name;
            if (!line.value.isEmpty()) {
                args << line.value;
            }
        }
    }
    return args;
}

QList<int> ArgsFile::indexesOf(const QString &name, Line::Kind kind) const
{
    QList<int> found;

    for (int i = 0; i < lines.size(); i++) {
        if (lines[i].kind == kind && canonicalName(lines[i].name) == name) {
            found << i;
        }
    }
    return found;
}

int ArgsFile::indexOf(const QString &name, Line::Kind kind) const
{
    const QList<int> found = indexesOf(name, kind);
    return found.isEmpty() ? -1 : found.first();
}

int ArgsFile::indexOfDevice(const std::function<bool(const QString &)> &match) const
{
    for (int i : indexesOf("device")) {
        if (match(valueAt(i).implied())) {
            return i;
        }
    }
    return -1;
}

OptionValue ArgsFile::valueAt(int index) const
{
    return OptionValue(lines[index].value);
}

void ArgsFile::setValueAt(int index, const OptionValue &value)
{
    lines[index].value = value.toString();
}

void ArgsFile::setValueAt(int index, const QString &value)
{
    lines[index].value = value;
}

int ArgsFile::add(const QString &name, const QString &value, Line::Kind kind)
{
    const QList<int> same = indexesOf(name, kind);
    Line line;
    int at = same.isEmpty() ? int(lines.size()) : same.last() + 1;

    line.kind = kind;
    line.name = name;
    line.value = value;
    lines.insert(at, line);
    return at;
}

void ArgsFile::removeAt(int index)
{
    lines.removeAt(index);
}

void ArgsFile::removeAll(const QString &name, Line::Kind kind)
{
    const QList<int> found = indexesOf(name, kind);

    for (qsizetype i = found.size() - 1; i >= 0; i--) {
        lines.removeAt(found[i]);
    }
}
