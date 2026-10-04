#include "renderer.h"
#include "stats.h"

#include "presentation-time-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "swapchain.h"

#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFunctions>
#include <QWindow>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gio/gio.h>
#include <wayland-client.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <poll.h>
#include <thread>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/dma-buf.h>

#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif
#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffULL
#endif

QRect fitRect(QSize view, QSize image)
{
    if (image.isEmpty() || view.isEmpty()) {
        return {};
    }
    if (image == view) {
        return QRect(QPoint(0, 0), view);
    }
    // keep the aspect ratio; integer math so the GUI thread maps input the same way
    int w = view.width(), h = int(int64_t(view.width()) * image.height() / image.width());
    if (h > view.height()) {
        h = view.height();
        w = int(int64_t(view.height()) * image.width() / image.height());
    }
    return QRect((view.width() - w) / 2, (view.height() - h) / 2, w, h);
}

namespace {
typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOES)(GLenum target, void *image);

constexpr uint32_t PIXMAN_x8r8g8b8 = 0x20020888;
constexpr uint32_t PIXMAN_a8r8g8b8 = 0x20028888;

struct CachedImage {
    dev_t dev = 0;
    ino_t ino = 0;
    uint32_t fourcc = 0, width = 0, height = 0, stride = 0, offset = 0;
    uint64_t modifier = 0;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint texture = 0, fbo = 0;
    uint64_t lastUse = 0;
};

using Replies = std::vector<std::pair<GDBusMethodInvocation *, int64_t>>;

// Answer deferred UpdateDMABUF calls; QEMU unblocks the guest's GPU queue.
void replyAll(Stats *stats, Replies &replies)
{
    for (auto &[inv, recv] : replies) {
        g_dbus_method_invocation_return_value(inv, nullptr);
        const int64_t t = nowNs();
        stats->updateReceived(recv, t);
        stats->replyDelay(t - recv);
    }
    replies.clear();
}

/*
 * Zero copy: a guest buffer attached to the display's surface as it is,
 * instead of a copy into a buffer of our own.  QEMU sends a scanout's real
 * layout (ScanoutDMABUF2 with a modifier other than DRM_FORMAT_MOD_INVALID)
 * when the guest can be held until we are done with the buffer; we say so
 * with Presentation.Released for every buffer of an UpdateDMABUF, once the
 * compositor released it and finished reading it, once our copy of it is
 * done, or at once when we skipped it.
 */
struct GuestBuffer {
    class RenderState *state = nullptr;
    uint64_t inode = 0;
    int fd = -1;                // a dup of plane 0: its readers before a release
    wl_buffer *buffer = nullptr;
    bool busy = false;          // with the compositor
    bool failed = false;        // the compositor did not take it: copied
    uint64_t lastUse = 0;
};

struct Feedback {
    class RenderState *state;
    int64_t recvNs;
    uint64_t serial;
    Replies replies; // --reply-on-present
    struct wp_presentation_feedback *proxy = nullptr; // "struct": a request has the same name
    wl_callback *frame = nullptr;   // the compositor's frame callback for the same commit
    int64_t swapNs = 0;             // when the frame was handed to the compositor
    int64_t frameDoneNs = 0;        // when the compositor let us draw again
};

class RenderState
{
public:
    RenderState(Stats *stats) : stats(stats) {}

    bool init(QOpenGLContext *ctx, QString *err);
    void initWaylandQueue(wl_display *display);
    void initWayland(wl_display *display);
    // the reader thread, with or without Wayland: the read fences need it
    void startReader();
    void wakeReader();
    void stopReaderThread();
    void finiWayland();
    // Returns the framebuffer to blit from; sets imported when a new EGLImage was made.
    GLuint prepare(const Scanout &s, bool *imported);
    void upload(const Scanout &s, const uint8_t *pixels, uint32_t stride);
    void draw(const Scanout *s, GLuint fbo, QSize view, GLuint target = 0);
    Feedback *requestFeedback(wl_surface *surface, int64_t recvNs, uint64_t serial,
                              Replies *replies);
    void dispatchWayland();
    void destroyCache();
    void presented(Feedback *d, int64_t t, uint32_t refresh);

    Stats *stats;
    QOpenGLContext *ctx = nullptr;
    QOpenGLFunctions *f = nullptr;
    QOpenGLExtraFunctions *ef = nullptr;
    EGLDisplay dpy = EGL_NO_DISPLAY;
    PFNEGLCREATEIMAGEKHRPROC createImage = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroyImage = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOES imageTargetTexture = nullptr;
    std::vector<CachedImage> cache;
    uint64_t useCounter = 0;
    // shared-memory scanouts (2D without GL)
    GLuint shmTexture = 0, shmFbo = 0;
    QSize shmSize;
    // Wayland presentation feedback, on a private event queue of this thread
    wl_display *wlDisplay = nullptr;
    wl_event_queue *queue = nullptr;
    wp_presentation *presentation = nullptr;
    uint32_t clockId = 0;
    // feedback the compositor has not answered yet: destroyed before the
    // queue, else an answer arriving later aborts libwayland
    std::vector<Feedback *> feedbacks;
    // The compositor's answers are read by a thread of their own, so that
    // QEMU hears of a frame on the screen at once and not with our next
    // frame: the guest's vertical blank is timed from it.  The queue is
    // dispatched under the lock, by that thread or by the render thread.
    std::mutex lock;
    std::thread reader;
    std::atomic<bool> stopReader{false};
    int wakeFd = -1;    // eventfd: a fence to poll, or the end (wakeReader)
    wl_surface *surfaceWrapper = nullptr; // the window's surface, on our queue
    wl_surface *wrappedSurface = nullptr;
    Swapchain *swapchain = nullptr; // the window's own buffers (its globals on our queue)
    // where a presented frame is reported (org.qemu.Display1.Presentation)
    GDBusConnection *sinkConn = nullptr;
    QByteArray sinkPath;
    // zero copy (see GuestBuffer)
    zwp_linux_dmabuf_v1 *dmabuf = nullptr;      // a binding of our own, on our queue
    bool zcSupported = false;                   // QEMU has Presentation.Released
    std::vector<GuestBuffer *> guests;
    uint64_t guestUse = 0;
    GuestBuffer *attached = nullptr;            // the guest buffer on the surface
    int zcScanout = -1;                         // what QEMU was told (Presentation.ZeroCopy)
    // read fences of released buffers: Released when signalled (under lock)
    std::vector<std::pair<int, uint64_t>> pendingReads;
    PFNEGLCREATESYNCKHRPROC createSync = nullptr;
    PFNEGLDESTROYSYNCKHRPROC destroySync = nullptr;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC dupFenceFd = nullptr;

