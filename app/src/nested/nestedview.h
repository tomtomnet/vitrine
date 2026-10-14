// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QPointer>
#include <QString>

#include "nested/desktopview.h"

class QTimer;
class QWidget;
class QWindow;
struct vmview;
struct wl_buffer;
struct wl_display;
struct wl_shm;
struct wl_subcompositor;
struct wl_subsurface;
struct wl_surface;

/*
 * The nested desktop in vitrine's window (NestedDesktop::Mode::Integrated):
 * serves the socket QEMU's cross-domain connects to, through the research's
 * vmview library (vmview.h), and shows the guest's window in the console
 * (widget(), the Desktop view) or alone on the screen (setFullScreen()).
 *
 * The guest's window is a subsurface of a wl_surface of vitrine's, vmview's
 * parent, itself a desync subsurface of a Qt window: the embedded one in
 * widget(), or the full-screen one, the surface moving between them.  It is
 * made again on the window's new surface when Qt replaces it (a hide, a
 * re-parenting): vmview cannot change its parent, and a parent destroyed
 * would leave the guest's window unmapped for good.  The surface's buffer
 * is one transparent pixel, which maps it; it takes no input, so that the
 * window under it keeps the pointer where the guest's window is not.
 *
 * The compositor sends the pointer over the guest's window to the guest,
 * and the keyboard of vitrine's window to it too while setInputEnabled()
 * and the view has Qt's focus (vmview_set_keyboard_focus): Qt gets the same
 * keys, which the view keeps from vitrine's widgets and shortcuts
 * meanwhile.  A click on the guest's window, which Qt never sees, comes as
 * pressed().
 *
 * vmview's callbacks run on its own thread: queued to the GUI thread.
 * Without Qt's Wayland platform there is no nested view: create() fails.
 */
class NestedView : public DesktopView
{
    Q_OBJECT

public:
    /* Serves @socket (an old socket there is replaced); null with @error */
    static NestedView *create(const QString &socket, QString *error, QObject *parent = nullptr);
    /* Stops serving: the guest's window is closed first (vmview_destroy()), which
       ends its session, as its nested KWin quits */
    ~NestedView() override;

    QString socket() const override;
    QWidget *widget() const override;
    bool isFullScreen() const override;
    void setFullScreen(bool on) override;
    void setInputEnabled(bool on) override;
    bool hasKeyboard() const override;
    void focus() override;
    int connections() const override;
    bool isMapped() const override;
    /* The guest's window's title: its session's live numbers */
    QString title() const;

Q_SIGNALS:
    void titleChanged(const QString &title);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    explicit NestedView(QObject *parent);
    bool init(const QString &socket, QString *error);
    /* The window the surface hangs from now */
    QWindow *hostWindow() const;
    void attachToHost();
    void detach();
    void watchPlatformWindow(QWindow *window);
    /* The guest's window's place and size, from the host window's */
    void updateGeometry();
    /* The surface's commit, for the guest's window's place to apply */
    void commitSurface();
    void updateKeyboard();

    vmview *m_view = nullptr;
    QString m_socket;
    wl_display *m_display = nullptr;
    wl_subcompositor *m_subcompositor = nullptr;
    wl_shm *m_shm = nullptr;
    wl_surface *m_surface = nullptr;    // vmview's parent
    wl_buffer *m_pixel = nullptr;       // its buffer
    wl_subsurface *m_subsurface = nullptr;
    wl_surface *m_parent = nullptr;     // the host window's, under m_subsurface
    QObject *m_platformWatch = nullptr;

    QPointer<QWidget> m_host;           // widget()
    QWidget *m_container = nullptr;     // embeds m_window, in m_host
    QWindow *m_window = nullptr;        // the embedded window
    QWindow *m_fullWindow = nullptr;    // full screen, while it is
    QTimer *m_commitAgain = nullptr;
    bool m_inputEnabled = true;
    bool m_keyboard = false;
    int m_connections = 0;
    bool m_mapped = false;
    QString m_title;
};
