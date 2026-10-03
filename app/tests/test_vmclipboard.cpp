// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The clipboard shared with the guest: the mime types and the state alone,
 * then VmClipboard against a stand-in for QEMU's side of the D-Bus
 * display's clipboard (ui/dbus-clipboard.c), served from a thread of its
 * own on a peer-to-peer connection as QEMU serves it.  The platform is
 * offscreen: the host clipboard is the process's own.
 */
#include <QApplication>
#include <QClipboard>
#include <QImage>
#include <QMimeData>
#include <QPointer>
#include <QSignalSpy>
#include <QTest>
#include <QKeyEvent>
#include <QWindow>

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

#include <gio/gio.h>
#include <sys/socket.h>

#include "vmview/vmclipboard.h"

namespace {

const char kPath[] = "/org/qemu/Display1/Clipboard";
const char kInterface[] = "org.qemu.Display1.Clipboard";
const char kText[] = "text/plain;charset=utf-8";
/* what QEMU asks for and offers */
const char *const kTexts[] = {kText, nullptr};

/* The interface, from ui/dbus-display1.xml */
const char kXml[] = R"XML(
<node>
  <interface name="org.qemu.Display1.Clipboard">
    <method name="Register"/>
    <method name="Unregister"/>
    <method name="Grab">
      <arg type="u" name="selection"/><arg type="u" name="serial"/><arg type="as" name="mimes"/>
    </method>
    <method name="Release"><arg type="u" name="selection"/></method>
    <method name="Request">
      <arg type="u" name="selection"/><arg type="as" name="mimes"/>
      <arg type="s" name="reply_mime" direction="out"/><arg type="ay" name="data" direction="out"/>
    </method>
    <property name="Interfaces" type="as" access="read"/>
  </interface>
</node>)XML";

/* "mime: text" of a Request's reply as FakeQemu::toClient() prints it */
QString replyText(const QString &reply)
{
    GVariant *v = g_variant_parse(G_VARIANT_TYPE("(say)"), reply.toUtf8().constData(), nullptr,
                                  nullptr, nullptr);
    if (!v) {
        return reply;
    }
    const char *mime = nullptr;
    GVariant *bytes = nullptr;
    gsize n = 0;
    g_variant_get(v, "(&s@ay)", &mime, &bytes);
    const auto *p = static_cast<const char *>(g_variant_get_fixed_array(bytes, &n, 1));
    const QString text = QString::fromUtf8(mime) + ": " + QString::fromUtf8(p, qsizetype(n));
    g_variant_unref(bytes);
    g_variant_unref(v);
    return text;
}

QMimeData *textData(const QString &text)
{
    auto *data = new QMimeData;
    data->setText(text);
    return data;
}

} // namespace

/*
 * QEMU's side: its Clipboard object on one end of a socket pair, the other
 * end for the client.  Every call the client makes is recorded as
 * "Method (args)"; calls to the client go out with toClient().
 */
