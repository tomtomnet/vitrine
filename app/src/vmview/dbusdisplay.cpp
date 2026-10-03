#include "dbusdisplay.h"
#include "common.h"
#include "stats.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <unistd.h>

namespace {
constexpr const char *kRoot = "/org/qemu/Display1";

struct CallData {
    Stats *stats;
    int64_t start;
    const char *method;
};

void callDone(GObject *source, GAsyncResult *res, gpointer user)
{
    auto *d = static_cast<CallData *>(user);
    GError *err = nullptr;
    GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
    if (ret) {
        if (d->stats) {
            d->stats->inputRoundTrip(nowNs() - d->start);
        }
        g_variant_unref(ret);
    } else {
        // Out-of-range positions and mouse-mode races are expected; keep quiet.
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_debug("%s failed: %s", d->method, err->message);
        }
        g_error_free(err);
    }
    delete d;
}
} // namespace

DBusDisplay::DBusDisplay(Stats *stats, QObject *parent) : QObject(parent), m_stats(stats) {}

DBusDisplay::~DBusDisplay()
{
    if (m_conn) {
        if (m_signalId) {
            g_dbus_connection_signal_unsubscribe(m_conn, m_signalId);
        }
        g_dbus_connection_close_sync(m_conn, nullptr, nullptr);
        g_object_unref(m_conn);
    }
}

bool DBusDisplay::connectPeer(int fd, QString *err)
{
    GError *gerr = nullptr;
    GSocket *socket = g_socket_new_from_fd(fd, &gerr);
    if (!socket) {
        *err = QString::fromUtf8(gerr->message);
        g_error_free(gerr);
        close(fd);
        return false;
    }
    GSocketConnection *sconn = g_socket_connection_factory_create_connection(socket);
    g_object_unref(socket);
    // QEMU is the authentication server of this p2p connection.
    m_conn = g_dbus_connection_new_sync(G_IO_STREAM(sconn), nullptr,
                                        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
                                        nullptr, nullptr, &gerr);
    g_object_unref(sconn);
    if (!m_conn) {
        *err = QStringLiteral("D-Bus peer: %1").arg(QString::fromUtf8(gerr->message));
        g_error_free(gerr);
        return false;
    }
    return true;
}

