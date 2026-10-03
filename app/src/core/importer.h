// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QStringList>

#include <functional>
#include <optional>

#include "core/argsfile.h"

/*
 * Imports the QEMU command of a launch script, or a pasted command line.
 * The text is split into words as sh would: quotes, escapes, line
 * continuations, ~, and the variables the script sets or the environment
 * has.  The QEMU command becomes the lines of a VM; paths relative to the
 * script become absolute, since QEMU runs in the VM folder; the other
 * commands of the script are kept as comments.
 */
namespace Importer {

struct Result {
    ArgsFile args;          // with #qemu if the script runs a QEMU by path
    QStringList notes;      // what to check by hand
    /* Relative paths not found from where the script runs: they stay
       relative, to the VM folder */
    QStringList missing;
};

/* The commands of @script, each as its words */
QList<QStringList> splitCommands(const QString &script, QStringList *notes = nullptr);

/*
 * @baseDir is where the script runs from.  @takesValue tells which options
 * take a value (from QemuInfo); without it, an option takes the next word
 * unless that starts with '-'.  Nothing if no command runs QEMU.
 */
std::optional<Result> importScript(const QString &script, const QString &baseDir,
                                   const std::function<bool(const QString &)> &takesValue = {});

}