class FakeQemu
{
public:
    FakeQemu()
    {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
            qFatal("socketpair");
        }
        m_ctx = g_main_context_new();
        m_loop = g_main_loop_new(m_ctx, FALSE);
        const int serverFd = sv[0];
        std::atomic<bool> ready{false};
        m_thread = std::thread([this, serverFd, &ready]() {
            g_main_context_push_thread_default(m_ctx);
            GSocket *socket = g_socket_new_from_fd(serverFd, nullptr);
            GSocketConnection *stream = g_socket_connection_factory_create_connection(socket);
            gchar *guid = g_dbus_generate_guid();
            /* QEMU is the authentication server; the client authenticates
               at the same time on the GUI thread (below) */
            g_dbus_connection_new(G_IO_STREAM(stream), guid,
                                  GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_SERVER |
                                                       G_DBUS_CONNECTION_FLAGS_DELAY_MESSAGE_PROCESSING),
                                  nullptr, nullptr,
                                  [](GObject *, GAsyncResult *res, gpointer self) {
                auto *d = static_cast<FakeQemu *>(self);
                d->m_conn = g_dbus_connection_new_finish(res, nullptr);
                if (d->m_conn) {
                    d->registerObject();
                    g_dbus_connection_start_message_processing(d->m_conn);
                }
            }, this);
            g_free(guid);
            g_object_unref(stream);
            g_object_unref(socket);
            ready = true;
            g_main_loop_run(m_loop);
            if (m_held) {
                g_dbus_method_invocation_return_dbus_error(m_held, "org.qemu.Error", "gone");
                m_held = nullptr;
            }
            if (m_conn) {
                g_dbus_connection_close_sync(m_conn, nullptr, nullptr);
                g_object_unref(m_conn);
            }
            while (g_main_context_iteration(m_ctx, FALSE)) {
            }
            g_main_context_pop_thread_default(m_ctx);
        });
        while (!ready) {
            std::this_thread::yield();
        }
        GSocket *socket = g_socket_new_from_fd(sv[1], nullptr);
        GSocketConnection *stream = g_socket_connection_factory_create_connection(socket);
        GError *err = nullptr;
        m_client = g_dbus_connection_new_sync(G_IO_STREAM(stream), nullptr,
                                              G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
                                              nullptr, nullptr, &err);
        g_object_unref(stream);
        g_object_unref(socket);
        if (!m_client) {
            qFatal("client connection: %s", err->message);
        }
    }

    ~FakeQemu()
    {
        g_main_loop_quit(m_loop);
        m_thread.join();
        g_dbus_connection_close_sync(m_client, nullptr, nullptr);
        g_object_unref(m_client);
        g_main_loop_unref(m_loop);
        g_main_context_unref(m_ctx);
    }

    /* The client's end */
    GDBusConnection *client() const { return m_client; }

    QStringList calls() const
    {
        std::lock_guard g(m_lock);
        return m_calls;
    }
    QStringList callsTo(const QString &method) const
    {
        QStringList list;
        for (const QString &call : calls()) {
            if (call.startsWith(method + ' ')) {
                list << call;
            }
        }
        return list;
    }
    void clearCalls()
    {
        std::lock_guard g(m_lock);
        m_calls.clear();
    }

    /* The guest's text, the answer to the client's Request; hold: none until letGo() */
    void setGuestData(const QByteArray &data, bool hold = false)
    {
        std::lock_guard g(m_lock);
        m_guestData = data;
        m_hold = hold;
    }
    void letGo()
    {
        g_main_context_invoke(m_ctx, [](gpointer self) {
            auto *d = static_cast<FakeQemu *>(self);
            if (d->m_held) {
                d->answer(std::exchange(d->m_held, nullptr));
            }
            return G_SOURCE_REMOVE;
        }, this);
    }
    bool holding() const { return m_holding; }

    /*
     * A call to the client's Clipboard, from QEMU's thread; *reply (if
     * given) gets the reply printed, or the error
     */
    void toClient(const char *method, GVariant *args, QString *reply = nullptr)
    {
        struct Call {
            FakeQemu *self;
            const char *method;
            GVariant *args;
            QString *reply;
            std::atomic<bool> done{false};
        } call{this, method, args ? g_variant_ref_sink(args) : nullptr, reply};
        g_main_context_invoke(m_ctx, [](gpointer data) {
            auto *c = static_cast<Call *>(data);
            g_dbus_connection_call(c->self->m_conn, nullptr, kPath, kInterface, c->method,
                                   c->args, nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
                                   [](GObject *source, GAsyncResult *res, gpointer data) {
                auto *c = static_cast<Call *>(data);
                GError *err = nullptr;
                GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
                if (c->reply) {
                    if (ret) {
                        gchar *text = g_variant_print(ret, FALSE);
                        *c->reply = QString::fromUtf8(text);
                        g_free(text);
                    } else {
                        *c->reply = QStringLiteral("error: ") + QString::fromUtf8(err->message);
                    }
                }
                if (ret) {
                    g_variant_unref(ret);
                }
                if (err) {
                    g_error_free(err);
                }
                c->done = true;
            }, c);
            return G_SOURCE_REMOVE;
        }, &call);
        /* the client answers from the GUI thread's loop */
        QTRY_VERIFY_WITH_TIMEOUT(call.done, 5000);
        if (call.args) {
            g_variant_unref(call.args);
        }
    }

