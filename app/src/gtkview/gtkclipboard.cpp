// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtkclipboard.h"

#include <QList>
#include <QLoggingCategory>
#include <QStringList>

#include <gdk/gdk.h>
#include <gio/gio.h>

#include <cstring>
#include <memory>
#include <utility>
#include <vector>

/* QT_LOGGING_RULES=vitrine.clipboard.debug=true shows what goes where */
Q_LOGGING_CATEGORY(lcGtkClipboard, "vitrine.clipboard", QtWarningMsg)

namespace {

const char kPath[] = "/org/qemu/Display1/Clipboard";
const char kInterface[] = "org.qemu.Display1.Clipboard";
/* QEMU's own error (ui/dbus-error.c) */
const char kError[] = "org.qemu.Display1.Error.Failed";
/* The selection shared: Clipboard (Primary = 1, Secondary = 2) */
constexpr uint32_t kSelection = 0;
/* Longer than QEMU's own 5 s for a guest that does not answer */
constexpr int kTimeoutMs = 10000;

/* What the D-Bus client serves, from ui/dbus-display1.xml */
const char kXml[] = R"XML(
<node>
  <interface name="org.qemu.Display1.Clipboard">
    <method name="Register"/>
    <method name="Unregister"/>
    <method name="Grab">
      <arg type="u" name="selection"/>
      <arg type="u" name="serial"/>
      <arg type="as" name="mimes"/>
    </method>
    <method name="Release">
      <arg type="u" name="selection"/>
    </method>
    <method name="Request">
      <arg type="u" name="selection"/>
      <arg type="as" name="mimes"/>
      <arg type="s" name="reply_mime" direction="out"/>
      <arg type="ay" name="data" direction="out"/>
    </method>
    <property name="Interfaces" type="as" access="read"/>
  </interface>
</node>)XML";

QStringList fromStrv(const char *const *strv)
{
    QStringList list;
    for (; strv && *strv; strv++) {
        list << QString::fromUtf8(*strv);
    }
    return list;
}

/* A string array for g_variant_new("^as"), alive as long as the object */
struct Strv {
    explicit Strv(const QStringList &list)
    {
        for (const QString &s : list) {
            bytes << s.toUtf8();
        }
        for (const QByteArray &b : bytes) {
            ptrs.push_back(b.constData());
        }
        ptrs.push_back(nullptr);
    }
    const char *const *get() const { return ptrs.data(); }

    QList<QByteArray> bytes;
    std::vector<const char *> ptrs;
};

/* A call or a read whose completion may come after the object went */
struct Pending {
    std::weak_ptr<bool> alive;
    GtkVmClipboard *self;
    uintptr_t tag;
    void (*done)(GtkVmClipboard *, GVariant *, const QString &error, uintptr_t tag) = nullptr;
};

} // namespace

GtkVmClipboard::GtkVmClipboard(GDBusConnection *connection, GdkDisplay *display)
    : m_conn(G_DBUS_CONNECTION(g_object_ref(connection))),
      m_clipboard(GDK_CLIPBOARD(g_object_ref(gdk_display_get_clipboard(display)))),
      m_cancel(g_cancellable_new()), m_alive(std::make_shared<bool>(true))
{
}

