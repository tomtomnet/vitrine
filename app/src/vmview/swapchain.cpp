#include "swapchain.h"

#include <algorithm>

#include "linux-dmabuf-v1-client-protocol.h"

#include <QDebug>
#include <cstdio>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <sys/sysmacros.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#ifndef EGL_DRM_RENDER_NODE_FILE_EXT
#define EGL_DRM_RENDER_NODE_FILE_EXT 0x3377
#endif

namespace {
constexpr uint32_t kFormat = DRM_FORMAT_XRGB8888;
constexpr size_t kMaxModifiers = 64;

struct FormatEntry {
    uint32_t format;
    uint32_t pad;
    uint64_t modifier;
};

const zwp_linux_dmabuf_feedback_v1_listener kFeedbackListener = {
    [](void *data, zwp_linux_dmabuf_feedback_v1 *) {
        static_cast<Swapchain *>(data)->feedbackDone();
    },
    [](void *data, zwp_linux_dmabuf_feedback_v1 *, int32_t fd, uint32_t size) {
        static_cast<Swapchain *>(data)->feedbackTable(fd, size);
    },
    [](void *, zwp_linux_dmabuf_feedback_v1 *, wl_array *dev) {
        if (getenv("EMBED_POC_DEBUG_FEEDBACK") && dev->size >= sizeof(dev_t)) {
            dev_t d;
            memcpy(&d, dev->data, sizeof(d));
            fprintf(stderr, "feedback: main device %u:%u\n", major(d), minor(d));
        }
    }, // main_device
    [](void *data, zwp_linux_dmabuf_feedback_v1 *) {
        static_cast<Swapchain *>(data)->feedbackTrancheDone();
    },
    [](void *, zwp_linux_dmabuf_feedback_v1 *, wl_array *) {}, // tranche_target_device
    [](void *data, zwp_linux_dmabuf_feedback_v1 *, wl_array *indices) {
        static_cast<Swapchain *>(data)->feedbackTrancheFormats(indices);
    },
    [](void *data, zwp_linux_dmabuf_feedback_v1 *, uint32_t flags) {
        static_cast<Swapchain *>(data)->feedbackTrancheFlags(flags);
    },
};

const wl_callback_listener kSyncListener = {
    [](void *data, wl_callback *cb, uint32_t) {
        *static_cast<bool *>(data) = true;
        wl_callback_destroy(cb);
    },
};

const wl_buffer_listener kBufferListener = {
    [](void *data, wl_buffer *) {
        auto *b = static_cast<Swapchain::Buffer *>(data);
        b->owner->bufferReleased(b);
    },
};
} // namespace

Swapchain::Swapchain(wl_display *display, wl_event_queue *queue, wl_surface *surface,
                     std::mutex *lock)
    : m_display(display), m_queue(queue), m_surface(surface), m_lock(lock)
{
    for (auto &b : m_bufs) {
        b.owner = this;
    }
}

Swapchain::~Swapchain()
{
    destroyBuffers();
    if (m_feedback) {
        zwp_linux_dmabuf_feedback_v1_destroy(m_feedback);
    }
    if (m_dmabuf) {
        zwp_linux_dmabuf_v1_destroy(m_dmabuf);
    }
    if (m_table) {
        munmap(m_table, m_tableSize);
    }
    if (m_gbm) {
        gbm_device_destroy(m_gbm);
    }
    if (m_drmFd >= 0) {
        close(m_drmFd);
    }
}

// --- the compositor's formats (on the private queue, under the lock) ------

static bool debugFeedback()
{
    static int on = -1;
    if (on < 0) {
        on = getenv("EMBED_POC_DEBUG_FEEDBACK") != nullptr;
    }
    return on;
}

void Swapchain::feedbackTable(int fd, uint32_t size)
{
    if (debugFeedback()) {
        fprintf(stderr, "feedback: format table fd %d size %u\n", fd, size);
    }
    if (m_table) {
        munmap(m_table, m_tableSize);
        m_table = nullptr;
    }
    void *p = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    m_table = p == MAP_FAILED ? nullptr : p;
    m_tableSize = size;
    close(fd);
}

