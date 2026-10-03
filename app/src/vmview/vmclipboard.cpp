// SPDX-License-Identifier: GPL-2.0-or-later
#include "vmclipboard.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QMimeData>

#include <cstring>
#include <memory>
#include <utility>
#include <vector>
#include <gio/gio.h>

/* QT_LOGGING_RULES=vitrine.clipboard.debug=true shows what goes where */
Q_LOGGING_CATEGORY(lcClipboard, "vitrine.clipboard", QtWarningMsg)

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

/* "text/plain; charset=UTF-8" -> "text/plain;charset=utf-8" */
QString normalized(const QString &mime)
{
    QString m = mime.toLower();
    m.remove(QLatin1Char(' '));
    return m;
}

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

} // namespace

QStringList ClipboardMime::textMimes()
{
    return {QLatin1String(kText), QStringLiteral("text/plain"), QStringLiteral("UTF8_STRING")};
}

bool ClipboardMime::isText(const QString &mime)
{
    const QString m = normalized(mime);
    return m == QLatin1String(kText) || m == QLatin1String("text/plain") ||
           m == QLatin1String("utf8_string");
}

QString ClipboardMime::pickText(const QStringList &mimes)
{
    for (const QString &mime : mimes) {
        if (isText(mime)) {
            return mime;
        }
    }
    return QString();
}

QString ClipboardMime::textOf(const QMimeData *data)
{
    QString text;

    if (!data) {
        return text;
    }
    /* Qt's own reading first: it knows the platform's names for text */
    if (data->hasText()) {
        text = data->text();
    }
    if (text.isEmpty()) {
        const QString mime = pickText(data->formats());
        if (!mime.isEmpty()) {
            text = QString::fromUtf8(data->data(mime));
        }
    }
    /* some X11 clients count the terminating NUL */
    while (text.endsWith(QChar(0))) {
        text.chop(1);
    }
    return text;
}

void ClipboardState::reset()
{
    m_next = 0;
}

ClipboardState::HostAction ClipboardState::hostChanged(const QString &text, bool again,
                                                       uint32_t *serial)
{
    if (text.isEmpty()) {
        /* no text now, e.g. an image: what the guest was offered is gone */
        m_hostText.clear();
        if (m_owner == Owner::Host) {
            m_owner = Owner::None;
            m_offered.clear();
            return HostAction::Release;
        }
        return HostAction::None;
    }
    /*
     * The same text again: Wayland tells the clipboard each time vitrine
     * gets the keyboard, the guest's own text comes back from the host
     * clipboard once put there, and Plasma's clipboard history may put it
     * there again.  Offering it would take the clipboard from the guest.
     */
    if (!again && text == m_hostText) {
        return HostAction::None;
    }
    m_hostText = text;
    m_offered = text;
    m_owner = Owner::Host;
    *serial = m_next++;
    return HostAction::Grab;
}

void ClipboardState::hostSeen(const QString &text)
{
    m_hostText = text;
}

bool ClipboardState::guestGrab(uint32_t serial, const QStringList &mimes)
{
    /* below the next one: our grab of the same serial won the race (the
       guest drops its own then too), or one we saw already */
    if (serial < m_next) {
        return false;
    }
    m_next = serial + 1;
    m_owner = Owner::Guest;
    m_guestSerial = serial;
    m_guestText = !ClipboardMime::pickText(mimes).isEmpty();
    m_offered.clear();
    return m_guestText;
}

void ClipboardState::guestRelease()
{
    /* the host clipboard keeps the text it got */
    if (m_owner == Owner::Guest) {
        m_owner = Owner::None;
    }
}

std::optional<QString> ClipboardState::guestText(uint32_t serial, const QString &text)
{
    if (m_owner != Owner::Guest || serial != m_guestSerial || text.isEmpty() ||
        text == m_hostText) {
        return std::nullopt;
    }
    return text;
}

bool ClipboardState::request(const QStringList &mimes, QString *mime, QByteArray *data) const
{
    const QString text = ClipboardMime::pickText(mimes);

    if (text.isEmpty()) {
        return false;
    }
    *mime = text;
    *data = m_offered.toUtf8();
    return true;
}

std::optional<uint32_t> ClipboardState::guestSerial() const
{
    if (m_owner == Owner::Guest && m_guestText) {
        return m_guestSerial;
    }
    return std::nullopt;
}

