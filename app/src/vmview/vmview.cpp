// SPDX-License-Identifier: GPL-2.0-or-later
#include "vmview.h"

#include "dbusdisplay.h"
#include "displaywindow.h"
#include "listener.h"
#include "qmp.h"
#include "renderer.h"
#include "vmclipboard.h"
#include "waylandextras.h"

#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"

#include <QGuiApplication>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMetaObject>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cerrno>
#include <cstring>
#include <utility>
#include <vector>
#include <gio/gio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

/* QEMU's key numbers: left Ctrl, left Alt, Delete */
constexpr uint32_t kCtrl = 0x1d, kAlt = 0x38, kDelete = 0xd3;
/* The name the fd goes by between getfd and add_client */
const char kFdName[] = "vitrine-display";

int connectUnix(const QString &path, QString *error)
{
    const QByteArray name = path.toLocal8Bit();
    sockaddr_un addr{};

    if (size_t(name.size()) >= sizeof(addr.sun_path)) {
        *error = VmView::tr("The path of the display socket is too long: %1").arg(path);
        return -1;
    }
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        *error = VmView::tr("socket: %1").arg(QString::fromLocal8Bit(strerror(errno)));
        return -1;
    }
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, name.constData(), size_t(name.size()));
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        *error = VmView::tr("Cannot connect to %1: %2")
                     .arg(path, QString::fromLocal8Bit(strerror(errno)));
        close(fd);
        return -1;
    }
    return fd;
}

/*
 * The display's connection and statistics, once the view is done with them.
 * GDBus completes DBusDisplay's calls on the GUI thread, from an idle source
 * one iteration after the reply arrives, with pointers to both (the input
 * calls' statistics, UIInfo.Apply's display): deleted at once, they would
 * leave the replies already in, and those the close fails, writing into
 * freed memory.  So the connection is closed first, which queues a
 * completion for every call still pending, and both go from an idle of
 * lower priority than those completions, which run before it.
 */
void retire(DBusDisplay *dbus, std::unique_ptr<Stats> stats)
{
    struct Retired {
        DBusDisplay *dbus;
        std::unique_ptr<Stats> stats;
    };

    if (GDBusConnection *conn = dbus->connection()) {
        g_dbus_connection_close_sync(conn, nullptr, nullptr);
        /* it may return before GDBus marks the connection closed, which is
           when the pending calls fail: at once, in practice */
        for (int i = 0; i < 1000 && !g_dbus_connection_is_closed(conn); i++) {
            g_usleep(100);
        }
    }
    /* not deleteLater(): Qt's posted events have the completions' priority */
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, [](gpointer data) -> gboolean {
        auto *retired = static_cast<Retired *>(data);
        delete retired->dbus;
        delete retired;
        return G_SOURCE_REMOVE;
    }, new Retired{dbus, std::move(stats)}, nullptr);
}

} // namespace

VmView::VmView(QObject *parent) : QObject(parent)
{
    m_host = new QWidget;
    m_host->setMinimumSize(160, 100);
    auto *layout = new QVBoxLayout(m_host);
    layout->setContentsMargins(0, 0, 0, 0);
    m_placeholder = new QLabel(tr("The VM is in full screen. Ctrl+Alt+F brings it back here."),
                               m_host);
    m_placeholder->setAlignment(Qt::AlignCenter);
    m_placeholder->setWordWrap(true);
    m_placeholder->hide();
    layout->addWidget(m_placeholder);
    /* the window around the screen gaining or losing the keyboard */
    connect(qGuiApp, &QGuiApplication::focusWindowChanged, this, &VmView::updateHostActive);
    /* as often as the render thread looks while it does not draw */
    m_undrawn = new QTimer(this);
    m_undrawn->setInterval(50);
    connect(m_undrawn, &QTimer::timeout, this, &VmView::releaseUndrawn);
}

VmView::~VmView()
{
    detach();
    delete m_host;
}

