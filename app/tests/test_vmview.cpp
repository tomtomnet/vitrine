// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * VmView against a stand-in for QEMU's display: the QMP monitor that hands
 * it a socket (getfd + add_client) and org.qemu.Display1 on the peer-to-peer
 * D-Bus connection, served from a thread of its own as QEMU serves it from
 * its own process.  The platform is offscreen: no window on anyone's screen,
 * no Wayland, no OpenGL (the render thread gives up at once, as it may on a
 * host without EGL), which leaves VmView's own part to test - attaching,
 * input, focus, full screen and taking it all down.
 */
#include <QApplication>
#include <QDir>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QSplitter>
#include <QTemporaryDir>
#include <QTest>
#include <QVBoxLayout>
#include <QWidget>
#include <QPointer>
#include <QRegularExpression>
#include <QWindow>
#include <qpa/qwindowsysteminterface.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "vmview/dbusdisplay.h"
#include "vmview/qmp.h"
#include "vmview/vmview.h"

namespace {

const char kVmXml[] = R"XML(
<node>
  <interface name="org.qemu.Display1.VM">
    <property name="Name" type="s" access="read"/>
    <property name="ConsoleIDs" type="au" access="read"/>
  </interface>
</node>)XML";

/* The part of ui/dbus-display1.xml the view uses */
const char kConsoleXml[] = R"XML(
<node>
  <interface name="org.qemu.Display1.Console">
    <method name="RegisterListener"><arg type="h" name="listener" direction="in"/></method>
    <property name="Type" type="s" access="read"/>
  </interface>
  <interface name="org.qemu.Display1.Keyboard">
    <method name="Press"><arg type="u" name="keycode" direction="in"/></method>
    <method name="Release"><arg type="u" name="keycode" direction="in"/></method>
  </interface>
  <interface name="org.qemu.Display1.Mouse">
    <method name="SetAbsPosition">
      <arg type="u" name="x" direction="in"/><arg type="u" name="y" direction="in"/>
    </method>
    <method name="RelMotion">
      <arg type="i" name="dx" direction="in"/><arg type="i" name="dy" direction="in"/>
    </method>
    <method name="Press"><arg type="u" name="button" direction="in"/></method>
    <method name="Release"><arg type="u" name="button" direction="in"/></method>
    <property name="IsAbsolute" type="b" access="read"/>
  </interface>
  <interface name="org.qemu.Display1.UIInfo">
    <method name="Apply"><arg type="a{sv}" name="info" direction="in"/></method>
  </interface>
  <interface name="org.qemu.Display1.Presentation">
    <method name="Released"><arg type="t" name="token" direction="in"/></method>
    <method name="ZeroCopy"><arg type="b" name="on" direction="in"/></method>
  </interface>
</node>)XML";

/* The client's side registers with it: Grab, Release and Request come from the client
   only once the guest's clipboard is involved */
const char kClipboardXml[] = R"XML(
<node>
  <interface name="org.qemu.Display1.Clipboard">
    <method name="Register"/>
    <method name="Unregister"/>
  </interface>
</node>)XML";

const char kRoot[] = "/org/qemu/Display1";

} // namespace

/*
 * QEMU's side: a QMP socket at @path; every D-Bus call the view makes is
 * recorded as "Interface.Method (args)", the interface without its
 * org.qemu.Display1. prefix
 */
