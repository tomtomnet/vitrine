#include "listener.h"

#include <sys/stat.h>
#include "stats.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <cstring>
#include <pthread.h>
#include <sys/socket.h>

namespace {
// The subset of ui/dbus-display1.xml that a Unix listener implements.
const char kIntrospection[] = R"XML(
<node>
  <interface name="org.qemu.Display1.Listener">
    <method name="Scanout">
      <arg type="u" name="width" direction="in"/><arg type="u" name="height" direction="in"/>
      <arg type="u" name="stride" direction="in"/><arg type="u" name="pixman_format" direction="in"/>
      <arg type="ay" name="data" direction="in"/>
    </method>
    <method name="Update">
      <arg type="i" name="x" direction="in"/><arg type="i" name="y" direction="in"/>
      <arg type="i" name="width" direction="in"/><arg type="i" name="height" direction="in"/>
      <arg type="u" name="stride" direction="in"/><arg type="u" name="pixman_format" direction="in"/>
      <arg type="ay" name="data" direction="in"/>
    </method>
    <method name="ScanoutDMABUF">
      <arg type="h" name="dmabuf" direction="in"/><arg type="u" name="width" direction="in"/>
      <arg type="u" name="height" direction="in"/><arg type="u" name="stride" direction="in"/>
      <arg type="u" name="fourcc" direction="in"/><arg type="t" name="modifier" direction="in"/>
      <arg type="b" name="y0_top" direction="in"/>
    </method>
    <method name="UpdateDMABUF">
      <arg type="i" name="x" direction="in"/><arg type="i" name="y" direction="in"/>
      <arg type="i" name="width" direction="in"/><arg type="i" name="height" direction="in"/>
    </method>
    <method name="Disable"/>
    <method name="MouseSet">
      <arg type="i" name="x" direction="in"/><arg type="i" name="y" direction="in"/>
      <arg type="i" name="on" direction="in"/>
    </method>
    <method name="CursorDefine">
      <arg type="i" name="width" direction="in"/><arg type="i" name="height" direction="in"/>
      <arg type="i" name="hot_x" direction="in"/><arg type="i" name="hot_y" direction="in"/>
      <arg type="ay" name="data" direction="in"/>
    </method>
    <property name="Interfaces" type="as" access="read"/>
  </interface>
  <interface name="org.qemu.Display1.Listener.Unix.Map">
    <method name="ScanoutMap">
      <arg type="h" name="handle" direction="in"/><arg type="u" name="offset" direction="in"/>
      <arg type="u" name="width" direction="in"/><arg type="u" name="height" direction="in"/>
      <arg type="u" name="stride" direction="in"/><arg type="u" name="pixman_format" direction="in"/>
    </method>
    <method name="UpdateMap">
      <arg type="i" name="x" direction="in"/><arg type="i" name="y" direction="in"/>
      <arg type="i" name="width" direction="in"/><arg type="i" name="height" direction="in"/>
    </method>
  </interface>
  <interface name="org.qemu.Display1.Listener.Unix.ScanoutDMABUF2">
    <method name="ScanoutDMABUF2">
      <arg type="ah" name="dmabuf" direction="in"/>
      <arg type="u" name="x" direction="in"/><arg type="u" name="y" direction="in"/>
      <arg type="u" name="width" direction="in"/><arg type="u" name="height" direction="in"/>
      <arg type="au" name="offset" direction="in"/><arg type="au" name="stride" direction="in"/>
      <arg type="u" name="num_planes" direction="in"/><arg type="u" name="fourcc" direction="in"/>
      <arg type="u" name="backing_width" direction="in"/><arg type="u" name="backing_height" direction="in"/>
      <arg type="t" name="modifier" direction="in"/><arg type="b" name="y0_top" direction="in"/>
    </method>
  </interface>
</node>)XML";

const char *const kInterfaces[] = {
    "org.qemu.Display1.Listener.Unix.Map",
    "org.qemu.Display1.Listener.Unix.ScanoutDMABUF2",
    // qemu-gui fork: UpdateDMABUF without holding the guest (ignored by
    // other QEMUs, which keep expecting replies)
    "org.qemu.Display1.Listener.Unix.AsyncUpdate",
    // qemu-gui fork: the guest's buffers attached as they are, released
    // through Presentation.Released (needs the two above)
    "org.qemu.Display1.Listener.Unix.ZeroCopy",
    nullptr,
};

int takeFd(GDBusMethodInvocation *inv, int handle)
{
    GUnixFDList *list = g_dbus_message_get_unix_fd_list(g_dbus_method_invocation_get_message(inv));
    if (!list) {
        return -1;
    }
    return g_unix_fd_list_get(list, handle, nullptr); // a dup we own
}