void Swapchain::feedbackTrancheFormats(wl_array *indices)
{
    m_tranche.clear();
    if (!m_table) {
        return;
    }
    const auto *entries = static_cast<const FormatEntry *>(m_table);
    const size_t n = m_tableSize / sizeof(FormatEntry);
    // (wl_array_for_each is C: its void* does not convert in C++)
    const auto *idx = static_cast<const uint16_t *>(indices->data);
    const size_t count = indices->size / sizeof(uint16_t);
    for (size_t k = 0; k < count; k++) {
        if (idx[k] < n && entries[idx[k]].format == kFormat && m_tranche.size() < kMaxModifiers) {
            m_tranche.push_back(entries[idx[k]].modifier);
        }
    }
    if (debugFeedback()) {
        fprintf(stderr, "feedback: tranche of %zu entries (table %zu), %zu XRGB8888\n", count, n,
                m_tranche.size());
    }
}

void Swapchain::feedbackTrancheFlags(uint32_t flags)
{
    m_trancheScanout = flags & ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SCANOUT;
}

void Swapchain::feedbackTrancheDone()
{
    // the scanout tranche wins; else the first with our format
    if (!m_tranche.empty() &&
        (m_modifiers.empty() || (m_trancheScanout && !m_chosenScanout))) {
        m_modifiers = m_tranche;
        m_chosenScanout = m_trancheScanout;
    }
    m_tranche.clear();
    m_trancheScanout = false;
}

void Swapchain::feedbackDone()
{
    if (debugFeedback()) {
        fprintf(stderr, "feedback: done, %zu modifiers chosen (scanout %d)\n", m_modifiers.size(),
                m_chosenScanout);
    }
    m_feedbackDone = true;
    if (!m_modifiers.empty()) {
        m_feedbackChanged = true;
    }
}

// --- setup ------------------------------------------------------------------

bool Swapchain::init(EGLDisplay dpy, QOpenGLFunctions *f, QString *why)
{
    m_dpy = dpy;
    m_f = f;
    if (!m_dmabuf) {
        *why = QStringLiteral("no linux-dmabuf v4");
        return false;
    }
    m_createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    m_destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    m_imageTargetTexture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    auto queryDisplayAttrib = reinterpret_cast<PFNEGLQUERYDISPLAYATTRIBEXTPROC>(
        eglGetProcAddress("eglQueryDisplayAttribEXT"));
    auto queryDeviceString = reinterpret_cast<PFNEGLQUERYDEVICESTRINGEXTPROC>(
        eglGetProcAddress("eglQueryDeviceStringEXT"));
    if (!m_createImage || !m_destroyImage || !m_imageTargetTexture || !queryDisplayAttrib ||
        !queryDeviceString) {
        *why = QStringLiteral("EGL lacks image import or device query");
        return false;
    }
    // the render device of the EGL display: the buffers come from it
    EGLAttrib device = 0;
    if (!queryDisplayAttrib(dpy, EGL_DEVICE_EXT, &device) || !device) {
        *why = QStringLiteral("no EGL device");
        return false;
    }
    const char *node = queryDeviceString(reinterpret_cast<EGLDeviceEXT>(device),
                                         EGL_DRM_RENDER_NODE_FILE_EXT);
    if (!node) {
        node = queryDeviceString(reinterpret_cast<EGLDeviceEXT>(device), EGL_DRM_DEVICE_FILE_EXT);
    }
    if (!node) {
        *why = QStringLiteral("the EGL device has no DRM node");
        return false;
    }
    m_drmFd = open(node, O_RDWR | O_CLOEXEC);
    if (m_drmFd < 0) {
        *why = QStringLiteral("%1: %2").arg(node, strerror(errno));
        return false;
    }
    m_gbm = gbm_create_device(m_drmFd);
    if (!m_gbm) {
        *why = QStringLiteral("no GBM device on %1").arg(node);
        return false;
    }
    // the modifiers the compositor takes for this surface.  The render thread's
    // reader thread dispatches the same queue under the lock: a round trip
    // dispatched here without it ran the feedback's events in both threads at
    // once, out of order - the tranches before the format table was mapped,
    // so no modifier was ever found (2026-10-03).  A sync point instead, its
    // events dispatched under the lock.
    bool synced = false;
    {
        std::lock_guard g(*m_lock);
        m_feedback = zwp_linux_dmabuf_v1_get_surface_feedback(m_dmabuf, m_surface);
        zwp_linux_dmabuf_feedback_v1_add_listener(m_feedback, &kFeedbackListener, this);
        auto *wrapped = static_cast<wl_display *>(wl_proxy_create_wrapper(m_display));
        wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapped), m_queue);
        wl_callback *cb = wl_display_sync(wrapped);
        wl_proxy_wrapper_destroy(wrapped);
        wl_callback_add_listener(cb, &kSyncListener, &synced);
    }
    wl_display_flush(m_display);
    for (int i = 0; i < 2000; i++) {
        {
            std::lock_guard g(*m_lock);
            wl_display_dispatch_queue_pending(m_display, m_queue);
            if (synced) {
                break;
            }
        }
        usleep(500);
    }
    std::lock_guard g(*m_lock);
    if (!m_feedbackDone || m_modifiers.empty()) {
        *why = QStringLiteral("the compositor offers no modifier for XRGB8888");
        return false;
    }
    m_feedbackChanged = false;
    fprintf(stderr, "swapchain: buffers from %s, %zu modifiers, scanout tranche %d\n", node,
            m_modifiers.size(), m_chosenScanout);
    return true;
}

