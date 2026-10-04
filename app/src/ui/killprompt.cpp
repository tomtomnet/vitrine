// SPDX-License-Identifier: GPL-2.0-or-later
#include "killprompt.h"

#include <QCoreApplication>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>

#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "ui/widgets.h"

static QString tr(const char *text)
{
    return QCoreApplication::translate("KillPrompt", text);
}

void KillPrompt::ask(QWidget *parent, Vm *vm)
{
    const QString name = "killPrompt-" + vm->id();

    if (auto *open = parent->findChild<QMessageBox *>(name, Qt::FindDirectChildrenOnly)) {
        open->raise();
        open->activateWindow();
        return;
    }
    if (vm->runner()->state() == VmRunner::State::Stopped) {
        return;
    }
    auto *box = Widgets::messageBox(
        QMessageBox::Warning, tr("%1 Does Not Respond").arg(vm->name()),
        tr("The QEMU of %1 does not end: it was asked to quit and is still running. It may "
           "still be writing to the VM's disks.")
            .arg(vm->name()),
        QMessageBox::NoButton, parent);
    box->setObjectName(name);
    box->setInformativeText(tr("Kill it to end it now: its disks may lose their last writes, "
                               "which can damage a qcow2 disk image. Or wait, and use Force "
                               "Off again later."));
    QPushButton *kill = box->addButton(tr("&Kill QEMU"), QMessageBox::DestructiveRole);
    QPushButton *wait = box->addButton(tr("&Wait"), QMessageBox::RejectRole);
    /* it comes up by itself: Enter or Escape must not kill anything */
    box->setDefaultButton(wait);
    box->setEscapeButton(wait);
    box->setAttribute(Qt::WA_DeleteOnClose);
    QObject::connect(vm->runner(), &VmRunner::stateChanged, box, [box](VmRunner::State state) {
        /* it ended by itself: nothing to decide */
        if (state == VmRunner::State::Stopped) {
            box->close();
        }
    });
    const QPointer<Vm> guard(vm);
    QObject::connect(box, &QMessageBox::finished, box, [box, kill, guard]() {
        if (box->clickedButton() == kill && guard) {
            guard->runner()->killQemu();
        }
    });
    box->open();
}
