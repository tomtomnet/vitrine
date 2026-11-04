// SPDX-License-Identifier: GPL-2.0-or-later
#include "vmdisplay.h"

#include "displaysurface.h"
#include "displaywidget.h"
#include "framestill.h"
#include "gtkclipboard.h"
#include "vmview/dbusdisplay.h"
#include "vmview/listener.h"
#include "vmview/qmp.h"
#include "vmview/renderer.h"

#include <QImage>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QWindow>

#include <adwaita.h>
#include <gtkmm/application.h>
#include <gtkmm/box.h>
#include <gtkmm/window.h>
#include <glibmm/main.h>

#include <cerrno>
#include <cmath>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>
#include <gio/gio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* QT_LOGGING_RULES=vitrine.view.debug=true: what the guest says of its screen */
Q_LOGGING_CATEGORY(lcView, "vitrine.view", QtWarningMsg)

namespace {

/* The name the fd goes by between getfd and add_client */
const char kFdName[] = "vitrine-display";

int connectUnix(const QString &path, QString *error)
{
    const QByteArray name = path.toLocal8Bit();
    sockaddr_un addr{};

    if (size_t(name.size()) >= sizeof(addr.sun_path)) {
        *error = QStringLiteral("The path of the display socket is too long: %1").arg(path);
        return -1;
    }
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        *error = QStringLiteral("socket: %1").arg(QString::fromLocal8Bit(strerror(errno)));
        return -1;
    }
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, name.constData(), size_t(name.size()));
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        *error = QStringLiteral("Cannot connect to %1: %2")
                     .arg(path, QString::fromLocal8Bit(strerror(errno)));
        close(fd);
        return -1;
    }
    return fd;
}

/* @fn on the GUI thread, from the listener's or the render thread */
void onGuiThread(std::function<void()> fn)
{
    g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer data) -> gboolean {
        (*static_cast<std::function<void()> *>(data))();
        return G_SOURCE_REMOVE;
    }, new std::function<void()>(std::move(fn)), [](gpointer data) {
        delete static_cast<std::function<void()> *>(data);
    });
}

/*
 * The display's connection and statistics, once the view is done with
 * them: GDBus completes DBusDisplay's calls on the GUI thread, from an idle
 * source one iteration after the reply arrives, with pointers to both.  So
 * the connection is closed first, which queues a completion for every call
 * still pending, and both go from an idle of lower priority than those
 * completions (src/vmview/vmview.cpp, retire()).
 */
void retire(DBusDisplay *dbus, std::unique_ptr<Stats> stats)
{
    struct Retired {
        DBusDisplay *dbus;
        std::unique_ptr<Stats> stats;
    };

    if (GDBusConnection *conn = dbus->connection()) {
        g_dbus_connection_close_sync(conn, nullptr, nullptr);
        for (int i = 0; i < 1000 && !g_dbus_connection_is_closed(conn); i++) {
            g_usleep(100);
        }
    }
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, [](gpointer data) -> gboolean {
        auto *retired = static_cast<Retired *>(data);
        delete retired->dbus;
        delete retired;
        return G_SOURCE_REMOVE;
    }, new Retired{dbus, std::move(stats)}, nullptr);
}

} // namespace

VmDisplay::VmDisplay() : m_alive(std::make_shared<bool>(true))
{
    m_host = new Gtk::Box(Gtk::Orientation::VERTICAL);
    m_host->set_size_request(160, 100);
    m_host->set_hexpand(true);
    m_host->set_vexpand(true);
    m_placeholder = adw_status_page_new();
    g_object_ref_sink(m_placeholder);
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(m_placeholder), "view-fullscreen-symbolic");
    adw_status_page_set_title(ADW_STATUS_PAGE(m_placeholder), "Full Screen");
    adw_status_page_set_description(ADW_STATUS_PAGE(m_placeholder),
                                    "Ctrl+Alt+F brings the VM back here.");
    gtk_widget_set_vexpand(m_placeholder, true);
    gtk_widget_set_visible(m_placeholder, false);
    gtk_box_append(GTK_BOX(m_host->gobj()), m_placeholder);
}

VmDisplay::~VmDisplay()
{
    detach();
    m_alive.reset();
    gtk_box_remove(GTK_BOX(m_host->gobj()), m_placeholder);
    g_object_unref(m_placeholder);
    delete m_host;
}

