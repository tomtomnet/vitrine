// SPDX-License-Identifier: GPL-2.0-or-later
#include "firmwarerepair.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMessageBox>
#include <QPushButton>

#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "ui/widgets.h"

namespace FirmwareRepair {

using FirmwareFiles::File;

static QString tr(const char *text)
{
    return QCoreApplication::translate("FirmwareRepair", text);
}

QList<File> files(const Vm *vm)
{
    return FirmwareFiles::list(vm->args(), vm->dir());
}

/* What putting new copies in the place of @files does */
static QString consequences(const QList<File> &files)
{
    bool vars = false, snapshots = false;
    QStringList text;

    for (const File &f : files) {
        vars |= f.role == File::Role::Vars;
        snapshots |= f.role == File::Role::Vars && FirmwareFiles::snapshotCount(f.path) > 0;
    }
    text << (vars ? tr("The UEFI variables become those of a new VM: the boot entries, the boot "
                       "order and the Secure Boot keys enrolled since are gone. Most systems "
                       "still start, from the fallback boot loader they install.")
                  : tr("Nothing of the VM is lost: the firmware code holds no settings."));
    text << tr("The old files stay in the VM folder, renamed to end in .bak.");
    if (snapshots) {
        text << tr("The snapshots keep the variables they were taken with, unless the old "
                   "file can no longer be read.");
    }
    return text.join("\n\n");
}

/* Puts the new copies in place, or tells why not */
static bool replace(QWidget *parent, Vm *vm, const QList<File> &files)
{
    const QString qemuImg = FirmwareFiles::qemuImg(vm->args());
    QStringList errors;

    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    for (const File &f : files) {
        QString error;
        if (!FirmwareFiles::reset(f, qemuImg, nullptr, nullptr, &error)) {
            errors << error;
        }
    }
    QGuiApplication::restoreOverrideCursor();
    if (!errors.isEmpty()) {
        Widgets::warn(parent, tr("Firmware"), errors.join('\n'));
        return false;
    }
    return true;
}

bool checkBeforeStart(QWidget *parent, Vm *vm)
{
    QList<File> damaged;
    QStringList lines;

    for (const File &f : files(vm)) {
        /* the start makes the missing ones again; without a template QEMU tells */
        if (!QFileInfo::exists(f.path) || f.templatePath.isEmpty()) {
            continue;
        }
        const QString problem = FirmwareFiles::problem(f);
        if (!problem.isEmpty()) {
            damaged << f;
            lines << QString("%1: %2").arg(f.name(), problem);
        }
    }
    if (damaged.isEmpty()) {
        return true;
    }

    QMessageBox box(QMessageBox::Warning, tr("Damaged Firmware"),
                    tr("Firmware files of %1 look damaged:").arg(vm->name()) + "\n\n" +
                        lines.join('\n'),
                    QMessageBox::NoButton, parent);
    QPushButton *renew = box.addButton(tr("&Replace and Start"), QMessageBox::AcceptRole);
    QPushButton *anyway = box.addButton(tr("Start &Anyway"), QMessageBox::ActionRole);

    box.addButton(QMessageBox::Cancel);
    box.setInformativeText(tr("New copies can replace them.") + "\n\n" + consequences(damaged));
    /* the KDE dialog would choose the default button itself */
    box.setOption(QMessageBox::Option::DontUseNativeDialog);
    box.setDefaultButton(renew);
    box.exec();
    if (box.clickedButton() == anyway) {
        return true;
    }
    return box.clickedButton() == renew && replace(parent, vm, damaged);
}

QList<File> named(const Vm *vm, const QString &error)
{
    QList<File> list;

    /* a lock names the file too, but a file in use is not damaged */
    if (FirmwareFiles::inUse(error)) {
        return list;
    }
    for (const File &f : files(vm)) {
        if (!f.templatePath.isEmpty() && error.contains(f.name())) {
            list << f;
        }
    }
    return list;
}

bool reset(QWidget *parent, Vm *vm, const QList<File> &files, const QString &accept)
{
    bool vars = false;

    if (files.isEmpty()) {
        return false;
    }
    if (vm->runner()->isActive()) {
        Widgets::inform(parent, tr("Firmware"),
                        tr("Shut %1 down first.").arg(vm->name()));
        return false;
    }
    for (const File &f : files) {
        vars |= f.role == File::Role::Vars;
    }
    QMessageBox box(QMessageBox::Question,
                    vars ? tr("Reset UEFI Variables") : tr("Replace Firmware"),
                    vars ? tr("Reset the UEFI variables of %1?").arg(vm->name())
                         : tr("Replace the firmware of %1 with a new copy?").arg(vm->name()),
                    QMessageBox::NoButton, parent);
    QPushButton *renew = box.addButton(accept, QMessageBox::DestructiveRole);
    QPushButton *cancel = box.addButton(QMessageBox::Cancel);

    box.setInformativeText(consequences(files));
    box.setOption(QMessageBox::Option::DontUseNativeDialog);
    box.setDefaultButton(cancel);
    box.exec();
    return box.clickedButton() == renew && replace(parent, vm, files);
}

}
