// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <optional>

#include <sys/types.h>

typedef struct _GdkDisplay GdkDisplay;

/*
 * The GPU this computer's desktop runs on, as its compositor names it: the
 * main device of zwp_linux_dmabuf_v1's default feedback (version 4: later
 * ones no longer send it), a DRM device number (a primary or a render
 * node), asked over GDK's Wayland connection on a queue of its own - the
 * Qt app's DesktopGpu, on GTK's connection.  None on another backend (the
 * tests' broadway) or with a compositor that does not say.  The core takes
 * it from here (HostGpus::setDesktopDevice()), which keeps Wayland out of
 * it.
 */
namespace DesktopGpu {

std::optional<dev_t> mainDevice(GdkDisplay *display);

}
