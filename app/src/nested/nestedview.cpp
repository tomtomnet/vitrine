// SPDX-License-Identifier: GPL-2.0-or-later
#include "nestedview.h"

#include "vmview.h"

#include <QFile>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPainter>
#include <QPlatformSurfaceEvent>
#include <QRasterWindow>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <qpa/qplatformwindow_p.h>

#include <wayland-client.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include <sys/mman.h>
#include <unistd.h>

namespace {

/* The embedded window, under the guest's: what shows where it does not */
class DesktopWindow : public QRasterWindow
{
public:
    QString message;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const QRect all(QPoint(), size());
        painter.fillRect(all, Qt::black);
        if (!message.isEmpty()) {
            painter.setPen(QColor(0xc8, 0xcd, 0xd2));
            painter.drawText(all.adjusted(24, 24, -24, -24),
                             Qt::AlignCenter | Qt::TextWordWrap, message);
        }
    }
};

/* The guest's desktop alone on a screen: black where the guest's window is not */
class FullScreenWindow : public QRasterWindow
{
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(QRect(QPoint(), size()), Qt::black);
    }
};

struct Globals {
    wl_subcompositor *subcompositor = nullptr;
    wl_shm *shm = nullptr;
};

const wl_registry_listener kRegistry = {
    [](void *data, wl_registry *reg, uint32_t name, const char *iface, uint32_t) {
        auto *g = static_cast<Globals *>(data);
        if (!strcmp(iface, wl_subcompositor_interface.name)) {
            g->subcompositor = static_cast<wl_subcompositor *>(
                wl_registry_bind(reg, name, &wl_subcompositor_interface, 1));
        } else if (!strcmp(iface, wl_shm_interface.name)) {
            g->shm = static_cast<wl_shm *>(wl_registry_bind(reg, name, &wl_shm_interface, 1));
        }
    },
    [](void *, wl_registry *, uint32_t) {},
};

/* The window's surface, while its platform window has one */
wl_surface *qtSurface(QWindow *window)
{
    auto *w = window ? window->nativeInterface<QNativeInterface::Private::QWaylandWindow>()
                     : nullptr;
    return w ? w->surface() : nullptr;
}

/* Ctrl+Alt+F, as on the VM's screen */
bool isFullScreenKey(const QKeyEvent *key)
{
    const Qt::KeyboardModifiers mods = key->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier |
                                                           Qt::AltModifier | Qt::MetaModifier);
    return mods == (Qt::ControlModifier | Qt::AltModifier) && key->key() == Qt::Key_F &&
           !key->isAutoRepeat();
}

} // namespace

NestedView *NestedView::create(const QString &socket, QString *error, QObject *parent)
{
    auto *view = new NestedView(parent);
    if (!view->init(socket, error)) {
        delete view;
        return nullptr;
    }
    return view;
}

NestedView::NestedView(QObject *parent) : DesktopView(parent) {}