private:
    void registerObject()
    {
        static const GDBusInterfaceVTable vtable = {methodCall, nullptr, nullptr, {}};
        GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(kXml, nullptr);
        g_dbus_connection_register_object(m_conn, kPath, node->interfaces[0], &vtable, this,
                                          nullptr, nullptr);
        g_dbus_node_info_unref(node);
    }

    void answer(GDBusMethodInvocation *inv)
    {
        QByteArray data;
        {
            std::lock_guard g(m_lock);
            data = m_guestData;
        }
        m_holding = false;
        GVariant *bytes = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, data.constData(),
                                                    gsize(data.size()), 1);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(s@ay)", kText, bytes));
    }

    static void methodCall(GDBusConnection *conn, const char *, const char *, const char *,
                           const char *method, GVariant *params, GDBusMethodInvocation *inv,
                           void *user)
    {
        auto *d = static_cast<FakeQemu *>(user);
        gchar *args = g_variant_print(params, FALSE);
        {
            std::lock_guard g(d->m_lock);
            d->m_calls << QStringLiteral("%1 %2").arg(QString::fromUtf8(method), args);
        }
        g_free(args);
        if (!strcmp(method, "Register")) {
            /*
             * As QEMU does: a proxy for the client's object, made
             * synchronously (it reads the object's properties, which the
             * client's GUI thread answers), then the serials reset, which
             * QEMU tells the client with a Register of its own
             */
            GError *err = nullptr;
            GVariant *props = g_dbus_connection_call_sync(
                conn, nullptr, kPath, "org.freedesktop.DBus.Properties", "GetAll",
                g_variant_new("(s)", kInterface), nullptr, G_DBUS_CALL_FLAGS_NONE, 3000, nullptr,
                &err);
            if (props) {
                g_variant_unref(props);
            } else {
                std::lock_guard g(d->m_lock);
                d->m_calls << QStringLiteral("GetAll failed: %1").arg(err->message);
                g_error_free(err);
            }
            g_dbus_connection_call(conn, nullptr, kPath, kInterface, "Register", nullptr, nullptr,
                                   G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
            g_dbus_method_invocation_return_value(inv, nullptr);
            return;
        }
        if (!strcmp(method, "Request")) {
            bool hold;
            {
                std::lock_guard g(d->m_lock);
                hold = d->m_hold;
            }
            if (hold) {
                d->m_held = inv;
                d->m_holding = true;
            } else {
                d->answer(inv);
            }
            return;
        }
        g_dbus_method_invocation_return_value(inv, nullptr);
    }

    GMainContext *m_ctx;
    GMainLoop *m_loop;
    std::thread m_thread;
    GDBusConnection *m_conn = nullptr;      // QEMU's thread
    GDBusConnection *m_client = nullptr;
    GDBusMethodInvocation *m_held = nullptr; // QEMU's thread
    std::atomic<bool> m_holding{false};
    mutable std::mutex m_lock;
    QStringList m_calls;
    QByteArray m_guestData;
    bool m_hold = false;
};

class TestVmClipboard : public QObject
{
    Q_OBJECT

private:
    static QClipboard *clipboard() { return QGuiApplication::clipboard(); }

    /* A bridge registered with @qemu, its first grabs done */
    static std::unique_ptr<VmClipboard> started(FakeQemu &qemu)
    {
        auto bridge = std::make_unique<VmClipboard>(qemu.client());
        QSignalSpy registered(bridge.get(), &VmClipboard::registered);
        QString error;
        if (!bridge->start(&error)) {
            qWarning("start: %s", qPrintable(error));
            return nullptr;
        }
        if (!registered.wait(5000)) {
            qWarning("not registered");
            return nullptr;
        }
        /* QEMU's Register of its own, after its reply */
        QTest::qWait(50);
        return bridge;
    }

private Q_SLOTS:
    void init()
    {
        clipboard()->clear();
    }

    void textMimes()
    {
        QCOMPARE(ClipboardMime::textMimes().first(), QString(kText));
        QVERIFY(ClipboardMime::isText(kText));
        QVERIFY(ClipboardMime::isText("text/plain; charset=UTF-8"));
        QVERIFY(ClipboardMime::isText("text/plain"));
        QVERIFY(ClipboardMime::isText("UTF8_STRING"));
        QVERIFY(!ClipboardMime::isText("text/html"));
        QVERIFY(!ClipboardMime::isText("image/png"));
        QVERIFY(!ClipboardMime::isText("text/plain;charset=iso-8859-1"));
        QCOMPARE(ClipboardMime::pickText({"image/png", "UTF8_STRING", kText}), QString("UTF8_STRING"));
        QCOMPARE(ClipboardMime::pickText({"image/png", "text/html"}), QString());
        QCOMPARE(ClipboardMime::pickText({}), QString());
    }

