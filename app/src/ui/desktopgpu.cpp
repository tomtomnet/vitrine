// SPDX-License-Identifier: GPL-2.0-or-later
#include "desktopgpu.h"

#include <QGuiApplication>

#include <cstring>

#include <unistd.h>
#include <wayland-client.h>

#include "linux-dmabuf-v1-client-protocol.h"

namespace {

struct Query {
    zwp_linux_dmabuf_v1 *dmabuf = nullptr;
    std::optional<dev_t> device;
    bool done = false;
};

const wl_registry_listener kRegistry = {
    [](void *data, wl_registry *registry, uint32_t name, const char *interface,
       uint32_t version) {
        auto *query = static_cast<Query *>(data);
        /* the default feedback came with version 4 */
        if (!query->dmabuf && version >= 4 &&
            strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
            query->dmabuf = static_cast<zwp_linux_dmabuf_v1 *>(
                wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, 4));
        }
    },
    [](void *, wl_registry *, uint32_t) {},
};

const zwp_linux_dmabuf_feedback_v1_listener kFeedback = {
    /* done: the feedback is complete */
    [](void *data, zwp_linux_dmabuf_feedback_v1 *) { static_cast<Query *>(data)->done = true; },
    /* format_table: not needed, but its fd is ours to close */
    [](void *, zwp_linux_dmabuf_feedback_v1 *, int32_t fd, uint32_t) { close(fd); },
    /* main_device */
    [](void *data, zwp_linux_dmabuf_feedback_v1 *, wl_array *device) {
        if (device->size == sizeof(dev_t)) {
            dev_t d;
            memcpy(&d, device->data, sizeof(d));
            static_cast<Query *>(data)->device = d;
        }
    },
    /* tranche_done, tranche_target_device, tranche_formats, tranche_flags */
    [](void *, zwp_linux_dmabuf_feedback_v1 *) {},
    [](void *, zwp_linux_dmabuf_feedback_v1 *, wl_array *) {},
    [](void *, zwp_linux_dmabuf_feedback_v1 *, wl_array *) {},
    [](void *, zwp_linux_dmabuf_feedback_v1 *, uint32_t) {},
};

}

namespace DesktopGpu {

std::optional<dev_t> mainDevice()
{
    auto *app = qGuiApp ? qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>()
                        : nullptr;
    wl_display *display = app ? app->display() : nullptr;
    Query query;

    if (!display) {
        return std::nullopt;
    }
    /* a queue of its own: Qt dispatches the default one */
    wl_event_queue *queue = wl_display_create_queue(display);
    auto *wrapped = static_cast<wl_display *>(wl_proxy_create_wrapper(display));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapped), queue);
    wl_registry *registry = wl_display_get_registry(wrapped);
    wl_registry_add_listener(registry, &kRegistry, &query);
    if (wl_display_roundtrip_queue(display, queue) >= 0 && query.dmabuf) {
        zwp_linux_dmabuf_feedback_v1 *feedback =
            zwp_linux_dmabuf_v1_get_default_feedback(query.dmabuf);
        zwp_linux_dmabuf_feedback_v1_add_listener(feedback, &kFeedback, &query);
        /* the compositor sends it all at once, up to done */
        for (int i = 0; i < 3 && !query.done; i++) {
            if (wl_display_roundtrip_queue(display, queue) < 0) {
                break;
            }
        }
        zwp_linux_dmabuf_feedback_v1_destroy(feedback);
        zwp_linux_dmabuf_v1_destroy(query.dmabuf);
    }
    wl_registry_destroy(registry);
    wl_proxy_wrapper_destroy(wrapped);
    wl_event_queue_destroy(queue);
    return query.done ? query.device : std::nullopt;
}

}
