// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>
#include <QStringList>

#include <optional>

#include "core/argsfile.h"

/*
 * Vitrine's settings of a 3D card, which the Display page turns on and
 * off: DRM native context, Venus, and the frame timing (the guest's vblank
 * locked to the host's, VmTemplate::cardProperties()), for a VM with one
 * virtio-gpu card with OpenGL (virtio-vga-gl, virtio-gpu-gl-pci...), and
 * what the QEMU the VM runs with offers of them.
 *
 * Turning one on adds what it needs and the VM lacks, as the template
 * writes it; a key set by hand that leaves it on stays the user's choice
 * (a hostmem of another size, a lead of its own).  Turning one off removes
 * its own keys, or says off where QEMU's default is on.  Every other key
 * and line stays as written.
 */
namespace CardSettings {

/*
 * What the QEMU a VM runs with has: the properties of a card's driver and
 * of kvm-accel, and its displays (-display help).  Not set: as vitrine's
 * QEMU has, all the template uses.
 */
struct Offers {
    std::optional<QStringList> card;
    std::optional<QStringList> accel;
    std::optional<QStringList> displays;
};

/*
 * Of the QEMU @args run with: its #qemu line, else the other QEMU of the
 * preferences, else vitrine's, for the card @driver (empty: the VM's own
 * 3D card, if it has one).  A chosen QEMU is asked (a few ms, kept until
 * the binary changes); one that does not answer offers nothing (no value),
 * unless it is vitrine's, and is asked again a minute later.  Vitrine's
 * has every property, built or not, and the displays a build has.
 */
std::optional<Offers> offers(const ArgsFile &args, const QString &driver = {});

enum class Feature {
    /*
     * DRM native context (drm_native_context=on), with blob resources in a
     * hostmem window, KVM honoring how the guest caches its GPU mappings
     * (honor-guest-pat=auto) and guest RAM in a shared memfd, which the
     * resources in guest memory need
     */
    NativeContext,
    /* Vulkan through Venus (venus=on), with blob resources in a hostmem window */
    Venus,
    /*
     * The guest's vblank at the host's display refresh (x-host-vblank),
     * ticked 3 ms before it (x-vblank-lead), that lead following what the
     * host's compositor needs (x-vblank-lead-auto)
     */
    FrameTiming,
};

/* The VM has a single virtio-gpu card with OpenGL, which takes these settings */
bool hasCard(const ArgsFile &args);
/* @feature is on for the VM's 3D card */
bool isOn(const ArgsFile &args, Feature feature);
/*
 * The QEMU of @offers has @feature for the card it was asked about.  But
 * Venus, which vitrine's QEMU lacks: its virglrenderer is built without it
 * (and the render server it runs), and QEMU's venus property, there all the
 * same, then fails the card's 3D altogether.  Only a QEMU chosen for the VM
 * that has the property offers it.
 */
bool isOffered(const Offers &offers, Feature feature);
/*
 * Turns @feature on or off for the VM's 3D card, with what the QEMU of
 * @offers has: QEMU refuses properties it does not know
 */
void set(ArgsFile &args, Feature feature, bool on, const Offers &offers);

}