class FakeDisplay
{
public:
    explicit FakeDisplay(const QString &path)
    {
        sockaddr_un addr{};
        const QByteArray name = path.toLocal8Bit();

        addr.sun_family = AF_UNIX;
        memcpy(addr.sun_path, name.constData(), size_t(name.size()));
        m_listenFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (bind(m_listenFd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 ||
            listen(m_listenFd, 4) < 0) {
            qFatal("FakeDisplay: cannot listen on %s", name.constData());
        }
        m_ctx = g_main_context_new();
        m_loop = g_main_loop_new(m_ctx, FALSE);
        m_bus = std::thread([this]() {
            g_main_context_push_thread_default(m_ctx);
            g_main_loop_run(m_loop);
            /* down with the loop's thread: what it made, it ends */
            for (GDBusMethodInvocation *inv : m_held) {
                g_dbus_method_invocation_return_dbus_error(inv, "org.qemu.Error", "gone");
            }
            m_held.clear();
            for (GDBusConnection *conn : std::exchange(m_open, {})) {
                g_signal_handlers_disconnect_by_data(conn, this);
                g_dbus_connection_close_sync(conn, nullptr, nullptr);
                g_object_unref(conn);
            }
            while (g_main_context_iteration(m_ctx, FALSE)) {
            }
            g_main_context_pop_thread_default(m_ctx);
        });
        m_qmp = std::thread([this]() { serveQmp(); });
    }

    ~FakeDisplay()
    {
        m_stop = true;
        m_qmp.join();
        close(m_listenFd);
        g_main_loop_quit(m_loop);
        m_bus.join();
        g_main_loop_unref(m_loop);
        g_main_context_unref(m_ctx);
    }

    QStringList calls() const
    {
        std::lock_guard g(m_lock);
        return m_calls;
    }
    QStringList callsTo(const QString &start) const
    {
        return calls().filter(QRegularExpression("^" + QRegularExpression::escape(start)));
    }
    void clearCalls()
    {
        std::lock_guard g(m_lock);
        m_calls.clear();
    }
    /* Keyboard, UIInfo and Clipboard calls wait for an answer until let go */
    void hold(bool on) { m_hold = on; }
    void letGo()
    {
        g_main_context_invoke(m_ctx, [](gpointer self) {
            auto *d = static_cast<FakeDisplay *>(self);
            for (GDBusMethodInvocation *inv : d->m_held) {
                g_dbus_method_invocation_return_value(inv, nullptr);
            }
            d->m_held.clear();
            return G_SOURCE_REMOVE;
        }, this);
    }
    /* Calls answered so far */
    int answered() const { return m_answered; }
    /* The view's listener is registered and connected */
    bool listening() const { return m_listener.load() != nullptr; }
    /* How many listeners connected so far */
    int listeners() const { return m_listeners; }
    /* A guest buffer (any file will do: its inode is the token) as the scanout */
    void scanout(int fd, uint32_t width, uint32_t height)
    {
        GUnixFDList *fds = g_unix_fd_list_new();
        g_unix_fd_list_append(fds, fd, nullptr);
        g_dbus_connection_call_with_unix_fd_list(
            m_listener, nullptr, "/org/qemu/Display1/Listener", "org.qemu.Display1.Listener",
            "ScanoutDMABUF",
            g_variant_new("(huuuutb)", 0, width, height, width * 4, 0x34325258u /* XR24 */,
                          guint64(0), FALSE),
            nullptr, G_DBUS_CALL_FLAGS_NONE, -1, fds, nullptr, nullptr, nullptr);
        g_object_unref(fds);
    }
    void update(uint32_t width, uint32_t height)
    {
        g_dbus_connection_call(m_listener, nullptr, "/org/qemu/Display1/Listener",
                               "org.qemu.Display1.Listener", "UpdateDMABUF",
                               g_variant_new("(iiii)", 0, 0, int(width), int(height)), nullptr,
                               G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
    }

private:
    void serveQmp()
    {
        while (!m_stop) {
            pollfd p{m_listenFd, POLLIN, 0};
            if (poll(&p, 1, 50) <= 0) {
                continue;
            }
            const int c = accept4(m_listenFd, nullptr, nullptr, SOCK_CLOEXEC);
            if (c < 0) {
                continue;
            }
            serveClient(c);
            close(c);
        }
    }

    static void send(int fd, const char *text)
    {
        if (write(fd, text, strlen(text)) < 0) {
            qWarning("FakeDisplay: write: %s", strerror(errno));
        }
    }

    void serveClient(int c)
    {
        QByteArray buffer;
        int passed = -1;

        send(c, "{\"QMP\": {\"version\": {}, \"capabilities\": []}}\n");
        while (!m_stop) {
            pollfd p{c, POLLIN, 0};
            if (poll(&p, 1, 50) <= 0) {
                continue;
            }
            char data[4096];
            alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))];
            iovec iov{data, sizeof(data)};
            msghdr msg{};
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            msg.msg_control = control;
            msg.msg_controllen = sizeof(control);
            const ssize_t n = recvmsg(c, &msg, MSG_CMSG_CLOEXEC);
            if (n <= 0) {
                break;
            }
            for (cmsghdr *h = CMSG_FIRSTHDR(&msg); h; h = CMSG_NXTHDR(&msg, h)) {
                if (h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_RIGHTS) {
                    memcpy(&passed, CMSG_DATA(h), sizeof(int));
                }
            }
            buffer.append(data, n);
            qsizetype nl;
            while ((nl = buffer.indexOf('\n')) >= 0) {
                const QByteArray line = buffer.left(nl);
                buffer.remove(0, nl + 1);
                if (line.contains("\"add_client\"") && passed >= 0) {
                    send(c, "{\"return\": {}}\n");
                    startPeer(std::exchange(passed, -1));
                } else {
                    /* qmp_capabilities, getfd */
                    send(c, "{\"return\": {}}\n");
                }
            }
        }
    }

    struct Peer {
        FakeDisplay *self;
        int fd;
    };

    /* A D-Bus connection on @fd, to @ready (g_dbus_connection_new_finish) */
    static void connectionOn(int fd, GAsyncReadyCallback ready, FakeDisplay *self, bool delay)
    {
        GSocket *socket = g_socket_new_from_fd(fd, nullptr);
        GSocketConnection *stream = g_socket_connection_factory_create_connection(socket);
        gchar *guid = g_dbus_generate_guid();
        /* QEMU is the authentication server of both connections */
        g_dbus_connection_new(G_IO_STREAM(stream), guid,
                              GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_SERVER |
                                                   (delay ? G_DBUS_CONNECTION_FLAGS_DELAY_MESSAGE_PROCESSING
                                                          : 0)),
                              nullptr, nullptr, ready, self);
        g_free(guid);
        g_object_unref(stream);
        g_object_unref(socket);
    }

    /* On the bus thread: the display's connection, served once its objects are there */
    void startPeer(int fd)
    {
        g_main_context_invoke(m_ctx, [](gpointer data) {
            auto *peer = static_cast<Peer *>(data);
            connectionOn(peer->fd, [](GObject *, GAsyncResult *res, gpointer self) {
                auto *d = static_cast<FakeDisplay *>(self);
                d->m_conn = g_dbus_connection_new_finish(res, nullptr);
                if (d->m_conn) {
                    d->keep(d->m_conn);
                    d->registerObjects();
                    g_dbus_connection_start_message_processing(d->m_conn);
                }
            }, peer->self, true);
            delete peer;
            return G_SOURCE_REMOVE;
        }, new Peer{this, fd});
    }

    /* Until the view closes it, as QEMU keeps a client's connections */
    void keep(GDBusConnection *conn)
    {
        m_open.push_back(conn);
        g_signal_connect(conn, "closed",
                         G_CALLBACK(+[](GDBusConnection *c, gboolean, GError *, gpointer self) {
            auto *d = static_cast<FakeDisplay *>(self);
            std::erase(d->m_open, c);
            GDBusConnection *current = c;
            d->m_listener.compare_exchange_strong(current, nullptr);
            if (d->m_conn == c) {
                d->m_conn = nullptr;
            }
            g_signal_handlers_disconnect_by_data(c, self);
            g_object_unref(c);
        }), this);
    }

    void registerObjects()
    {
        static const GDBusInterfaceVTable vtable = {methodCall, getProperty, nullptr, {}};
        for (const auto &[path, xml] : {std::pair{QByteArray(kRoot) + "/VM", kVmXml},
                                        std::pair{QByteArray(kRoot) + "/Console_0", kConsoleXml},
                                        std::pair{QByteArray(kRoot) + "/Clipboard", kClipboardXml}}) {
            GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(xml, nullptr);
            for (int i = 0; node->interfaces[i]; i++) {
                g_dbus_connection_register_object(m_conn, path.constData(), node->interfaces[i],
                                                  &vtable, this, nullptr, nullptr);
            }
            g_dbus_node_info_unref(node);
        }
    }

    /*
     * As QEMU does, the listener's interfaces first: its answer comes from
     * its thread's loop.  Until then, the view's Listener::stop() would
     * lose its quit (g_main_loop_quit() before g_main_loop_run()) and wait
     * for ever: the research side's listener.cpp, which a view taken down
     * as its listener connects hits too.
     */
    void listenerConnected(GDBusConnection *conn)
    {
        g_dbus_connection_call(conn, nullptr, "/org/qemu/Display1/Listener",
                               "org.freedesktop.DBus.Properties", "Get",
                               g_variant_new("(ss)", "org.qemu.Display1.Listener", "Interfaces"),
                               G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
                               [](GObject *source, GAsyncResult *res, gpointer self) {
            auto *conn = G_DBUS_CONNECTION(source);
            if (GVariant *ret = g_dbus_connection_call_finish(conn, res, nullptr)) {
                g_variant_unref(ret);
                auto *d = static_cast<FakeDisplay *>(self);
                d->keep(conn);
                d->m_listener = conn;
                d->m_listeners++;
            } else {
                g_object_unref(conn);
            }
        }, this);
    }

    static GVariant *getProperty(GDBusConnection *, const char *, const char *,
                                 const char *, const char *property, GError **, void *)
    {
        if (!strcmp(property, "Name")) {
            return g_variant_new_string("fake");
        }
        if (!strcmp(property, "ConsoleIDs")) {
            const guint32 ids[] = {0};
            return g_variant_new_fixed_array(G_VARIANT_TYPE_UINT32, ids, 1, sizeof(guint32));
        }
        if (!strcmp(property, "Type")) {
            return g_variant_new_string("Graphic");
        }
        if (!strcmp(property, "IsAbsolute")) {
            return g_variant_new_boolean(TRUE);
        }
        return nullptr;
    }

    static void methodCall(GDBusConnection *, const char *, const char *, const char *iface,
                           const char *method, GVariant *params, GDBusMethodInvocation *inv,
                           void *user)
    {
        auto *d = static_cast<FakeDisplay *>(user);
        const QString shortIface = QString::fromUtf8(iface).section('.', 3);
        gchar *args = g_variant_print(params, FALSE);
        {
            std::lock_guard g(d->m_lock);
            d->m_calls << QString("%1.%2 %3").arg(shortIface, method, args);
        }
        g_free(args);
        if (!strcmp(method, "RegisterListener")) {
            GUnixFDList *list =
                g_dbus_message_get_unix_fd_list(g_dbus_method_invocation_get_message(inv));
            const int fd = list ? g_unix_fd_list_get(list, 0, nullptr) : -1;
            g_dbus_method_invocation_return_value(inv, nullptr);
            if (fd >= 0) {
                connectionOn(fd, [](GObject *, GAsyncResult *res, gpointer self) {
                    if (GDBusConnection *conn = g_dbus_connection_new_finish(res, nullptr)) {
                        static_cast<FakeDisplay *>(self)->listenerConnected(conn);
                    }
                }, d, false);
            }
            return;
        }
        if (d->m_hold &&
            (shortIface == "Keyboard" || shortIface == "UIInfo" || shortIface == "Clipboard")) {
            d->m_held.push_back(inv);
            return;
        }
        g_dbus_method_invocation_return_value(inv, nullptr);
        d->m_answered++;
    }

    int m_listenFd = -1;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_hold{false};
    std::atomic<int> m_answered{0};
    std::thread m_qmp, m_bus;
    GMainContext *m_ctx;
    GMainLoop *m_loop;
    GDBusConnection *m_conn = nullptr;              // bus thread
    std::atomic<GDBusConnection *> m_listener{nullptr};
    std::atomic<int> m_listeners{0};
    std::vector<GDBusConnection *> m_open;          // bus thread
    std::vector<GDBusMethodInvocation *> m_held;    // bus thread
    mutable std::mutex m_lock;
    QStringList m_calls;
};