VmClipboard::VmClipboard(GDBusConnection *connection, QObject *parent)
    : QObject(parent), m_conn(G_DBUS_CONNECTION(g_object_ref(connection)))
{
}

VmClipboard::~VmClipboard()
{
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

bool VmClipboard::start(QString *error)
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
        *error = tr("Clipboard: %1").arg(QString::fromUtf8(err->message));
        g_error_free(err);
        return false;
    }
    QClipboard *clipboard = QGuiApplication::clipboard();
    connect(clipboard, &QClipboard::dataChanged, this, &VmClipboard::hostClipboardChanged);
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this,
            &VmClipboard::applicationStateChanged);
    m_state.hostSeen(ClipboardMime::textOf(clipboard->mimeData(QClipboard::Clipboard)));
    callRegister(true);
    return true;
}

void VmClipboard::callRegister(bool retry)
{
    call("Register", nullptr, nullptr,
         [](VmClipboard *self, GVariant *, const QString &error, uintptr_t retry) {
        if (error.isEmpty()) {
            qCDebug(lcClipboard) << "registered";
            self->m_registered = true;
            /* the host's text for the guest, as when its agent connects */
            self->announce(true);
            Q_EMIT self->registered();
        } else if (retry && error.contains(QLatin1String("already registered"))) {
            /*
             * A view of this VM before this one, whose connection QEMU
             * has not seen close yet: on a peer-to-peer connection any
             * caller may unregister it (QEMU compares the callers' bus
             * names, which there are none of).
             */
            qCDebug(lcClipboard) << "taking over from a previous view";
            self->call("Unregister", nullptr, nullptr, nullptr);
            self->callRegister(false);
        } else {
            qWarning("vitrine: clipboard: %s", qPrintable(error));
        }
    }, retry);
}

void VmClipboard::hostClipboardChanged()
{
    const QMimeData *data = QGuiApplication::clipboard()->mimeData(QClipboard::Clipboard);

    /* ours: the guest's text, put there by setHostClipboard() */
    if (data && data == m_ownData) {
        return;
    }
    const QString text = ClipboardMime::textOf(data);
    if (!m_registered) {
        m_state.hostSeen(text);
        return;
    }
    uint32_t serial = 0;
    switch (m_state.hostChanged(text, false, &serial)) {
    case ClipboardState::HostAction::Grab: {
        /* newer than what the guest copied while vitrine had not the keyboard */
        m_pending.reset();
        qCDebug(lcClipboard) << "host text offered, serial" << serial << "length" << text.size();
        const Strv mimes(ClipboardMime::textMimes());
        call("Grab", g_variant_new("(uu^as)", kSelection, serial, mimes.get()), nullptr, nullptr);
        break;
    }
    case ClipboardState::HostAction::Release:
        qCDebug(lcClipboard) << "host clipboard without text: released";
        call("Release", g_variant_new("(u)", kSelection), nullptr, nullptr);
        break;
    case ClipboardState::HostAction::None:
        break;
    }
}

void VmClipboard::announce(bool again)
{
    uint32_t serial = 0;
    const QString text = m_state.hostText();

    if (m_state.hostChanged(text, again, &serial) ==
        ClipboardState::HostAction::Grab) {
        qCDebug(lcClipboard) << "host text offered again, serial" << serial;
        const Strv mimes(ClipboardMime::textMimes());
        call("Grab", g_variant_new("(uu^as)", kSelection, serial, mimes.get()), nullptr, nullptr);
    }
}

void VmClipboard::fetch()
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
         [](VmClipboard *self, GVariant *reply, const QString &error, uintptr_t serial) {
        self->m_fetching = false;
        if (reply) {
            self->fetched(uint32_t(serial), reply);
        } else {
            /* e.g. the guest's agent did not answer in time */
            qCDebug(lcClipboard) << "guest text not fetched:" << error;
        }
        if (self->m_fetchAgain) {
            self->fetch();
        }
    }, *serial);
}

void VmClipboard::fetched(uint32_t serial, GVariant *reply)
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
        qCDebug(lcClipboard) << "guest data of type" << mime << "left out";
        return;
    }
    const std::optional<QString> text = m_state.guestText(serial, QString::fromUtf8(raw));
    if (!text) {
        qCDebug(lcClipboard) << "guest text of serial" << serial << "unchanged or outdated";
        return;
    }
    if (canSetHostClipboard()) {
        setHostClipboard(*text);
    } else {
        qCDebug(lcClipboard) << "guest text waits for the keyboard";
        m_pending = *text;
    }
}

