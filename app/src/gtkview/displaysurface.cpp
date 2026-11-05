// SPDX-License-Identifier: GPL-2.0-or-later
#include "displaysurface.h"

#include "viewporter-client-protocol.h"

#include <QOpenGLContext>

#include <gdk/wayland/gdkwayland.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#include <sys/mman.h>
#include <unistd.h>

namespace {

struct Globals {
    wl_subcompositor *subcompositor = nullptr;
    wp_viewporter *viewporter = nullptr;
    wl_shm *shm = nullptr;
};

const wl_registry_listener kRegistry = {
    [](void *data, wl_registry *reg, uint32_t name, const char *iface, uint32_t) {
        auto *g = static_cast<Globals *>(data);
        if (!strcmp(iface, wl_subcompositor_interface.name)) {
            g->subcompositor = static_cast<wl_subcompositor *>(
                wl_registry_bind(reg, name, &wl_subcompositor_interface, 1));
        } else if (!strcmp(iface, wp_viewporter_interface.name)) {
            g->viewporter = static_cast<wp_viewporter *>(
                wl_registry_bind(reg, name, &wp_viewporter_interface, 1));
        } else if (!strcmp(iface, wl_shm_interface.name)) {
            g->shm = static_cast<wl_shm *>(wl_registry_bind(reg, name, &wl_shm_interface, 1));
        }
    },
    [](void *, wl_registry *, uint32_t) {},
};

/* The EGL display of GTK's Wayland connection, for the render threads'
   contexts: one for the process, never terminated (GTK's own GL may use
   the same one) */
bool initEgl(wl_display *display)
{
    static EGLDisplay dpy = [display]() {
        EGLDisplay d = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, display, nullptr);
        if (d != EGL_NO_DISPLAY && !eglInitialize(d, nullptr, nullptr)) {
            fprintf(stderr, "vitrine: display: EGL does not start: 0x%x\n", eglGetError());
            d = EGL_NO_DISPLAY;
        }
        return d;
    }();
    QtGuiCompat::setEglDisplay(dpy);
    return dpy != EGL_NO_DISPLAY;
}

} // namespace

std::unique_ptr<DisplaySurface> DisplaySurface::create(GdkDisplay *display)
{
    std::unique_ptr<DisplaySurface> surface(new DisplaySurface);
    if (!surface->init(display)) {
        return nullptr;
    }
    return surface;
}

bool DisplaySurface::init(GdkDisplay *display)
{
    if (!GDK_IS_WAYLAND_DISPLAY(display)) {
        return false;
    }
    m_display = gdk_wayland_display_get_wl_display(display);
    wl_compositor *compositor = gdk_wayland_display_get_wl_compositor(display);
    if (!m_display || !compositor ||
        wl_proxy_get_version(reinterpret_cast<wl_proxy *>(compositor)) < 4) {
        return false;   // damage_buffer: wl_compositor 4
    }
    if (!initEgl(m_display)) {
        return false;
    }
    /* bound on a queue of their own, then given to the default one, which
       GDK dispatches (what they make has no events vitrine reads) */
    Globals g;
    wl_event_queue *queue = wl_display_create_queue(m_display);
    auto *wrapped = static_cast<wl_display *>(wl_proxy_create_wrapper(m_display));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapped), queue);
    wl_registry *reg = wl_display_get_registry(wrapped);
    wl_registry_add_listener(reg, &kRegistry, &g);
    wl_display_roundtrip_queue(m_display, queue);
    for (auto *p : {reinterpret_cast<wl_proxy *>(g.subcompositor),
                    reinterpret_cast<wl_proxy *>(g.viewporter),
                    reinterpret_cast<wl_proxy *>(g.shm)}) {
        if (p) {
            wl_proxy_set_queue(p, nullptr);
        }
    }
    wl_registry_destroy(reg);
    wl_proxy_wrapper_destroy(wrapped);
    wl_event_queue_destroy(queue);
    m_subcompositor = g.subcompositor;
    m_viewporter = g.viewporter;
    m_shm = g.shm;
    if (!m_subcompositor) {
        return false;
    }

    m_surface = wl_compositor_create_surface(compositor);
    /* the pointer and the keyboard stay the window's: GTK's, under it */
    wl_region *none = wl_compositor_create_region(compositor);
    wl_surface_set_input_region(m_surface, none);
    wl_region_destroy(none);
    /* whatever the buffers' format: nothing under it shows through */
    wl_region *all = wl_compositor_create_region(compositor);
    wl_region_add(all, 0, 0, 1 << 15, 1 << 15);
    wl_surface_set_opaque_region(m_surface, all);
    wl_region_destroy(all);
    if (m_viewporter) {
        m_viewport = wp_viewporter_get_viewport(m_viewporter, m_surface);
    }
    if (g_strcmp0(g_getenv("VITRINE_VIEW_COMPOSITE"), "1") == 0) {
        initOverlay(compositor);
    }
    wl_display_flush(m_display);
    return true;
}