/*
 * The pointer as a Wayland compositor gives it to a window with subsurfaces,
 * KWin 6.7's way (seat.cpp, SeatInterface::notifyPointerMotion): each event
 * goes to the surface under the pointer that takes input - a child window
 * over the top-level's own surface, unless it is transparent for input -
 * with a leave and an enter when that surface changes, a button held or
 * not.  Only the top-level stays the same while a button is down; there is
 * no other here.  Positions are in the top-level's coordinates.
 */
class Pointer
{
public:
    explicit Pointer(QWindow *top) : m_top(top) {}

    void move(QPointF pos) { deliver(pos, QEvent::MouseMove, Qt::NoButton); }
    void press(QPointF pos, Qt::MouseButton button = Qt::LeftButton)
    {
        m_buttons |= button;
        deliver(pos, QEvent::MouseButtonPress, button);
    }
    void release(QPointF pos, Qt::MouseButton button = Qt::LeftButton)
    {
        m_buttons &= ~button;
        deliver(pos, QEvent::MouseButtonRelease, button);
    }
    /* One notch of a wheel, up */
    void wheel(QPointF pos)
    {
        QWindow *target = focusAt(pos);
        QWindowSystemInterface::handleWheelEvent(target, localIn(target, pos),
                                                 m_top->mapToGlobal(pos), QPoint(), QPoint(0, 120));
        QWindowSystemInterface::flushWindowSystemEvents();
    }
    /* The pointer goes off the window */
    void leave()
    {
        if (m_focus) {
            QWindowSystemInterface::handleLeaveEvent<QWindowSystemInterface::SynchronousDelivery>(
                std::exchange(m_focus, nullptr));
        }
    }
    /* Where the events go at @pos */
    QWindow *surfaceAt(QPointF pos) const
    {
        const QObjectList children = m_top->children();
        for (auto it = children.crbegin(); it != children.crend(); ++it) {
            auto *window = qobject_cast<QWindow *>(*it);
            if (window && window->isVisible() &&
                !(window->flags() & Qt::WindowTransparentForInput) &&
                window->geometry().contains(pos.toPoint())) {
                return window;
            }
        }
        return m_top;
    }

private:
    QPointF localIn(QWindow *target, QPointF pos) const
    {
        return target == m_top ? pos : pos - QPointF(target->position());
    }
    QWindow *focusAt(QPointF pos)
    {
        QWindow *target = surfaceAt(pos);
        if (target != m_focus) {
            leave();
            QWindowSystemInterface::handleEnterEvent<QWindowSystemInterface::SynchronousDelivery>(
                target, localIn(target, pos), m_top->mapToGlobal(pos));
            m_focus = target;
        }
        return target;
    }
    void deliver(QPointF pos, QEvent::Type type, Qt::MouseButton button)
    {
        QWindow *target = focusAt(pos);
        QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::SynchronousDelivery>(
            target, localIn(target, pos), m_top->mapToGlobal(pos), m_buttons, button, type);
    }