    GuestBuffer *guestBuffer(const Scanout &s);
    void guestReleased(GuestBuffer *g);         // the compositor's release (under lock)
    void released(uint64_t inode);
    void releaseAfterGpu(uint64_t inode);       // after the GL work queued so far
    void sendZeroCopy(bool on);
    void destroyGuests();

    void forget(Feedback *d)
    {
        std::erase(feedbacks, d);
        wp_presentation_feedback_destroy(d->proxy);
        if (d->frame) {
            wl_callback_destroy(d->frame);
        }
        delete d;
    }
    void readEvents();
};

const wl_callback_listener kFrameListener = {
    [](void *data, wl_callback *cb, uint32_t) {
        auto *d = static_cast<Feedback *>(data);
        d->frameDoneNs = nowNs();
        wl_callback_destroy(cb);
        d->frame = nullptr;
    },
};

const wp_presentation_feedback_listener kFeedbackListener = {
    [](void *, struct wp_presentation_feedback *, wl_output *) {},
    [](void *data, struct wp_presentation_feedback *fb, uint32_t sec_hi, uint32_t sec_lo,
       uint32_t nsec, uint32_t refresh, uint32_t, uint32_t, uint32_t) {
        auto *d = static_cast<Feedback *>(data);
        int64_t t = int64_t((uint64_t(sec_hi) << 32) | sec_lo) * 1000000000 + nsec;
        d->state->stats->framePresented(d->recvNs, t);
        d->state->stats->trace('P', d->serial, t);
        d->state->presented(d, t, refresh);
        replyAll(d->state->stats, d->replies);
        d->state->forget(d);
    },
    [](void *data, struct wp_presentation_feedback *fb) {
        auto *d = static_cast<Feedback *>(data);
        d->state->stats->frameDiscarded();
        replyAll(d->state->stats, d->replies);
        d->state->forget(d);
    },
};

const wp_presentation_listener kPresentationListener = {
    [](void *data, wp_presentation *, uint32_t clk) {
        static_cast<RenderState *>(data)->clockId = clk;
    },
};

const wl_registry_listener kRegistryListener = {
    [](void *data, wl_registry *reg, uint32_t name, const char *iface, uint32_t version) {
        auto *s = static_cast<RenderState *>(data);
        if (strcmp(iface, wp_presentation_interface.name) == 0) {
            // version 2 when there is one: under a variable refresh rate a
            // version 1 client gets refresh 0, so QEMU keeps its default
            s->presentation = static_cast<wp_presentation *>(wl_registry_bind(
                reg, name, &wp_presentation_interface,
                std::min(version, uint32_t(wp_presentation_interface.version))));
            wp_presentation_add_listener(s->presentation, &kPresentationListener, s);
        } else if (strcmp(iface, zwp_linux_dmabuf_v1_interface.name) == 0) {
            if (s->swapchain) {
                s->swapchain->setDmabuf(static_cast<zwp_linux_dmabuf_v1 *>(
                    wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, 4)));
            }
            // the guest's buffers (zero copy): a binding the swapchain does not own
            s->dmabuf = static_cast<zwp_linux_dmabuf_v1 *>(
                wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, 4));
        }
    },
    [](void *, wl_registry *, uint32_t) {},
};

// A frame is on the screen: QEMU may time the guest's vertical blank from
// it.  One-way, as the interface says.
void RenderState::presented(Feedback *d, int64_t t, uint32_t refresh)
{
    if (swapchain && refresh) {
        swapchain->setRefreshNs(refresh);
    }
    if (!sinkConn || sinkPath.isEmpty() || clockId != CLOCK_MONOTONIC) {
        return;
    }
    int64_t compositorLead = d->frameDoneNs ? t - d->frameDoneNs : 0;
    int64_t swapLead = d->swapNs ? t - d->swapNs : 0;
    g_dbus_connection_call(sinkConn, nullptr, sinkPath.constData(),
                           "org.qemu.Display1.Presentation", "Presented",
                           g_variant_new("(xuxx)", t, refresh, compositorLead, swapLead),
                           nullptr, G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, nullptr, nullptr,
                           nullptr);
}

const wl_buffer_listener kGuestBufferListener = {
    [](void *data, wl_buffer *) {
        auto *g = static_cast<GuestBuffer *>(data);
        g->busy = false;
        g->state->guestReleased(g);
    },
};

struct Made {
    wl_buffer *buffer = nullptr;
    bool done = false;
};

const zwp_linux_buffer_params_v1_listener kParamsListener = {
    [](void *data, zwp_linux_buffer_params_v1 *, wl_buffer *buffer) {
        auto *m = static_cast<Made *>(data);
        m->buffer = buffer;
        m->done = true;
    },
    [](void *data, zwp_linux_buffer_params_v1 *) {
        static_cast<Made *>(data)->done = true;
    },
};

