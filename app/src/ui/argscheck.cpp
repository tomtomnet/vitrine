// SPDX-License-Identifier: GPL-2.0-or-later
#include "argscheck.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>

#include "core/argsfile.h"
#include "core/qemuinfo.h"
#include "core/vmconfig.h"

static QString optionName(const QString &word)
{
    return word.mid(word.startsWith("--") ? 2 : 1);
}

/* the editor's words, as before */
static QString tr(const char *text)
{
    return QCoreApplication::translate("ArgsEditor", text);
}

QList<ArgsProblem> checkArgs(const QString &text, const QemuInfo *info, const QString &vmDir)
{
    const bool knowsOptions = info && !info->options.isEmpty();
    const bool knowsDevices = info && !info->devices.isEmpty();
    const QStringList lines = text.split('\n');
    QList<ArgsProblem> problems;

    for (int n = 0; n < lines.size(); n++) {
        const QString line = lines[n].trimmed();

        if (line.isEmpty()) {
            continue;
        }
        if (line.startsWith('#')) {
            if (line.startsWith("#qemu ") || line == "#qemu") {
                const QString qemu = VmConfig::qemuBinary(ArgsFile::parse(line));
                if (qemu.isEmpty()) {
                    problems << ArgsProblem{n, tr("#qemu needs the path of a QEMU binary")};
                } else if (!QFileInfo(qemu).isExecutable()) {
                    problems << ArgsProblem{n, tr("%1 is not an executable").arg(qemu)};
                }
            } else if (line.startsWith("#env ") || line == "#env") {
                if (!VmConfig::isEnvAssignment(line.mid(5).trimmed())) {
                    problems << ArgsProblem{n, tr("#env needs NAME=VALUE")};
                }
            } else if (line.startsWith("#share ") || line == "#share") {
                const OptionValue v(line.mid(7).trimmed());
                if (v.get("tag").isEmpty() || v.get("path").isEmpty()) {
                    problems << ArgsProblem{n, tr("#share needs tag= and path=")};
                } else if (!QFileInfo(v.get("path")).isDir()) {
                    problems << ArgsProblem{
                        n, tr("Shared folder %1 does not exist").arg(v.get("path"))};
                }
            }
            continue;
        }
        if (!line.startsWith('-') || line.size() < 2) {
            problems << ArgsProblem{n, tr("Not an option: QEMU never sees this line")};
            continue;
        }

        const qsizetype space = line.indexOf(QRegularExpression("\\s"));
        const QString name = optionName(space < 0 ? line : line.left(space));
        const QString value = space < 0 ? QString() : line.mid(space + 1).trimmed();

        if (knowsOptions) {
            const QemuOptionDoc *doc = info->option(name);
            if (!doc) {
                problems << ArgsProblem{n, tr("Unknown option -%1").arg(name)};
                continue;
            }
            if (doc->takesValue && value.isEmpty()) {
                problems << ArgsProblem{n, tr("-%1 needs a value").arg(name)};
            } else if (!doc->takesValue && !value.isEmpty()) {
                problems << ArgsProblem{n, tr("-%1 takes no value").arg(name)};
            }
        }
        if (knowsDevices && name == "device") {
            const QString driver = OptionValue(value).implied();
            if (!driver.isEmpty() && driver != "help" && !info->device(driver)) {
                problems << ArgsProblem{n, tr("Unknown device %1").arg(driver)};
            }
        }
    }
    if (!vmDir.isEmpty()) {
        const QDir dir(vmDir);
        for (const VmConfig::FileRef &f : VmConfig::files(ArgsFile::parse(text))) {
            if (QFileInfo::exists(dir.absoluteFilePath(f.path))) {
                continue;
            }
            problems << ArgsProblem{
                f.line, QDir::isAbsolutePath(f.path)
                            ? tr("%1 does not exist").arg(f.path)
                            : tr("%1 is not in the VM folder: give its full path, or copy it "
                                 "there").arg(f.path)};
        }
        std::stable_sort(problems.begin(), problems.end(),
                         [](const ArgsProblem &a, const ArgsProblem &b) {
                             return a.line < b.line;
                         });
    }
    return problems;
}
