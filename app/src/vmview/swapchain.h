// The window's own buffers, attached through linux-dmabuf.
//
// Qt's EGL window takes its buffers from Mesa, which frees a buffer unused
// for twenty frames and allocates one again when the compositor holds
// three: a flip that comes a refresh late makes it hold three, and the first
// scanout of a new buffer waits for the kernel to move it into contiguous
// memory (2.8 ms for a 4K buffer on amdgpu), which is a flip a refresh late.
// Frames then repeat on the screen in a chain (QEMU's SDL window had the
// same, ui/sdl2-swapchain.c is the original of this).
//
// Here four GBM buffers are made once, on the render device of the EGL
// display, with the modifiers the compositor offers for the surface (its
// scanout tranche when it names one), each imported into GL as a
// framebuffer to blit the guest into, and attached to the window's surface
// through linux-dmabuf.  A buffer is used again once the compositor released
// it.  The first frames go through Qt's swap, which maps the window.
// Anything missing leaves the window with Qt's swap; --no-swapchain does too.
#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <QOpenGLFunctions>
#include <QString>
#include <wayland-client.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

struct gbm_device;
struct gbm_bo;
struct zwp_linux_dmabuf_v1;
struct zwp_linux_dmabuf_feedback_v1;

class Swapchain
{
public:
    static constexpr int kBuffers = 4;
    static constexpr int kQtPresentsFirst = 2;

    struct Buffer {
        gbm_bo *bo = nullptr;
        wl_buffer *buffer = nullptr;
        EGLImageKHR image = EGL_NO_IMAGE_KHR;
        GLuint texture = 0, fbo = 0;
        std::atomic<bool> busy{false}; // with the compositor
        Swapchain *owner = nullptr;
    };

    // The compositor's events (feedback, releases) arrive on @queue, which is
    // dispatched under @lock (by the render thread or its reader thread).
    Swapchain(wl_display *display, wl_event_queue *queue, wl_surface *surface, std::mutex *lock);
    ~Swapchain();

    // On the render thread with the GL context current.  False: use Qt's swap.
    bool init(EGLDisplay dpy, QOpenGLFunctions *f, QString *why);
    // A frame of @width x @height is about to be drawn: the buffer to draw
    // into (its framebuffer, row 0 the top), or null when Qt should swap this
    // one (the first ones) or when every buffer is with the compositor.
    Buffer *acquire(int width, int height, bool *qtSwap);
    // The buffer drawn: attach, damage, commit, flush.
    void commit(Buffer *b, int width, int height);
    // Whether the compositor puts the buffers straight on the display plane:
    // 1 direct scanout, 0 composited, -1 unknown (no own buffers, or no run
    // of frames seen yet).  It does not tell (KWin's presentation feedback
    // never says zero-copy, its scanout tranche only comes when a buffer
    // failed its checks); what shows it is how long it keeps a buffer: on the
    // display plane one stays with it until the flip away from it, so between
    // two frames of a run it holds two of ours, composited one (released as
    // soon as it rendered from the next).  Counted from the third frame of a
    // run of frames each within a refresh and a half of the previous, over
    // the last eight such frames.
    int scanout() const;
    void setRefreshNs(int64_t ns) { m_refreshNs = ns; }
    // The compositor's linux-dmabuf global, bound by the caller's registry listener.
    void setDmabuf(zwp_linux_dmabuf_v1 *dmabuf) { m_dmabuf = dmabuf; }
    zwp_linux_dmabuf_v1 *dmabuf() const { return m_dmabuf; }

    // feedback events (public for the C listeners)
    void feedbackTable(int fd, uint32_t size);
    void feedbackTrancheFormats(wl_array *indices);
    void feedbackTrancheFlags(uint32_t flags);
    void feedbackTrancheDone();
    void feedbackDone();
    void bufferReleased(Buffer *b) { b->busy = false; }

private:
    void vote();
    bool createBuffers(int width, int height);
    bool createBuffer(Buffer *b, int width, int height);
    void destroyBuffer(Buffer *b);
    void destroyBuffers();

    wl_display *m_display;
    wl_event_queue *m_queue;
    wl_surface *m_surface;
    std::mutex *m_lock;
    zwp_linux_dmabuf_v1 *m_dmabuf = nullptr;
    zwp_linux_dmabuf_feedback_v1 *m_feedback = nullptr;
    EGLDisplay m_dpy = EGL_NO_DISPLAY;
    QOpenGLFunctions *m_f = nullptr;
    PFNEGLCREATEIMAGEKHRPROC m_createImage = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC m_destroyImage = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC m_imageTargetTexture = nullptr;
    int m_drmFd = -1;
    gbm_device *m_gbm = nullptr;
    // the compositor's format table and the modifiers for XRGB8888
    void *m_table = nullptr;
    size_t m_tableSize = 0;
    std::vector<uint64_t> m_modifiers, m_tranche;
    bool m_trancheScanout = false, m_chosenScanout = false;
    bool m_feedbackDone = false, m_feedbackChanged = false;
    Buffer m_bufs[kBuffers];
    int m_width = 0, m_height = 0;
    int m_presents = 0;
    bool m_failed = false;
    // the scanout vote
    int64_t m_refreshNs = 0;
    int64_t m_committedNs = 0;
    int m_inARow = 0;
    uint8_t m_heldVotes = 0;
    int m_nVotes = 0;
};
