// Render thread: imports the guest's scanout dmabufs as EGLImages (zero-copy),
// blits the newest frame into the display window's own Wayland surface, and
// asks the compositor when each frame really reached the screen.
#pragma once

#include "common.h"
#include "framemailbox.h"

#include <QRect>
#include <QThread>
#include <atomic>

class QOpenGLContext;
class QWindow;
class Stats;
struct wl_display;
struct wl_surface;
typedef struct _GDBusConnection GDBusConnection;

// A surface of the caller's own for the render thread instead of the
// window's (vitrine: a wl_surface of its own, a subsurface of the window's,
// that Qt never touches): the render thread makes its context current,
// sizes, presents through it and lets it go at its end; setWayland() then
// changes nothing.  Its calls come from the render thread.
class RenderTarget
{
public:
    virtual ~RenderTarget() = default;
    virtual wl_display *display() const = 0;
    virtual wl_surface *surface() const = 0;
    // @ctx current, drawing to the surface; @size: its size in buffer pixels
    virtual bool makeCurrent(QOpenGLContext *ctx, QSize *size) = 0;
    // what was drawn into framebuffer 0, to the surface
    virtual void swapBuffers(QOpenGLContext *ctx) = 0;
    // the thread ends: @ctx is no longer current
    virtual void done(QOpenGLContext *ctx) = 0;
};

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
    // before start()
    void setRenderTarget(RenderTarget *target);
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
    RenderTarget *m_target = nullptr;
};