    void textOf()
    {
        QCOMPARE(ClipboardMime::textOf(nullptr), QString());
        QMimeData text;
        text.setText(QString::fromUtf8("héllo ✓"));
        QCOMPARE(ClipboardMime::textOf(&text), QString::fromUtf8("héllo ✓"));
        /* only under another name for UTF-8 text, with a NUL at the end */
        QMimeData x11;
        x11.setData("UTF8_STRING", QByteArray("été\0", 5));
        QCOMPARE(ClipboardMime::textOf(&x11), QString::fromUtf8("été"));
        QMimeData image;
        image.setImageData(QImage(4, 4, QImage::Format_RGB32));
        QCOMPARE(ClipboardMime::textOf(&image), QString());
        /* files: their URLs, as a host editor pastes them */
        QMimeData files;
        files.setUrls({QUrl("file:///etc/hosts")});
        QCOMPARE(ClipboardMime::textOf(&files), QString("file:///etc/hosts"));
    }

    /* Serials: ours follow the last grab seen, guest grabs that lost are dropped */
    void serials()
    {
        ClipboardState s;
        uint32_t serial = 99;
        QCOMPARE(s.hostChanged("a", false, &serial), ClipboardState::HostAction::Grab);
        QCOMPARE(serial, 0u);
        QVERIFY(s.hostOwns());
        /* the guest's agent counts from 0 too: its grab after ours is 1 */
        QVERIFY(s.guestGrab(1, {kText}));
        QVERIFY(s.guestOwns());
        QCOMPARE(s.guestSerial(), std::optional<uint32_t>(1));
        QCOMPARE(s.hostChanged("b", false, &serial), ClipboardState::HostAction::Grab);
        QCOMPARE(serial, 2u);
        /* a guest grab of the same serial as ours lost the race */
        QVERIFY(!s.guestGrab(2, {kText}));
        QVERIFY(s.hostOwns());
        /* and an older one too */
        QVERIFY(!s.guestGrab(1, {kText}));
        QVERIFY(s.guestGrab(3, {kText}));
        /* the guest's agent is ahead (grabs we did not see): we follow */
        QVERIFY(s.guestGrab(7, {kText}));
        QCOMPARE(s.hostChanged("c", false, &serial), ClipboardState::HostAction::Grab);
        QCOMPARE(serial, 8u);
        /* QEMU resets: from 0 again */
        s.reset();
        QCOMPARE(s.nextSerial(), 0u);
        QVERIFY(s.guestGrab(0, {kText}));
        QCOMPARE(s.nextSerial(), 1u);
    }

    /* Nothing goes back where it came from */
    void noEcho()
    {
        ClipboardState s;
        uint32_t serial = 0;
        QVERIFY(s.guestGrab(0, {kText}));
        QCOMPARE(s.guestText(0, "guest"), std::optional<QString>("guest"));
        s.hostSeen("guest");        // put in the host clipboard
        /* the host clipboard reports it: not offered back */
        QCOMPARE(s.hostChanged("guest", false, &serial), ClipboardState::HostAction::None);
        QVERIFY(s.guestOwns());
        /* the guest copies the same text again (its X11/Wayland bridge) */
        QVERIFY(s.guestGrab(1, {kText}));
        QCOMPARE(s.guestText(1, "guest"), std::nullopt);
        /* the host's text, offered, then copied again by the guest */
        QCOMPARE(s.hostChanged("host", false, &serial), ClipboardState::HostAction::Grab);
        QCOMPARE(serial, 2u);
        QVERIFY(s.guestGrab(3, {kText}));
        QCOMPARE(s.guestText(3, "host"), std::nullopt);
        /* Wayland telling the same host clipboard again at focus */
        QCOMPARE(s.hostChanged("host", false, &serial), ClipboardState::HostAction::None);
        QVERIFY(s.guestOwns());
        /* but again to an agent that just connected */
        s.reset();
        QCOMPARE(s.hostChanged("host", true, &serial), ClipboardState::HostAction::Grab);
        QCOMPARE(serial, 0u);
    }