void VmClipboard::setHostClipboard(const QString &text)
{
    auto *data = new QMimeData;

    data->setText(text);
    data->setData(QLatin1String(ClipboardMime::kText), text.toUtf8());
    m_ownData = data;
    m_state.hostSeen(text);
    qCDebug(lcClipboard) << "guest text in the host clipboard, length" << text.size();
    QGuiApplication::clipboard()->setMimeData(data, QClipboard::Clipboard);
    Q_EMIT hostClipboardSet(text);
}

/*
 * A Wayland compositor takes a new clipboard only from the client that has
 * the keyboard (KWin cancels it otherwise, and Qt does not say so)
 */
bool VmClipboard::canSetHostClipboard() const
{
    return !QGuiApplication::platformName().startsWith(QLatin1String("wayland")) ||
           QGuiApplication::applicationState() == Qt::ApplicationActive;
}

void VmClipboard::applicationStateChanged()
{
    if (m_pending && canSetHostClipboard()) {
        setHostClipboard(*std::exchange(m_pending, std::nullopt));
    }
}

namespace {
struct PendingCall {
    QPointer<VmClipboard> self;
    void (*done)(VmClipboard *, GVariant *, const QString &, uintptr_t);
    uintptr_t tag;
};
} // namespace

void VmClipboard::call(const char *method, GVariant *args, const GVariantType *replyType,
                       void (*done)(VmClipboard *, GVariant *, const QString &, uintptr_t),
                       uintptr_t tag)
{
    g_dbus_connection_call(
        m_conn, nullptr, kPath, kInterface, method, args, replyType, G_DBUS_CALL_FLAGS_NONE,
        kTimeoutMs, nullptr,
        [](GObject *source, GAsyncResult *res, gpointer user) {
            std::unique_ptr<PendingCall> pending(static_cast<PendingCall *>(user));
            GError *err = nullptr;
            GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
            const QString error = err ? QString::fromUtf8(err->message) : QString();

            /* the view may have gone meanwhile */
            if (pending->self && pending->done) {
                pending->done(pending->self, reply, error, pending->tag);
            } else if (err && pending->self) {
                qCDebug(lcClipboard) << "call failed:" << error;
            }
            if (reply) {
                g_variant_unref(reply);
            }
            if (err) {
                g_error_free(err);
            }
        },
        new PendingCall{this, done, tag});
}

void VmClipboard::methodCall(GDBusConnection *, const char *, const char *, const char *,
                             const char *method, GVariant *params,
                             GDBusMethodInvocation *invocation, void *user)
{
    auto *self = static_cast<VmClipboard *>(user);
    ClipboardState &state = self->m_state;

    if (!strcmp(method, "Register")) {
        /* QEMU reset the serials: at our Register, and when the guest's
           agent connects (which then needs the host's text again) */
        qCDebug(lcClipboard) << "serials reset";
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
            qCDebug(lcClipboard) << "guest grab, serial" << serial << list;
            self->fetch();
        } else {
            qCDebug(lcClipboard) << "guest grab dropped, serial" << serial << "next"
                                 << state.nextSerial() << list;
        }
    } else if (!strcmp(method, "Release")) {
        guint32 selection = 0;

        g_variant_get(params, "(u)", &selection);
        if (selection == kSelection) {
            qCDebug(lcClipboard) << "guest released";
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
            qCDebug(lcClipboard) << "host text pasted in the guest, length" << data.size();
            GVariant *bytes = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, data.constData(),
                                                        gsize(data.size()), 1);
            g_dbus_method_invocation_return_value(
                invocation, g_variant_new("(s@ay)", mime.toUtf8().constData(), bytes));
        }
    } else {
        g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.DBus.Error.UnknownMethod",
                                                   method);
    }
}

GVariant *VmClipboard::getProperty(GDBusConnection *, const char *, const char *, const char *,
                                   const char *property, GError **, void *)
{
    if (!strcmp(property, "Interfaces")) {
        /* no interfaces beyond this one */
        return g_variant_new_strv(nullptr, 0);
    }
    return nullptr;
}