bool VmDisplay::attach(const QString &monitorSocket, QString *error, bool guestWaits, int console)
{
    if (m_dbus) {
        return true;
    }
    const int fd = connectUnix(monitorSocket, error);
    if (fd < 0) {
        return false;
    }
    int sv[2];
    {
        Qmp qmp(fd);    // closes the monitor connection when done
        QJsonObject ret;

        if (!qmp.handshake(error, 5000)) {
            return false;
        }
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
            *error = QStringLiteral("socketpair: %1").arg(QString::fromLocal8Bit(strerror(errno)));
            return false;
        }
        bool ok = qmp.execute(QStringLiteral("getfd"),
                              {{QStringLiteral("fdname"), QLatin1String(kFdName)}}, &ret,
                              error, sv[1]);
        close(sv[1]);
        if (ok) {
            ok = qmp.execute(QStringLiteral("add_client"),
                             {{QStringLiteral("protocol"), QStringLiteral("@dbus-display")},
                              {QStringLiteral("fdname"), QLatin1String(kFdName)}},
                             &ret, error);
        }
        if (!ok) {
            close(sv[0]);
            return false;
        }
    }

    if (!m_stats) {
        m_stats = std::make_unique<Stats>();
    }
    /* no parent: it may outlive the view (retire()) */
    m_dbus = new DBusDisplay(m_stats.get());
    if (!m_dbus->connectPeer(sv[0], error) || !m_dbus->selectConsole(error, console)) {
        detach();
        return false;
    }
    m_console = m_dbus->consolePath().section('_', -1).toInt();

    /* from the listener thread to the widget, on the GUI thread */
    const std::weak_ptr<bool> alive = m_alive;
    Listener::Callbacks cb;
    cb.cursorDefine = [this, alive](QImage image, int hotX, int hotY) {
        onGuiThread([this, alive, image, hotX, hotY]() {
            if (alive.expired()) {
                return;
            }
            qCDebug(lcView) << "guest cursor" << image.width() << "x" << image.height() << "hot spot"
                            << hotX << hotY;
            m_cursor = std::make_unique<QImage>(image);
            m_cursorHotX = hotX;
            m_cursorHotY = hotY;
            if (m_widget) {
                m_widget->setGuestCursor(image, hotX, hotY);
            }
        });
    };
    cb.mouseSet = [this, alive](int, int, bool visible) {
        onGuiThread([this, alive, visible]() {
            if (alive.expired()) {
                return;
            }
            if (visible != m_cursorVisible) {
                qCDebug(lcView) << "guest cursor" << (visible ? "shown" : "hidden");
            }
            m_cursorVisible = visible;
            if (m_widget) {
                m_widget->setGuestCursorVisible(visible);
            }
        });
    };
    cb.scanoutSize = [this, alive](uint32_t w, uint32_t h) {
        onGuiThread([this, alive, w, h]() {
            if (alive.expired()) {
                return;
            }
            if (QSize(int(w), int(h)) != m_guestSize) {
                qCDebug(lcView) << "guest screen" << w << "x" << h;
            }
            m_guestSize = QSize(int(w), int(h));
            if (m_widget) {
                m_widget->setGuestSize(w, h);
            }
            /* a new listener gets the scanout QEMU shows but not the update
               that draws it: the first scanout, and any of a new size, makes
               the view dirty (src/vmview/vmview.cpp) */
            if (m_renderer) {
                m_renderer->requestRedraw();
            }
        });
    };
    m_listener = std::make_unique<Listener>(&m_mailbox, m_stats.get(), m_opts, cb);
    createWindow(false);
    const int listenerFd = m_listener->start(error);
    if (listenerFd < 0 || !m_dbus->registerListener(listenerFd, error)) {
        detach();
        return false;
    }
    QObject::connect(m_dbus, &DBusDisplay::uiInfoApplied, [this, alive](const QString &) {
        if (!alive.expired() && !std::exchange(m_uiInfoApplied, true)) {
            screenInfoApplied.emit();
        }
    });
    sendFirstUiInfo(guestWaits);
    /*
     * Last: QEMU reads the clipboard object's properties as it takes the
     * Register, with its main loop stopped until this thread answers them,
     * so no synchronous call to QEMU may follow on this thread before it
     * gets back to its event loop.  Without it, the screen still works.
     */
    m_clipboard = new GtkVmClipboard(m_dbus->connection(), gdk_display_get_default());
    if (QString message; !m_clipboard->start(&message)) {
        fprintf(stderr, "vitrine: %s\n", qPrintable(message));
        delete std::exchange(m_clipboard, nullptr);
    }
    return true;
}