    void outdatedGuestText()
    {
        ClipboardState s;
        uint32_t serial = 0;
        QVERIFY(s.guestGrab(4, {kText}));
        /* a newer guest grab while its text was on its way */
        QVERIFY(s.guestGrab(5, {kText}));
        QCOMPARE(s.guestText(4, "old"), std::nullopt);
        QCOMPARE(s.guestText(5, "new"), std::optional<QString>("new"));
        /* the host copied meanwhile */
        QCOMPARE(s.hostChanged("host", false, &serial), ClipboardState::HostAction::Grab);
        QCOMPARE(s.guestText(5, "new"), std::nullopt);
        /* guest grabs without text are not fetched */
        QVERIFY(!s.guestGrab(9, {"image/png"}));
        QVERIFY(s.guestOwns());
        QCOMPARE(s.guestSerial(), std::nullopt);
        /* released: the host clipboard keeps its text */
        s.guestRelease();
        QVERIFY(!s.guestOwns());
        QCOMPARE(s.hostText(), QString("host"));
    }

    void hostWithoutText()
    {
        ClipboardState s;
        uint32_t serial = 0;
        QCOMPARE(s.hostChanged("", false, &serial), ClipboardState::HostAction::None);
        QCOMPARE(s.hostChanged("a", false, &serial), ClipboardState::HostAction::Grab);
        QCOMPARE(s.hostChanged("", false, &serial), ClipboardState::HostAction::Release);
        QVERIFY(!s.hostOwns());
        QCOMPARE(s.hostChanged("", false, &serial), ClipboardState::HostAction::None);
        /* the same text after an image is new again */
        QCOMPARE(s.hostChanged("a", false, &serial), ClipboardState::HostAction::Grab);
    }

    void request()
    {
        ClipboardState s;
        uint32_t serial = 0;
        QString mime;
        QByteArray data;
        s.hostChanged(QString::fromUtf8("ça ✓"), false, &serial);
        QVERIFY(s.request({kText}, &mime, &data));
        QCOMPARE(mime, QString(kText));
        QCOMPARE(data, QString::fromUtf8("ça ✓").toUtf8());
        QVERIFY(s.request({"image/png", "text/plain"}, &mime, &data));
        QCOMPARE(mime, QString("text/plain"));
        QVERIFY(!s.request({"image/png"}, &mime, &data));
    }

    /* Against QEMU's side: registration, then the host's text offered */
    void registers()
    {
        clipboard()->setMimeData(textData("before"));
        FakeQemu qemu;
        auto bridge = started(qemu);
        QVERIFY(bridge);
        QVERIFY(bridge->isRegistered());
        const QStringList calls = qemu.calls();
        QVERIFY2(!calls.filter("GetAll failed").size(), qPrintable(calls.join('\n')));
        QCOMPARE(qemu.callsTo("Register").size(), 1);
        /* offered at registration, and again at QEMU's reset */
        QTRY_VERIFY(!qemu.callsTo("Grab").isEmpty());
        QVERIFY(qemu.callsTo("Grab").first().startsWith("Grab (0, 0, ['text/plain;charset=utf-8'"));
        QString reply;
        qemu.toClient("Request", g_variant_new("(u^as)", 0, kTexts), &reply);
        QCOMPARE(replyText(reply), QString("text/plain;charset=utf-8: before"));
        bridge.reset();
        QTRY_COMPARE(qemu.callsTo("Unregister").size(), 1);
    }