// --- the buffers ------------------------------------------------------------

void Swapchain::destroyBuffer(Buffer *b)
{
    if (b->buffer) {
        wl_buffer_destroy(b->buffer);
        b->buffer = nullptr;
    }
    if (b->fbo) {
        m_f->glDeleteFramebuffers(1, &b->fbo);
        b->fbo = 0;
    }
    if (b->texture) {
        m_f->glDeleteTextures(1, &b->texture);
        b->texture = 0;
    }
    if (b->image != EGL_NO_IMAGE_KHR) {
        m_destroyImage(m_dpy, b->image);
        b->image = EGL_NO_IMAGE_KHR;
    }
    if (b->bo) {
        gbm_bo_destroy(b->bo);
        b->bo = nullptr;
    }
    b->busy = false;
}

bool Swapchain::createBuffer(Buffer *b, int width, int height)
{
    b->bo = gbm_bo_create_with_modifiers2(m_gbm, width, height, kFormat, m_modifiers.data(),
                                          m_modifiers.size(),
                                          GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
    if (!b->bo) {
        b->bo = gbm_bo_create(m_gbm, width, height, kFormat,
                              GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR);
        if (!b->bo) {
            qWarning("swapchain: no GBM buffer of %dx%d", width, height);
            return false;
        }
    }
    const uint64_t modifier = gbm_bo_get_modifier(b->bo);
    int planes = gbm_bo_get_plane_count(b->bo);
    if (planes > 4) {
        planes = 4;
    }
    int fds[4] = {-1, -1, -1, -1};
    uint32_t offsets[4] = {}, strides[4] = {};
    bool ok = true;
    for (int i = 0; i < planes; i++) {
        fds[i] = gbm_bo_get_fd_for_plane(b->bo, i);
        offsets[i] = gbm_bo_get_offset(b->bo, i);
        strides[i] = gbm_bo_get_stride_for_plane(b->bo, i);
        if (fds[i] < 0) {
            qWarning("swapchain: no fd for plane %d", i);
            ok = false;
        }
    }
    if (ok) {
        std::lock_guard g(*m_lock);
        zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(m_dmabuf);
        for (int i = 0; i < planes; i++) {
            zwp_linux_buffer_params_v1_add(params, fds[i], i, offsets[i], strides[i],
                                           modifier >> 32, modifier & 0xffffffff);
        }
        b->buffer = zwp_linux_buffer_params_v1_create_immed(params, width, height, kFormat, 0);
        zwp_linux_buffer_params_v1_destroy(params);
        wl_buffer_add_listener(b->buffer, &kBufferListener, b);
    }
    if (ok) {
        // the same buffer as a GL framebuffer, to blit the guest into
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
        EGLint a[64];
        int i = 0;
        a[i++] = EGL_WIDTH;
        a[i++] = width;
        a[i++] = EGL_HEIGHT;
        a[i++] = height;
        a[i++] = EGL_LINUX_DRM_FOURCC_EXT;
        a[i++] = EGLint(kFormat);
        for (int pl = 0; pl < planes; pl++) {
            a[i++] = planeAttr[pl][0];
            a[i++] = fds[pl];
            a[i++] = planeAttr[pl][1];
            a[i++] = EGLint(offsets[pl]);
            a[i++] = planeAttr[pl][2];
            a[i++] = EGLint(strides[pl]);
            if (modifier != DRM_FORMAT_MOD_INVALID) {
                a[i++] = planeAttr[pl][3];
                a[i++] = EGLint(modifier & 0xffffffff);
                a[i++] = planeAttr[pl][4];
                a[i++] = EGLint(modifier >> 32);
            }
        }
        a[i++] = EGL_NONE;
        b->image = m_createImage(m_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, a);
        if (b->image == EGL_NO_IMAGE_KHR) {
            qWarning("swapchain: the buffer does not import into EGL: 0x%x", eglGetError());
            ok = false;
        } else {
            m_f->glGenTextures(1, &b->texture);
            m_f->glBindTexture(GL_TEXTURE_2D, b->texture);
            m_imageTargetTexture(GL_TEXTURE_2D, b->image);
            m_f->glGenFramebuffers(1, &b->fbo);
            m_f->glBindFramebuffer(GL_FRAMEBUFFER, b->fbo);
            m_f->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                        b->texture, 0);
            if (m_f->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                qWarning("swapchain: the buffer's framebuffer is incomplete");
                ok = false;
            }
            m_f->glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
    }
    // EGL and the compositor hold their own references to the planes
    for (int i = 0; i < planes; i++) {
        if (fds[i] >= 0) {
            close(fds[i]);
        }
    }
    if (!ok) {
        destroyBuffer(b);
    }
    return ok;
}

void Swapchain::destroyBuffers()
{
    for (auto &b : m_bufs) {
        destroyBuffer(&b);
    }
    m_width = m_height = 0;
}

bool Swapchain::createBuffers(int width, int height)
{
    destroyBuffers();
    for (auto &b : m_bufs) {
        if (!createBuffer(&b, width, height)) {
            destroyBuffers();
            return false;
        }
    }
    m_width = width;
    m_height = height;
    m_feedbackChanged = false;
    return true;
}

// --- a frame ----------------------------------------------------------------

Swapchain::Buffer *Swapchain::acquire(int width, int height, bool *qtSwap)
{
    *qtSwap = true;
    if (m_failed) {
        return nullptr;
    }
    if (m_presents < kQtPresentsFirst) {
        // Qt maps the window with its first swaps
        m_presents++;
        return nullptr;
    }
    bool recreate;
    {
        std::lock_guard g(*m_lock);
        wl_display_dispatch_queue_pending(m_display, m_queue);
        recreate = width != m_width || height != m_height || m_feedbackChanged;
    }
    if (recreate && !createBuffers(width, height)) {
        m_failed = true;
        destroyBuffers();
        return nullptr;
    }
    *qtSwap = false;
    // the reader thread dispatches the compositor's releases as they come;
    // one may be on its way: a moment for it
    for (int tries = 0; tries < 10; tries++) {
        for (auto &b : m_bufs) {
            if (!b.busy) {
                vote();
                return &b;
            }
        }
        wl_display_flush(m_display);
        usleep(100);
    }
    // every buffer is with the compositor: this frame stays in the guest's
    return nullptr;
}

// A frame is about to be committed and the releases are read: how many
// buffers the compositor still holds (see scanout() in the header).
void Swapchain::vote()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const int64_t now = int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
    const int64_t gap = m_refreshNs > 0 ? m_refreshNs * 3 / 2 : 25000000;
    if (m_committedNs && now - m_committedNs < gap) {
        m_inARow++;
    } else {
        m_inARow = 0;
    }
    m_committedNs = now;
    if (m_inARow < 2) {
        return;
    }
    int held = 0;
    for (const auto &b : m_bufs) {
        held += b.busy ? 1 : 0;
    }
    m_heldVotes = uint8_t(m_heldVotes << 1 | (held >= 2 ? 1 : 0));
    m_nVotes = std::min(m_nVotes + 1, 8);
}

int Swapchain::scanout() const
{
    if (m_failed || !m_width || m_nVotes < 4) {
        return -1;
    }
    int yes = 0;
    for (int i = 0; i < m_nVotes; i++) {
        yes += m_heldVotes >> i & 1;
    }
    return yes * 2 > m_nVotes ? 1 : 0;
}

void Swapchain::commit(Buffer *b, int width, int height)
{
    // the GPU work is queued before the compositor hears of the buffer
    m_f->glFlush();
    b->busy = true;
    wl_surface_attach(m_surface, b->buffer, 0, 0);
    wl_surface_damage_buffer(m_surface, 0, 0, width, height);
    wl_surface_commit(m_surface);
    wl_display_flush(m_display);
}