bool NestedView::init(const QString &socket, QString *error)
{
    auto *app = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    wl_compositor *compositor = app ? app->compositor() : nullptr;

    m_socket = socket;
    m_display = app ? app->display() : nullptr;
    if (!compositor || !m_display) {
        *error = tr("The nested desktop shows in Vitrine's window on a Wayland desktop only");
        return false;
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
                    reinterpret_cast<wl_proxy *>(g.shm)}) {
        if (p) {
            wl_proxy_set_queue(p, nullptr);
        }
    }
    wl_registry_destroy(reg);
    wl_proxy_wrapper_destroy(wrapped);
    wl_event_queue_destroy(queue);
    m_subcompositor = g.subcompositor;
    m_shm = g.shm;
    if (!m_subcompositor || !m_shm) {
        *error = tr("The compositor lacks what the nested desktop needs (wl_subcompositor, wl_shm)");
        return false;
    }

    /* the guest's window's parent: one transparent pixel, no input */
    m_surface = wl_compositor_create_surface(compositor);
    wl_region *none = wl_compositor_create_region(compositor);
    wl_surface_set_input_region(m_surface, none);
    wl_region_destroy(none);
    const int fd = memfd_create("vitrine-nested-desktop", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, 4) < 0) {
        *error = tr("memfd: %1").arg(QString::fromLocal8Bit(strerror(errno)));
        if (fd >= 0) {
            close(fd);
        }
        return false;
    }
    wl_shm_pool *pool = wl_shm_create_pool(m_shm, fd, 4);
    m_pixel = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_surface_attach(m_surface, m_pixel, 0, 0);
    wl_surface_damage(m_surface, 0, 0, 1, 1);
    wl_surface_commit(m_surface);
    wl_display_flush(m_display);

    /* the library's thread: its callbacks to this one */
    vmview_callbacks cb{};
    cb.data = this;
    cb.title = [](void *data, const char *title) {
        auto *self = static_cast<NestedView *>(data);
        QMetaObject::invokeMethod(self, [self, text = QString::fromUtf8(title)]() {
            self->m_title = text;
            Q_EMIT self->titleChanged(text);
        }, Qt::QueuedConnection);
    };
    cb.fullscreen = [](void *data, bool on) {
        auto *self = static_cast<NestedView *>(data);
        QMetaObject::invokeMethod(self, [self, on]() { Q_EMIT self->fullScreenRequested(on); },
                                  Qt::QueuedConnection);
    };
    cb.connection = [](void *data, int count) {
        auto *self = static_cast<NestedView *>(data);
        QMetaObject::invokeMethod(self, [self, count]() {
            if (count != self->m_connections) {
                self->m_connections = count;
                Q_EMIT self->connectionsChanged(count);
            }
        }, Qt::QueuedConnection);
    };
    cb.mapped = [](void *data, bool mapped) {
        auto *self = static_cast<NestedView *>(data);
        QMetaObject::invokeMethod(self, [self, mapped]() {
            if (mapped != self->m_mapped) {
                self->m_mapped = mapped;
                self->updateGeometry();
                Q_EMIT self->mappedChanged(mapped);
            }
        }, Qt::QueuedConnection);
    };
    /* a click on the guest's window, which Qt never sees: the keys to it */
    cb.pressed = [](void *data) {
        auto *self = static_cast<NestedView *>(data);
        QMetaObject::invokeMethod(self, [self]() { self->focus(); }, Qt::QueuedConnection);
    };
    m_view = vmview_new(m_display, m_surface, QFile::encodeName(socket).constData(), &cb);
    if (!m_view) {
        *error = tr("Cannot serve the nested desktop's socket %1: %2")
                     .arg(socket, QString::fromLocal8Bit(strerror(errno)));
        return false;
    }

    /* the embedded view: its window takes no input, which comes to the container */
    m_host = new QWidget;
    m_host->setMinimumSize(160, 100);
    auto *layout = new QVBoxLayout(m_host);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *window = new DesktopWindow;
    m_window = window;
    m_window->setFlag(Qt::WindowTransparentForInput);
    m_window->installEventFilter(this);
    m_container = QWidget::createWindowContainer(m_window, m_host);
    m_container->setFocusPolicy(Qt::StrongFocus);
    m_container->installEventFilter(this);
    layout->addWidget(m_container);
    m_commitAgain = new QTimer(this);
    m_commitAgain->setSingleShot(true);
    m_commitAgain->setInterval(50);
    connect(m_commitAgain, &QTimer::timeout, this, &NestedView::commitSurface);
    connect(qGuiApp, &QGuiApplication::focusWindowChanged, this, &NestedView::updateKeyboard);
    if (m_window->handle()) {
        watchPlatformWindow(m_window);
    }
    updateGeometry();
    return true;
}

