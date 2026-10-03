// Hand-off between the D-Bus listener thread (which answers QEMU at once)
// and the render thread (which draws the newest frame at its own pace).
#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

typedef struct _GDBusMethodInvocation GDBusMethodInvocation;

struct Scanout {
    enum Kind { Dmabuf, Image, Map } kind = Dmabuf;
    uint32_t width = 0, height = 0;           // visible rectangle
    uint32_t x = 0, y = 0;                    // its offset in the backing buffer
    uint32_t backingWidth = 0, backingHeight = 0;
    // Dmabuf
    struct Plane { int fd = -1; uint32_t offset = 0, stride = 0; };
    std::vector<Plane> planes;
    uint32_t fourcc = 0;
    uint64_t modifier = 0;
    bool y0Top = false;
    uint64_t inode = 0;                       // the dma-buf's inode: QEMU's token for zero copy
    // Image (pixels sent in the message) and Map (shared memory)
    uint32_t stride = 0, pixmanFormat = 0;
    std::vector<uint8_t> pixels;              // Image, guarded by the mailbox lock
    void *map = nullptr;                      // Map
    size_t mapLength = 0;
    uint32_t mapOffset = 0;

    Scanout() = default;
    Scanout(const Scanout &) = delete;
    Scanout &operator=(const Scanout &) = delete;
    ~Scanout()
    {
        for (auto &p : planes) {
            if (p.fd >= 0) {
                close(p.fd);
            }
        }
        if (map) {
            munmap(map, mapLength);
        }
    }
};

struct FrameMailbox {
    std::mutex lock;
    std::condition_variable cond;
    std::shared_ptr<Scanout> scanout;   // current scanout, or null (disabled)
    uint64_t scanoutSerial = 0;
    uint64_t updateSerial = 0;          // one per Update*/Scanout* message
    int64_t updateRecvNs = 0;           // receive time of the newest update
    // zero copy: the buffers (inodes) of the UpdateDMABUF calls since the
    // render thread last looked; each is released (Presentation.Released) once done
    std::vector<uint64_t> updated;
    // --late-reply: UpdateDMABUF calls answered after presenting (with receive time)
    std::vector<std::pair<GDBusMethodInvocation *, int64_t>> deferredReplies;
    bool dirtyView = false;             // resize/expose: redraw without a new frame
    bool quit = false;
};