GtkVmClipboard::~GtkVmClipboard()
{
    m_alive.reset();
    /* the host clipboard's reads still on their way end cancelled */
    g_cancellable_cancel(m_cancel);
    g_object_unref(m_cancel);
    if (m_changedId) {
        g_signal_handler_disconnect(m_clipboard, m_changedId);
    }
    if (m_ownContent) {
        g_object_unref(m_ownContent);
    }
    g_object_unref(m_clipboard);
    if (m_objectId) {
        /* QEMU's calls in flight are refused from here on */
        g_dbus_connection_unregister_object(m_conn, m_objectId);
    }
    if (m_registered && !g_dbus_connection_is_closed(m_conn)) {
        /* QEMU also unregisters the peer when the connection closes */
        g_dbus_connection_call(m_conn, nullptr, kPath, kInterface, "Unregister", nullptr,
                               nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
    }
    g_object_unref(m_conn);
}

bool GtkVmClipboard::start(QString *error)
{
    static GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(kXml, nullptr);
    static const GDBusInterfaceVTable vtable = {methodCall, getProperty, nullptr, {}};
    GError *err = nullptr;

    if (m_objectId) {
        return true;
    }
    /* first, for QEMU to find when it takes the Register: it reads the
       object's properties then, waiting for them with its main loop stopped */
    m_objectId = g_dbus_connection_register_object(m_conn, kPath, node->interfaces[0], &vtable,
                                                   this, nullptr, &err);
    if (!m_objectId) {
        *error = QStringLiteral("Clipboard: %1").arg(QString::fromUtf8(err->message));
        g_error_free(err);
        return false;
    }
    m_changedId = g_signal_connect_swapped(m_clipboard, "changed", G_CALLBACK(+[](gpointer self) {
        static_cast<GtkVmClipboard *>(self)->hostClipboardChanged();
    }), this);
    /* what it holds now: offered once registered */
    readHost();
    callRegister(true);
    return true;
}

void GtkVmClipboard::callRegister(bool retry)
{
    call("Register", nullptr, nullptr,
         [](GtkVmClipboard *self, GVariant *, const QString &error, uintptr_t retry) {
        if (error.isEmpty()) {
            qCDebug(lcGtkClipboard) << "registered";
            self->m_registered = true;
            /* the host's text for the guest, as when its agent connects */
            self->announce(true);
            if (self->registered) {
                self->registered();
            }
        } else if (retry && error.contains(QLatin1String("already registered"))) {
            /* a view of this VM before this one, whose connection QEMU has
               not seen close yet (src/vmview/vmclipboard.cpp) */
            qCDebug(lcGtkClipboard) << "taking over from a previous view";
            self->call("Unregister", nullptr, nullptr, nullptr);
            self->callRegister(false);
        } else {
            qWarning("vitrine: clipboard: %s", qPrintable(error));
        }
    }, retry);
}

void GtkVmClipboard::hostClipboardChanged()
{
    /* ours: the guest's text, put there by setHostClipboard() - not what
       another part of vitrine copied, which is the host's text as well */
    if (m_ownContent && gdk_clipboard_get_content(m_clipboard) == m_ownContent) {
        return;
    }
    readHost();
}

/* The host clipboard's text, asynchronously: GTK has no other way */
void GtkVmClipboard::readHost()
{
    auto *pending = new Pending{m_alive, this, uintptr_t(++m_readGeneration), nullptr};
    gdk_clipboard_read_text_async(m_clipboard, m_cancel,
                                  [](GObject *source, GAsyncResult *res, gpointer data) {
        std::unique_ptr<Pending> pending(static_cast<Pending *>(data));
        GError *err = nullptr;
        char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), res, &err);
        /* no text in it (an image, files) reads as none */
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED) && !pending->alive.expired()) {
            pending->self->hostRead(pending->tag, text ? QString::fromUtf8(text) : QString());
        }
        g_free(text);
        g_clear_error(&err);
    }, pending);
}

void GtkVmClipboard::hostRead(uint64_t generation, const QString &text)
{
    if (generation != m_readGeneration) {
        return;     // a newer change is being read
    }
    /* the guest's text gone at once, the host's back: refused (see the header) */
    const bool refused = m_attempt && m_attemptTime.elapsed() < kRefusalMs && text == m_hostBefore;
    if (refused) {
        qCDebug(lcGtkClipboard) << "guest text refused by the compositor: again at the next input";
        m_state.hostSeen(m_hostBefore);
        m_pending = *std::exchange(m_attempt, std::nullopt);
        return;
    }
    m_attempt.reset();
    if (!m_registered) {
        m_state.hostSeen(text);
        return;
    }
    uint32_t serial = 0;
    switch (m_state.hostChanged(text, false, &serial)) {
    case ClipboardState::HostAction::Grab: {
        /* newer than the guest's text the compositor refused */
        m_pending.reset();
        qCDebug(lcGtkClipboard) << "host text offered, serial" << serial << "length" << text.size();
        const Strv mimes(ClipboardMime::textMimes());
        call("Grab", g_variant_new("(uu^as)", kSelection, serial, mimes.get()), nullptr, nullptr);
        break;
    }
    case ClipboardState::HostAction::Release:
        qCDebug(lcGtkClipboard) << "host clipboard without text: released";
        call("Release", g_variant_new("(u)", kSelection), nullptr, nullptr);
        break;
    case ClipboardState::HostAction::None:
        break;
    }
}

