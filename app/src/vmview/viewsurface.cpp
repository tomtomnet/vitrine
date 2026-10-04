// SPDX-License-Identifier: GPL-2.0-or-later
#include "viewsurface.h"

#include "viewporter-client-protocol.h"

#include <QBackingStore>
#include <QEvent>
#include <QLoggingCategory>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QPainter>
#include <QPlatformSurfaceEvent>
#include <QWindow>
#include <qpa/qplatformwindow_p.h>

#include <EGL/egl.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include <cstring>

Q_LOGGING_CATEGORY(lcViewSurface, "vitrine.viewsurface", QtWarningMsg)

namespace {

struct Globals {
    wl_subcompositor *subcompositor = nullptr;
    wp_viewporter *viewporter = nullptr;
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
        }
    },
    [](void *, wl_registry *, uint32_t) {},
};

/* The window's surface, while its platform window has one */
wl_surface *qtSurface(QWindow *window)
{
    auto *w = window->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
    return w ? w->surface() : nullptr;
}

} // namespace

ViewSurface *ViewSurface::create(QWindow *window)
{
    auto *view = new ViewSurface(window);
    if (!view->init()) {
        delete view;
        return nullptr;
    }
    return view;
}

ViewSurface::ViewSurface(QWindow *window) : m_window(window) {}

bool ViewSurface::init()
{
    auto *app = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    wl_compositor *compositor = app ? app->compositor() : nullptr;
    m_display = app ? app->display() : nullptr;
    if (!compositor || !m_display || wl_proxy_get_version(
                                         reinterpret_cast<wl_proxy *>(compositor)) < 4) {
        return false;   // damage_buffer: wl_compositor 4
    }
    /* bound on a queue of their own, then given to the default one, which
       Qt dispatches (what they make has no events vitrine reads) */
    Globals g;
    wl_event_queue *queue = wl_display_create_queue(m_display);
    auto *wrapped = static_cast<wl_display *>(wl_proxy_create_wrapper(m_display));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapped), queue);
    wl_registry *reg = wl_display_get_registry(wrapped);
    wl_registry_add_listener(reg, &kRegistry, &g);
    wl_display_roundtrip_queue(m_display, queue);
    for (auto *p : {reinterpret_cast<wl_proxy *>(g.subcompositor),
                    reinterpret_cast<wl_proxy *>(g.viewporter)}) {
        if (p) {
            wl_proxy_set_queue(p, nullptr);
        }
    }
    wl_registry_destroy(reg);
    wl_proxy_wrapper_destroy(wrapped);
    wl_event_queue_destroy(queue);
    m_subcompositor = g.subcompositor;
    m_viewporter = g.viewporter;
    if (!m_subcompositor) {
        return false;
    }

    m_surface = wl_compositor_create_surface(compositor);
    /* the pointer and the keyboard stay the window's, under it */
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
    wl_display_flush(m_display);

    /* black under the view: Qt's raster window, in its own buffer */
    m_window->setSurfaceType(QSurface::RasterSurface);
    m_backingStore = new QBackingStore(m_window);
    m_offscreen = new QOffscreenSurface(nullptr, this);
    m_offscreen->setFormat(m_window->requestedFormat());
    m_offscreen->create();
    m_window->installEventFilter(this);
    if (m_window->handle()) {
        watchPlatformWindow();
    }
    updateSize();
    return true;
}

