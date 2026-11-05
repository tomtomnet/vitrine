// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QString>

struct QemuInfo;

/*
 * What is wrong with the arguments of a VM, line by line, as the QEMU
 * documentation of its QEMU and its folder tell: what both apps' argument
 * editors list under the text.  Qt Core only.
 */

/* What is wrong with the arguments, per line (0-based) */
struct ArgsProblem {
    int line;
    QString message;
};
/* With @vmDir, the files the arguments read must exist (relative to it) */
QList<ArgsProblem> checkArgs(const QString &text, const QemuInfo *info,
                             const QString &vmDir = {});