bool VmView::attach(const QString &monitorSocket, QString *error)
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
            *error = tr("socketpair: %1").arg(QString::fromLocal8Bit(strerror(errno)));
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

    m_wayland = new WaylandExtras(this);
    if (!m_wayland->init()) {
        delete m_wayland;
        m_wayland = nullptr;
    }
    if (!m_stats) {
        m_stats = std::make_unique<Stats>();
    }
    /* no parent: it may outlive the view (retire()) */
    m_dbus = new DBusDisplay(m_stats.get());
    if (!m_dbus->connectPeer(sv[0], error) || !m_dbus->selectConsole(error)) {
        detach();
        return false;
    }

    /* from the listener thread to the window, on the GUI thread */
    Listener::Callbacks cb;
    cb.cursorDefine = [this](QImage image, int hotX, int hotY) {
        QMetaObject::invokeMethod(this, [this, image, hotX, hotY]() {
            m_cursor = image;
            m_cursorHotX = hotX;
            m_cursorHotY = hotY;
            if (m_window) {
                m_window->setGuestCursor(image, hotX, hotY);
            }
        }, Qt::QueuedConnection);
    };
    cb.mouseSet = [this](int, int, bool visible) {
        QMetaObject::invokeMethod(this, [this, visible]() {
            m_cursorVisible = visible;
            if (m_window) {
                m_window->setGuestCursorVisible(visible);
            }
        }, Qt::QueuedConnection);
    };
    cb.scanoutSize = [this](uint32_t w, uint32_t h) {
        QMetaObject::invokeMethod(this, [this, w, h]() {
            m_guestSize = QSize(int(w), int(h));
            if (m_window) {
                m_window->setGuestSize(w, h);
            }
            /* A new listener gets the scanout QEMU shows, but not the update
               that draws it (console.c, displaychangelistener_display_console),
               and the renderer draws only on an update or a dirty view.  The
               window's first expose makes the view dirty: if the scanout came
               first, as it does on KWin (2026-10-03, re-attached to an idle
               guest, QEMU's trace: scanout, then the frame presented, the
               guest's next update 7 s later), that draws it; if it comes
               after, an idle guest would stay black until it repaints.  So
               the first scanout, and any of a new size, makes it dirty too. */
            if (m_renderer) {
                m_renderer->requestRedraw();
            }
        }, Qt::QueuedConnection);
    };
    m_listener = std::make_unique<Listener>(&m_mailbox, m_stats.get(), m_opts, cb);
    createWindow(false);
    const int listenerFd = m_listener->start(error);
    if (listenerFd < 0 || !m_dbus->registerListener(listenerFd, error)) {
        detach();
        return false;
    }
    m_window->sendUiInfo();
    /*
     * Last: QEMU reads the clipboard object's properties as it takes the
     * Register, with its main loop stopped until this thread answers them,
     * so no synchronous call to QEMU may follow on this thread before it
     * gets back to its event loop.  Without it, the screen still works.
     */
    m_clipboard = new VmClipboard(m_dbus->connection(), this);
    if (QString message; !m_clipboard->start(&message)) {
        qWarning("vitrine: %s", qPrintable(message));
        delete std::exchange(m_clipboard, nullptr);
    }
    return true;
}

void VmView::detach()
{
    destroyWindow();
    if (m_listener) {
        m_listener->stop();
        m_listener.reset();
    }
    /* before the connection closes: QEMU's calls to it end here */
    delete std::exchange(m_clipboard, nullptr);
    if (m_dbus) {
        retire(std::exchange(m_dbus, nullptr), std::move(m_stats));
    }
    if (m_wayland) {
        /*
         * ~WaylandExtras destroys the inhibitor and pointer lock it made,
         * not the three globals init() bound: each attach left them bound,
         * in the compositor too, for as long as vitrine ran.  Until the
         * research side's destructor does it, they go here, taken out
         * first (so that one doing it then finds none), after the objects
         * made from them.
         */
        auto *inhibit = std::exchange(m_wayland->m_inhibitManager, nullptr);
        auto *constraints = std::exchange(m_wayland->m_constraints, nullptr);
        auto *relative = std::exchange(m_wayland->m_relativeManager, nullptr);
        delete std::exchange(m_wayland, nullptr);
        if (relative) {
            zwp_relative_pointer_manager_v1_destroy(relative);
        }
        if (constraints) {
            zwp_pointer_constraints_v1_destroy(constraints);
        }
        if (inhibit) {
            zwp_keyboard_shortcuts_inhibit_manager_v1_destroy(inhibit);
        }
    }
}

bool VmView::isAttached() const
{
    return m_dbus;
}

QWidget *VmView::widget() const
{
    return m_host;
}

bool VmView::isFullScreen() const
{
    return m_fullScreen;
}

