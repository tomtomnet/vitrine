// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QObject>
#include <QSize>

#include <mutex>

#include "renderer.h"

class QBackingStore;
class QOffscreenSurface;
class QWindow;
struct wl_display;
struct wl_egl_window;
struct wl_subcompositor;
struct wl_subsurface;
struct wl_surface;
struct wp_viewport;
struct wp_viewporter;

/*
 * The VM view's own Wayland surface: a wl_surface of vitrine's, a desync
 * subsurface of the view's window, which the render thread alone sizes,
 * draws into and commits (Renderer::RenderTarget), and which Qt never
 * touches.  The render thread used to commit on the window's own surface,
 * with nothing in step with Qt: Qt's hide (attach NULL, commit), its
 * re-creation of the surface (re-parenting, destroy) and its configures
 * raced the frames - a protocol error on the next show of a top-level
 * (KWin's unconfigured_buffer), a crash on a destroyed surface (the
 * research's static analysis, qt-client.md F3).
 *
 * The window, Qt's, keeps the input (the subsurface takes none: its input
 * region is empty) and gets a black buffer of its own size, as a parent
 * surface without one is not shown, nor its subsurfaces.  The subsurface
 * goes with Qt's surface and is made again on the new one; the render
 * thread does not notice: its surface stays.  Full screen is the window
 * as a full-screen top-level, which the subsurface covers.
 *
 * Wayland only: create() gives none elsewhere, and the window keeps Qt's
 * surface (the research client's way).
 */
class ViewSurface : public QObject, public RenderTarget
{
    Q_OBJECT

public:
    /* For @window, before it is shown: its surface type becomes raster
       (black, under the view); null without Wayland or what it needs */
    static ViewSurface *create(QWindow *window);
    /* The render thread is done with it */
    ~ViewSurface() override;

    /* RenderTarget, on the render thread */
    wl_display *display() const override;
    wl_surface *surface() const override;
    bool makeCurrent(QOpenGLContext *ctx, QSize *size) override;
    void swapBuffers(QOpenGLContext *ctx) override;
    void done(QOpenGLContext *ctx) override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    explicit ViewSurface(QWindow *window);
    bool init();
    /* The subsurface, on the window's surface of now, if it has one */
    void attachToParent();
    void detachFromParent();
    void watchPlatformWindow();
    /* The window's own buffer: black, of its size */
    void paintParent();
    void updateSize();

    QWindow *m_window;
    wl_display *m_display = nullptr;
    wl_subcompositor *m_subcompositor = nullptr;
    wp_viewporter *m_viewporter = nullptr;
    wl_surface *m_surface = nullptr;
    wp_viewport *m_viewport = nullptr;
    wl_subsurface *m_subsurface = nullptr;
    wl_surface *m_parent = nullptr;     // the window's, under m_subsurface
    QObject *m_platformWatch = nullptr; // the window's platform window watched
    QBackingStore *m_backingStore = nullptr;
    /* the context's surface for Qt (functions, doneCurrent), made on the GUI thread */
    QOffscreenSurface *m_offscreen = nullptr;

    /* the window's size, for the render thread: in buffer pixels, and logical */
    std::mutex m_sizeLock;
    QSize m_size, m_logical;
    qreal m_dpr = 1;

    /* render thread */
    bool m_qtCurrent = false;
    wl_egl_window *m_eglWindow = nullptr;
    void *m_eglDisplay = nullptr;      // EGLDisplay
    void *m_eglSurface = nullptr;      // EGLSurface
    QSize m_eglSize, m_viewportSize;
    int m_scale = 0;                    // without a viewporter
    bool m_intervalSet = false;
};