void VmDisplay::detach()
{
    destroyWindow();
    if (m_listener) {
        m_listener->stop();
        m_listener.reset();
    }
    /* before the connection closes: QEMU's calls to it end here */
    delete std::exchange(m_clipboard, nullptr);
    if (m_dbus) {
        /* the answers of its calls still to come are not the view's */
        QObject::disconnect(m_dbus, nullptr, nullptr, nullptr);
        retire(std::exchange(m_dbus, nullptr), std::move(m_stats));
    }
}

bool VmDisplay::isAttached() const
{
    return m_dbus;
}

int VmDisplay::console() const
{
    return m_dbus ? m_console : -1;
}

Gtk::Widget *VmDisplay::widget() const
{
    return m_host;
}

QString VmDisplay::vmName() const
{
    return m_dbus ? m_dbus->vmName() : QString();
}

void VmDisplay::createWindow(bool fullScreen)
{
    m_opts.toplevel = fullScreen;
    m_opts.fullscreen = fullScreen;
    m_surface = DisplaySurface::create(gdk_display_get_default());
    if (!m_surface) {
        /* not Wayland, or no subsurfaces: nothing to show the frames on;
           the guest's buffers still go back (releaseUndrawn) */
        fprintf(stderr, "vitrine: display: the VM's screen needs a Wayland session\n");
        m_renderFailed = true;
        startUndrawn();
        return;
    }
    m_widget = new DisplayWidget(m_dbus, m_surface.get(), m_opts);
    if (!m_guestSize.isEmpty()) {
        m_widget->setGuestSize(uint32_t(m_guestSize.width()), uint32_t(m_guestSize.height()));
    }
    if (m_cursor) {
        m_widget->setGuestCursor(*m_cursor, m_cursorHotX, m_cursorHotY);
    }
    m_widget->setGuestCursorVisible(m_cursorVisible);
    m_widget->setInputEnabled(m_inputEnabled);
    m_widget->grabChanged.connect([this]() { checkGrab(); });
    m_widget->focusChanged.connect([this]() { updateHostActive(); });
    m_widget->screenChanged.connect([this]() { screenChanged(); });
    /* the guest's text the compositor refused goes again at the next input */
    m_widget->inputSeen.connect([this]() {
        if (m_clipboard && m_clipboard->waitsForInput()) {
            m_clipboard->inputSeen();
        }
    });
    m_widget->exposedChanged.connect([this](bool exposed) {
        /* the render thread takes it from here */
        if (exposed) {
            m_exposedOnce = true;
            if (!m_renderFailed) {
                m_undrawn.disconnect();
            }
        }
    });
    /* from the widget's own key handler, which the switch deletes: later */
    const std::weak_ptr<bool> alive = m_alive;
    m_widget->fullScreenToggled.connect([this, alive]() {
        Glib::signal_idle().connect_once([this, alive]() {
            if (!alive.expired()) {
                setFullScreen(!m_fullScreen);
            }
        });
    });

    if (fullScreen) {
        m_fullScreenWindow = new Gtk::Window;
        m_fullScreenWindow->set_title(m_dbus->vmName().toStdString());
        if (auto *top = dynamic_cast<Gtk::Window *>(m_host->get_root())) {
            if (auto app = top->get_application()) {
                m_fullScreenWindow->set_application(app);
            }
        }
        m_fullScreenWindow->set_child(*m_widget);
        /* closed by the desktop, e.g. from the overview: back to the page */
        m_fullScreenWindow->signal_close_request().connect([this, alive]() {
            Glib::signal_idle().connect_once([this, alive]() {
                if (!alive.expired()) {
                    setFullScreen(false);
                }
            });
            return true;
        }, false);
        /* the renderer tells QEMU when nothing composites the guest's buffers */
        m_fullScreenWindow->property_fullscreened().signal_changed().connect([this]() {
            if (m_widget && m_fullScreenWindow) {
                m_widget->setFullScreenWindow(m_fullScreenWindow->is_fullscreen());
            }
        });
        gtk_widget_set_visible(m_placeholder, true);
    } else {
        gtk_widget_set_visible(m_placeholder, false);
        if (m_covered) {
            m_widget->setCovered(true, takeFrameStill(&m_mailbox, gdk_display_get_default()));
        }
        m_host->append(*m_widget);
    }

    m_renderer = new Renderer(m_widget->rendererWindow(), &m_mailbox, m_stats.get(), m_opts);
    m_renderer->setObjectName(QStringLiteral("render"));   // the thread's name in /proc
    m_renderer->setRenderTarget(m_surface.get());
    m_renderer->setPresentationSink(m_dbus->connection(), m_dbus->consolePath().toUtf8());
    /* on the render thread, as it ends */
    QObject::connect(m_renderer, &Renderer::failed,
                     [this, alive, renderer = m_renderer](const QString &message) {
        fprintf(stderr, "vitrine: display: %s\n", qPrintable(message));
        onGuiThread([this, alive, renderer]() {
            /* nothing else gives the guest's buffers back - if it is still
               this window's (a full-screen switch may have replaced it) */
            if (!alive.expired() && renderer == m_renderer) {
                m_renderFailed = true;
                startUndrawn();
            }
        });
    });
    m_widget->setRenderer(m_renderer);
    {
        /* a renderer stopping leaves its quit flag behind */
        std::lock_guard g(m_mailbox.lock);
        m_mailbox.quit = false;
    }
    m_exposedOnce = false;
    m_renderFailed = false;
    startUndrawn();
    m_renderer->start();
    if (m_widget->isExposed()) {
        m_renderer->setExposed(true);
    }
    m_renderer->requestRedraw();
    if (fullScreen) {
        m_fullScreenWindow->fullscreen();
        m_fullScreenWindow->present();
    }
}

