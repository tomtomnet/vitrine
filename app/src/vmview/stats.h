// Per-second frame and latency statistics, fed by the listener, render and
// GUI threads.
#pragma once

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

class Stats
{
public:
    // Listener thread: a display update arrived and was answered.
    void updateReceived(int64_t recvNs, int64_t replyNs);
    void scanoutReceived();
    // Render thread.
    void frameDrawn(int64_t recvNs, int64_t swapEndNs, bool imported);
    void framePresented(int64_t recvNs, int64_t presentNs);
    void frameDiscarded();
    // GUI thread: round trip of an input call to QEMU.
    void inputRoundTrip(int64_t ns);
    // Deferred replies (--late-reply): how long QEMU waited for us.
    void replyDelay(int64_t ns);
    // GDBus internals: socket read (worker thread) -> handler, reply -> socket write.
    void gdbusHops(int64_t inNs, int64_t outNs);

    struct Summary {
        double seconds = 0;
        int updates = 0, scanouts = 0, drawn = 0, imports = 0, presented = 0, discarded = 0;
        double replyUsP50 = 0, replyUsMax = 0;       // receive -> reply (listener thread)
        double drawMsP50 = 0, drawMsP99 = 0;         // receive -> swap done
        double presentMsP50 = 0, presentMsP99 = 0;   // receive -> on screen (wp_presentation)
        double intervalMsP50 = 0, intervalMsP99 = 0; // between presented frames
        double arrivalMsP50 = 0, arrivalMsP99 = 0;   // between received updates
        int inputs = 0;
        double inputUsP50 = 0, inputUsP99 = 0;
        double lateReplyMsP50 = 0;
        double hopInUsP50 = 0, hopOutUsP50 = 0;
    };
    Summary take();

    // Per-frame trace lines (all threads), CLOCK_MONOTONIC ns.
    void openTrace(const char *path);
    void trace(char kind, uint64_t serial, int64_t a, int64_t b = 0);

private:
    std::mutex m_lock;
    int64_t m_windowStart = 0;
    int m_updates = 0, m_scanouts = 0, m_drawn = 0, m_imports = 0, m_presented = 0, m_discarded = 0;
    std::vector<int64_t> m_reply, m_draw, m_present, m_interval, m_arrival, m_input, m_late,
        m_hopIn, m_hopOut;
    int64_t m_lastPresent = 0, m_lastArrival = 0;
    FILE *m_trace = nullptr;
};