NestedView::~NestedView()
{
    /* first: the guest's objects on the surface go, and its callbacks stop */
    vmview_destroy(std::exchange(m_view, nullptr));
    detach();
    delete m_platformWatch;
    if (m_fullWindow) {
        m_fullWindow->removeEventFilter(this);
        delete m_fullWindow;
    }
    if (m_container) {
        m_container->removeEventFilter(this);
    }
    if (m_window) {
        m_window->removeEventFilter(this);
    }
    delete m_host;      // the container and its window
    if (m_surface) {
        wl_surface_destroy(m_surface);
    }
    if (m_pixel) {
        wl_buffer_destroy(m_pixel);
    }
    if (m_shm) {
        wl_shm_destroy(m_shm);
    }
    if (m_subcompositor) {
        wl_subcompositor_destroy(m_subcompositor);
    }
    if (m_display) {
        wl_display_flush(m_display);
    }
}

QString NestedView::socket() const
{
    return m_socket;
}

QWidget *NestedView::widget() const
{
    return m_host;
}

bool NestedView::isFullScreen() const
{
    return m_fullWindow;
}

QWindow *NestedView::hostWindow() const
{
    return m_fullWindow ? m_fullWindow : m_window;
}

void NestedView::setFullScreen(bool on)
{
    if (on == isFullScreen() || !m_window) {
        return;
    }
    detach();
    delete m_platformWatch;
    m_platformWatch = nullptr;
    if (on) {
        m_fullWindow = new FullScreenWindow;
        m_fullWindow->setTitle(m_title);
        if (m_host && m_host->screen()) {
            m_fullWindow->setScreen(m_host->screen());
        }
        m_fullWindow->installEventFilter(this);
        m_fullWindow->showFullScreen();
        m_fullWindow->requestActivate();
        if (m_fullWindow->handle()) {
            watchPlatformWindow(m_fullWindow);
        }
    } else {
        QWindow *full = std::exchange(m_fullWindow, nullptr);
        full->removeEventFilter(this);
        delete full;
        if (m_window->handle()) {
            watchPlatformWindow(m_window);
        }
        focus();
    }
    updateGeometry();
    updateKeyboard();
    Q_EMIT fullScreenChanged(on);
}

void NestedView::setInputEnabled(bool on)
{
    if (on != m_inputEnabled) {
        m_inputEnabled = on;
        updateKeyboard();
    }
}

bool NestedView::hasKeyboard() const
{
    return m_keyboard;
}

void NestedView::focus()
{
    if (m_fullWindow) {
        m_fullWindow->requestActivate();
    } else if (m_container) {
        m_container->setFocus(Qt::OtherFocusReason);
    }
}

int NestedView::connections() const
{
    return m_connections;
}

bool NestedView::isMapped() const
{
    return m_mapped;
}

QString NestedView::title() const
{
    return m_title;
}

/*
 * Qt makes a window's surface again (re-parenting, a hide and show)
 * without a new platform window: the subsurface follows, as ViewSurface's.
 * Qt says so after the old surface went: no request may reach the
 * subsurface until then but its destruction.
 */
void NestedView::watchPlatformWindow(QWindow *window)
{
    auto *w = window->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
    delete std::exchange(m_platformWatch, nullptr);
    if (!w) {
        return;
    }
    m_platformWatch = new QObject(this);
    connect(w, &QNativeInterface::Private::QWaylandWindow::surfaceDestroyed, m_platformWatch,
            [this]() { detach(); });
    connect(w, &QNativeInterface::Private::QWaylandWindow::surfaceCreated, m_platformWatch,
            [this]() { attachToHost(); });
    attachToHost();
}

void NestedView::attachToHost()
{
    QWindow *window = hostWindow();
    wl_surface *parent = qtSurface(window);

    if (parent == m_parent || !m_surface) {
        return;
    }
    detach();
    if (!parent) {
        return;
    }
    m_parent = parent;
    m_subsurface = wl_subcompositor_get_subsurface(m_subcompositor, m_surface, parent);
    wl_subsurface_set_position(m_subsurface, 0, 0);
    /* the guest's frames show as they come, not with the window's commits */
    wl_subsurface_set_desync(m_subsurface);
    commitSurface();
    /* a new subsurface counts from its parent's next commit */
    if (auto *raster = qobject_cast<QRasterWindow *>(window)) {
        raster->update();
    }
}

