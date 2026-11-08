// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QSize>

#include <memory>
#include <mutex>

#include "vmview/renderer.h"

struct wl_compositor;
struct wl_display;
struct wl_buffer;
struct wl_egl_window;
struct wl_shm;
struct wl_subcompositor;
struct wl_subsurface;
struct wl_surface;
struct wp_viewport;
struct wp_viewporter;
typedef struct _GdkDisplay GdkDisplay;
typedef struct _GdkSurface GdkSurface;

/*
 * The VM display's own Wayland surface in GTK: a wl_surface of vitrine's,
 * a desync subsurface of the window's (GTK's toplevel surface), which the
 * render thread alone sizes, draws into and commits (RenderTarget), and
 * which GTK never touches - the GTK counterpart of src/vmview's
 * ViewSurface.
 *
 * GTK knows nothing of it: the widget under it (DisplayWidget) keeps the
 * input (its input region is empty) and draws black, and tells it where to
 * go as GTK lays the window out.  Its position is the parent's state, which
 * takes effect with GTK's next commit of the window (made to happen with
 * gdk_wayland_surface_force_next_commit()); its frames show as they come.
 * Over the window's own drawing: whatever GTK draws there - an in-window
 * dialog, a toast - is under it.  Hidden, it has no subsurface: a surface
 * without a role is not shown, and the render thread may go on committing
 * to it.
 *
 * setOverlay(): a nearly transparent pixel of its own, without input, above
 * the frames' top left corner, so that the compositor composites a
 * full-screen view rather than scanning its frames out directly (GNOME's
 * Mutter scans out only a topmost surface that covers the screen):
 * VmDisplay's full screen for a guest whose flips wait for the release of
 * the buffer shown (VmDisplay::setFlipsWaitForRelease).
 */
class DisplaySurface : public RenderTarget
{
public:
    /* On @display, if it is a Wayland one with what the view needs: else null */
    static std::unique_ptr<DisplaySurface> create(GdkDisplay *display);
    /* Once the render thread is done with it */
    ~DisplaySurface() override;

    /* GUI thread */
    /* On @parent's surface, shown; again on another parent moves it there */
    void attachTo(GdkSurface *parent);
    /* Hidden: no longer on the window */
    void detach();
    bool isAttached() const { return m_subsurface; }
    /* Where it goes on the window's surface, in its logical pixels: with
       GTK's next commit of the window; false if it was there already */
    bool setPosition(int x, int y);
    /* Its size in logical pixels and the window's scale (fractional): the
       render thread's buffers are of their product, rounded */
    void setSize(int width, int height, double scale);
    /* That product: what the guest is told to be */
    QSize physicalSize() const;
    /* The pixel above the frames, or none: on the window at once when
       attached, else once attached */
    void setOverlay(bool on);

    /* RenderTarget, on the render thread */
    wl_display *display() const override;
    wl_surface *surface() const override;
    bool makeCurrent(QOpenGLContext *ctx, QSize *size) override;
    void swapBuffers(QOpenGLContext *ctx) override;
    void done(QOpenGLContext *ctx) override;

private:
    DisplaySurface() = default;
    bool init(GdkDisplay *display);
    bool initOverlay();
    void attachOverlay(wl_surface *parent);

    wl_display *m_display = nullptr;
    wl_compositor *m_compositor = nullptr;   // GDK's
    wl_subcompositor *m_subcompositor = nullptr;
    wp_viewporter *m_viewporter = nullptr;
    wl_surface *m_surface = nullptr;
    wp_viewport *m_viewport = nullptr;
    /* setOverlay(): the pixel above the frames */
    wl_shm *m_shm = nullptr;
    wl_surface *m_overlay = nullptr;
    wl_buffer *m_overlayPixel = nullptr;
    bool m_overlayOn = false;
    /* GUI thread */
    wl_subsurface *m_subsurface = nullptr;
    wl_subsurface *m_overlaySub = nullptr;
    GdkSurface *m_parent = nullptr;
    int m_x = 0, m_y = 0;
    bool m_placed = false;

    /* the size for the render thread: logical, in buffer pixels, the scale */
    mutable std::mutex m_sizeLock;
    QSize m_logical, m_size;
    double m_scale = 1;

    /* render thread */
    wl_egl_window *m_eglWindow = nullptr;
    void *m_eglDisplay = nullptr;   // EGLDisplay
    void *m_eglSurface = nullptr;   // EGLSurface
    QSize m_eglSize, m_viewportSize;
    int m_bufferScale = 0;          // without a viewporter
    bool m_intervalSet = false;
};