void VmView::createWindow(bool fullScreen)
{
    m_opts.toplevel = fullScreen;
    m_opts.fullscreen = fullScreen;
    m_window = new DisplayWindow(m_dbus, m_wayland, m_opts);
    if (!m_guestSize.isEmpty()) {
        m_window->setGuestSize(uint32_t(m_guestSize.width()), uint32_t(m_guestSize.height()));
    }
    if (!m_cursor.isNull()) {
        m_window->setGuestCursor(m_cursor, m_cursorHotX, m_cursorHotY);
    }
    m_window->setGuestCursorVisible(m_cursorVisible);
    connect(m_window, &DisplayWindow::grabChanged, this, &VmView::checkGrab);
    /* the grab its keys and clicks take, and in full screen its closing */
    m_window->installEventFilter(this);
    /* queued: Ctrl+Alt+F comes from the window's own key handler, and the
       switch deletes the window */
    connect(m_window, &DisplayWindow::fullScreenToggled, this,
            [this]() { setFullScreen(!m_fullScreen); }, Qt::QueuedConnection);

    if (fullScreen) {
        m_window->setTitle(m_dbus->vmName());
        if (m_host && m_host->screen()) {
            m_window->setScreen(m_host->screen());
        }
        m_placeholder->show();
    } else {
        m_container = QWidget::createWindowContainer(m_window, m_host);
        m_container->setFocusPolicy(Qt::StrongFocus);
        /* Wayland gives the keyboard to the top-level surface, never to the
           screen's subsurface: its keys come to the container */
        m_container->installEventFilter(this);
        connect(m_window, &DisplayWindow::pressed, m_container,
                [this]() { m_container->setFocus(Qt::MouseFocusReason); });
        m_placeholder->hide();
        m_host->layout()->addWidget(m_container);
        /* now, not when the layout gets to it: focus() needs it shown, as
           the window gets the focus back when it is active again */
        m_container->show();
    }

    m_renderer = new Renderer(m_window, &m_mailbox, m_stats.get(), m_opts);
    m_renderer->setObjectName(QStringLiteral("render"));   // the thread's name in /proc
    m_renderer->setPresentationSink(m_dbus->connection(), m_dbus->consolePath().toUtf8());
    connect(m_renderer, &Renderer::failed, this,
            [this, renderer = m_renderer](const QString &message) {
        qWarning("vitrine: display: %s", qPrintable(message));
        /* it has ended: nothing else gives the guest's buffers back - if it
           is still this window's (queued: a full-screen switch may have
           replaced it, and the new one draws) */
        if (renderer == m_renderer) {
            m_renderFailed = true;
            m_undrawn->start();
        }
    });
    m_window->setRenderer(m_renderer);
    {
        /* a renderer stopping leaves its quit flag behind */
        std::lock_guard g(m_mailbox.lock);
        m_mailbox.quit = false;
    }
    m_exposedOnce = false;
    m_renderFailed = false;
    m_undrawn->start();
    m_renderer->start();
    if (m_window->isExposed()) {
        m_renderer->setExposed(true);
    }
    m_renderer->requestRedraw();
    if (fullScreen) {
        m_window->showFullScreen();
        m_window->requestActivate();
    }
}

void VmView::destroyWindow()
{
    m_undrawn->stop();
    if (m_window) {
        /* key releases to the guest, and the desktop's shortcuts back */
        m_window->setHostActive(false);
    }
    if (m_renderer) {
        /* before its window: it draws into it */
        m_renderer->stop();
        delete m_renderer;
        m_renderer = nullptr;
    }
    /*
     * Out of reach before they go: ~QWindowContainer deletes the window
     * first, then ~QWidget clears the focus, which sends the container
     * FocusOut through eventFilter() - with the members still set, that
     * would reach the deleted window (updateHostActive) and signal
     * grabChanged() for a view half torn down.  The keyboard went with
     * setHostActive(false) above; setFullScreen() tells the new state.
     */
    QWidget *container = std::exchange(m_container, nullptr);
    DisplayWindow *window = std::exchange(m_window, nullptr);
    m_hasKeyboard = false;
    if (container) {
        container->removeEventFilter(this);
        delete container;       // and the window it holds
    } else if (window) {
        window->removeEventFilter(this);
        delete window;
    }
}

void VmView::setFullScreen(bool on)
{
    if (!m_dbus || on == m_fullScreen) {
        return;
    }
    destroyWindow();
    if (!on && m_dbus->connection()) {
        /*
         * The full-screen renderer may have told QEMU the guest's buffers
         * went to the screen as they are (Presentation.ZeroCopy), and it
         * does not take it back as it ends; the embedded one does at its
         * first frame, which a console not shown never draws.
         */
        g_dbus_connection_call(m_dbus->connection(), nullptr,
                               m_dbus->consolePath().toUtf8().constData(),
                               "org.qemu.Display1.Presentation", "ZeroCopy",
                               g_variant_new("(b)", FALSE), nullptr,
                               G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, nullptr, nullptr, nullptr);
    }
    m_fullScreen = on;
    createWindow(on);
    if (!on) {
        focus();
    }
    updateHostActive();
    Q_EMIT fullScreenChanged(on);
    checkGrab();
}

bool VmView::grabbed() const
{
    return m_window && m_window->grabbed();
}

QString VmView::grabState() const
{
    return m_window ? m_window->grabState() : QString();
}

void VmView::setGrab(bool on)
{
    if (m_window) {
        m_window->setGrab(on);
    }
    checkGrab();
}

