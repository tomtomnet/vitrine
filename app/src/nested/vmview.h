/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 tomtomnet */
/* vmview: a VM's desktop window shown inside a window of the app (2026-10-12, route B of
 * findings/vitrine-embedding-plan-2026-10-12.md).
 *
 * QEMU's virtio-gpu cross-domain (wayland-socket-path=) connects to a socket this library serves
 * instead of the host compositor's. The guest's Wayland protocol is passed on to the host compositor
 * on the APP's own connection (its own event queue, its own thread), so the guest's surfaces are
 * surfaces of the app: the guest's xdg_toplevel becomes a desynchronized subsurface of a surface of
 * the app's window. Buffers (dma-buf), presentation feedback, commit timing and fifo go straight
 * through: in full screen the host can scan the guest's frames out as it does the standalone window.
 * Only host globals of a fixed list reach the guest (see vmview.cpp), at most at the versions this
 * library knows.
 *
 * Threads: vmview_* may be called from any thread. The callbacks run on the library's thread: hand
 * them over to the app's GUI thread. After vmview_set_geometry the app must commit its parent
 * surface (a subsurface's position applies with the parent's next commit): repaint the window.
 */
#ifndef VMVIEW_H
#define VMVIEW_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wl_display;
struct wl_surface;
struct vmview;

struct vmview_callbacks {
    void *data;
    /* the guest's window title (the nested desktop's live stats are in it) */
    void (*title)(void *data, const char *title);
    /* the guest asks for full screen (on) or a window (off), e.g. its Ctrl+Alt+F */
    void (*fullscreen)(void *data, bool on);
    /* a guest connection came or went (connected: the number of connections) */
    void (*connection)(void *data, int connected);
    /* the guest's window was mapped (shown) or unmapped */
    void (*mapped)(void *data, bool mapped);
    /* a pointer button went down on the guest's window, which the app never sees: it may give
       the guest the keyboard (vitrine) */
    void (*pressed)(void *data);
};

/* Serve SOCKET_PATH (created; an old socket there is replaced) and show the guest's window as a
 * subsurface of PARENT, a surface of the app on DISPLAY (the app's own connection). NULL on error
 * (errno set, a message on stderr). */
struct vmview *vmview_new(struct wl_display *display, struct wl_surface *parent, const char *socket_path,
                          const struct vmview_callbacks *callbacks);
/* Where the guest's window goes, in PARENT's logical coordinates, and its size (the guest's desktop
 * follows the size). */
void vmview_set_geometry(struct vmview *view, int x, int y, int width, int height);
/* The app's window is full screen: the guest's window is told so (its configure states). */
void vmview_set_fullscreen(struct vmview *view, bool fullscreen);
/* The guest gets the keyboard while the app's window has it (default on); off: keys stay with the app. */
void vmview_set_keyboard_focus(struct vmview *view, bool focus);
/* Stops serving, closes the guest's connections and destroys everything it made on the display. */
void vmview_destroy(struct vmview *view);

#ifdef __cplusplus
}
#endif
#endif