void VmDisplay::destroyWindow()
{
    m_undrawn.disconnect();
    m_uiInfo.disconnect();
    if (m_widget) {
        /* key releases to the guest, and the desktop's shortcuts back */
        m_widget->setHostActive(false);
    }
    if (m_renderer) {
        /* before its surface: it draws into it.  The widget forgets it
           first: taken off the window, it unmaps */
        if (m_widget) {
            m_widget->setRenderer(nullptr);
        }
        m_renderer->stop();
        delete m_renderer;
        m_renderer = nullptr;
    }
    if (DisplayWidget *widget = std::exchange(m_widget, nullptr)) {
        if (m_fullScreenWindow) {
            m_fullScreenWindow->unset_child();
        } else {
            m_host->remove(*widget);
        }
        delete widget;
    }
    delete std::exchange(m_fullScreenWindow, nullptr);
    /* once the render thread is done with it, and off the window */
    m_surface.reset();
    m_hasKeyboard = false;
}

void VmDisplay::setFullScreen(bool on)
{
    if (!m_dbus || on == m_fullScreen) {
        return;
    }
    destroyWindow();
    if (!on && m_dbus->connection()) {
        /* the full-screen renderer may have told QEMU the guest's buffers
           went to the screen as they are, and does not take it back as it
           ends (src/vmview/vmview.cpp) */
        g_dbus_connection_call(m_dbus->connection(), nullptr,
                               m_dbus->consolePath().toUtf8().constData(),
                               "org.qemu.Display1.Presentation", "ZeroCopy",
                               g_variant_new("(b)", FALSE), nullptr,
                               G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, nullptr, nullptr, nullptr);
    }
    m_fullScreen = on;
    qCDebug(lcView) << (on ? "full screen" : "back in the window");
    createWindow(on);
    focus();
    updateHostActive();
    fullScreenChanged.emit(on);
    checkGrab();
}

void VmDisplay::setCovered(bool on)
{
    if (on == m_covered) {
        return;
    }
    m_covered = on;
    if (m_widget && !m_fullScreen) {
        /* the still first: the guest may render into its buffer again once
           the render thread lets it go */
        GdkTexture *still = on ? takeFrameStill(&m_mailbox, gtk_widget_get_display(m_widget->gobj()))
                               : nullptr;
        m_widget->setCovered(on, still);
    }
}

bool VmDisplay::grabbed() const
{
    return m_widget && m_widget->grabbed();
}

QString VmDisplay::grabState() const
{
    return m_widget ? m_widget->grabState() : QString();
}

void VmDisplay::setGrab(bool on)
{
    if (m_widget) {
        m_widget->setGrab(on);
    }
    checkGrab();
}

void VmDisplay::checkGrab()
{
    if (grabState() != m_grabState) {
        m_grabState = grabState();
        grabChanged.emit();
    }
}

