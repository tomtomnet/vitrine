// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QString>

#include "core/gpucontexts.h"
#include "core/guestshutdown.h"

class ArgsFile;
class Vm;
struct QemuInfo;

/*
 * What the GTK app says of a VM, in words: its state, the summary on its
 * console's first page, the sections of its details, the notices of a
 * shutdown.  The Qt app's words (VmDetails, VmConsole, ShutdownNotice),
 * from the same core calls; Qt Core only, no GTK, so that tests read them.
 */
namespace VmSummary {

/* "Running", "Powered off"... */
QString stateText(const Vm *vm);
/* The guest shut down, and -no-shutdown keeps QEMU open until Force Off */
bool keptOpen(const Vm *vm);
/* "4 GiB of memory, 4 processors, screen in this window.", then its disks */
QString homeText(const ArgsFile &args);

struct Row {
    QString key;
    QString value;
    /* a line under the value: a disk's usage */
    QString detail;
    /* a file or folder the row opens: the VM's folder, its log, a share */
    QString path;
};

struct Section {
    QString title;
    QList<Row> rows;
};

/*
 * The details of @vm, in sections as the Qt app's Details tab has them:
 * System, Display, Storage, Network and Sound, Shared Folders, USB
 * Devices, PCI Devices, Files; empty ones left out.  @info: the QEMU
 * documentation of its QEMU, null until loaded.  @contexts and
 * @venusUsed: what the guest does with the 3D card's contexts, while it
 * runs (GpuContexts); Unknown when stopped.
 */
QList<Section> details(const Vm *vm, const QemuInfo *info,
                       GpuContexts::Status contexts = GpuContexts::Status::Unknown,
                       bool venusUsed = false);

/*
 * Why a running guest does not get the DRM native context it was given,
 * for a note over its details; empty when it does, or nothing is known
 */
QString nativeContextNote(const Vm *vm, GpuContexts::Status contexts);

/* What a shutdown request of the VM @name says while it tries @way; empty for None */
QString shutdownText(const QString &name, GuestShutdown::Way way);
/* How long it says it, in ms: the guest may ask on its screen a while */
int shutdownTimeout(GuestShutdown::Way way);

}