void replyNow(GDBusMethodInvocation *inv)
{
    g_dbus_method_invocation_return_value(inv, nullptr);
}
} // namespace

Listener::Listener(FrameMailbox *mailbox, Stats *stats, const Options &opts, Callbacks cb)
    : m_mb(mailbox), m_stats(stats), m_opts(opts), m_cb(std::move(cb))
{
}

Listener::~Listener()
{
    stop();
}

int Listener::start(QString *err)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
        *err = QStringLiteral("socketpair failed");
        return -1;
    }
    m_ctx = g_main_context_new();
    m_loop = g_main_loop_new(m_ctx, FALSE);
    m_thread = std::thread([this, fd = sv[0]]() { run(fd); });
    return sv[1];
}

void Listener::stop()
{
    if (m_thread.joinable()) {
        g_main_loop_quit(m_loop);
        m_thread.join();
    }
    if (m_loop) {
        g_main_loop_unref(m_loop);
        m_loop = nullptr;
    }
    if (m_ctx) {
        g_main_context_unref(m_ctx);
        m_ctx = nullptr;
    }
}

void Listener::run(int fd)
{
    pthread_setname_np(pthread_self(), "dbus-listener");
    g_main_context_push_thread_default(m_ctx);

    GError *err = nullptr;
    GSocket *sock = g_socket_new_from_fd(fd, &err);
    GSocketConnection *sconn = g_socket_connection_factory_create_connection(sock);
    g_object_unref(sock);
    // QEMU authenticates as the server once it has handled RegisterListener.
    m_conn = g_dbus_connection_new_sync(
        G_IO_STREAM(sconn), nullptr,
        GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                             G_DBUS_CONNECTION_FLAGS_DELAY_MESSAGE_PROCESSING),
        nullptr, nullptr, &err);
    g_object_unref(sconn);
    if (!m_conn) {
        qWarning("listener connection: %s", err->message);
        g_error_free(err);
        g_main_context_pop_thread_default(m_ctx);
        return;
    }

    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(kIntrospection, &err);
    static const GDBusInterfaceVTable vtable = {methodCall, getProperty, nullptr, {}};
    for (int i = 0; node->interfaces[i]; i++) {
        // Registered from this thread: calls are dispatched on m_ctx.
        if (!g_dbus_connection_register_object(m_conn, "/org/qemu/Display1/Listener",
                                               node->interfaces[i], &vtable, this, nullptr,
                                               &err)) {
            qWarning("register %s: %s", node->interfaces[i]->name, err->message);
            g_clear_error(&err);
        }
    }
    g_dbus_node_info_unref(node);
    g_dbus_connection_add_filter(m_conn, filter, this, nullptr);
    g_dbus_connection_start_message_processing(m_conn);

    g_main_loop_run(m_loop);

    g_dbus_connection_close_sync(m_conn, nullptr, nullptr);
    g_object_unref(m_conn);
    m_conn = nullptr;
    g_main_context_pop_thread_default(m_ctx);
}

GDBusMessage *Listener::filter(GDBusConnection *, GDBusMessage *msg, int incoming, void *user)
{
    auto *self = static_cast<Listener *>(user);
    const int64_t now = nowNs();
    if (incoming) {
        if (g_dbus_message_get_message_type(msg) == G_DBUS_MESSAGE_TYPE_METHOD_CALL &&
            g_strcmp0(g_dbus_message_get_member(msg), "UpdateDMABUF") == 0) {
            self->m_workerIn = now;
            self->m_replySerial = g_dbus_message_get_serial(msg);
        }
    } else if (g_dbus_message_get_message_type(msg) == G_DBUS_MESSAGE_TYPE_METHOD_RETURN &&
               g_dbus_message_get_reply_serial(msg) == self->m_replySerial) {
        const int64_t at = self->m_replyAt.exchange(0);
        if (at) {
            self->m_stats->gdbusHops(0, now - at);
        }
    }
    return msg;
}

void Listener::methodCall(GDBusConnection *, const char *, const char *, const char *,
                          const char *method, GVariant *params, GDBusMethodInvocation *inv,
                          void *user)
{
    static_cast<Listener *>(user)->handleCall(method, params, inv);
}

GVariant *Listener::getProperty(GDBusConnection *, const char *, const char *,
                                const char *, const char *property, GError **, void *user)
{
    auto *self = static_cast<Listener *>(user);
    if (g_strcmp0(property, "Interfaces") == 0) {
        const bool async = self->m_opts.asyncUpdate && !self->m_opts.lateReply &&
                           !self->m_opts.replyOnPresent;
        return g_variant_new_strv(kInterfaces, !async ? 2 : self->m_opts.zeroCopy ? 4 : 3);
    }
    return nullptr;
}