void VmDisplay::setInputEnabled(bool on)
{
    if (on != m_inputEnabled) {
        m_inputEnabled = on;
        if (m_widget) {
            m_widget->setInputEnabled(on);
        }
        /* off: the keys held go up and the grab goes, as when the window
           loses the keyboard */
        updateHostActive();
        grabChanged.emit();
    }
}

void VmDisplay::focus()
{
    if (!m_widget) {
        return;
    }
    if (m_fullScreenWindow) {
        m_fullScreenWindow->present();
    }
    m_widget->grab_focus();
}

Stats::Summary VmDisplay::takeStats()
{
    return m_stats ? m_stats->take() : Stats::Summary();
}

/*
 * Until its widget is first shown, the render thread waits before its loop
 * and nothing gives the guest's buffers back: with zero copy, QEMU holds
 * the buffer of each UpdateDMABUF until Presentation.Released, and a guest
 * whose flushes wait for that runs at 1/8 of the refresh rate.  So the view
 * does what the loop does while it does not draw (src/vmview/vmview.cpp).
 */
void VmDisplay::startUndrawn()
{
    m_undrawn.disconnect();
    m_undrawn = Glib::signal_timeout().connect([this]() {
        releaseUndrawn();
        return true;
    }, 50);
}

void VmDisplay::releaseUndrawn()
{
    std::vector<uint64_t> updated;
    std::vector<std::pair<GDBusMethodInvocation *, int64_t>> deferred;

    if (!m_dbus || (m_exposedOnce && !m_renderFailed)) {
        m_undrawn.disconnect();
        return;
    }
    {
        std::lock_guard g(m_mailbox.lock);
        updated.swap(m_mailbox.updated);
        deferred.swap(m_mailbox.deferredReplies);
    }
    const QByteArray path = m_dbus->consolePath().toUtf8();
    for (uint64_t inode : updated) {
        if (inode && m_dbus->connection()) {
            g_dbus_connection_call(m_dbus->connection(), nullptr, path.constData(),
                                   "org.qemu.Display1.Presentation", "Released",
                                   g_variant_new("(t)", guint64(inode)), nullptr,
                                   G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, nullptr, nullptr, nullptr);
        }
    }
    for (const auto &[invocation, received] : deferred) {
        g_dbus_method_invocation_return_value(invocation, nullptr);
    }
}

/*
 * The guest's screen before the widget has a size: a guest started paused
 * for it waits for QEMU's answer.  Until the widget has a size of its own,
 * the window's around it (src/vmview/vmview.cpp, sendFirstUiInfo()).
 */
void VmDisplay::sendFirstUiInfo(bool guestWaits)
{
    if (!m_widget) {
        return;
    }
    if (!guestWaits || !m_widget->physicalSize().isEmpty()) {
        m_widget->sendUiInfo();
        return;
    }
    QSize size(1024, 768);
    double scale = 1;
    if (auto *top = dynamic_cast<Gtk::Window *>(m_host->get_root())) {
        if (top->get_width() > 0 && top->get_height() > 0) {
            size = QSize(top->get_width(), top->get_height());
        }
        if (GdkSurface *surface = top->get_surface() ? top->get_surface()->gobj() : nullptr) {
            scale = gdk_surface_get_scale(surface);
        }
    }
    m_dbus->applyUiInfo(uint32_t(std::lround(size.width() * scale)),
                        uint32_t(std::lround(size.height() * scale)),
                        m_opts.sendRefreshRate ? m_widget->refreshMilliHz() : 0, 0, 0);
}

/* The window's scale, monitor or refresh rate changed: one UI info for
   them once they settled */
void VmDisplay::screenChanged()
{
    m_uiInfo.disconnect();
    m_uiInfo = Glib::signal_timeout().connect([this]() {
        if (m_widget) {
            m_widget->sendUiInfo();
        }
        return false;
    }, 100);
}

/* The screen has the keyboard while the widget has the focus in the
   active window and input is on */
void VmDisplay::updateHostActive()
{
    bool active = false;

    if (m_widget) {
        auto *top = dynamic_cast<Gtk::Window *>(m_widget->get_root());
        active = m_inputEnabled && m_widget->is_focus() && top && top->is_active();
        m_widget->setHostActive(active);
    }
    if (active != m_hasKeyboard) {
        m_hasKeyboard = active;
        m_grabState = grabState();
        grabChanged.emit();
    } else {
        /* losing the keyboard releases the grab */
        checkGrab();
    }
}