void VmView::checkGrab()
{
    if (grabState() != m_grabState) {
        m_grabState = grabState();
        Q_EMIT grabChanged();
    }
}

bool VmView::hasKeyboard() const
{
    return m_hasKeyboard;
}

void VmView::setInputEnabled(bool on)
{
    if (on != m_inputEnabled) {
        m_inputEnabled = on;
        /* off: the keys held go up and the grab goes, as when the window
           loses the keyboard - Ctrl+Alt+G could not release it now */
        updateHostActive();
    }
}

bool VmView::inputEnabled() const
{
    return m_inputEnabled;
}

void VmView::focus()
{
    if (m_container) {
        m_container->setFocus(Qt::OtherFocusReason);
    } else if (m_window) {
        m_window->requestActivate();
    }
}

void VmView::sendCtrlAltDel()
{
    if (!m_dbus) {
        return;
    }
    for (uint32_t key : {kCtrl, kAlt, kDelete}) {
        m_dbus->keyPress(key);
    }
    for (uint32_t key : {kDelete, kAlt, kCtrl}) {
        m_dbus->keyRelease(key);
    }
}

Stats::Summary VmView::takeStats()
{
    return m_stats ? m_stats->take() : Stats::Summary();
}

QSize VmView::guestSize() const
{
    return m_guestSize;
}

QString VmView::vmName() const
{
    return m_dbus ? m_dbus->vmName() : QString();
}

bool VmView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_window) {
        switch (event->type()) {
        case QEvent::Expose:
            /* the render thread takes it from here (it has the same event) */
            if (m_window->isExposed()) {
                m_exposedOnce = true;
                if (!m_renderFailed) {
                    m_undrawn->stop();
                }
            }
            break;
        case QEvent::Close:
            if (!m_fullScreen) {
                break;
            }
            /* closed by the desktop, e.g. from the task bar: back to the widget,
               not from the window's own event handler, which the switch deletes */
            event->ignore();
            QMetaObject::invokeMethod(this, [this]() { setFullScreen(false); },
                                      Qt::QueuedConnection);
            return true;
        case QEvent::KeyPress:
        case QEvent::MouseButtonPress:
            /* once the window has handled it */
            QMetaObject::invokeMethod(this, &VmView::checkGrab, Qt::QueuedConnection);
            break;
        default:
            break;
        }
    }
    if (watched == m_container && m_window) {
        switch (event->type()) {
        case QEvent::ShortcutOverride:
            if (!m_inputEnabled) {
                break;          // the window's shortcuts
            }
            event->accept();    // the guest's keys before the menus' shortcuts
            return true;
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
            if (!m_inputEnabled) {
                break;          // QEMU would drop them
            }
            m_window->handleKey(static_cast<QKeyEvent *>(event),
                                event->type() == QEvent::KeyPress);
            checkGrab();
            return true;
        case QEvent::FocusIn:
        case QEvent::FocusOut:
            updateHostActive();
            break;
        default:
            break;
        }
    }
    return QObject::eventFilter(watched, event);
}

/*
 * Until its window is first exposed, the render thread waits before its
 * loop (renderer.cpp), and nothing gives the guest's buffers back: with zero
 * copy, QEMU holds the buffer of each UpdateDMABUF until Presentation.
 * Released, and a guest whose flushes wait for that ran at 1/8 of the
 * refresh rate (QEMU's timeout) for as long as its console was not shown -
 * every running VM's but the selected one's, after vitrine starts again or
 * when full screen ends on another VM.  So the view does what the loop does
 * while it does not draw: each buffer released, each deferred reply sent.
 * Not the buffer the first frame will show, which QEMU then no longer
 * holds: the loop's not-exposed path has the same gap, until the guest's
 * next frame.
 */
void VmView::releaseUndrawn()
{
    std::vector<uint64_t> updated;
    std::vector<std::pair<GDBusMethodInvocation *, int64_t>> deferred;

    if (!m_dbus || (m_exposedOnce && !m_renderFailed)) {
        m_undrawn->stop();
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
            /* one-way, as the renderer sends it */
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

/* Embedded, the screen has the keyboard while its container has the focus
   in the active window and input is on; in full screen the window tells
   itself */
void VmView::updateHostActive()
{
    bool active = false;

    if (m_window && m_container) {
        active = m_inputEnabled && m_container->hasFocus() &&
                 m_container->window()->isActiveWindow();
        m_window->setHostActive(active);
    } else if (m_window) {
        active = m_window->isActive();
    }
    if (active != m_hasKeyboard) {
        m_hasKeyboard = active;
        m_grabState = grabState();
        Q_EMIT grabChanged();
    } else {
        /* losing the keyboard releases the grab */
        checkGrab();
    }
}