void GtkVmClipboard::announce(bool again)
{
    uint32_t serial = 0;
    const QString text = m_state.hostText();

    if (m_state.hostChanged(text, again, &serial) == ClipboardState::HostAction::Grab) {
        qCDebug(lcGtkClipboard) << "host text offered again, serial" << serial;
        const Strv mimes(ClipboardMime::textMimes());
        call("Grab", g_variant_new("(uu^as)", kSelection, serial, mimes.get()), nullptr, nullptr);
    }
}

void GtkVmClipboard::fetch()
{
    const std::optional<uint32_t> serial = m_state.guestSerial();

    if (!serial) {
        return;
    }
    if (m_fetching) {
        m_fetchAgain = true;
        return;
    }
    m_fetching = true;
    m_fetchAgain = false;
    const Strv mimes({QLatin1String(ClipboardMime::kText)});
    call("Request", g_variant_new("(u^as)", kSelection, mimes.get()), G_VARIANT_TYPE("(say)"),
         [](GtkVmClipboard *self, GVariant *reply, const QString &error, uintptr_t serial) {
        self->m_fetching = false;
        if (reply) {
            self->fetched(uint32_t(serial), reply);
        } else {
            /* e.g. the guest's agent did not answer in time */
            qCDebug(lcGtkClipboard) << "guest text not fetched:" << error;
        }
        if (self->m_fetchAgain) {
            self->fetch();
        }
    }, *serial);
}

void GtkVmClipboard::fetched(uint32_t serial, GVariant *reply)
{
    const char *mime = nullptr;
    GVariant *bytes = nullptr;
    gsize size = 0;

    g_variant_get(reply, "(&s@ay)", &mime, &bytes);
    const auto *data = static_cast<const char *>(g_variant_get_fixed_array(bytes, &size, 1));
    QByteArray raw(data, qsizetype(size));
    g_variant_unref(bytes);
    while (raw.endsWith('\0')) {
        raw.chop(1);
    }
    if (!ClipboardMime::isText(QString::fromUtf8(mime))) {
        qCDebug(lcGtkClipboard) << "guest data of type" << mime << "left out";
        return;
    }
    const std::optional<QString> text = m_state.guestText(serial, QString::fromUtf8(raw));
    if (!text) {
        qCDebug(lcGtkClipboard) << "guest text of serial" << serial << "unchanged or outdated";
        return;
    }
    setHostClipboard(*text);
}

void GtkVmClipboard::setHostClipboard(const QString &text, bool again)
{
    /* once more at vitrine's next input, which KWin takes when it refused
       this one: GTK does not tell (it keeps the clipboard as its own) */
    if (again) {
        m_pending.reset();
    } else {
        m_pending = text;
    }
    m_attempt = text;
    m_hostBefore = m_state.hostText();
    m_attemptTime.start();
    m_state.hostSeen(text);
    qCDebug(lcGtkClipboard) << "guest text in the host clipboard, length" << text.size();
    /* ours from here: its own change is not read back (hostClipboardChanged()) */
    if (m_ownContent) {
        g_object_unref(std::exchange(m_ownContent, nullptr));
    }
    GdkContentProvider *content = gdk_content_provider_new_typed(
        G_TYPE_STRING, text.toUtf8().constData());
    m_ownContent = GDK_CONTENT_PROVIDER(g_object_ref(content));
    gdk_clipboard_set_content(m_clipboard, content);
    g_object_unref(content);
    if (hostClipboardSet) {
        hostClipboardSet(text);
    }
}

void GtkVmClipboard::inputSeen()
{
    if (!m_pending) {
        return;
    }
    /* after it: GDK takes the input's serial as it hands it to GTK */
    const std::weak_ptr<bool> alive = m_alive;
    auto *pending = new Pending{alive, this, 0, nullptr};
    g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer data) -> gboolean {
        auto *p = static_cast<Pending *>(data);
        if (!p->alive.expired() && p->self->m_pending) {
            p->self->setHostClipboard(*p->self->m_pending, true);
        }
        return G_SOURCE_REMOVE;
    }, pending, [](gpointer data) { delete static_cast<Pending *>(data); });
}

