// Small helpers shared by the threads of the PoC.
#pragma once

#include <cstdint>
#include <ctime>

// CLOCK_MONOTONIC in ns: the clock QEMU's TEST ONLY trace and the Wayland
// presentation feedback (KWin) use, so timestamps compare across processes.
inline int64_t nowNs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

struct Options {
    bool vsync = false;          // swap interval 1 instead of 0 (0 = like QEMU's SDL display)
    bool lateReply = false;      // reply to UpdateDMABUF after our swap
    bool replyOnPresent = false; // reply once the compositor showed the frame (what a
                                 // frame-clock-driven client like rdw amounts to)
    bool sendRefreshRate = true; // report the host refresh rate through UIInfo.Apply
    bool asyncUpdate = true;     // advertise Listener.Unix.AsyncUpdate (fork patch 0002)
    bool zeroCopy = true;        // advertise Listener.Unix.ZeroCopy: attach the guest's buffers as they are
    double refreshOverride = 0;  // Hz, 0 = the screen's
    int guestWidth = 0;          // fixed guest mode, 0 = follow the window
    int guestHeight = 0;
    int windowWidth = 0;         // initial window size in logical pixels
    int windowHeight = 0;
    bool presentationFeedback = true;
    bool ownSwapchain = true;  // the window's own buffers through linux-dmabuf (swapchain.h)
    bool gl = true;              // -display dbus,gl=on (dmabuf) or gl=off (shared memory)
    double duration = 0;         // quit after N seconds (scripted runs)
    int inputTestHz = 0;         // synthetic absolute mouse moves per second
    bool fullscreen = false;
    double selfTestInput = 0;    // seconds after start: synthetic key/mouse events
    double grabTest = 0;         // seconds after start: engage the keyboard/mouse grab
    bool toplevel = false;       // display in its own top-level window, not a subsurface
    const char *logPath = nullptr;
    const char *tracePath = nullptr;
    const char *renderNode = nullptr;
};