    QWindow *m_top;
    QWindow *m_focus = nullptr;
    Qt::MouseButtons m_buttons;
};

class TestVmView : public QObject
{
    Q_OBJECT

private:
    /* out of the build tree: the socket's path fits in sun_path wherever that is */
    QTemporaryDir m_dir{QDir::tempPath() + "/vitrine-vmview-XXXXXX"};
    QString socketPath() const { return m_dir.filePath("qmp"); }

    /* The DisplayWindow the view made */
    static QWindow *displayWindow()
    {
        for (QWindow *w : QGuiApplication::allWindows()) {
            if (!strcmp(w->metaObject()->className(), "DisplayWindow")) {
                return w;
            }
        }
        return nullptr;
    }

    /* Its container in the view's widget, where it is embedded */
    static QWidget *screenContainer(VmView *view)
    {
        for (QWidget *w : view->widget()->findChildren<QWidget *>()) {
            if (!strcmp(w->metaObject()->className(), "QWindowContainer")) {
                return w;
            }
        }
        return nullptr;
    }

    /* The guest's screen, @width x @height from its scanout, as the view learns it */
    static bool guestScreen(FakeDisplay &qemu, VmView *view, int width, int height)
    {
        const int buffer = memfd_create("guest", MFD_CLOEXEC);
        if (buffer < 0 || ftruncate(buffer, off_t(width) * height * 4) != 0) {
            return false;
        }
        qemu.scanout(buffer, uint32_t(width), uint32_t(height));
        close(buffer);
        return QTest::qWaitFor([&]() { return view->guestSize() == QSize(width, height); });
    }

    /* A key as the compositor gives it: XKB keycode = evdev + 8 (KEY_A: 30) */
    static void key(QObject *to, QEvent::Type type, Qt::Key k = Qt::Key_A, quint32 evdev = 30,
                    Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        QKeyEvent e(type, k, mods, evdev + 8, 0, 0, QString());
        QCoreApplication::sendEvent(to, &e);
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        QVERIFY(socketPath().toLocal8Bit().size() < 100);
    }

    void cleanup()
    {
        /* the deferred parts of the views gone */
        QTest::qWait(50);
        QFile::remove(socketPath());
    }

    void attachAndDetach()
    {
        FakeDisplay qemu(socketPath());
        auto *view = new VmView;
        QString error;
        QVERIFY2(view->attach(socketPath(), &error), qPrintable(error));
        QVERIFY(view->isAttached());
        QCOMPARE(view->vmName(), QString("fake"));
        QTRY_VERIFY(qemu.listening());
        QCOMPARE(qemu.callsTo("Console.RegisterListener").size(), 1);
        delete view;
    }