bool DBusDisplay::selectConsole(QString *err)
{
    GError *gerr = nullptr;
    QByteArray vm = QByteArray(kRoot) + "/VM";
    GVariant *ret = g_dbus_connection_call_sync(
        m_conn, nullptr, vm.constData(), "org.freedesktop.DBus.Properties", "GetAll",
        g_variant_new("(s)", "org.qemu.Display1.VM"), G_VARIANT_TYPE("(a{sv})"),
        G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &gerr);
    if (!ret) {
        *err = QStringLiteral("VM properties: %1").arg(QString::fromUtf8(gerr->message));
        g_error_free(gerr);
        return false;
    }
    GVariant *dict = g_variant_get_child_value(ret, 0);
    const char *name = nullptr;
    if (g_variant_lookup(dict, "Name", "&s", &name) && name) {
        m_name = QString::fromUtf8(name);
    }
    GVariant *ids = g_variant_lookup_value(dict, "ConsoleIDs", G_VARIANT_TYPE("au"));
    gsize n = 0;
    const guint32 *idv = ids ? static_cast<const guint32 *>(
                                   g_variant_get_fixed_array(ids, &n, sizeof(guint32)))
                             : nullptr;
    for (gsize i = 0; i < n && m_path.isEmpty(); i++) {
        QByteArray path = QByteArray(kRoot) + "/Console_" + QByteArray::number(idv[i]);
        GVariant *t = g_dbus_connection_call_sync(
            m_conn, nullptr, path.constData(), "org.freedesktop.DBus.Properties", "Get",
            g_variant_new("(ss)", "org.qemu.Display1.Console", "Type"),
            G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr);
        if (t) {
            GVariant *v = nullptr;
            g_variant_get(t, "(v)", &v);
            if (g_strcmp0(g_variant_get_string(v, nullptr), "Graphic") == 0) {
                m_path = QString::fromLatin1(path);
            }
            g_variant_unref(v);
            g_variant_unref(t);
        }
    }
    if (ids) {
        g_variant_unref(ids);
    }
    g_variant_unref(dict);
    g_variant_unref(ret);
    if (m_path.isEmpty()) {
        *err = QStringLiteral("no graphic console");
        return false;
    }

    QByteArray p = m_path.toLatin1();
    m_signalId = g_dbus_connection_signal_subscribe(
        m_conn, nullptr, "org.freedesktop.DBus.Properties", "PropertiesChanged",
        p.constData(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE, propertiesChanged, this, nullptr);
    readMouseMode();
    return true;
}

void DBusDisplay::readMouseMode()
{
    QByteArray p = m_path.toLatin1();
    GVariant *t = g_dbus_connection_call_sync(
        m_conn, nullptr, p.constData(), "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", "org.qemu.Display1.Mouse", "IsAbsolute"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr);
    if (t) {
        GVariant *v = nullptr;
        g_variant_get(t, "(v)", &v);
        bool abs = g_variant_get_boolean(v);
        g_variant_unref(v);
        g_variant_unref(t);
        if (abs != m_absolute) {
            m_absolute = abs;
            Q_EMIT mouseModeChanged(abs);
        }
    }
}

void DBusDisplay::propertiesChanged(GDBusConnection *, const char *, const char *,
                                    const char *, const char *, GVariant *params, void *user)
{
    auto *self = static_cast<DBusDisplay *>(user);
    const char *iface = nullptr;
    GVariant *changed = nullptr;
    g_variant_get(params, "(&s@a{sv}@as)", &iface, &changed, nullptr);
    gboolean abs;
    if (g_strcmp0(iface, "org.qemu.Display1.Mouse") == 0 &&
        g_variant_lookup(changed, "IsAbsolute", "b", &abs)) {
        if (bool(abs) != self->m_absolute) {
            self->m_absolute = abs;
            Q_EMIT self->mouseModeChanged(abs);
        }
    }
    g_variant_unref(changed);
}

bool DBusDisplay::registerListener(int peerFd, QString *err)
{
    GUnixFDList *fds = g_unix_fd_list_new_from_array(&peerFd, 1); // takes ownership
    QByteArray p = m_path.toLatin1();
    // Asynchronous: QEMU answers, then runs the listener's authentication
    // with our listener thread, which must not be blocked by this call.
    g_dbus_connection_call_with_unix_fd_list(
        m_conn, nullptr, p.constData(), "org.qemu.Display1.Console", "RegisterListener",
        g_variant_new("(h)", 0), nullptr, G_DBUS_CALL_FLAGS_NONE, -1, fds, nullptr,
        [](GObject *src, GAsyncResult *res, gpointer) {
            GError *e = nullptr;
            GVariant *r = g_dbus_connection_call_with_unix_fd_list_finish(
                G_DBUS_CONNECTION(src), nullptr, res, &e);
            if (!r) {
                qWarning("RegisterListener failed: %s", e->message);
                g_error_free(e);
            } else {
                g_variant_unref(r);
            }
        },
        nullptr);
    g_object_unref(fds);
    Q_UNUSED(err);
    return true;
}

void DBusDisplay::call(const char *iface, const char *method, GVariant *args, bool measure)
{
    if (!m_conn || m_path.isEmpty()) {
        g_variant_unref(g_variant_ref_sink(args));
        return;
    }
    QByteArray p = m_path.toLatin1();
    auto *d = new CallData{measure ? m_stats : nullptr, nowNs(), method};
    g_dbus_connection_call(m_conn, nullptr, p.constData(), iface, method, args, nullptr,
                           G_DBUS_CALL_FLAGS_NONE, -1, nullptr, callDone, d);
}

void DBusDisplay::applyUiInfo(uint32_t width, uint32_t height, uint32_t refreshMilliHz,
                              uint16_t widthMm, uint16_t heightMm)
{
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&b, "{sv}", "width", g_variant_new_uint32(width));
    g_variant_builder_add(&b, "{sv}", "height", g_variant_new_uint32(height));
    if (widthMm && heightMm) {
        g_variant_builder_add(&b, "{sv}", "width_mm", g_variant_new_uint16(widthMm));
        g_variant_builder_add(&b, "{sv}", "height_mm", g_variant_new_uint16(heightMm));
    }
    if (refreshMilliHz) {
        g_variant_builder_add(&b, "{sv}", "refresh_rate", g_variant_new_uint32(refreshMilliHz));
    }
    QByteArray p = m_path.toLatin1();
    g_dbus_connection_call(
        m_conn, nullptr, p.constData(), "org.qemu.Display1.UIInfo", "Apply",
        g_variant_new("(a{sv})", &b), nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
        [](GObject *src, GAsyncResult *res, gpointer user) {
            GError *e = nullptr;
            GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
            auto *self = static_cast<DBusDisplay *>(user);
            if (r) {
                g_variant_unref(r);
                Q_EMIT self->uiInfoApplied(QStringLiteral("ok"));
            } else {
                Q_EMIT self->uiInfoApplied(QString::fromUtf8(e->message));
                g_error_free(e);
            }
        },
        this);
}

void DBusDisplay::keyPress(uint32_t qnum)
{
    call("org.qemu.Display1.Keyboard", "Press", g_variant_new("(u)", qnum), true);
}

void DBusDisplay::keyRelease(uint32_t qnum)
{
    call("org.qemu.Display1.Keyboard", "Release", g_variant_new("(u)", qnum), true);
}

void DBusDisplay::mouseAbs(uint32_t x, uint32_t y)
{
    call("org.qemu.Display1.Mouse", "SetAbsPosition", g_variant_new("(uu)", x, y), true);
}

void DBusDisplay::mouseRel(int dx, int dy)
{
    call("org.qemu.Display1.Mouse", "RelMotion", g_variant_new("(ii)", dx, dy), true);
}

void DBusDisplay::mouseButton(uint32_t button, bool down)
{
    call("org.qemu.Display1.Mouse", down ? "Press" : "Release", g_variant_new("(u)", button),
         true);
}
