// Native Wayland protocols Qt has no API for:
//  - zwp_keyboard_shortcuts_inhibit_manager_v1: keyboard grab (Alt+Tab, Meta...
//    go to the guest), on the top-level surface, which holds keyboard focus;
//  - zwp_pointer_constraints_v1 + zwp_relative_pointer_manager_v1: mouse grab
//    for guests without an absolute device, on the display's own surface;
//  - zwp_pointer_constraints_v1 confinement with an absolute device: KWin runs
//    its modifier + button window actions (Meta+drag) on a client's window
//    unless the pointer is constrained, shortcuts inhibitor or not.
#pragma once

#include <QObject>

class QWindow;
struct wl_display;
struct wl_seat;
struct wl_pointer;
struct wl_surface;
struct zwp_keyboard_shortcuts_inhibit_manager_v1;
struct zwp_keyboard_shortcuts_inhibitor_v1;
struct zwp_pointer_constraints_v1;
struct zwp_locked_pointer_v1;
struct zwp_confined_pointer_v1;
struct zwp_relative_pointer_manager_v1;
struct zwp_relative_pointer_v1;

class WaylandExtras : public QObject
{
    Q_OBJECT
public:
    explicit WaylandExtras(QObject *parent = nullptr);
    ~WaylandExtras() override;

    bool init();
    static wl_surface *surfaceOf(QWindow *window);
    wl_display *display() const { return m_display; }

    bool setShortcutsInhibited(QWindow *topLevel, bool on);
    bool setPointerLocked(QWindow *window, bool on);
    // the pointer stays on @window's surface (a top-level: KWin only honours
    // constraints on a window's main surface); one constraint per surface
    bool setPointerConfined(QWindow *window, bool on);
    bool hasShortcutsInhibit() const { return m_inhibitManager; }
    bool hasPointerLock() const { return m_constraints && m_relativeManager; }

Q_SIGNALS:
    void relativeMotion(double dx, double dy);
    void shortcutsInhibitActive(bool active);
    void pointerLockActive(bool active);
    void pointerConfineActive(bool active);

public:
    // protocol callbacks
    void onRelative(double dx, double dy) { Q_EMIT relativeMotion(dx, dy); }
    void onInhibit(bool active) { Q_EMIT shortcutsInhibitActive(active); }
    void onLocked(bool active) { Q_EMIT pointerLockActive(active); }
    void onConfined(bool active) { Q_EMIT pointerConfineActive(active); }
    zwp_keyboard_shortcuts_inhibit_manager_v1 *m_inhibitManager = nullptr;
    zwp_pointer_constraints_v1 *m_constraints = nullptr;
    zwp_relative_pointer_manager_v1 *m_relativeManager = nullptr;

private:
    wl_display *m_display = nullptr;
    wl_seat *m_seat = nullptr;
    wl_pointer *m_pointer = nullptr;
    zwp_keyboard_shortcuts_inhibitor_v1 *m_inhibitor = nullptr;
    zwp_locked_pointer_v1 *m_locked = nullptr;
    zwp_confined_pointer_v1 *m_confined = nullptr;
    zwp_relative_pointer_v1 *m_relative = nullptr;
};
