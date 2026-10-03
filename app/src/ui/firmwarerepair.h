// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QString>

#include "core/firmwarefiles.h"

class QWidget;
class Vm;

/*
 * The damaged firmware copies of a VM (core/firmwarefiles): what the user
 * is asked before a start, after a failed one, and by the Reset UEFI
 * Variables button of the settings.  The missing ones the start makes
 * again by itself.
 */
namespace FirmwareRepair {

/* The firmware copies in the folder of @vm, as its saved arguments name them */
QList<FirmwareFiles::File> files(const Vm *vm);

/*
 * Before a start: if firmware copies of @vm look damaged, asks whether to
 * put new ones in their place.  False when the start is called off.
 */
bool checkBeforeStart(QWidget *parent, Vm *vm);

/* The firmware copies of @vm the error of a failed start names */
QList<FirmwareFiles::File> named(const Vm *vm, const QString &error);

/*
 * Asks, then puts new copies of the templates of @files in their place,
 * keeping the old files; true when it did.  @accept is the text of the
 * button that does it.
 */
bool reset(QWidget *parent, Vm *vm, const QList<FirmwareFiles::File> &files,
           const QString &accept);

}
