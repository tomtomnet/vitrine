// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>

#include <optional>

#include "core/argsfile.h"

/*
 * What the host's KVM can do that the VM's arguments may ask for.
 *
 * honor-guest-pat: KVM then uses the guest's caching of its mappings (the
 * GPU's, with native context) instead of ignoring it.  KVM offers that from
 * Linux 6.16 on, and only on CPUs with self-snoop (Intel) - on AMD it is
 * what KVM does anyway.  QEMU refuses to start with honor-guest-pat=on where
 * KVM cannot (Debian 13, Ubuntu 24.04 and RHEL 10 kernels among others);
 * auto does what KVM can.  Vitrine writes auto; VMs made before have on.
 */
namespace HostKvm {

/* The quirk QEMU disables for honor-guest-pat: offered by this host's KVM
   (true), not (false), or /dev/kvm cannot tell (no value: QEMU decides) */
std::optional<bool> canHonorGuestPat();

/*
 * @args as started on a host whose KVM can (@canHonor) or cannot honor the
 * guest's PAT: honor-guest-pat=on, which would make QEMU refuse to start
 * where it cannot, as auto, with why in @note; as they are otherwise
 */
ArgsFile withHostPat(const ArgsFile &args, std::optional<bool> canHonor, QString *note);

}
