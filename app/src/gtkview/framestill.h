// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

struct FrameMailbox;
typedef struct _GdkDisplay GdkDisplay;
typedef struct _GdkTexture GdkTexture;

/*
 * The guest's frame on the screen now, as a texture of GTK's own, frozen:
 * what the view shows while something GTK draws covers it (VmDisplay::
 * setCovered()).  The guest's buffer (the mailbox's scanout) is imported by
 * GTK and read back at once - the guest may render into it again as soon as
 * the render thread lets it go - its visible part, upright.  A scanout in
 * shared memory is copied.  Null without a frame, or when GTK cannot import
 * the buffer.  On the GUI thread.
 */
GdkTexture *takeFrameStill(FrameMailbox *mailbox, GdkDisplay *display);