/*
 * VITRINE_VIEW_COMPOSITE=1, for A/B runs: one pixel of black at 1/255
 * opacity, without input, to go above the frames once attached.  A
 * compositor scans out only a topmost surface that covers the whole
 * screen: over the full-screen view, this one makes it composite.
 */
void DisplaySurface::initOverlay(wl_compositor *compositor)
{
    const uint32_t pixel = 0x01000000;   // ARGB8888, premultiplied
    const int fd = m_shm ? memfd_create("vitrine-view-overlay", MFD_CLOEXEC) : -1;
    if (fd < 0 || write(fd, &pixel, sizeof pixel) != ssize_t(sizeof pixel)) {
        fprintf(stderr, "vitrine: display: VITRINE_VIEW_COMPOSITE: no pixel over the view\n");
        if (fd >= 0) {
            close(fd);
        }
        return;
    }
    wl_shm_pool *pool = wl_shm_create_pool(m_shm, fd, sizeof pixel);
    m_overlayPixel = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    m_overlay = wl_compositor_create_surface(compositor);
    wl_region *none = wl_compositor_create_region(compositor);
    wl_surface_set_input_region(m_overlay, none);
    wl_region_destroy(none);
    wl_surface_attach(m_overlay, m_overlayPixel, 0, 0);
    wl_surface_damage_buffer(m_overlay, 0, 0, 1, 1);
    wl_surface_commit(m_overlay);
}

DisplaySurface::~DisplaySurface()
{
    /* a render thread that failed before its end did not let it go (done()) */
    if (m_eglSurface) {
        eglDestroySurface(m_eglDisplay, m_eglSurface);
    }
    if (m_eglWindow) {
        wl_egl_window_destroy(m_eglWindow);
    }
    detach();
    if (m_overlay) {
        wl_surface_destroy(m_overlay);
    }
    if (m_overlayPixel) {
        wl_buffer_destroy(m_overlayPixel);
    }
    if (m_shm) {
        wl_shm_destroy(m_shm);
    }
    if (m_viewport) {
        wp_viewport_destroy(m_viewport);
    }
    if (m_surface) {
        wl_surface_destroy(m_surface);
    }
    if (m_viewporter) {
        wp_viewporter_destroy(m_viewporter);
    }
    if (m_subcompositor) {
        wl_subcompositor_destroy(m_subcompositor);
    }
    if (m_display) {
        wl_display_flush(m_display);
    }
}

void DisplaySurface::attachTo(GdkSurface *parent)
{
    if (parent == m_parent && m_subsurface) {
        return;
    }
    detach();
    wl_surface *surface = parent ? gdk_wayland_surface_get_wl_surface(parent) : nullptr;
    if (!surface) {
        return;
    }
    m_parent = parent;
    m_placed = false;
    m_subsurface = wl_subcompositor_get_subsurface(m_subcompositor, m_surface, surface);
    /* its frames show as they come, not with the window's next commit */
    wl_subsurface_set_desync(m_subsurface);
    wl_subsurface_set_position(m_subsurface, m_x, m_y);
    if (m_overlay) {
        m_overlaySub = wl_subcompositor_get_subsurface(m_subcompositor, m_overlay, surface);
        wl_subsurface_place_above(m_overlaySub, m_surface);
        wl_subsurface_set_position(m_overlaySub, m_x, m_y);
    }
    wl_display_flush(m_display);
    /* a new subsurface, like its position, counts from the parent's next
       commit: GTK's, which it makes for its next frame even if it draws
       nothing new */
    gdk_wayland_surface_force_next_commit(parent);
}

void DisplaySurface::detach()
{
    if (m_overlaySub) {
        wl_subsurface_destroy(m_overlaySub);
        m_overlaySub = nullptr;
    }
    if (m_subsurface) {
        /* unmapped at once */
        wl_subsurface_destroy(m_subsurface);
        m_subsurface = nullptr;
        wl_display_flush(m_display);
    }
    m_parent = nullptr;
}

