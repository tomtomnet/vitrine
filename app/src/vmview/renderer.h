// Render thread: imports the guest's scanout dmabufs as EGLImages (zero-copy),
// blits the newest frame into the display window's own Wayland surface, and
// asks the compositor when each frame really reached the screen.
#pragma once

#include "common.h"
#include "framemailbox.h"

#include <QRect>
#include <QThread>
#include <atomic>

class QWindow;
class Stats;
struct wl_display;
struct wl_surface;
typedef struct _GDBusConnection GDBusConnection;

// Where a width x height image lands in a view (letterboxed, top-left origin).
QRect fitRect(QSize view, QSize image);

class Renderer : public QThread
{
    Q_OBJECT
public:
    Renderer(QWindow *window, FrameMailbox *mailbox, Stats *stats, const Options &opts);
    ~Renderer() override;

    void setExposed(bool exposed);
    void setWayland(wl_display *display, wl_surface *surface);
    // Where to tell that a frame reached the screen: the console's
    // org.qemu.Display1.Presentation, on the control connection
    void setPresentationSink(GDBusConnection *conn, const QByteArray &consolePath);
    void requestRedraw();
    void stop();

Q_SIGNALS:
    void failed(const QString &message);

protected:
    void run() override;

private:
    QWindow *m_window;
    FrameMailbox *m_mb;
    Stats *m_stats;
    Options m_opts;
    std::atomic<bool> m_exposed{false};
    std::atomic<wl_display *> m_wlDisplay{nullptr};
    std::atomic<wl_surface *> m_wlSurface{nullptr};
    std::atomic<GDBusConnection *> m_sinkConn{nullptr};
    QByteArray m_sinkPath;
};