    void hostToGuest()
    {
        FakeQemu qemu;
        auto bridge = started(qemu);
        QVERIFY(bridge);
        QVERIFY(qemu.callsTo("Grab").isEmpty());    // nothing in the host clipboard
        const QString text = QString::fromUtf8("Grüße, 東京 ✓");
        for (int i = 0; i < 3; i++) {
            const QString copy = text + QString::number(i);
            clipboard()->setMimeData(textData(copy));
            QTRY_COMPARE(qemu.callsTo("Grab").size(), i + 1);
            QVERIFY(qemu.callsTo("Grab").last().startsWith(QString("Grab (0, %1,").arg(i)));
            QString reply;
            qemu.toClient("Request", g_variant_new("(u^as)", 0, kTexts),
                          &reply);
            QCOMPARE(replyText(reply), QString(kText) + ": " + copy);
        }
        /* the same text again (as Wayland tells it at each focus): no grab */
        clipboard()->setMimeData(textData(text + "2"));
        QTest::qWait(100);
        QCOMPARE(qemu.callsTo("Grab").size(), 3);
        /* an image: the guest no longer gets the text */
        auto *image = new QMimeData;
        image->setImageData(QImage(4, 4, QImage::Format_RGB32));
        clipboard()->setMimeData(image);
        QTRY_COMPARE(qemu.callsTo("Release"), QStringList({"Release (0,)"}));
        /* another selection than the clipboard is not shared */
        QString reply;
        qemu.toClient("Request", g_variant_new("(u^as)", 1, kTexts), &reply);
        QVERIFY(reply.startsWith("error:"));
    }

    void guestToHost()
    {
        FakeQemu qemu;
        auto bridge = started(qemu);
        QVERIFY(bridge);
        QSignalSpy set(bridge.get(), &VmClipboard::hostClipboardSet);
        for (uint32_t i = 0; i < 3; i++) {
            const QString text = QString::fromUtf8("copié dans l'invité ✓ %1").arg(i);
            /* spice-vdagent's text may end with a NUL */
            qemu.setGuestData(text.toUtf8() + QByteArray(1, '\0'));
            qemu.toClient("Grab", g_variant_new("(uu^as)", 0, i, kTexts));
            QTRY_COMPARE(clipboard()->text(), text);
            QCOMPARE(qemu.callsTo("Request").size(), int(i) + 1);
        }
        QCOMPARE(set.size(), 3);
        /* not offered back to the guest */
        QTest::qWait(100);
        QVERIFY(qemu.callsTo("Grab").isEmpty());
        /* a host copy now carries the serial after the guest's last grab */
        clipboard()->setMimeData(textData("host"));
        QTRY_COMPARE(qemu.callsTo("Grab").size(), 1);
        QVERIFY(qemu.callsTo("Grab").first().startsWith("Grab (0, 3,"));
        /* a guest grab that lost the race to it is not fetched */
        qemu.toClient("Grab", g_variant_new("(uu^as)", 0, 3u, kTexts));
        QTest::qWait(100);
        QCOMPARE(qemu.callsTo("Request").size(), 3);
        QCOMPARE(clipboard()->text(), QString("host"));
        /* the primary selection is left alone */
        qemu.toClient("Grab", g_variant_new("(uu^as)", 1, 10u, kTexts));
        QTest::qWait(100);
        QCOMPARE(qemu.callsTo("Request").size(), 3);
        /* QEMU resets the serials (the guest's agent connected): offered again from 0 */
        qemu.toClient("Register", nullptr);
        QTRY_COMPARE(qemu.callsTo("Grab").size(), 2);
        QVERIFY(qemu.callsTo("Grab").last().startsWith("Grab (0, 0,"));
        /* the guest's text same as the host's: the host clipboard is left as it is */
        qemu.setGuestData("host");
        qemu.toClient("Grab", g_variant_new("(uu^as)", 0, 1u, kTexts));
        QTRY_COMPARE(qemu.callsTo("Request").size(), 4);
        QTest::qWait(100);
        QCOMPARE(set.size(), 3);
    }

    /* Guest grabs while the text of one is on its way: one Request at a time,
       the newest text wins */
    void grabsWhileFetching()
    {
        FakeQemu qemu;
        auto bridge = started(qemu);
        QVERIFY(bridge);
        qemu.setGuestData("first", true);
        qemu.toClient("Grab", g_variant_new("(uu^as)", 0, 0u, kTexts));
        QTRY_VERIFY(qemu.holding());
        qemu.toClient("Grab", g_variant_new("(uu^as)", 0, 1u, kTexts));
        QTest::qWait(100);
        QCOMPARE(qemu.callsTo("Request").size(), 1);
        qemu.setGuestData("second");
        qemu.letGo();
        QTRY_COMPARE(clipboard()->text(), QString("second"));
        QCOMPARE(qemu.callsTo("Request").size(), 2);
    }