bool DisplaySurface::setPosition(int x, int y)
{
    if (x == m_x && y == m_y && m_placed) {
        return false;
    }
    m_placed = true;
    m_x = x;
    m_y = y;
    if (m_subsurface) {
        wl_subsurface_set_position(m_subsurface, x, y);
        if (m_overlaySub) {
            wl_subsurface_set_position(m_overlaySub, x, y);
        }
        wl_display_flush(m_display);
        gdk_wayland_surface_force_next_commit(m_parent);
    }
    return true;
}

void DisplaySurface::setSize(int width, int height, double scale)
{
    std::lock_guard g(m_sizeLock);
    m_logical = QSize(width, height);
    m_scale = scale > 0 ? scale : 1;
    m_size = QSize(int(std::lround(width * m_scale)), int(std::lround(height * m_scale)));
}

QSize DisplaySurface::physicalSize() const
{
    std::lock_guard g(m_sizeLock);
    return m_size;
}

wl_display *DisplaySurface::display() const
{
    return m_display;
}

wl_surface *DisplaySurface::surface() const
{
    return m_surface;
}

/*
 * On the render thread: the context current on an EGL window surface of
 * the wl_surface, which the copies fall back to when the window's own
 * buffers (Swapchain) are not there yet or fail.  The size and the viewport
 * (the buffer in physical pixels on the widget's logical size) go with the
 * frame drawn now, committed by this thread.
 */
bool DisplaySurface::makeCurrent(QOpenGLContext *ctx, QSize *size)
{
    auto *egl = ctx->nativeInterface<QNativeInterface::QEGLContext>();
    if (!egl) {
        return false;
    }
    QSize logical;
    double scale;
    {
        std::lock_guard g(m_sizeLock);
        *size = m_size;
        logical = m_logical;
        scale = m_scale;
    }
    /* never 0: the buffers made for a size of 0 fail for good (Swapchain) */
    *size = size->expandedTo(QSize(1, 1));
    const EGLDisplay dpy = egl->display();
    m_eglDisplay = dpy;
    if (!m_eglSurface) {
        m_eglWindow = wl_egl_window_create(m_surface, size->width(), size->height());
        m_eglSurface = eglCreateWindowSurface(
            dpy, egl->config(), reinterpret_cast<EGLNativeWindowType>(m_eglWindow), nullptr);
        if (m_eglSurface == EGL_NO_SURFACE) {
            fprintf(stderr, "vitrine: display: no EGL surface for the view: 0x%x\n", eglGetError());
            m_eglSurface = nullptr;
            wl_egl_window_destroy(std::exchange(m_eglWindow, nullptr));
            return false;
        }
        m_eglSize = *size;
    } else if (*size != m_eglSize) {
        wl_egl_window_resize(m_eglWindow, size->width(), size->height(), 0, 0);
        m_eglSize = *size;
    }
    if (m_viewport) {
        if (logical != m_viewportSize) {
            if (logical.isEmpty()) {
                wp_viewport_set_destination(m_viewport, -1, -1);
            } else {
                wp_viewport_set_destination(m_viewport, logical.width(), logical.height());
            }
            m_viewportSize = logical;
        }
    } else {
        /* integer ratios only, then */
        const int bufferScale = std::max(1, int(std::lround(scale)));
        if (bufferScale != m_bufferScale) {
            wl_surface_set_buffer_scale(m_surface, bufferScale);
            m_bufferScale = bufferScale;
        }
    }
    if (!eglMakeCurrent(dpy, m_eglSurface, m_eglSurface, egl->nativeContext())) {
        return false;
    }
    if (!m_intervalSet) {
        /* as Qt would for the window: 0, each frame at once */
        eglSwapInterval(dpy, ctx->format().swapInterval() < 0 ? 0 : ctx->format().swapInterval());
        m_intervalSet = true;
    }
    return true;
}

void DisplaySurface::swapBuffers(QOpenGLContext *ctx)
{
    auto *egl = ctx->nativeInterface<QNativeInterface::QEGLContext>();
    if (egl && m_eglSurface) {
        eglSwapBuffers(egl->display(), m_eglSurface);
    }
}

void DisplaySurface::done(QOpenGLContext *ctx)
{
    auto *egl = ctx->nativeInterface<QNativeInterface::QEGLContext>();
    if (egl && m_eglSurface) {
        eglDestroySurface(egl->display(), m_eglSurface);
    }
    m_eglSurface = nullptr;
    if (m_eglWindow) {
        wl_egl_window_destroy(std::exchange(m_eglWindow, nullptr));
    }
    m_intervalSet = false;
}