    /*
     * The screen has the keyboard when its window goes: the VM stops while
     * someone types in it, or goes full screen.  Nothing may reach the old
     * window once it is deleted, nor signal while the view has none.
     */
    void windowGoesWithKeyboard_data()
    {
        QTest::addColumn<bool>("fullScreen");
        QTest::newRow("view deleted") << false;
        QTest::newRow("full screen") << true;
    }
    void windowGoesWithKeyboard()
    {
        QFETCH(bool, fullScreen);
        FakeDisplay qemu(socketPath());
        QWidget top;
        auto *layout = new QVBoxLayout(&top);
        auto *view = new VmView;
        QString error;
        QVERIFY2(view->attach(socketPath(), &error), qPrintable(error));
        QTRY_VERIFY(qemu.listening());
        layout->addWidget(view->widget());
        top.resize(640, 480);
        top.show();
        QVERIFY(QTest::qWaitForWindowActive(&top));
        view->focus();
        QTRY_VERIFY(view->hasKeyboard());

        QPointer<QWindow> old = displayWindow();
        QVERIFY(old);
        QStringList wrong;
        connect(view, &VmView::grabChanged, this, [&]() {
            /* between the old window's end and a new one: what the view
               would read is gone */
            if (old.isNull() && !displayWindow()) {
                wrong << "grabChanged() without a window";
            }
            /* what MainWindow reads on it */
            view->grabState();
            view->grabbed();
        });
        if (fullScreen) {
            view->setFullScreen(true);
            QVERIFY(view->isFullScreen());
            QVERIFY(displayWindow());
            QVERIFY(displayWindow() != old);
            view->setFullScreen(false);
        }
        delete view;
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(", ")));
    }

    /*
     * GDBus completes a call one GUI-thread iteration after its reply, with
     * pointers to the view's display and statistics: the replies already
     * in when the view goes, and the call the close fails (UIInfo.Apply,
     * unanswered), complete after it.
     */
    void lateCompletions()
    {
        FakeDisplay qemu(socketPath());
        QWidget top;
        auto *layout = new QVBoxLayout(&top);
        auto *view = new VmView;
        QString error;
        QVERIFY2(view->attach(socketPath(), &error), qPrintable(error));
        QTRY_VERIFY(qemu.listening());
        layout->addWidget(view->widget());
        /* a size for the guest: UIInfo.Apply, left unanswered */
        qemu.hold(true);
        top.resize(800, 600);
        top.show();
        QTRY_VERIFY(!qemu.callsTo("UIInfo.Apply").isEmpty());
        qemu.hold(false);

        /* key calls answered while the GUI thread does not look */
        const int before = qemu.answered();
        view->sendCtrlAltDel();
        for (int i = 0; i < 400 && qemu.answered() < before + 6; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        QCOMPARE(qemu.answered(), before + 6);
        /* the replies on their way in, their completions queued */
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        delete view;
        /* they run now */
        QTest::qWait(200);
        qemu.letGo();
        QTest::qWait(50);
    }

    /*
     * A view never shown - another VM's console is - gives the guest's
     * buffers back all the same: QEMU holds each until it is released
     */
    void unshownViewReleases()
    {
        FakeDisplay qemu(socketPath());
        VmView view;
        QString error;
        QVERIFY2(view.attach(socketPath(), &error), qPrintable(error));
        QTRY_VERIFY(qemu.listening());
        const int buffer = memfd_create("guest", MFD_CLOEXEC);
        struct stat st{};
        QVERIFY(buffer >= 0 && ftruncate(buffer, 64 * 64 * 4) == 0 && fstat(buffer, &st) == 0);
        qemu.scanout(buffer, 64, 64);
        close(buffer);
        for (int i = 0; i < 3; i++) {
            qemu.update(64, 64);
        }
        QTRY_COMPARE(qemu.callsTo("Presentation.Released").size(), 3);
        for (const QString &call : qemu.callsTo("Presentation.Released")) {
            QCOMPARE(call, QString("Presentation.Released (%1,)").arg(st.st_ino));
        }
    }

    /* Back from full screen: QEMU no longer counts on the screen taking the
       guest's buffers as they are, even if the console is not shown */
    void fullScreenEnds()
    {
        FakeDisplay qemu(socketPath());
        VmView view;
        QString error;
        QVERIFY2(view.attach(socketPath(), &error), qPrintable(error));
        QTRY_VERIFY(qemu.listening());
        QSignalSpy changed(&view, &VmView::fullScreenChanged);
        view.setFullScreen(true);
        QVERIFY(view.isFullScreen());
        QVERIFY(qemu.callsTo("Presentation.ZeroCopy").isEmpty());
        view.setFullScreen(false);
        QVERIFY(!view.isFullScreen());
        QCOMPARE(changed.size(), 2);
        QTRY_COMPARE(qemu.callsTo("Presentation.ZeroCopy"), QStringList({"Presentation.ZeroCopy (false,)"}));
    }

    /*
     * The full-screen window closed by the desktop (its title bar, the task
     * bar): the window refuses the close, which would delete it under the
     * render thread and quit the app with the last window, and the view goes
     * back to the widget
     */
    void fullScreenClosedByDesktop()
    {
        FakeDisplay qemu(socketPath());
        VmView view;
        QString error;
        QVERIFY2(view.attach(socketPath(), &error), qPrintable(error));
        QTRY_VERIFY(qemu.listening());
        view.setFullScreen(true);
        QPointer<QWindow> full = displayWindow();
        QVERIFY(full && !full->parent());
        /* refused; not at once: from the window's own event handler */
        QVERIFY(!full->close());
        QVERIFY(view.isFullScreen());
        QTRY_VERIFY(!view.isFullScreen());
        QVERIFY(full.isNull());
        QVERIFY(displayWindow());
    }

    /*
     * Paused, QEMU drops the keys: the screen takes none, so that they reach
     * the window's shortcuts (Resume among them), and lets go of those held
     */
    void inputOff()
    {
        FakeDisplay qemu(socketPath());
        QWidget top;
        auto *layout = new QVBoxLayout(&top);
        auto *view = new VmView(&top);
        QString error;
        QVERIFY2(view->attach(socketPath(), &error), qPrintable(error));
        QTRY_VERIFY(qemu.listening());
        layout->addWidget(view->widget());
        top.resize(640, 480);
        top.show();
        QVERIFY(QTest::qWaitForWindowActive(&top));
        view->focus();
        QTRY_VERIFY(view->hasKeyboard());
        QWidget *screen = QApplication::focusWidget();
        QVERIFY(screen);
        auto override = [screen]() {
            QKeyEvent e(QEvent::ShortcutOverride, Qt::Key_P, Qt::ControlModifier, 25 + 8, 0, 0);
            e.ignore();
            QCoreApplication::sendEvent(screen, &e);
            return e.isAccepted();
        };

        QVERIFY(view->inputEnabled());
        QVERIFY(override());
        key(screen, QEvent::KeyPress);
        QTRY_COMPARE(qemu.callsTo("Keyboard."), QStringList({"Keyboard.Press (30,)"}));

        /* off with A held: released, the screen loses the keyboard */
        QSignalSpy changed(view, &VmView::grabChanged);
        view->setInputEnabled(false);
        QVERIFY(!view->inputEnabled());
        QVERIFY(!view->hasKeyboard());
        QVERIFY(!changed.isEmpty());
        QTRY_COMPARE(qemu.callsTo("Keyboard."),
                     QStringList({"Keyboard.Press (30,)", "Keyboard.Release (30,)"}));
        QVERIFY(!override());
        key(screen, QEvent::KeyPress);
        key(screen, QEvent::KeyRelease);
        QTest::qWait(100);
        QCOMPARE(qemu.callsTo("Keyboard.").size(), 2);

        /* on again: the keys go to the guest again */
        view->setInputEnabled(true);
        QTRY_VERIFY(view->hasKeyboard());
        QVERIFY(override());
        key(screen, QEvent::KeyPress);
        key(screen, QEvent::KeyRelease);
        QTRY_COMPARE(qemu.callsTo("Keyboard.").size(), 4);
        delete view;
    }

    /* A guest that runs keeps its mode while the view has no size (a
       console not shown, after vitrine started again) */
    void noScreenInfoUnshown()
    {
        FakeDisplay qemu(socketPath());
        VmView view;
        QString error;
        QVERIFY2(view.attach(socketPath(), &error), qPrintable(error));
        QTRY_VERIFY(qemu.listening());
        QTest::qWait(300);
        QCOMPARE(qemu.callsTo("UIInfo.Apply").size(), 0);
    }

    /*
     * The guest's screen goes to QEMU as the view attaches, before its
     * window has a size: a guest paused until then runs at the answer
     * (screenInfoApplied, once), with the refresh rate of the screen
     */
    void screenInfoAtAttach()
    {
        FakeDisplay qemu(socketPath());
        auto *view = new VmView;
        QSignalSpy applied(view, &VmView::screenInfoApplied);
        QString error;
        qemu.hold(true);
        QVERIFY2(view->attach(socketPath(), &error, true), qPrintable(error));
        QTRY_COMPARE(qemu.callsTo("UIInfo.Apply").size(), 1);
        const QString call = qemu.callsTo("UIInfo.Apply").first();
        QVERIFY2(call.contains("'width': <uint32") && call.contains("'refresh_rate': <uint32"),
                 qPrintable(call));
        QTest::qWait(50);
        QCOMPARE(applied.size(), 0);
        qemu.letGo();
        QTRY_COMPARE(applied.size(), 1);
        qemu.hold(false);

        /* the window's own size: again, no signal */
        QWidget top;
        auto *layout = new QVBoxLayout(&top);
        layout->addWidget(view->widget());
        top.resize(640, 480);
        top.show();
        QTRY_COMPARE(qemu.callsTo("UIInfo.Apply").size(), 2);
        QTest::qWait(50);
        QCOMPARE(applied.size(), 1);

        /* a new pixel ratio, from a move to another screen (which the
           size in logical pixels does not tell): the guest's mode again */
        QEvent dpr(QEvent::DevicePixelRatioChange);
        QCoreApplication::sendEvent(displayWindow(), &dpr);
        QTRY_COMPARE(qemu.callsTo("UIInfo.Apply").size(), 3);
        QCOMPARE(applied.size(), 1);
        delete view;
    }

    /*
     * A view attached and detached while QEMU runs leaves no file behind:
     * each listener stopped while QEMU was connected kept an eventfd (its
     * GLib context, which the closed connection's queued signal held), and
     * vitrine runs for days with the views of VMs coming and going
     */
    void noFdsLeft()
    {
        FakeDisplay qemu(socketPath());
        auto open = []() {
            return QDir("/proc/self/fd").entryList(QDir::System | QDir::Files |
                                                   QDir::NoDotAndDotDot).size();
        };
        int attached = 0;
        auto cycle = [&]() {
            auto *view = new VmView;
            QString error;
            QVERIFY2(view->attach(socketPath(), &error), qPrintable(error));
            const int expected = ++attached;
            QTRY_COMPARE(qemu.listeners(), expected);
            delete view;
            QTest::qWait(50);
        };
        cycle();    // what stays for good (GLib's worker, Qt's)
        const qsizetype before = open();
        for (int i = 0; i < 5; i++) {
            cycle();
        }
        if (open() != before) {
            /* which, for the log */
            for (const QFileInfo &fd : QDir("/proc/self/fd").entryInfoList(
                     QDir::System | QDir::Files | QDir::NoDotAndDotDot)) {
                qWarning("fd %s -> %s", qPrintable(fd.fileName()), qPrintable(fd.symLinkTarget()));
            }
        }
        QCOMPARE(open(), before);
    }

    /*
     * A view taken down as soon as it attached (its VM stopped at once):
     * its listener's thread may still be in its handshake with QEMU, where
     * the quit of Listener::stop() was lost and the join waited for ever
     */
    void deleteRightAfterAttach()
    {
        FakeDisplay qemu(socketPath());
        for (int i = 0; i < 5; i++) {
            auto *view = new VmView;
            QString error;
            QVERIFY2(view->attach(socketPath(), &error), qPrintable(error));
            delete view;
        }
        QTest::qWait(100);
    }

    /*
     * The display's calls still on their way when it goes complete later,
     * from GLib on the GUI thread, with a pointer to it: they must not reach
     * it (qt-client.md F14; VmView's retire() also keeps it until then)
     */
    void displayGoesWithCallsInFlight()
    {
        FakeDisplay qemu(socketPath());
        /* the view's way to the display's connection: getfd + add_client */
        const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un addr{};
        const QByteArray path = socketPath().toLocal8Bit();
        addr.sun_family = AF_UNIX;
        memcpy(addr.sun_path, path.constData(), size_t(path.size()));
        QVERIFY(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
        int sv[2];
        QVERIFY(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);
        {
            Qmp qmp(fd);
            QJsonObject ret;
            QString error;
            QVERIFY2(qmp.handshake(&error, 5000), qPrintable(error));
            QVERIFY(qmp.execute("getfd", {{"fdname", "d"}}, &ret, &error, sv[1]));
            QVERIFY(qmp.execute("add_client", {{"protocol", "@dbus-display"}, {"fdname", "d"}},
                                &ret, &error));
        }
        close(sv[1]);
        auto *display = new DBusDisplay(nullptr);
        QString error;
        QVERIFY2(display->connectPeer(sv[0], &error), qPrintable(error));
        QVERIFY2(display->selectConsole(&error), qPrintable(error));
        bool applied = false;
        connect(display, &DBusDisplay::uiInfoApplied, this, [&applied]() { applied = true; });

        /* one answered while the GUI thread does not look, one held */
        display->applyUiInfo(640, 480, 60000, 0, 0);
        for (int i = 0; i < 400 && qemu.callsTo("UIInfo.Apply").isEmpty(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        qemu.hold(true);
        display->applyUiInfo(800, 600, 60000, 0, 0);
        for (int i = 0; i < 400 && qemu.callsTo("UIInfo.Apply").size() < 2; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        QCOMPARE(qemu.callsTo("UIInfo.Apply").size(), 2);
        delete display;
        qemu.letGo();
        QTest::qWait(200);
        QVERIFY(!applied);
    }

    /*
     * A button down in the guest goes up when the pointer is taken away
     * without its release (a compositor's grab, a screen lock: Qt gets a
     * Leave and forgets the button) or the view loses the keyboard, else
     * the guest drags on with the next motion (qt-client.md F11)
     */
    void buttonsReleased()
    {
        FakeDisplay qemu(socketPath());
        QWidget top;
        auto *layout = new QVBoxLayout(&top);
        auto *view = new VmView(&top);
        QString error;
        QVERIFY2(view->attach(socketPath(), &error), qPrintable(error));
        QTRY_VERIFY(qemu.listening());
        layout->addWidget(view->widget());
        top.resize(640, 480);
        top.show();
        QVERIFY(QTest::qWaitForWindowActive(&top));
        view->focus();
        QTRY_VERIFY(view->hasKeyboard());
        QWindow *screen = displayWindow();
        QVERIFY(screen);
        auto press = [screen](Qt::MouseButton button) {
            QMouseEvent e(QEvent::MouseButtonPress, QPointF(10, 10), QPointF(10, 10), button,
                          button, Qt::NoModifier);
            QCoreApplication::sendEvent(screen, &e);
        };

        press(Qt::LeftButton);
        QTRY_COMPARE(qemu.callsTo("Mouse.Press"), QStringList({"Mouse.Press (0,)"}));
        QEvent leave(QEvent::Leave);
        QCoreApplication::sendEvent(screen, &leave);
        QTRY_COMPARE(qemu.callsTo("Mouse.Release"), QStringList({"Mouse.Release (0,)"}));

        press(Qt::RightButton);
        QTRY_COMPARE(qemu.callsTo("Mouse.Press").size(), 2);
        view->setInputEnabled(false);
        QTRY_COMPARE(qemu.callsTo("Mouse.Release"),
                     QStringList({"Mouse.Release (0,)", "Mouse.Release (2,)"}));
        /* once */
        QCoreApplication::sendEvent(screen, &leave);
        QTest::qWait(100);
        QCOMPARE(qemu.callsTo("Mouse.Release").size(), 2);
        delete view;
    }

    /*
     * The user's report (2026-10-05): the main window's splitter, the list
     * of VMs on its left and the screen on its right, moved left but not
     * right.  Dragged right, the pointer is over the screen at its first
     * move, and the compositor gave it to the screen's surface (a button
     * held does not keep it on the window's own): the splitter stopped, and
     * the guest got the moves and the release.  Now the splitter follows
     * both ways, and the guest gets nothing of a drag that is not its own.
     */
    void splitterDraggedOverScreen()
    {
        FakeDisplay qemu(socketPath());
        QSplitter top;
        /* gone before the window it is in, the test failed or not */
        std::unique_ptr<VmView> view(new VmView);
        QString error;
        QVERIFY2(view->attach(socketPath(), &error), qPrintable(error));
        QTRY_VERIFY(qemu.listening());
        QVERIFY(guestScreen(qemu, view.get(), 64, 64));     // its moves would reach the guest
        top.addWidget(new QWidget);
        top.addWidget(view->widget());
        top.setChildrenCollapsible(false);
        top.resize(800, 500);
        top.setSizes({260, 540});
        top.show();
        QVERIFY(QTest::qWaitForWindowExposed(&top));
        QTRY_VERIFY(displayWindow() && displayWindow()->isVisible());
        const int before = top.sizes().first();
        qemu.clearCalls();

        Pointer pointer(top.windowHandle());
        QSplitterHandle *handle = top.handle(1);
        auto drag = [&](int by) {
            const QPointF start = handle->mapTo(&top, QPointF(handle->rect().center()));
            pointer.move(start);
            pointer.press(start);
            for (int i = 1; i <= 20; i++) {
                pointer.move(start + QPointF(by * i / 20.0, 0));
            }
            pointer.release(start + QPointF(by, 0));
            /* away from the handle: over the screen */
            pointer.move(QPointF(top.width() - 20, top.height() / 2.0));
        };

        drag(200);
        QCOMPARE(top.sizes().first(), before + 200);
        drag(-100);
        QCOMPARE(top.sizes().first(), before + 100);
        /* the moves after the drags: the guest's own, the only ones it got */
        QTest::qWait(100);
        const QStringList calls = qemu.callsTo("Mouse.");
        QVERIFY2(!calls.filter(QRegularExpression("^Mouse.(Press|Release)")).size(),
                 qPrintable(calls.join(", ")));
        QCOMPARE(calls.size(), 2);
    }

    /*
     * The pointer over the screen reaches the guest, through the window
     * around it: moves (in the guest's pixels), both presses of a double
     * click, the wheel, the right button (and no context menu of the
     * window's), and a drag that goes on past the screen's edge, with the
     * button down until its release, as with any widget
     */
    void screenTakesPointer()
    {
        FakeDisplay qemu(socketPath());
        QWidget top;
        auto *layout = new QHBoxLayout(&top);
        auto *side = new QWidget;
        std::unique_ptr<VmView> view(new VmView);
        QString error;
        QVERIFY2(view->attach(socketPath(), &error), qPrintable(error));
        QTRY_VERIFY(qemu.listening());
        QVERIFY(guestScreen(qemu, view.get(), 640, 480));
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        side->setFixedWidth(100);
        layout->addWidget(side);
        layout->addWidget(view->widget(), 1);
        top.setContextMenuPolicy(Qt::CustomContextMenu);
        QSignalSpy menu(&top, &QWidget::customContextMenuRequested);
        /* the screen as large as the guest's: the same pixels (scale 1) */
        top.resize(100 + 640, 480);
        top.show();
        QVERIFY(QTest::qWaitForWindowExposed(&top));
        QTRY_VERIFY(displayWindow() && displayWindow()->size() == QSize(640, 480));
        qemu.clearCalls();
        Pointer pointer(top.windowHandle());

        pointer.move(QPointF(110, 20));
        QTRY_COMPARE(qemu.callsTo("Mouse."), QStringList({"Mouse.SetAbsPosition (10, 20)"}));

        qemu.clearCalls();
        for (int i = 0; i < 2; i++) {
            pointer.press(QPointF(110, 20));
            pointer.release(QPointF(110, 20));
        }
        pointer.wheel(QPointF(110, 20));
        pointer.press(QPointF(110, 20), Qt::RightButton);
        pointer.release(QPointF(110, 20), Qt::RightButton);
        QTRY_COMPARE(qemu.callsTo("Mouse.Press"),
                     QStringList({"Mouse.Press (0,)", "Mouse.Press (0,)", "Mouse.Press (3,)",
                                  "Mouse.Press (2,)"}));
        QTRY_COMPARE(qemu.callsTo("Mouse.Release"),
                     QStringList({"Mouse.Release (0,)", "Mouse.Release (0,)",
                                  "Mouse.Release (3,)", "Mouse.Release (2,)"}));
        QCOMPARE(menu.size(), 0);

        /* pressed in the screen, released over the side: at the screen's edge
           in between */
        auto lastAt = [&qemu]() {
            const QStringList at = qemu.callsTo("Mouse.SetAbsPosition");
            return at.value(at.size() - 1);
        };
        qemu.clearCalls();
        pointer.press(QPointF(400, 200));
        pointer.move(QPointF(300, 200));
        QTRY_COMPARE(lastAt(), QString("Mouse.SetAbsPosition (200, 200)"));
        pointer.move(QPointF(50, 200));
        QTRY_COMPARE(lastAt(), QString("Mouse.SetAbsPosition (0, 200)"));
        QTest::qWait(100);
        QCOMPARE(qemu.callsTo("Mouse.Release").size(), 0);
        pointer.release(QPointF(50, 200));
        QTRY_COMPARE(qemu.callsTo("Mouse.Release"), QStringList({"Mouse.Release (0,)"}));
        /* the pointer off the window: nothing held */
        pointer.press(QPointF(400, 200));
        pointer.leave();
        QTRY_COMPARE(qemu.callsTo("Mouse.Release").size(), 2);
    }

    /*
     * The guest's cursor shows over the screen: the window's own surface
     * never has the pointer, its container does, which has the cursor the
     * window was given (DisplayWindow's: the guest's, or none)
     */
    void cursorOnScreen()
    {
        FakeDisplay qemu(socketPath());
        QWidget top;
        auto *layout = new QVBoxLayout(&top);
        std::unique_ptr<VmView> view(new VmView);
        QString error;
        QVERIFY2(view->attach(socketPath(), &error), qPrintable(error));
        QTRY_VERIFY(qemu.listening());
        layout->addWidget(view->widget());
        top.resize(640, 480);
        top.show();
        QVERIFY(QTest::qWaitForWindowExposed(&top));
        QWidget *container = screenContainer(view.get());
        QVERIFY(container);

        displayWindow()->setCursor(Qt::BlankCursor);
        QCOMPARE(container->cursor().shape(), Qt::BlankCursor);
        QPixmap guest(16, 16);
        guest.fill(Qt::red);
        displayWindow()->setCursor(QCursor(guest, 3, 4));
        QCOMPARE(container->cursor().shape(), Qt::BitmapCursor);
        QCOMPARE(container->cursor().hotSpot(), QPoint(3, 4));

        /* in full screen the window is its own, with the pointer: back, the
           new window's cursor is on the new container */
        view->setFullScreen(true);
        QVERIFY(!(displayWindow()->flags() & Qt::WindowTransparentForInput));
        Pointer full(displayWindow());
        qemu.clearCalls();
        full.press(QPointF(10, 10));
        full.release(QPointF(10, 10));
        QTRY_COMPARE(qemu.callsTo("Mouse.Release"), QStringList({"Mouse.Release (0,)"}));
        displayWindow()->setCursor(Qt::BlankCursor);
        view->setFullScreen(false);
        QVERIFY(screenContainer(view.get()) && screenContainer(view.get()) != container);
        QCOMPARE(screenContainer(view.get())->cursor().shape(), displayWindow()->cursor().shape());
    }

    /* The view shares the clipboard on its connection; it may go before
       QEMU answers its Register */
    void clipboard()
    {
        FakeDisplay qemu(socketPath());
        auto *view = new VmView;
        QString error;
        qemu.hold(true);
        QVERIFY2(view->attach(socketPath(), &error), qPrintable(error));
        QTRY_COMPARE(qemu.callsTo("Clipboard."), QStringList({"Clipboard.Register ()"}));
        delete view;
        qemu.hold(false);
        qemu.letGo();
        QTest::qWait(100);
    }

    void attachFails()
    {
        VmView view;
        QString error;
        QVERIFY(!view.attach(socketPath(), &error));
        QVERIFY(error.startsWith("Cannot connect to"));
        QVERIFY(!view.isAttached());
    }
};

int main(int argc, char **argv)
{
    /* no window on the user's screen, no GL through it */
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qunsetenv("DISPLAY");
    qunsetenv("WAYLAND_DISPLAY");
    QApplication app(argc, argv);
    TestVmView test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}

#include "test_vmview.moc"
