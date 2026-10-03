// org.qemu.Display1.Listener on its own thread and GLib main context.
//
// QEMU keeps the guest's GPU command queue blocked from UpdateDMABUF until
// our reply (qemu_console_hw_gl_block), so the reply goes out as soon as the
// message is parsed, before any drawing, unless --late-reply asks otherwise.
#pragma once

#include "common.h"
#include "framemailbox.h"

#include <QImage>
#include <QObject>
#include <atomic>
#include <functional>
#include <thread>

typedef struct _GMainContext GMainContext;
typedef struct _GMainLoop GMainLoop;
typedef struct _GDBusConnection GDBusConnection;
typedef struct _GVariant GVariant;
class Stats;

class Listener
{
public:
    struct Callbacks {
        std::function<void(QImage cursor, int hotX, int hotY)> cursorDefine;
        std::function<void(int x, int y, bool visible)> mouseSet;
        std::function<void(uint32_t w, uint32_t h)> scanoutSize;
    };

    Listener(FrameMailbox *mailbox, Stats *stats, const Options &opts, Callbacks cb);
    ~Listener();

    // Starts the thread; returns the fd to pass to Console.RegisterListener.
    int start(QString *err);
    void stop();

private:
    void run(int fd);
    void handleCall(const char *method, GVariant *params, GDBusMethodInvocation *inv);
    void publish(std::shared_ptr<Scanout> scanout, int64_t recvNs);
    void update(int64_t recvNs, GDBusMethodInvocation *inv);

    static void methodCall(GDBusConnection *, const char *, const char *, const char *,
                           const char *, GVariant *, GDBusMethodInvocation *, void *);
    static GVariant *getProperty(GDBusConnection *, const char *, const char *,
                                 const char *, const char *, struct _GError **, void *);

    FrameMailbox *m_mb;
    Stats *m_stats;
    Options m_opts;
    Callbacks m_cb;
    std::thread m_thread;
    GMainContext *m_ctx = nullptr;
    GMainLoop *m_loop = nullptr;
    GDBusConnection *m_conn = nullptr;
    uint64_t m_serial = 0;
    uint32_t m_lastW = 0, m_lastH = 0;
    // GDBus hop measurement (filter runs in GDBus's worker thread)
    std::atomic<int64_t> m_workerIn{0};    // UpdateDMABUF read from the socket
    std::atomic<int64_t> m_replyAt{0};     // our reply handed to GDBus
    std::atomic<uint32_t> m_replySerial{0};
    static struct _GDBusMessage *filter(GDBusConnection *, struct _GDBusMessage *, int, void *);
};
