// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

#include "core/argsfile.h"

/*
 * Vitrine's 3D card for the Linux VMs made before the template gave it
 * (VmTemplate::cardProperties()), or by hand: what the VM lacks of it, and
 * the update that adds that.  New VMs made while Fedora's QEMU stood in
 * for vitrine's got a card without native context and the vblank timing,
 * and -accel kvm without honor-guest-pat; older ones may have guest RAM
 * that is no shared memfd, which udmabuf needs for guest-memory blobs.
 *
 * Only what the VM leaves out is offered: a property set by hand, to any
 * value and on the card's line or with -global, is the user's choice.
 * Applying changes those keys and lines alone; every other key and line
 * stays as written.
 */
namespace CardUpdate {

struct Change {
    enum Kind {
        /* @key=@value on the card, which lacks @key */
        AddProperty,
        /* venus=off off the card: QEMU's default, a line that says nothing */
        RemoveVenus,
        /* honor-guest-pat=on on -accel kvm */
        HonorGuestPat,
        /* guest RAM in a shared memfd backend, of the same size */
        SharedMemory,
    };
    Kind kind = AddProperty;
    QString key;
    QString value;

    bool operator==(const Change &other) const = default;
};

/*
 * What the QEMU the VM runs with has: the properties of its card's driver
 * and of kvm-accel.  Not set: vitrine's QEMU, built or not, which has all
 * the template uses.
 */
struct Offers {
    std::optional<QStringList> card;
    std::optional<QStringList> accel;
};

/*
 * Of the QEMU @args run with: its #qemu line, else the other QEMU of the
 * preferences, else vitrine's.  A chosen QEMU is asked (a few ms, kept);
 * one that does not answer offers nothing (no value), unless it is
 * vitrine's.
 */
std::optional<Offers> offers(const ArgsFile &args);

/*
 * What @args lack of vitrine's card, for a Linux guest (the #guest
 * directive; without it, one without Hyper-V enlightenments) on a PC with
 * a single virtio-gpu card with OpenGL (virtio-gpu-gl-pci, virtio-vga-gl,
 * virtio-gpu-gl), in the order to show them; none otherwise.  The vblank
 * lead goes with the host vblank, which must not be off; venus=off goes
 * only with other changes.
 */
QList<Change> changes(const ArgsFile &args, const Offers &offers);
/* With offers(): none when the VM's QEMU does not answer */
QList<Change> changes(const ArgsFile &args);

/*
 * @args with @changes: the card's new properties in the template's order
 * among those it has, -accel and the memory backend as the settings
 * pages write them (VmConfig)
 */
ArgsFile apply(const ArgsFile &args, const QList<Change> &changes);

/* What @change does, in plain words, then its argument */
QString describe(const Change &change);

/* The lines that differ between @before and @after, in their order */
struct LineDiff {
    QStringList before;
    QStringList after;
};
LineDiff diff(const ArgsFile &before, const ArgsFile &after);

/*
 * Don't Ask Again, per VM: a setting of vitrine's keyed by the VM's id,
 * which leaves vm.args alone
 */
bool isDeclined(const QString &vmId);
void setDeclined(const QString &vmId, bool declined);

}
