// The VM display: a native QWindow (a Wayland subsurface once embedded with
// QWidget::createWindowContainer) that the render thread draws into directly,
// so no Qt widget compositing or extra copy sits between guest and screen.
// Input events are forwarded to QEMU from here, on the GUI thread.
//
// Keys: on Wayland the compositor gives keyboard focus to the top-level
// surface, never to a subsurface, so an embedded display gets no key events
// of its own. MainWindow forwards them from the container widget to
// handleKey(); a top-level display (--toplevel, or X11) gets them here.
#pragma once

#include "common.h"

#include <QCursor>
#include <QImage>
#include <QSet>
#include <QTimer>
#include <QWindow>

class DBusDisplay;
class Renderer;
class WaylandExtras;

class DisplayWindow : public QWindow
{
    Q_OBJECT
public:
    DisplayWindow(DBusDisplay *display, WaylandExtras *wayland, const Options &opts);

    void setRenderer(Renderer *renderer) { m_renderer = renderer; }
    void setGuestSize(uint32_t w, uint32_t h);
    void setGuestCursor(const QImage &image, int hotX, int hotY);
    void setGuestCursorVisible(bool visible);
    QSize physicalSize() const;
    double refreshRateHz() const;
    // compositor shortcuts go to the guest: grabbed with Ctrl+Alt+G, or
    // automatically while an absolute pointer is over the display (as SDL)
    bool grabbed() const { return m_grab || m_autoGrab; }
    QString grabState() const;
    void sendUiInfo();
    void setGrab(bool on);
    void handleKey(QKeyEvent *e, bool down);
    // whether the window around the display has the keyboard (embedded mode)
    void setHostActive(bool active);

Q_SIGNALS:
    void grabChanged();
    void pressed();           // a click: the display wants the keyboard
    void fullScreenToggled(); // Ctrl+Alt+F
    void closeRequested();    // the top-level view's own close: QEMU is quit first

protected:
    bool event(QEvent *e) override;
    void exposeEvent(QExposeEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void focusInEvent(QFocusEvent *) override;
    void focusOutEvent(QFocusEvent *) override;

private:
    void key(QKeyEvent *e, bool down);
    bool guestPos(QPointF pos, uint32_t *x, uint32_t *y) const;
    void releaseAllKeys();
    void updateCursor();
    void updateGrab();
    bool windowActionModifierHeld() const;

    DBusDisplay *m_display;
    WaylandExtras *m_wayland;
    Options m_opts;
    Renderer *m_renderer = nullptr;
    QTimer m_uiInfoTimer;
    QSize m_guestSize;
    QSet<uint32_t> m_pressed;
    QCursor m_guestCursor{Qt::ArrowCursor};
    bool m_cursorVisible = true;
    bool m_grab = false;         // Ctrl+Alt+G, or a click with a relative mouse
    bool m_autoGrab = false;     // absolute pointer over the display
    bool m_suppressAuto = false; // Ctrl+Alt+G released it until the pointer leaves
    bool m_pointerInside = false;
    bool m_hostActive = false;
    bool m_inhibited = false, m_locked = false, m_confined = false; // what the compositor was asked
    bool m_waylandSet = false;
    double m_relX = 0, m_relY = 0;
    int m_wheelAccum = 0;
    int m_wheelAccumX = 0;
};