    /* The view goes while QEMU still has to answer its Request */
    void goneWhileFetching()
    {
        FakeQemu qemu;
        auto bridge = started(qemu);
        QVERIFY(bridge);
        qemu.setGuestData("late", true);
        qemu.toClient("Grab", g_variant_new("(uu^as)", 0, 0u, kTexts));
        QTRY_VERIFY(qemu.holding());
        bridge.reset();
        qemu.letGo();
        QTest::qWait(200);
        QVERIFY(clipboard()->text() != "late");
        /* and QEMU's calls get an error from then on */
        QString reply;
        qemu.toClient("Request", g_variant_new("(u^as)", 0, kTexts), &reply);
        QVERIFY(reply.startsWith("error:"));
    }

    /*
     * The compositor refuses the guest's text (KWin: an input serial older
     * than the clipboard's): the host's text comes back at once.  It is not
     * offered to the guest, and the guest's is set again at the next input.
     */
    void refusedByCompositor()
    {
        clipboard()->setMimeData(textData("host before"));
        FakeQemu qemu;
        auto bridge = started(qemu);
        QVERIFY(bridge);
        QTRY_COMPARE(qemu.callsTo("Grab").size(), 1);   // "host before" offered
        QSignalSpy set(bridge.get(), &VmClipboard::hostClipboardSet);
        qemu.setGuestData("guest text");
        qemu.toClient("Grab", g_variant_new("(uu^as)", 0, 5u, kTexts));
        QTRY_COMPARE(set.size(), 1);
        /* what Qt reports when the compositor cancels the selection */
        clipboard()->setMimeData(textData("host before"));
        QTest::qWait(100);
        QCOMPARE(qemu.callsTo("Grab").size(), 1);
        QCOMPARE(clipboard()->text(), QString("host before"));
        /* input in a window of vitrine: set again */
        QWindow window;
        QKeyEvent press(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, "a");
        QCoreApplication::sendEvent(&window, &press);
        QTRY_COMPARE(clipboard()->text(), QString("guest text"));
        QCOMPARE(set.size(), 2);
        /* another text coming then is a host copy: offered, nothing waits */
        qemu.toClient("Grab", g_variant_new("(uu^as)", 0, 6u, kTexts));
        QTRY_COMPARE(set.size(), 2);
        qemu.setGuestData("guest again");
        qemu.toClient("Grab", g_variant_new("(uu^as)", 0, 7u, kTexts));
        QTRY_COMPARE(set.size(), 3);
        clipboard()->setMimeData(textData("new host text"));
        QTRY_COMPARE(qemu.callsTo("Grab").size(), 2);
        QVERIFY(qemu.callsTo("Grab").last().startsWith("Grab (0, 8,"));
        QCoreApplication::sendEvent(&window, &press);
        QTest::qWait(100);
        QCOMPARE(clipboard()->text(), QString("new host text"));
    }

    /* Two VMs: what one guest copies goes to the other through the host clipboard */
    void twoGuests()
    {
        FakeQemu qemu1, qemu2;
        auto bridge1 = started(qemu1);
        auto bridge2 = started(qemu2);
        QVERIFY(bridge1 && bridge2);
        qemu1.setGuestData("from vm1");
        qemu1.toClient("Grab", g_variant_new("(uu^as)", 0, 0u, kTexts));
        QTRY_COMPARE(clipboard()->text(), QString("from vm1"));
        QTRY_COMPARE(qemu2.callsTo("Grab").size(), 1);
        QTest::qWait(100);
        QVERIFY(qemu1.callsTo("Grab").isEmpty());
        QString reply;
        qemu2.toClient("Request", g_variant_new("(u^as)", 0, kTexts), &reply);
        QCOMPARE(replyText(reply), QString("text/plain;charset=utf-8: from vm1"));
    }
};

int main(int argc, char **argv)
{
    /* the process's own clipboard, no window on anyone's screen */
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qunsetenv("DISPLAY");
    qunsetenv("WAYLAND_DISPLAY");
    QApplication app(argc, argv);
    TestVmClipboard test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}

#include "test_vmclipboard.moc"
