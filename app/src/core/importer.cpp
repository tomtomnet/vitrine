// SPDX-License-Identifier: GPL-2.0-or-later
#include "importer.h"

#include <QCoreApplication>
#include <QDir>
#include <QHash>
#include <QRegularExpression>

#include "core/vmconfig.h"
#include "core/vmhardware.h"

namespace Importer {

static QString tr(const char *text)
{
    return QCoreApplication::translate("Importer", text);
}

static bool isAssignment(const QString &word)
{
    static const QRegularExpression re("^[A-Za-z_]\\w*=");
    return re.match(word).hasMatch();
}

QList<QStringList> splitCommands(const QString &script, QStringList *notes)
{
    static const QRegularExpression variable("\\$(?:\\{(\\w+)\\}|(\\w+))");
    QHash<QString, QString> vars;
    QList<QStringList> commands;
    QStringList words;
    QString word;
    bool inWord = false;
    const qsizetype n = script.size();

    auto note = [&](const QString &text) {
        if (notes && !notes->contains(text)) {
            *notes << text;
        }
    };
    auto endWord = [&]() {
        if (inWord) {
            words << word;
            word.clear();
            inWord = false;
        }
    };
    auto endCommand = [&]() {
        endWord();
        if (words.isEmpty()) {
            return;
        }
        /* NAME=value, or export NAME=value, sets a variable */
        const QStringList assignments = words.first() == "export" ? words.mid(1) : words;
        if (std::all_of(assignments.begin(), assignments.end(), isAssignment)) {
            for (const QString &a : assignments) {
                vars[a.section('=', 0, 0)] = a.section('=', 1);
            }
        }
        commands << words;
        words.clear();
    };
    /* at a '$', moving @i to the end of what it expands */
    auto expand = [&](qsizetype &i) -> QString {
        const QRegularExpressionMatch m = variable.match(
            script, i, QRegularExpression::NormalMatch,
            QRegularExpression::AnchorAtOffsetMatchOption);

        if (!m.hasMatch()) {
            if (script.mid(i, 2) == "$(" || script.mid(i, 2) == "${") {
                note(tr("Command substitutions and ${...} expansions are copied as they are."));
            }
            return "$";
        }
        const QString name = m.captured(1).isEmpty() ? m.captured(2) : m.captured(1);
        i = m.capturedEnd() - 1;
        if (vars.contains(name)) {
            return vars[name];
        }
        if (qEnvironmentVariableIsSet(qPrintable(name))) {
            return qEnvironmentVariable(qPrintable(name));
        }
        note(tr("Unknown variable %1, copied as it is.").arg(m.captured(0)));
        return m.captured(0);
    };

    for (qsizetype i = 0; i < n; i++) {
        const QChar c = script[i];

        if (c == '\\') {
            /* a line continuation, or an escaped character */
            if (i + 1 < n && script[i + 1] != '\n') {
                word += script[i + 1];
                inWord = true;
            }
            i++;
        } else if (c == '\'') {
            qsizetype end = script.indexOf('\'', i + 1);
            if (end < 0) {
                end = n;
            }
            word += script.mid(i + 1, end - i - 1);
            inWord = true;
            i = end;
        } else if (c == '"') {
            inWord = true;
            for (i++; i < n && script[i] != '"'; i++) {
                if (script[i] == '\\' && i + 1 < n &&
                    QStringLiteral("$`\"\\\n").contains(script[i + 1])) {
                    if (script[i + 1] != '\n') {
                        word += script[i + 1];
                    }
                    i++;
                } else if (script[i] == '$') {
                    word += expand(i);
                } else {
                    word += script[i];
                }
            }
        } else if (c == '$') {
            word += expand(i);
            inWord = true;
        } else if (c == '#' && !inWord) {
            const qsizetype end = script.indexOf('\n', i);
            i = (end < 0 ? n : end) - 1;
        } else if (c == '~' && !inWord && (i + 1 == n || script[i + 1] == '/' ||
                                           script[i + 1].isSpace())) {
            word += QDir::homePath();
            inWord = true;
        } else if (c == '\n' || c == ';' || c == '&' || c == '|') {
            endCommand();
            /* && and || */
            if ((c == '&' || c == '|') && i + 1 < n && script[i + 1] == c) {
                i++;
            }
        } else if (c.isSpace()) {
            endWord();
        } else {
            word += c;
            inWord = true;
        }
    }
    endCommand();
    return commands;
}

std::optional<Result> importScript(const QString &script, const QString &baseDir,
                                   const std::function<bool(const QString &)> &takesValue)
{
    static const QRegularExpression qemu("^(qemu-system-.+|qemu-kvm)$");
    Result result;
    QDir base(baseDir);
    QStringList argv, others;

    for (const QStringList &command : splitCommands(script, &result.notes)) {
        qsizetype program = -1;

        if (argv.isEmpty()) {
            for (qsizetype i = 0; i < command.size() && program < 0; i++) {
                if (qemu.match(QFileInfo(command[i]).fileName()).hasMatch()) {
                    program = i;
                }
            }
        }
        if (program < 0) {
            /* the relative paths of QEMU are from there */
            if (argv.isEmpty() && command.size() == 2 && command[0] == "cd") {
                base.setPath(QDir::cleanPath(base.absoluteFilePath(command[1])));
            }
            others << command.join(' ');
            continue;
        }
        if (program > 0) {
            result.notes << tr("Not applied: %1 (environment variables and wrappers "
                               "around QEMU)").arg(command.mid(0, program).join(' '));
        }
        argv = command.mid(program);
    }
    if (argv.isEmpty()) {
        return std::nullopt;
    }


    ArgsFile &args = result.args;
    if (!others.isEmpty()) {
        args.lines << ArgsFile::Line{ArgsFile::Line::Comment, {}, {},
                                     "# " + tr("The script also ran:")};
        for (const QString &other : std::as_const(others)) {
            args.lines << ArgsFile::Line{ArgsFile::Line::Comment, {}, {}, "#   " + other};
        }
    }
    if (argv[0].contains('/')) {
        VmConfig::setQemuBinary(args, QDir::cleanPath(base.absoluteFilePath(argv[0])));
    }

    for (qsizetype i = 1; i < argv.size(); i++) {
        const QString &arg = argv[i];
        ArgsFile::Line line{ArgsFile::Line::Option, {}, {}, {}};

        if (arg.size() < 2 || !arg.startsWith('-')) {
            /* qemu-system-x86_64 disk.img */
            line.name = "hda";
            line.value = arg;
        } else {
            line.name = arg.mid(arg.startsWith("--") ? 2 : 1);
            const bool value = takesValue ? takesValue(line.name)
                                          : i + 1 < argv.size() && !argv[i + 1].startsWith('-');
            if (value && i + 1 < argv.size()) {
                line.value = argv[++i];
            }
        }
        if (line.name == "daemonize" || line.name == "pidfile") {
            result.notes << tr("-%1 was left out: the manager runs QEMU in the background "
                               "itself.").arg(line.name);
            continue;
        }
        args.lines << line;
    }

    /* as the Display page does: native context maps the host GPU's memory
       with the guest's caching, write-combining, which KVM ignores without it */
    if (VmConfig::graphics(args).nativeContext && VmConfig::accel(args).startsWith("kvm") &&
        VmConfig::accelProperty(args, "honor-guest-pat").isEmpty()) {
        VmConfig::setAccelProperty(args, "honor-guest-pat", "on");
        result.notes << tr("honor-guest-pat=on was added to -accel, for DRM native context.");
    }

    /* QEMU runs in the VM folder: the paths from the script's become absolute */
    for (const VmConfig::FileRef &file : VmConfig::files(args)) {
        const QString path = QDir::cleanPath(base.absoluteFilePath(file.path));

        if (QDir::isAbsolutePath(file.path)) {
            continue;
        }
        if (QFileInfo::exists(path)) {
            VmConfig::setFile(args, file, path);
        } else if (!result.missing.contains(file.path)) {
            result.missing << file.path;
        }
    }
    return result;
}

}
