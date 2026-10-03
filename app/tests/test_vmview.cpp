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
#include <QKeyEvent>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QVBoxLayout>
#include <QWidget>
#include <QPointer>
#include <QRegularExpression>
#include <QWindow>

#include <atomic>
#include <chrono>
#include <cstring>
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
            for (GDBusConnection *conn : {m_listener.load(), m_conn}) {
                if (conn) {
                    g_dbus_connection_close_sync(conn, nullptr, nullptr);
                    g_object_unref(conn);
                }
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
    /* Keyboard and UIInfo calls wait for an answer until let go */
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
                    d->registerObjects();
                    g_dbus_connection_start_message_processing(d->m_conn);
                }
            }, peer->self, true);
            delete peer;
            return G_SOURCE_REMOVE;
        }, new Peer{this, fd});
    }

    void registerObjects()
    {
        static const GDBusInterfaceVTable vtable = {methodCall, getProperty, nullptr, {}};
        for (const auto &[path, xml] : {std::pair{QByteArray(kRoot) + "/VM", kVmXml},
                                        std::pair{QByteArray(kRoot) + "/Console_0", kConsoleXml}}) {
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
                static_cast<FakeDisplay *>(self)->m_listener = conn;
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
        if (d->m_hold && (shortIface == "Keyboard" || shortIface == "UIInfo")) {
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
    std::vector<GDBusMethodInvocation *> m_held;    // bus thread
    mutable std::mutex m_lock;
    QStringList m_calls;
};

class TestVmView : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir{QCoreApplication::applicationDirPath() + "/vmview-XXXXXX"};
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
