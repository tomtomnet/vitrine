// SPDX-License-Identifier: GPL-2.0-or-later
#include "waylandsandbox.h"

#include <QCoreApplication>

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <wayland-client.h>

#include "security-context-v1-client-protocol.h"

namespace WaylandSandbox {

static QString tr(const char *text)
{
    return QCoreApplication::translate("WaylandSandbox", text);
}

static QString lastError()
{
    return QString::fromLocal8Bit(strerror(errno));
}

static const wl_registry_listener kRegistry = {
    [](void *data, wl_registry *registry, uint32_t name, const char *iface, uint32_t) {
        auto **manager = static_cast<wp_security_context_manager_v1 **>(data);
        if (!*manager && !strcmp(iface, wp_security_context_manager_v1_interface.name)) {
            *manager = static_cast<wp_security_context_manager_v1 *>(
                wl_registry_bind(registry, name, &wp_security_context_manager_v1_interface, 1));
        }
    },
    [](void *, wl_registry *, uint32_t) {},
};

/* A socket listening on @path, close-on-exec */
static int listenOn(const QString &path, QString *error)
{
    const QByteArray name = path.toLocal8Bit();
    sockaddr_un addr{};

    if (size_t(name.size()) >= sizeof(addr.sun_path)) {
        *error = tr("The path of the socket is too long: %1").arg(path);
        return -1;
    }
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        *error = tr("socket: %1").arg(lastError());
        return -1;
    }
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, name.constData(), size_t(name.size()));
    unlink(name.constData());
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 || ::listen(fd, 16) < 0) {
        *error = tr("Cannot listen on %1: %2").arg(path, lastError());
        close(fd);
        unlink(name.constData());
        return -1;
    }
    return fd;
}

int listen(const QString &compositor, const QString &path, const QString &appId,
           const QString &instanceId, QString *error)
{
    wl_display *display = wl_display_connect(compositor.toLocal8Bit().constData());
    if (!display) {
        *error = tr("Cannot connect to the compositor at %1: %2").arg(compositor, lastError());
        return -1;
    }
    wp_security_context_manager_v1 *manager = nullptr;
    wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistry, &manager);
    const auto end = [&](int out) {
        if (manager) {
            wp_security_context_manager_v1_destroy(manager);
        }
        wl_registry_destroy(registry);
        wl_display_flush(display);
        wl_display_disconnect(display);
        return out;
    };

    if (wl_display_roundtrip(display) < 0) {
        *error = tr("The compositor at %1 did not answer").arg(compositor);
        return end(-1);
    }
    if (!manager) {
        *error = tr("The compositor offers no wp_security_context_v1");
        return end(-1);
    }
    const int listenFd = listenOn(path, error);
    if (listenFd < 0) {
        return end(-1);
    }
    int closeFds[2];
    if (pipe2(closeFds, O_CLOEXEC) < 0) {
        *error = tr("pipe: %1").arg(lastError());
        close(listenFd);
        unlink(path.toLocal8Bit().constData());
        return end(-1);
    }
    wp_security_context_v1 *context =
        wp_security_context_manager_v1_create_listener(manager, listenFd, closeFds[0]);
    wp_security_context_v1_set_sandbox_engine(context, "vitrine");
    wp_security_context_v1_set_app_id(context, appId.toUtf8().constData());
    if (!instanceId.isEmpty()) {
        wp_security_context_v1_set_instance_id(context, instanceId.toUtf8().constData());
    }
    wp_security_context_v1_commit(context);
    /* the compositor has its own copies of both now */
    const bool taken = wl_display_roundtrip(display) >= 0;
    close(listenFd);
    close(closeFds[0]);
    /* on a failed connection, the requests of the proxies' ends go nowhere */
    wp_security_context_v1_destroy(context);
    if (!taken) {
        *error = tr("The compositor refused the socket (Wayland error %1)")
                     .arg(wl_display_get_error(display));
        close(closeFds[1]);
        unlink(path.toLocal8Bit().constData());
        return end(-1);
    }
    return end(closeFds[1]);
}

}