void Listener::publish(std::shared_ptr<Scanout> scanout, int64_t recvNs)
{
    uint32_t w = scanout ? scanout->width : 0, h = scanout ? scanout->height : 0;
    bool isUpdate = scanout && scanout->kind == Scanout::Image; // carries pixels
    {
        std::lock_guard g(m_mb->lock);
        m_mb->scanout = std::move(scanout);
        m_mb->scanoutSerial++;
        if (isUpdate) {
            m_mb->updateSerial++;
            m_mb->updateRecvNs = recvNs;
        }
    }
    m_mb->cond.notify_one();
    m_stats->scanoutReceived();
    // every page flip is a new scanout: wake the GUI thread only for new sizes
    if (m_cb.scanoutSize && (w != m_lastW || h != m_lastH)) {
        m_lastW = w;
        m_lastH = h;
        m_cb.scanoutSize(w, h);
    }
}

void Listener::update(int64_t recvNs, GDBusMethodInvocation *inv)
{
    uint64_t serial;
    {
        std::lock_guard g(m_mb->lock);
        if (inv) {
            m_mb->deferredReplies.emplace_back(inv, recvNs);
        }
        serial = ++m_mb->updateSerial;
        m_mb->updateRecvNs = recvNs;
        if (m_mb->scanout && m_mb->scanout->inode) {
            m_mb->updated.push_back(m_mb->scanout->inode);
        }
    }
    m_mb->cond.notify_one();
    m_stats->trace('R', serial, recvNs);
}

