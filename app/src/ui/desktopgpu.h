// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <optional>

#include <sys/types.h>

/*
 * The GPU this computer's desktop runs on, as its compositor names it: the
 * main device of zwp_linux_dmabuf_v1's default feedback (version 4), a DRM
 * device number (a primary or a render node), asked over the application's
 * Wayland connection on a queue of its own.  None on X11, offscreen, or
 * with a compositor that does not say.  The core takes it from here
 * (HostGpus::setDesktopDevice()), which keeps Wayland out of it.
 */
namespace DesktopGpu {

std::optional<dev_t> mainDevice();

}
