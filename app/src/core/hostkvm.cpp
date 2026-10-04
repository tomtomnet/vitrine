// SPDX-License-Identifier: GPL-2.0-or-later
#include "hostkvm.h"

#include <QCoreApplication>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "core/vmhardware.h"

/* <linux/kvm.h> and <asm/kvm.h> (x86), stable ABI: not every build host has them */
static const unsigned long kKvmCheckExtension = 0xae03;   // _IO(KVMIO, 0x03)
static const int kCapDisableQuirks2 = 213;                // KVM_CAP_DISABLE_QUIRKS2
static const int kQuirkIgnoreGuestPat = 1 << 9;           // KVM_X86_QUIRK_IGNORE_GUEST_PAT

std::optional<bool> HostKvm::canHonorGuestPat()
{
    const int fd = ::open("/dev/kvm", O_RDWR | O_CLOEXEC);

    if (fd < 0) {
        return std::nullopt;
    }
    /* the quirks a VM may disable: those QEMU checks before it disables
       this one (target/i386/kvm/kvm.c); older kernels lack the capability
       (0) or the quirk */
    const int quirks = ::ioctl(fd, kKvmCheckExtension, kCapDisableQuirks2);
    ::close(fd);
    if (quirks < 0) {
        return std::nullopt;
    }
    return (quirks & kQuirkIgnoreGuestPat) != 0;
}

ArgsFile HostKvm::withHostPat(const ArgsFile &args,
                              const std::function<std::optional<bool>()> &canHonor, QString *note)
{
    /* /dev/kvm asked only when it matters */
    if (!VmConfig::accel(args).startsWith("kvm") ||
        VmConfig::accelProperty(args, "honor-guest-pat") != "on" || canHonor().value_or(true)) {
        return args;
    }
    ArgsFile out = args;
    VmConfig::setAccelProperty(out, "honor-guest-pat", "auto");
    if (note) {
        *note = QCoreApplication::translate(
            "HostKvm", "honor-guest-pat=on started as auto: this host's KVM cannot honor the "
                       "guest's memory types (Linux 6.16 and later can, on CPUs with "
                       "self-snoop), and QEMU would not start with on");
    }
    return out;
}