ViewSurface::~ViewSurface()
{
    /* a render thread that failed before its end did not let it go (done()) */
    if (m_eglSurface) {
        eglDestroySurface(m_eglDisplay, m_eglSurface);
    }
    if (m_eglWindow) {
        wl_egl_window_destroy(m_eglWindow);
    }
    m_window->removeEventFilter(this);
    detachFromParent();
    delete m_platformWatch;
    delete m_backingStore;
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

/*
 * Qt makes the window's surface again (re-parenting, a role change) without
 * a new platform window: the subsurface follows.  Qt says so after the old
 * surface went, which leaves the subsurface without a parent until then:
 * no request may reach it but its destruction.
 */
void ViewSurface::watchPlatformWindow()
{
    auto *w = m_window->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
    delete std::exchange(m_platformWatch, nullptr);
    if (!w) {
        return;
    }
    m_platformWatch = new QObject(this);
    connect(w, &QNativeInterface::Private::QWaylandWindow::surfaceDestroyed, m_platformWatch,
            [this]() { detachFromParent(); });
    connect(w, &QNativeInterface::Private::QWaylandWindow::surfaceCreated, m_platformWatch,
            [this]() { attachToParent(); });
    attachToParent();
}

void ViewSurface::attachToParent()
{
    wl_surface *parent = qtSurface(m_window);
    if (parent == m_parent || !m_surface) {
        return;
    }
    detachFromParent();
    if (!parent) {
        return;
    }
    m_parent = parent;
    qCDebug(lcViewSurface) << "subsurface on the surface of" << m_window;
    m_subsurface = wl_subcompositor_get_subsurface(m_subcompositor, m_surface, parent);
    /* its frames show as they come, not with the window's next commit */
    wl_subsurface_set_desync(m_subsurface);
    wl_display_flush(m_display);
    /* a new subsurface counts from the parent's next commit */
    paintParent();
}

void ViewSurface::detachFromParent()
{
    if (m_subsurface) {
        qCDebug(lcViewSurface) << "subsurface off the surface of" << m_window;
        wl_subsurface_destroy(m_subsurface);
        m_subsurface = nullptr;
        wl_display_flush(m_display);
    }
    m_parent = nullptr;
}

bool ViewSurface::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_window) {
        switch (event->type()) {
        case QEvent::PlatformSurface:
            if (static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType() ==
                QPlatformSurfaceEvent::SurfaceCreated) {
                watchPlatformWindow();
            } else {
                /* before Qt's surface goes: the subsurface first */
                delete std::exchange(m_platformWatch, nullptr);
                detachFromParent();
            }
            break;
        case QEvent::Expose:
            paintParent();
            break;
        case QEvent::Resize:
        case QEvent::DevicePixelRatioChange:
            updateSize();
            paintParent();
            break;
        default:
            break;
        }
    }
    return QObject::eventFilter(watched, event);
}

/* Qt's commit of the window, with a buffer of its size: it shows, and the
   subsurface over it with it */
void ViewSurface::paintParent()
{
    if (!m_window->isExposed()) {
        return;
    }
    const QRect rect(QPoint(), m_window->size());
    if (rect.isEmpty()) {
        return;
    }
    m_backingStore->resize(rect.size());
    m_backingStore->beginPaint(rect);
    {
        QPainter painter(m_backingStore->paintDevice());
        painter.fillRect(rect, Qt::black);
    }
    m_backingStore->endPaint();
    m_backingStore->flush(rect);
}

/* As DisplayWindow::physicalSize(): what the guest is told to be */
void ViewSurface::updateSize()
{
    const qreal dpr = m_window->devicePixelRatio();
    const QSize logical = m_window->size();
    std::lock_guard g(m_sizeLock);
    m_logical = logical;
    m_dpr = dpr;
    m_size = QSize(qRound(logical.width() * dpr), qRound(logical.height() * dpr));
}

wl_display *ViewSurface::display() const
{
    return m_display;
}

wl_surface *ViewSurface::surface() const
{
    return m_surface;
}

/*
 * On the render thread: Qt's context current for Qt (its functions resolve
 * through it, doneCurrent ends it) on the offscreen surface, then for EGL on
 * an EGL window surface of the wl_surface, which the copies fall back to
 * when the window's own buffers (Swapchain) are not there yet or fail.  The
 * size and the viewport (the buffer in physical pixels on the window's
 * logical size) go with the frame drawn now, committed by this thread.
 */
bool ViewSurface::makeCurrent(QOpenGLContext *ctx, QSize *size)
{
    auto *egl = ctx->nativeInterface<QNativeInterface::QEGLContext>();
    if (!egl) {
        return false;
    }
    if (!m_qtCurrent) {
        if (!ctx->makeCurrent(m_offscreen)) {
            return false;
        }
        m_qtCurrent = true;
    }
    QSize logical;
    qreal dpr;
    {
        std::lock_guard g(m_sizeLock);
        *size = m_size;
        logical = m_logical;
        dpr = m_dpr;
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
            qWarning("vitrine: display: no EGL surface for the view: 0x%x", eglGetError());
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
        const int scale = qMax(1, qRound(dpr));
        if (scale != m_scale) {
            wl_surface_set_buffer_scale(m_surface, scale);
            m_scale = scale;
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

void ViewSurface::swapBuffers(QOpenGLContext *ctx)
{
    auto *egl = ctx->nativeInterface<QNativeInterface::QEGLContext>();
    if (egl && m_eglSurface) {
        eglSwapBuffers(egl->display(), m_eglSurface);
    }
}

void ViewSurface::done(QOpenGLContext *ctx)
{
    auto *egl = ctx->nativeInterface<QNativeInterface::QEGLContext>();
    if (egl && m_eglSurface) {
        eglDestroySurface(egl->display(), m_eglSurface);
    }
    m_eglSurface = nullptr;
    if (m_eglWindow) {
        wl_egl_window_destroy(std::exchange(m_eglWindow, nullptr));
    }
    m_qtCurrent = false;
    m_intervalSet = false;
}
