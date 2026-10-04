// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

class QWidget;
class Vm;

/*
 * A VM's QEMU that does not end after Force Off (VmRunner::notResponding()):
 * vitrine never kills QEMU by itself, since a SIGKILL loses what QEMU had not
 * written to the disks yet.  The user decides, told what it costs.
 */
namespace KillPrompt {

/*
 * Asks over @parent whether to kill the QEMU of @vm: one box per VM at a
 * time (asked again, the open one comes to the front), gone when QEMU ends
 * by itself meanwhile
 */
void ask(QWidget *parent, Vm *vm);

}