void Listener::handleCall(const char *m, GVariant *p, GDBusMethodInvocation *inv)
{
    const int64_t recv = nowNs();

    if (strcmp(m, "UpdateDMABUF") == 0 || strcmp(m, "UpdateMap") == 0) {
        if ((m_opts.lateReply || m_opts.replyOnPresent) && strcmp(m, "UpdateDMABUF") == 0) {
            update(recv, inv); // the render thread replies after presenting
            return;
        }
        const int64_t in = m_workerIn.exchange(0);
        m_replyAt = nowNs();
        replyNow(inv); // unblocks the guest's GPU queue in QEMU
        m_stats->updateReceived(recv, nowNs());
        if (in) {
            m_stats->gdbusHops(recv - in, 0);
        }
        update(recv, nullptr);
        return;
    }

    if (strcmp(m, "ScanoutDMABUF") == 0) {
        gint32 handle;
        guint32 w, h, stride, fourcc;
        guint64 modifier;
        gboolean y0Top;
        g_variant_get(p, "(huuuutb)", &handle, &w, &h, &stride, &fourcc, &modifier, &y0Top);
        auto s = std::make_shared<Scanout>();
        s->kind = Scanout::Dmabuf;
        s->width = s->backingWidth = w;
        s->height = s->backingHeight = h;
        s->planes.push_back({takeFd(inv, handle), 0, stride});
        s->fourcc = fourcc;
        s->modifier = modifier;
        if (s->planes[0].fd >= 0) {
            struct stat st{};
            if (fstat(s->planes[0].fd, &st) == 0) {
                s->inode = st.st_ino;
            }
        }
        s->y0Top = y0Top;
        replyNow(inv);
        publish(std::move(s), recv);
        return;
    }

    if (strcmp(m, "ScanoutDMABUF2") == 0) {
        GVariant *fds, *offsets, *strides;
        guint32 x, y, w, h, nplanes, fourcc, bw, bh;
        guint64 modifier;
        gboolean y0Top;
        g_variant_get(p, "(@ahuuuu@au@auuuuutb)", &fds, &x, &y, &w, &h, &offsets, &strides,
                      &nplanes, &fourcc, &bw, &bh, &modifier, &y0Top);
        auto s = std::make_shared<Scanout>();
        s->kind = Scanout::Dmabuf;
        s->x = x;
        s->y = y;
        s->width = w;
        s->height = h;
        s->backingWidth = bw;
        s->backingHeight = bh;
        s->fourcc = fourcc;
        s->modifier = modifier;
        s->y0Top = y0Top;
        gsize no = 0, ns = 0;
        auto *ov = static_cast<const guint32 *>(g_variant_get_fixed_array(offsets, &no, 4));
        auto *sv = static_cast<const guint32 *>(g_variant_get_fixed_array(strides, &ns, 4));
        gsize nfd = g_variant_n_children(fds);
        for (guint32 i = 0; i < nplanes && i < no && i < ns; i++) {
            int fd = -1;
            if (i < nfd) {
                gint32 hdl;
                g_variant_get_child(fds, i, "h", &hdl);
                fd = takeFd(inv, hdl);
            } else if (!s->planes.empty()) {
                fd = dup(s->planes[0].fd); // planes may share one buffer
            }
            s->planes.push_back({fd, ov[i], sv[i]});
        }
        g_variant_unref(fds);
        g_variant_unref(offsets);
        g_variant_unref(strides);
        if (!s->planes.empty() && s->planes[0].fd >= 0) {
            struct stat st{};
            if (fstat(s->planes[0].fd, &st) == 0) {
                s->inode = st.st_ino;
            }
        }
        replyNow(inv);
        publish(std::move(s), recv);
        return;
    }

    if (strcmp(m, "ScanoutMap") == 0) {
        gint32 handle;
        guint32 offset, w, h, stride, format;
        g_variant_get(p, "(huuuuu)", &handle, &offset, &w, &h, &stride, &format);
        int fd = takeFd(inv, handle);
        auto s = std::make_shared<Scanout>();
        s->kind = Scanout::Map;
        s->width = s->backingWidth = w;
        s->height = s->backingHeight = h;
        s->stride = stride;
        s->pixmanFormat = format;
        s->mapLength = size_t(offset) + size_t(stride) * h;
        s->mapOffset = offset;
        void *map = fd >= 0 ? mmap(nullptr, s->mapLength, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
        if (fd >= 0) {
            close(fd);
        }
        s->map = map == MAP_FAILED ? nullptr : map;
        replyNow(inv);
        publish(s->map ? std::move(s) : nullptr, recv);
        return;
    }

    if (strcmp(m, "Scanout") == 0) {
        guint32 w, h, stride, format;
        GVariant *data;
        g_variant_get(p, "(uuuu@ay)", &w, &h, &stride, &format, &data);
        gsize n = 0;
        auto *bytes = static_cast<const uint8_t *>(g_variant_get_fixed_array(data, &n, 1));
        auto s = std::make_shared<Scanout>();
        s->kind = Scanout::Image;
        s->width = s->backingWidth = w;
        s->height = s->backingHeight = h;
        s->stride = w * 4;
        s->pixmanFormat = format;
        s->pixels.resize(size_t(s->stride) * h);
        for (guint32 row = 0; row < h && size_t(row) * stride + s->stride <= n; row++) {
            memcpy(&s->pixels[size_t(row) * s->stride], bytes + size_t(row) * stride, s->stride);
        }
        g_variant_unref(data);
        replyNow(inv);
        publish(std::move(s), recv);
        return;
    }

    if (strcmp(m, "Update") == 0) {
        gint32 x, y, w, h;
        guint32 stride, format;
        GVariant *data;
        g_variant_get(p, "(iiiiuu@ay)", &x, &y, &w, &h, &stride, &format, &data);
        gsize n = 0;
        auto *bytes = static_cast<const uint8_t *>(g_variant_get_fixed_array(data, &n, 1));
        {
            std::lock_guard g(m_mb->lock);
            Scanout *s = m_mb->scanout.get();
            if (s && s->kind == Scanout::Image && x >= 0 && y >= 0 &&
                uint32_t(x + w) <= s->width && uint32_t(y + h) <= s->height) {
                for (gint32 row = 0; row < h && size_t(row) * stride + size_t(w) * 4 <= n; row++) {
                    memcpy(&s->pixels[size_t(y + row) * s->stride + size_t(x) * 4],
                           bytes + size_t(row) * stride, size_t(w) * 4);
                }
            }
        }
        g_variant_unref(data);
        replyNow(inv);
        m_stats->updateReceived(recv, nowNs());
        update(recv, nullptr);
        return;
    }

    if (strcmp(m, "Disable") == 0) {
        replyNow(inv);
        publish(nullptr, recv);
        return;
    }

    if (strcmp(m, "MouseSet") == 0) {
        gint32 x, y, on;
        g_variant_get(p, "(iii)", &x, &y, &on);
        replyNow(inv);
        if (m_cb.mouseSet) {
            m_cb.mouseSet(x, y, on);
        }
        return;
    }

    if (strcmp(m, "CursorDefine") == 0) {
        gint32 w, h, hx, hy;
        GVariant *data;
        g_variant_get(p, "(iiii@ay)", &w, &h, &hx, &hy, &data);
        gsize n = 0;
        auto *bytes = static_cast<const uint8_t *>(g_variant_get_fixed_array(data, &n, 1));
        QImage img;
        if (w > 0 && h > 0 && n >= size_t(w) * h * 4) {
            img = QImage(bytes, w, h, w * 4, QImage::Format_ARGB32).copy();
        }
        g_variant_unref(data);
        replyNow(inv);
        if (m_cb.cursorDefine && !img.isNull()) {
            m_cb.cursorDefine(img, hx, hy);
        }
        return;
    }

    g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod",
                                               m);
}