// Zero copy: QEMU may let the guest render into the buffer again.  One-way.
void RenderState::released(uint64_t inode)
{
    if (!sinkConn || sinkPath.isEmpty() || !inode) {
        return;
    }
    g_dbus_connection_call(sinkConn, nullptr, sinkPath.constData(),
                           "org.qemu.Display1.Presentation", "Released",
                           g_variant_new("(t)", static_cast<guint64>(inode)), nullptr,
                           G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, nullptr, nullptr, nullptr);
}

void RenderState::sendZeroCopy(bool on)
{
    if (zcScanout == int(on) || !sinkConn || sinkPath.isEmpty()) {
        return;
    }
    zcScanout = on;
    g_dbus_connection_call(sinkConn, nullptr, sinkPath.constData(),
                           "org.qemu.Display1.Presentation", "ZeroCopy",
                           g_variant_new("(b)", gboolean(on)), nullptr,
                           G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, nullptr, nullptr, nullptr);
}

/*
 * The compositor gave a guest buffer back (called under the lock, from the
 * dispatch of our queue).  It may release a buffer while its composite of it
 * still runs, counting on implicit sync to hold the writer, which the
 * guest's writes skip: the buffer goes back once those reads are done too.
 */
void RenderState::guestReleased(GuestBuffer *g)
{
    struct dma_buf_export_sync_file arg = {};
    arg.flags = DMA_BUF_SYNC_WRITE;
    arg.fd = -1;
    if (g->fd >= 0 && ioctl(g->fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &arg) == 0 && arg.fd >= 0) {
        pollfd pfd = {arg.fd, POLLIN, 0};
        if (poll(&pfd, 1, 0) != 1) {
            pendingReads.emplace_back(arg.fd, g->inode); // the reader thread polls it
            wakeReader();
            return;
        }
        close(arg.fd);
    }
    released(g->inode);
}

// After our copy of the buffer: once the GL work queued so far is done
void RenderState::releaseAfterGpu(uint64_t inode)
{
    // the fence's fd only for a thread that polls it: else a GPU wait
    if (createSync && dupFenceFd && destroySync && reader.joinable()) {
        const EGLint attrs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, EGL_NO_NATIVE_FENCE_FD_ANDROID,
                                EGL_NONE};
        EGLSyncKHR sync = createSync(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, attrs);
        if (sync != EGL_NO_SYNC_KHR) {
            f->glFlush();
            int fd = dupFenceFd(dpy, sync);
            destroySync(dpy, sync);
            if (fd >= 0) {
                std::lock_guard g(lock);
                pendingReads.emplace_back(fd, inode);
                wakeReader();
                return;
            }
        }
    }
    f->glFinish();
    released(inode);
}

/*
 * The wl_buffer of a guest buffer, made once per buffer (QEMU's scanouts of
 * the same buffer come with new fds: known by the inode).  Created with
 * create, not create_immed: a buffer the compositor cannot import is then a
 * failed event, not a protocol error that ends the connection.
 */