void NestedView::detach()
{
    if (m_subsurface) {
        wl_subsurface_destroy(m_subsurface);
        m_subsurface = nullptr;
        wl_display_flush(m_display);
    }
    m_parent = nullptr;
}

void NestedView::updateGeometry()
{
    QWindow *window = hostWindow();
    auto *embedded = static_cast<DesktopWindow *>(m_window);

    if (embedded) {
        embedded->message =
            m_fullWindow ? tr("The guest's desktop is in full screen. Ctrl+Alt+F brings it "
                              "back here.")
            : m_mapped   ? QString()
                         : tr("The guest's desktop is not running.\nLog in to the guest's "
                              "Plasma (crosvm window) session from the Screen view "
                              "(Ctrl+Alt+2).");
        embedded->update();
    }
    if (!m_view || !window) {
        return;
    }
    vmview_set_fullscreen(m_view, m_fullWindow != nullptr);
    vmview_set_geometry(m_view, 0, 0, qMax(1, window->width()), qMax(1, window->height()));
    commitSurface();
    /* the library's thread places the guest's window after this: once more then */
    m_commitAgain->start();
}

void NestedView::commitSurface()
{
    if (m_surface) {
        wl_surface_commit(m_surface);
        wl_display_flush(m_display);
    }
}

/* The keys to the guest while its view has Qt's focus in the active window, or is full screen and active */
void NestedView::updateKeyboard()
{
    bool want = false;

    if (m_inputEnabled && m_fullWindow) {
        want = m_fullWindow->isActive();
    } else if (m_inputEnabled && m_container) {
        want = m_container->hasFocus() && m_container->window()->isActiveWindow();
    }
    if (m_view) {
        vmview_set_keyboard_focus(m_view, want);
    }
    if (want != m_keyboard) {
        m_keyboard = want;
        Q_EMIT keyboardChanged();
    }
}

bool NestedView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == hostWindow()) {
        switch (event->type()) {
        case QEvent::PlatformSurface:
            if (static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType() ==
                QPlatformSurfaceEvent::SurfaceCreated) {
                watchPlatformWindow(hostWindow());
            } else {
                /* before Qt's surface goes: the subsurface first */
                delete std::exchange(m_platformWatch, nullptr);
                detach();
            }
            break;
        case QEvent::Resize:
            updateGeometry();
            break;
        default:
            break;
        }
    }
    if ((watched == m_container || watched == m_fullWindow) && m_inputEnabled) {
        switch (event->type()) {
        case QEvent::ShortcutOverride:
            /* the guest's keys before the menus' shortcuts */
            event->accept();
            return true;
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
            /* the guest has them, through vmview; Ctrl+Alt+F its own once it runs */
            if (event->type() == QEvent::KeyPress && !m_mapped &&
                isFullScreenKey(static_cast<QKeyEvent *>(event))) {
                Q_EMIT fullScreenToggled();
            }
            return true;
        default:
            break;
        }
    }
    if (watched == m_container) {
        switch (event->type()) {
        case QEvent::FocusIn:
        case QEvent::FocusOut:
            updateKeyboard();
            break;
        case QEvent::MouseButtonPress:
            /* beside the guest's window: the keys to it */
            m_container->setFocus(Qt::MouseFocusReason);
            break;
        default:
            break;
        }
    }
    if (watched == m_fullWindow) {
        switch (event->type()) {
        case QEvent::FocusIn:
        case QEvent::FocusOut:
            updateKeyboard();
            break;
        case QEvent::Close:
            /* closed by the desktop, from its task bar: back in the console */
            event->ignore();
            QMetaObject::invokeMethod(this, [this]() { setFullScreen(false); },
                                      Qt::QueuedConnection);
            return true;
        default:
            break;
        }
    }
    return QObject::eventFilter(watched, event);
}
