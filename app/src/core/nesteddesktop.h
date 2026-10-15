// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>
#include <QStringList>

#include <optional>

#include "core/argsfile.h"

/*
 * The nested desktop: the guest's KDE session through a KWin of its own,
 * which draws into a Wayland window of this computer's desktop over
 * virtio-gpu-rutabaga's cross-domain (the research's crosvm-desktop, under
 * QEMU).  The VM has one card, virtio-vga-rutabaga with cross-domain on
 * (VmConfig::setNestedCard()), whose VGA part shows the firmware and the
 * boot loader; the guest, the nested session of the guest tools, which
 * starts when the boot's words (QEMU's fw_cfg) say crosvm_desktop=1.
 *
 * Where the guest's desktop shows follows the VM's display:
 *  - D-Bus, vitrine's window: the integrated mode.  The guest's window is a
 *    part of vitrine's (the console's Desktop view, app/src/nested), beside
 *    the VM's screen (the Screen view: the card's console, which the D-Bus
 *    display shows with OpenGL).  QEMU's cross-domain connects to a socket
 *    vitrine serves, which must listen before QEMU starts.  The guest's
 *    session does not outlive vitrine: it ends when vitrine quits, and the
 *    guest waits at its login screen.
 *  - SDL or GTK: a window of its own on this computer's desktop, QEMU's
 *    window showing the VM's screen; no display: that window alone.  QEMU
 *    connects to the compositor itself, through a socket the compositor
 *    treats as a sandbox's when it can (WaylandSandbox).
 *
 * One card, not two: until 2026-10-15 vitrine gave such VMs
 * virtio-gpu-rutabaga-pci beside QEMU's VGA.  OVMF shows its console, and
 * the boot loader's, on every display card, and draws on a card without
 * VGA through virtio commands, one round trip each: GRUB's menu, drawn at
 * that pace, held the boot (Fedora 44 KDE: the kernel at 11.6 s, against
 * 6.9 s with the one card, whose VGA part OVMF draws on directly).
 */
namespace NestedDesktop {

enum class Mode {
    Off,
    Integrated,     // the D-Bus display: in vitrine's window
    OwnWindow,      // SDL, GTK or no display: the guest's desktop in a window of its own
};
/* A rutabaga card with cross-domain on, and where the screen shows */
Mode mode(const ArgsFile &args);

/*
 * What the QEMU @args run with has of it: offered when it has
 * virtio-vga-rutabaga with cross-domain; @properties, the card's (not
 * set: QEMU not asked - vitrine's, not built yet, has them all).  Asked
 * from the VM's #qemu, else the preferences' QEMU, else vitrine's, in a
 * few ms, kept until the binary changes.
 */
struct Offer {
    bool offered = false;
    /* QEMU answered; else it is not offered, but for vitrine's QEMU not built yet */
    bool answered = false;
    std::optional<QStringList> properties;
};
Offer offer(const ArgsFile &args);

/*
 * The card's -device value: the research launcher's properties, those of
 * @properties if given (QEMU refuses the ones it lacks), cross-domain
 * always; the socket comes at start (withRunArgs())
 */
QString cardDevice(const std::optional<QStringList> &properties);

/*
 * On: the nested desktop's card in place of the VM's (with the properties
 * cardDevice() writes), guest RAM in a shared memfd (the card's blobs), the
 * in-kernel PIT dropping lost ticks rather than reinjecting them (which
 * keeps AVIC off for the whole VM), and an ISA pvpanic: a guest that panics
 * at the end of its shutdown says so, and the runner quits QEMU (Fedora's
 * kernel has no driver for pvpanic-pci).  Off: the template's 3D card takes
 * the card's place (its 3D settings are the Display page's); the rest
 * stays, harmless without it.
 */
void set(ArgsFile &args, bool on, const std::optional<QStringList> &properties = {});

/*
 * QEMU's console of the VM's screen in a run of @args, for the D-Bus
 * display: 0, the one card's; 1 with the two cards of before, whose QEMU's
 * VGA comes first - the virtio card's screen, where the login screen is (a
 * QEMU an older vitrine started, found running)
 */
int screenConsole(const ArgsFile &args);

/*
 * The words the guest's nested session reads at boot, as the research's
 * UEFI boot passes them through fw_cfg: crosvm_desktop=1,
 * crosvm_desktop_guest_dmabuf=1 when the card has x-guest-dmabuf on, and
 * crosvm_desktop_label=@label, the VM's name before the window's title
 * (one word: its spaces become no-break spaces)
 */
QString bootWords(const ArgsFile &args, const QString &label);
/*
 * @args with what the runner adds at start for the nested desktop: the
 * socket QEMU's cross-domain connects to (wayland-socket-path= on the card,
 * unless the line sets one) and bootWords() in fw_cfg's
 * opt/org.drm-native-context/cmdline (unless the VM has that entry)
 */
ArgsFile withRunArgs(const ArgsFile &args, const QString &socket, const QString &label);
/* QEMU's: SDL's renderer through Vulkan, as its OpenGL one clashes with rutabaga's */
QStringList environment(const ArgsFile &args);
/* This computer's compositor's socket (WAYLAND_DISPLAY in XDG_RUNTIME_DIR,
   unless a path); empty when there is none */
QString compositorSocket();

}