GuestBuffer *RenderState::guestBuffer(const Scanout &s)
{
    for (auto *g : guests) {
        if (g->inode == s.inode) {
            if (g->failed) {
                return nullptr;
            }
            g->lastUse = ++guestUse;
            return g;
        }
    }
    if (!dmabuf || s.planes.empty()) {
        return nullptr;
    }
    // bounded: the one used longest ago that the compositor does not hold
    if (guests.size() >= 8) {
        std::lock_guard g(lock);
        auto victim = guests.end();
        for (auto it = guests.begin(); it != guests.end(); ++it) {
            if (!(*it)->busy && *it != attached &&
                (victim == guests.end() || (*it)->lastUse < (*victim)->lastUse)) {
                victim = it;
            }
        }
        if (victim == guests.end()) {
            return nullptr;
        }
        if ((*victim)->buffer) {
            wl_buffer_destroy((*victim)->buffer);
        }
        if ((*victim)->fd >= 0) {
            close((*victim)->fd);
        }
        delete *victim;
        guests.erase(victim);
    }
    auto *g = new GuestBuffer;
    g->state = this;
    g->inode = s.inode;
    g->lastUse = ++guestUse;
    guests.push_back(g);

    Made made;
    zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(dmabuf);
    for (size_t i = 0; i < s.planes.size() && i < 4; i++) {
        zwp_linux_buffer_params_v1_add(params, s.planes[i].fd, uint32_t(i), s.planes[i].offset,
                                       s.planes[i].stride, uint32_t(s.modifier >> 32),
                                       uint32_t(s.modifier & 0xffffffff));
    }
    zwp_linux_buffer_params_v1_add_listener(params, &kParamsListener, &made);
    zwp_linux_buffer_params_v1_create(params, int(s.backingWidth), int(s.backingHeight), s.fourcc, 0);
    wl_display_flush(wlDisplay);
    // the answer is dispatched on our queue, by the reader thread or here
    for (int i = 0; i < 500; i++) {
        {
            std::lock_guard l(lock);
            wl_display_dispatch_queue_pending(wlDisplay, queue);
            if (made.done) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    {
        std::lock_guard l(lock);
        zwp_linux_buffer_params_v1_destroy(params);
        if (!made.buffer) {
            g->failed = true;
            fprintf(stderr, "zero copy: the compositor did not take guest buffer %llu "
                    "(modifier 0x%llx): copied\n", (unsigned long long)s.inode,
                    (unsigned long long)s.modifier);
            return nullptr;
        }
        g->buffer = made.buffer;
        wl_buffer_add_listener(g->buffer, &kGuestBufferListener, g);
    }
    g->fd = fcntl(s.planes[0].fd, F_DUPFD_CLOEXEC, 0);
    fprintf(stderr, "zero copy: guest buffer %llu %ux%u modifier 0x%llx attached as it is\n",
            (unsigned long long)s.inode, s.backingWidth, s.backingHeight,
            (unsigned long long)s.modifier);
    return g;
}

// Before the queue goes (called with the reader thread stopped)
void RenderState::destroyGuests()
{
    for (auto *g : guests) {
        if (g->buffer) {
            wl_buffer_destroy(g->buffer);
        }
        if (g->fd >= 0) {
            close(g->fd);
        }
        if (g->busy || g == attached) {
            released(g->inode);
        }
        delete g;
    }
    guests.clear();
    attached = nullptr;
    for (auto &[fd, inode] : pendingReads) {
        close(fd);
        released(inode);
    }
    pendingReads.clear();
    if (dmabuf) {
        zwp_linux_dmabuf_v1_destroy(dmabuf);
        dmabuf = nullptr;
    }
}

// The reader thread: the compositor's events for our queue, as they come.
// libwayland lets several threads read the display; Qt's own reader and this
// one each prepare, poll and read or cancel.
void RenderState::readEvents()
{
    // without Wayland (a window without the handles, X11) the fences alone:
    // the copies' fences go nowhere else, and Released waits for them
    const int fd = wlDisplay ? wl_display_get_fd(wlDisplay) : -1;
    while (!stopReader) {
        if (wlDisplay) {
            {
                std::lock_guard g(lock);
                while (wl_display_prepare_read_queue(wlDisplay, queue) != 0) {
                    wl_display_dispatch_queue_pending(wlDisplay, queue);
                }
            }
            wl_display_flush(wlDisplay);
        }
        // and the read fences of released guest buffers (zero copy), and the
        // wake-up when one is queued: polled at once, not at the next event
        std::vector<pollfd> pfds{{fd, POLLIN, 0}, {wakeFd, POLLIN, 0}};
        std::vector<std::pair<int, uint64_t>> pending;
        {
            std::lock_guard g(lock);
            pending = pendingReads;
        }
        for (auto &[sfd, inode] : pending) {
            pfds.push_back({sfd, POLLIN, 0});
        }
        int r = poll(pfds.data(), pfds.size(), 100);
        if (wlDisplay) {
            if (r > 0 && (pfds[0].revents & POLLIN)) {
                wl_display_read_events(wlDisplay);
            } else {
                wl_display_cancel_read(wlDisplay);
            }
        }
        if (pfds[1].revents & POLLIN) {
            eventfd_t n;
            eventfd_read(wakeFd, &n);
        }
        std::lock_guard g(lock);
        for (size_t k = 2; k < pfds.size(); k++) {
            if (pfds[k].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) {
                const uint64_t inode = pending[k - 2].second;
                std::erase(pendingReads, pending[k - 2]);
                close(pfds[k].fd);
                released(inode);
            }
        }
        if (wlDisplay) {
            wl_display_dispatch_queue_pending(wlDisplay, queue);
        }
    }
}

bool RenderState::init(QOpenGLContext *c, QString *err)
{
    ctx = c;
    f = ctx->functions();
    ef = ctx->extraFunctions();
    auto *egl = ctx->nativeInterface<QNativeInterface::QEGLContext>();
    if (!egl) {
        *err = QStringLiteral("the OpenGL context is not EGL based");
        return false;
    }
    dpy = egl->display();
    createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    imageTargetTexture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOES>(
        ctx->getProcAddress("glEGLImageTargetTexture2DOES"));
    const char *exts = eglQueryString(dpy, EGL_EXTENSIONS);
    if (!createImage || !destroyImage || !imageTargetTexture || !exts ||
        !strstr(exts, "EGL_EXT_image_dma_buf_import")) {
        *err = QStringLiteral("EGL_EXT_image_dma_buf_import is not available");
        return false;
    }
    if (!strstr(exts, "EGL_EXT_image_dma_buf_import_modifiers")) {
        qWarning("EGL_EXT_image_dma_buf_import_modifiers missing: only implicit modifiers");
    }
    if (strstr(exts, "EGL_ANDROID_native_fence_sync")) {
        createSync = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(eglGetProcAddress("eglCreateSyncKHR"));
        destroySync = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(eglGetProcAddress("eglDestroySyncKHR"));
        dupFenceFd = reinterpret_cast<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>(
            eglGetProcAddress("eglDupNativeFenceFDANDROID"));
    }
    return true;
}

void RenderState::initWaylandQueue(wl_display *display)
{
    wlDisplay = display;
    queue = wl_display_create_queue(display);
}

void RenderState::initWayland(wl_display *display)
{
    if (!queue) {
        initWaylandQueue(display);
    }
    auto *wrapped = static_cast<wl_display *>(wl_proxy_create_wrapper(display));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapped), queue);
    wl_registry *reg = wl_display_get_registry(wrapped);
    wl_registry_add_listener(reg, &kRegistryListener, this);
    wl_display_roundtrip_queue(display, queue);
    wl_display_roundtrip_queue(display, queue); // clock_id
    wl_registry_destroy(reg);
    wl_proxy_wrapper_destroy(wrapped);
    if (!presentation) {
        qWarning("no wp_presentation: frames on screen will not be measured");
    } else if (clockId != CLOCK_MONOTONIC) {
        qWarning("compositor presentation clock %u is not CLOCK_MONOTONIC", clockId);
    }
    startReader();
}

void RenderState::startReader()
{
    if (reader.joinable()) {
        return;
    }
    if (wakeFd < 0) {
        wakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    }
    stopReader = false;
    reader = std::thread([this] { readEvents(); });
}

void RenderState::wakeReader()
{
    if (wakeFd >= 0) {
        eventfd_write(wakeFd, 1);
    }
}

void RenderState::stopReaderThread()
{
    if (reader.joinable()) {
        stopReader = true;
        wakeReader();
        reader.join();
    }
}