void GtkVmClipboard::call(const char *method, GVariant *args, const GVariantType *replyType,
                          Done done, uintptr_t tag)
{
    g_dbus_connection_call(
        m_conn, nullptr, kPath, kInterface, method, args, replyType, G_DBUS_CALL_FLAGS_NONE,
        kTimeoutMs, nullptr,
        [](GObject *source, GAsyncResult *res, gpointer user) {
            std::unique_ptr<Pending> pending(static_cast<Pending *>(user));
            GError *err = nullptr;
            GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
            const QString error = err ? QString::fromUtf8(err->message) : QString();

            /* the view may have gone meanwhile */
            if (!pending->alive.expired() && pending->done) {
                pending->done(pending->self, reply, error, pending->tag);
            } else if (err && !pending->alive.expired()) {
                qCDebug(lcGtkClipboard) << "call failed:" << error;
            }
            if (reply) {
                g_variant_unref(reply);
            }
            if (err) {
                g_error_free(err);
            }
        },
        new Pending{m_alive, this, tag, done});
}

void GtkVmClipboard::methodCall(GDBusConnection *, const char *, const char *, const char *,
                                const char *method, GVariant *params,
                                GDBusMethodInvocation *invocation, void *user)
{
    auto *self = static_cast<GtkVmClipboard *>(user);
    ClipboardState &state = self->m_state;

    if (!strcmp(method, "Register")) {
        /* QEMU reset the serials: at our Register, and when the guest's
           agent connects (which then needs the host's text again) */
        qCDebug(lcGtkClipboard) << "serials reset";
        state.reset();
        g_dbus_method_invocation_return_value(invocation, nullptr);
        if (self->m_registered) {
            self->announce(true);
        }
    } else if (!strcmp(method, "Unregister")) {
        g_dbus_method_invocation_return_value(invocation, nullptr);
    } else if (!strcmp(method, "Grab")) {
        guint32 selection = 0, serial = 0;
        const char **mimes = nullptr;

        g_variant_get(params, "(uu^a&s)", &selection, &serial, &mimes);
        const QStringList list = fromStrv(mimes);
        g_free(mimes);
        g_dbus_method_invocation_return_value(invocation, nullptr);
        if (selection != kSelection) {
            return;
        }
        if (state.guestGrab(serial, list)) {
            qCDebug(lcGtkClipboard) << "guest grab, serial" << serial << list;
            self->fetch();
        } else {
            qCDebug(lcGtkClipboard) << "guest grab dropped, serial" << serial << "next"
                                    << state.nextSerial() << list;
        }
    } else if (!strcmp(method, "Release")) {
        guint32 selection = 0;

        g_variant_get(params, "(u)", &selection);
        if (selection == kSelection) {
            qCDebug(lcGtkClipboard) << "guest released";
            state.guestRelease();
        }
        g_dbus_method_invocation_return_value(invocation, nullptr);
    } else if (!strcmp(method, "Request")) {
        guint32 selection = 0;
        const char **mimes = nullptr;
        QString mime;
        QByteArray data;

        g_variant_get(params, "(u^a&s)", &selection, &mimes);
        const QStringList list = fromStrv(mimes);
        g_free(mimes);
        if (selection != kSelection) {
            g_dbus_method_invocation_return_dbus_error(invocation, kError,
                                                       "Only the clipboard selection is shared");
        } else if (!state.request(list, &mime, &data)) {
            g_dbus_method_invocation_return_dbus_error(invocation, kError,
                                                       "Unhandled MIME types requested");
        } else {
            qCDebug(lcGtkClipboard) << "host text pasted in the guest, length" << data.size();
            GVariant *bytes = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, data.constData(),
                                                        gsize(data.size()), 1);
            g_dbus_method_invocation_return_value(
                invocation, g_variant_new("(s@ay)", mime.toUtf8().constData(), bytes));
        }
    } else {
        g_dbus_method_invocation_return_dbus_error(
            invocation, "org.freedesktop.DBus.Error.UnknownMethod", method);
    }
}

GVariant *GtkVmClipboard::getProperty(GDBusConnection *, const char *, const char *, const char *,
                                      const char *property, GError **, void *)
{
    if (!strcmp(property, "Interfaces")) {
        /* no interfaces beyond this one */
        return g_variant_new_strv(nullptr, 0);
    }
    return nullptr;
}