void RenderState::finiWayland()
{
    stopReaderThread();
    std::lock_guard g(lock);
    // the frames still on their way: their replies go now, their proxies before the queue
    while (!feedbacks.empty()) {
        replyAll(stats, feedbacks.back()->replies);
        forget(feedbacks.back());
    }
    if (surfaceWrapper) {
        wl_proxy_wrapper_destroy(surfaceWrapper);
        surfaceWrapper = nullptr;
    }
    if (presentation) {
        wp_presentation_destroy(presentation);
        presentation = nullptr;
    }
    if (queue) {
        wl_event_queue_destroy(queue);
        queue = nullptr;
    }
    if (wakeFd >= 0) {
        close(wakeFd);
        wakeFd = -1;
    }
}

Feedback *RenderState::requestFeedback(wl_surface *surface, int64_t recvNs, uint64_t serial,
                                       Replies *replies)
{
    if (!presentation || !surface) {
        return nullptr;
    }
    std::lock_guard g(lock);
    struct wp_presentation_feedback *fb = wp_presentation_feedback(presentation, surface);
    auto *d = new Feedback{this, recvNs, serial, {}, fb};
    if (replies) {
        d->replies = std::move(*replies);
        replies->clear();
    }
    wp_presentation_feedback_add_listener(fb, &kFeedbackListener, d);
    // the frame callback of the same commit, on our queue (the surface is on Qt's)
    if (wrappedSurface != surface) {
        if (surfaceWrapper) {
            wl_proxy_wrapper_destroy(surfaceWrapper);
        }
        surfaceWrapper = static_cast<wl_surface *>(wl_proxy_create_wrapper(surface));
        wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(surfaceWrapper), queue);
        wrappedSurface = surface;
    }
    d->frame = wl_surface_frame(surfaceWrapper);
    wl_callback_add_listener(d->frame, &kFrameListener, d);
    feedbacks.push_back(d);
    return d;
}

void RenderState::dispatchWayland()
{
    if (queue) {
        std::lock_guard g(lock);
        wl_display_dispatch_queue_pending(wlDisplay, queue);
    }
}

GLuint RenderState::prepare(const Scanout &s, bool *imported)
{
    *imported = false;
    if (s.kind != Scanout::Dmabuf) {
        QSize size(int(s.width), int(s.height));
        if (!shmTexture || shmSize != size) {
            if (!shmTexture) {
                f->glGenTextures(1, &shmTexture);
                f->glGenFramebuffers(1, &shmFbo);
            }
            f->glBindTexture(GL_TEXTURE_2D, shmTexture);
            f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size.width(), size.height(), 0, GL_BGRA,
                            GL_UNSIGNED_BYTE, nullptr);
            f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            f->glBindFramebuffer(GL_READ_FRAMEBUFFER, shmFbo);
            f->glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                      shmTexture, 0);
            shmSize = size;
        }
        return shmFbo;
    }
    if (s.planes.empty() || s.planes[0].fd < 0) {
        return 0;
    }

    // The same guest buffer comes back with a new fd at every page flip:
    // recognise it by its dma-buf inode and reuse the EGLImage.
    struct stat st{};
    fstat(s.planes[0].fd, &st);
    const auto &p0 = s.planes[0];
    for (auto &c : cache) {
        if (c.ino == st.st_ino && c.dev == st.st_dev && c.fourcc == s.fourcc &&
            c.width == s.backingWidth && c.height == s.backingHeight && c.stride == p0.stride &&
            c.offset == p0.offset && c.modifier == s.modifier) {
            c.lastUse = ++useCounter;
            return c.fbo;
        }
    }

    EGLint a[64];
    int i = 0;
    a[i++] = EGL_WIDTH;
    a[i++] = EGLint(s.backingWidth);
    a[i++] = EGL_HEIGHT;
    a[i++] = EGLint(s.backingHeight);
    a[i++] = EGL_LINUX_DRM_FOURCC_EXT;
    a[i++] = EGLint(s.fourcc);
    static const EGLint planeAttr[4][5] = {
        {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE0_PITCH_EXT,
         EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT},
        {EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
         EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT},
        {EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
         EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT},
        {EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT,
         EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT},
    };
    for (size_t pl = 0; pl < s.planes.size() && pl < 4; pl++) {
        a[i++] = planeAttr[pl][0];
        a[i++] = s.planes[pl].fd;
        a[i++] = planeAttr[pl][1];
        a[i++] = EGLint(s.planes[pl].offset);
        a[i++] = planeAttr[pl][2];
        a[i++] = EGLint(s.planes[pl].stride);
        if (s.modifier != DRM_FORMAT_MOD_INVALID) {
            a[i++] = planeAttr[pl][3];
            a[i++] = EGLint(s.modifier & 0xffffffff);
            a[i++] = planeAttr[pl][4];
            a[i++] = EGLint(s.modifier >> 32);
        }
    }
    a[i++] = EGL_NONE;

    EGLImageKHR image = createImage(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, a);
    if (image == EGL_NO_IMAGE_KHR) {
        qWarning("eglCreateImageKHR(dmabuf %ux%u fourcc %08x mod %llx) failed: 0x%x",
                 s.backingWidth, s.backingHeight, s.fourcc, (unsigned long long)s.modifier,
                 eglGetError());
        return 0;
    }
    CachedImage c;
    c.dev = st.st_dev;
    c.ino = st.st_ino;
    c.fourcc = s.fourcc;
    c.width = s.backingWidth;
    c.height = s.backingHeight;
    c.stride = p0.stride;
    c.offset = p0.offset;
    c.modifier = s.modifier;
    c.image = image;
    c.lastUse = ++useCounter;
    f->glGenTextures(1, &c.texture);
    f->glBindTexture(GL_TEXTURE_2D, c.texture);
    imageTargetTexture(GL_TEXTURE_2D, image);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    f->glGenFramebuffers(1, &c.fbo);
    f->glBindFramebuffer(GL_READ_FRAMEBUFFER, c.fbo);
    f->glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                              c.texture, 0);

    // Bounded cache: evict the least recently used buffer.
    if (cache.size() >= 6) {
        auto lru = std::min_element(cache.begin(), cache.end(), [](auto &x, auto &y) {
            return x.lastUse < y.lastUse;
        });
        f->glDeleteFramebuffers(1, &lru->fbo);
        f->glDeleteTextures(1, &lru->texture);
        destroyImage(dpy, lru->image);
        cache.erase(lru);
    }
    cache.push_back(c);
    *imported = true;
    return c.fbo;
}

void RenderState::upload(const Scanout &s, const uint8_t *pixels, uint32_t stride)
{
    if (s.pixmanFormat != PIXMAN_x8r8g8b8 && s.pixmanFormat != PIXMAN_a8r8g8b8) {
        static bool warned;
        if (!warned) {
            qWarning("2D scanout format %08x not handled by the PoC", s.pixmanFormat);
            warned = true;
        }
        return;
    }
    f->glBindTexture(GL_TEXTURE_2D, shmTexture);
    f->glPixelStorei(GL_UNPACK_ROW_LENGTH, int(stride / 4));
    f->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, int(s.width), int(s.height), GL_BGRA,
                       GL_UNSIGNED_BYTE, pixels);
    f->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

void RenderState::draw(const Scanout *s, GLuint fbo, QSize view, GLuint target)
{
    // The window's framebuffer has row 0 at the bottom; a buffer of our own
    // (target) has it at the top, as the compositor shows it.
    const bool topDown = target != 0;
    f->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target ? target : ctx->defaultFramebufferObject());
    f->glViewport(0, 0, view.width(), view.height());
    QRect dst = s ? fitRect(view, QSize(int(s->width), int(s->height))) : QRect();
    if (!fbo || dst.size() != view) {
        // letterbox (only then: a full-screen clear costs bandwidth at 4K)
        f->glClearColor(0, 0, 0, 1);
        f->glClear(GL_COLOR_BUFFER_BIT);
    }
    if (!fbo || dst.isEmpty()) {
        return;
    }
    // Memory row 0 is the top of the image unless y0_top (same rule as QEMU's egl_fb_blit).
    bool flip = (s->kind != Scanout::Dmabuf || !s->y0Top) != topDown;
    int sx0 = int(s->x), sx1 = int(s->x + s->width);
    int sy0 = flip ? int(s->y + s->height) : int(s->y);
    int sy1 = flip ? int(s->y) : int(s->y + s->height);
    int dy0 = topDown ? dst.top() : view.height() - dst.bottom() - 1;
    int dy1 = topDown ? dst.bottom() + 1 : view.height() - dst.top();
    f->glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    ef->glBlitFramebuffer(sx0, sy0, sx1, sy1, dst.left(), dy0, dst.right() + 1, dy1,
                          GL_COLOR_BUFFER_BIT,
                          dst.size() == QSize(int(s->width), int(s->height)) ? GL_NEAREST
                                                                              : GL_LINEAR);
}

void RenderState::destroyCache()
{
    for (auto &c : cache) {
        f->glDeleteFramebuffers(1, &c.fbo);
        f->glDeleteTextures(1, &c.texture);
        destroyImage(dpy, c.image);
    }
    cache.clear();
    if (shmTexture) {
        f->glDeleteFramebuffers(1, &shmFbo);
        f->glDeleteTextures(1, &shmTexture);
        shmTexture = shmFbo = 0;
    }
}
} // namespace

Renderer::Renderer(QWindow *window, FrameMailbox *mailbox, Stats *stats, const Options &opts)
    : m_window(window), m_mb(mailbox), m_stats(stats), m_opts(opts)
{
}

Renderer::~Renderer()
{
    stop();
}

void Renderer::setExposed(bool exposed)
{
    m_exposed = exposed;
    requestRedraw();
}

void Renderer::setWayland(wl_display *display, wl_surface *surface)
{
    if (m_target) {
        return; // the target's surface, not the window's
    }
    m_wlDisplay = display;
    m_wlSurface = surface;
}

void Renderer::setRenderTarget(RenderTarget *target)
{
    m_target = target;
    if (target) {
        m_wlDisplay = target->display();
        m_wlSurface = target->surface();
    }
}

void Renderer::setPresentationSink(GDBusConnection *conn, const QByteArray &consolePath)
{
    m_sinkPath = consolePath;
    m_sinkConn = conn;
}

void Renderer::requestRedraw()
{
    {
        std::lock_guard g(m_mb->lock);
        m_mb->dirtyView = true;
    }
    m_mb->cond.notify_one();
}

void Renderer::stop()
{
    {
        std::lock_guard g(m_mb->lock);
        m_mb->quit = true;
    }
    m_mb->cond.notify_one();
    wait();
}

void Renderer::run()
{
    QOpenGLContext ctx;
    ctx.setFormat(m_window->requestedFormat());
    if (!ctx.create()) {
        Q_EMIT failed(QStringLiteral("cannot create an OpenGL context"));
        return;
    }
    while (!m_exposed) { // the window must be mapped before the first makeCurrent
        std::unique_lock lk(m_mb->lock);
        if (m_mb->quit) {
            return;
        }
        m_mb->cond.wait_for(lk, std::chrono::milliseconds(20));
    }
    QSize targetSize; // with a target: the size of its surface
    auto makeCurrent = [&] {
        return m_target ? m_target->makeCurrent(&ctx, &targetSize) : ctx.makeCurrent(m_window);
    };
    if (!makeCurrent()) {
        Q_EMIT failed(QStringLiteral("cannot make the OpenGL context current"));
        return;
    }
    RenderState st(m_stats);
    QString err;
    if (!st.init(&ctx, &err)) {
        Q_EMIT failed(err);
        return;
    }
    st.sinkConn = m_sinkConn;
    st.sinkPath = m_sinkPath;
    // zero copy, when this QEMU knows Presentation.Released (the qemu-gui fork)
    if (m_opts.zeroCopy && m_sinkConn && !m_sinkPath.isEmpty()) {
        GVariant *r = g_dbus_connection_call_sync(
            m_sinkConn, nullptr, m_sinkPath.constData(), "org.freedesktop.DBus.Introspectable",
            "Introspect", nullptr, G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr,
            nullptr);
        if (r) {
            const char *xml = nullptr;
            g_variant_get(r, "(&s)", &xml);
            st.zcSupported = xml && strstr(xml, "name=\"Released\"");
            g_variant_unref(r);
        }
        fprintf(stderr, "renderer: zero copy %s\n",
                st.zcSupported ? "on" : "off (this QEMU has no Presentation.Released)");
    }
    std::unique_ptr<Swapchain> own;
    fprintf(stderr, "renderer: presentation feedback %d, wayland display %p surface %p, own swapchain %d\n",
            m_opts.presentationFeedback, static_cast<void *>(m_wlDisplay),
            static_cast<void *>(m_wlSurface), m_opts.ownSwapchain);
    if (m_opts.presentationFeedback && m_wlDisplay) {
        if (m_opts.ownSwapchain && m_wlSurface) {
            st.initWaylandQueue(m_wlDisplay);
            own = std::make_unique<Swapchain>(m_wlDisplay, st.queue, m_wlSurface, &st.lock);
            st.swapchain = own.get();
        }
        st.initWayland(m_wlDisplay);
        if (own) {
            QString why;
            if (!own->init(st.dpy, st.f, &why)) {
                fprintf(stderr, "swapchain: %s: Qt's EGL window presents\n", qPrintable(why));
                st.swapchain = nullptr;
                own.reset();
            }
        }
    }
    // the fences of the copies of buffers QEMU holds, without Wayland too
    if (st.zcSupported) {
        st.startReader();
    }
    EGLDisplay eglDpy = st.dpy;

    uint64_t drawnUpdate = 0, seenScanout = 0;
    int lastScanout = -1;
    // the scanout vote as last seen, and since when (see below)
    int votedScanout = -1;
    int64_t votedSinceNs = 0;
    std::shared_ptr<Scanout> scanout;
    GLuint fbo = 0;
    for (;;) {
        st.dispatchWayland();
        std::unique_lock lk(m_mb->lock);
        // poll the compositor's feedback often while replies wait on it
        auto timeout = std::chrono::milliseconds(
            m_opts.replyOnPresent ? 1 : 50);
        m_mb->cond.wait_for(lk, timeout, [&] {
            return m_mb->quit || m_mb->updateSerial != drawnUpdate ||
                   m_mb->scanoutSerial != seenScanout || m_mb->dirtyView ||
                   !m_mb->deferredReplies.empty();
        });
        if (m_mb->quit) {
            break;
        }
        const bool newScanout = m_mb->scanoutSerial != seenScanout;
        const uint64_t update = m_mb->updateSerial;
        const int64_t recvNs = m_mb->updateRecvNs;
        bool redraw = update != drawnUpdate || m_mb->dirtyView;
        m_mb->dirtyView = false;
        seenScanout = m_mb->scanoutSerial;
        scanout = m_mb->scanout;
        auto deferred = std::move(m_mb->deferredReplies);
        m_mb->deferredReplies.clear();
        auto updated = std::move(m_mb->updated);
        m_mb->updated.clear();
        uint64_t drawnInode = 0;    // the buffer this pass shows (released by its own path)

        bool imported = false;
        if (newScanout) {
            // import now; draw with the UpdateDMABUF/UpdateMap that follows
            // (Image scanouts carry pixels and bumped updateSerial themselves)
            fbo = scanout ? st.prepare(*scanout, &imported) : 0;
            if (!scanout) {
                redraw = true; // disabled: show black
            }
        }
        if (scanout && scanout->kind == Scanout::Image && redraw) {
            st.upload(*scanout, scanout->pixels.data(), scanout->stride); // under the lock
        }
        lk.unlock();

        st.dispatchWayland();
        if (scanout && scanout->kind == Scanout::Map && redraw) {
            st.upload(*scanout, static_cast<const uint8_t *>(scanout->map) + scanout->mapOffset,
                      scanout->stride);
        }

        if (redraw && m_exposed) {
            makeCurrent();
            // the EGL surface size is authoritative (fractional scales round there)
            EGLint w = 0, h = 0;
            if (m_target) {
                w = targetSize.width();
                h = targetSize.height();
            } else {
                EGLSurface surf = eglGetCurrentSurface(EGL_DRAW);
                eglQuerySurface(eglDpy, surf, EGL_WIDTH, &w);
                eglQuerySurface(eglDpy, surf, EGL_HEIGHT, &h);
            }
            // zero copy: the guest's buffer itself, when it fills the view 1:1
            GuestBuffer *gb = nullptr;
            if (st.zcSupported && scanout && scanout->kind == Scanout::Dmabuf &&
                scanout->inode && scanout->modifier != DRM_FORMAT_MOD_INVALID &&
                !scanout->y0Top && !scanout->x && !scanout->y &&
                scanout->width == scanout->backingWidth &&
                scanout->height == scanout->backingHeight &&
                int(scanout->backingWidth) == w && int(scanout->backingHeight) == h && m_wlSurface) {
                gb = st.guestBuffer(*scanout);
            }
            if (gb) {
                Feedback *fb = st.requestFeedback(m_wlSurface, recvNs, update,
                                                  m_opts.replyOnPresent ? &deferred : nullptr);
                wl_surface_attach(m_wlSurface, gb->buffer, 0, 0);
                // all of it: the compositor may not have shown the guest's previous frame
                wl_surface_damage_buffer(m_wlSurface, 0, 0, INT32_MAX, INT32_MAX);
                wl_surface_commit(m_wlSurface);
                wl_display_flush(m_wlDisplay);
                {
                    std::lock_guard g(st.lock);
                    gb->busy = true;
                    st.attached = gb;
                }
                drawnInode = gb->inode;
                // the late swap target when nothing composites it: a full-screen window of its own
                st.sendZeroCopy(m_opts.toplevel && (m_window->windowStates() & Qt::WindowFullScreen));
                const int64_t t = nowNs();
                if (fb) {
                    std::lock_guard g(st.lock);
                    if (std::find(st.feedbacks.begin(), st.feedbacks.end(), fb) != st.feedbacks.end()) {
                        fb->swapNs = t;
                    }
                }
                m_stats->frameDrawn(update != drawnUpdate ? recvNs : 0, t, imported);
                m_stats->trace('D', update, t);
                drawnUpdate = update;
                replyAll(m_stats, deferred);
                for (uint64_t ino : updated) {
                    if (ino != drawnInode) {
                        st.released(ino);   // skipped: never shown
                    }
                }
                st.dispatchWayland();
                continue;
            }
            bool qtSwap = true;
            Swapchain::Buffer *buf = own ? own->acquire(w, h, &qtSwap) : nullptr;
            if (!qtSwap && !buf) {
                // every buffer is with the compositor: the frame stays in the guest's
                replyAll(m_stats, deferred);
                for (uint64_t ino : updated) {
                    if (!st.attached || ino != st.attached->inode) {
                        st.released(ino);
                    }
                }
                drawnUpdate = update;
                continue;
            }
            st.draw(scanout.get(), fbo, QSize(w, h), buf ? buf->fbo : 0);
            // zero copy negotiated: the buffer goes back once the copy is done
            if (st.zcSupported && scanout && scanout->kind == Scanout::Dmabuf &&
                scanout->inode && scanout->modifier != DRM_FORMAT_MOD_INVALID) {
                drawnInode = scanout->inode;
                if (!st.attached || st.attached->inode != drawnInode) {
                    st.releaseAfterGpu(drawnInode);
                }
            }
            Feedback *fb = st.requestFeedback(m_wlSurface, recvNs, update,
                                              m_opts.replyOnPresent ? &deferred : nullptr);
            if (st.zcSupported) {
                std::lock_guard g(st.lock);
                st.attached = nullptr;   // our own buffer replaces the guest's on the surface
            }
            st.sendZeroCopy(false);
            if (buf) {
                own->commit(buf, w, h);
                // direct scanout or not, when it changes (see Swapchain::scanout),
                // once the vote held 2 s: at its majority's edge it flips with
                // every frame
                const int sc = own->scanout();
                const int64_t votedNs = nowNs();
                if (sc != votedScanout) {
                    votedScanout = sc;
                    votedSinceNs = votedNs;
                } else if (sc >= 0 && sc != lastScanout && votedNs - votedSinceNs >= 2000000000) {
                    lastScanout = sc;
                    fprintf(stderr, "swapchain: the compositor %s the window\n",
                            sc ? "scans out (direct)" : "composites");
                }
            } else if (m_target) {
                m_target->swapBuffers(&ctx);
            } else {
                ctx.swapBuffers(m_window);
            }
            const int64_t t = nowNs();
            if (fb) {
                std::lock_guard g(st.lock);
                // still ours: an answer needs the compositor's next frame
                if (std::find(st.feedbacks.begin(), st.feedbacks.end(), fb) != st.feedbacks.end()) {
                    fb->swapNs = t;
                }
            }
            m_stats->frameDrawn(update != drawnUpdate ? recvNs : 0, t, imported);
            m_stats->trace('D', update, t);
        }
        // not shown: Qt's hide took the guest's buffer off the window's
        // surface, nothing does off a target's - the compositor would keep it,
        // and QEMU the guest's flushes of it until they time out
        if (!m_exposed && m_target && st.attached) {
            wl_surface_attach(m_wlSurface, nullptr, 0, 0);
            wl_surface_commit(m_wlSurface);
            wl_display_flush(m_wlDisplay);
            std::lock_guard g(st.lock);
            st.attached = nullptr; // released when the compositor lets it go
        }
        drawnUpdate = update;
        // zero copy: the buffers of the updates this pass did not show
        for (uint64_t ino : updated) {
            if (ino != drawnInode && (!st.attached || ino != st.attached->inode)) {
                st.released(ino);
            }
        }

        // --late-reply: QEMU (and the guest's GPU queue) waited for this.
        // (--reply-on-present handed them to the feedback; anything left,
        // e.g. while not exposed, is answered now.)
        replyAll(m_stats, deferred);
        st.dispatchWayland();
    }

    // answer whatever is left so QEMU does not wait for a dead client
    {
        std::lock_guard g(m_mb->lock);
        for (auto &[inv, recv] : m_mb->deferredReplies) {
            g_dbus_method_invocation_return_value(inv, nullptr);
        }
        m_mb->deferredReplies.clear();
    }
    makeCurrent();
    st.destroyCache();
    // the buffers' proxies go before the queue, with nobody reading it
    st.stopReaderThread();
    st.destroyGuests();
    st.swapchain = nullptr;
    own.reset();
    st.finiWayland();
    ctx.doneCurrent();
    if (m_target) {
        m_target->done(&ctx);
    }
}
