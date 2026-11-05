// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QSize>
#include <QString>

#include <cstdint>
#include <functional>
#include <set>

/*
 * What src/vmview's DisplayWindow does with the keyboard and the pointer,
 * without a toolkit: the GTK widget (DisplayWidget) feeds it GTK's events,
 * and tests feed it theirs.
 *
 * Keys come as evdev codes (Wayland's XKB keycode - 8) and go to QEMU as
 * its qnum codes; a key already down is a repeat, which the guest makes on
 * its own.  The pointer comes in the widget's logical pixels and goes to
 * the guest's pixels through the letterbox the renderer draws (fitRect).
 *
 * The grab, as QEMU's SDL display has it: an absolute pointer over the
 * display with the keyboard on it takes the desktop's shortcuts (Alt+Tab,
 * Super...) for the guest; Ctrl+Alt+G takes them until it is pressed again,
 * and releasing that way holds off the automatic grab until the pointer
 * leaves; a guest without an absolute pointer locks the pointer at the
 * first click.  While the grab holds and a key of KWin's window actions is
 * down (Meta, Alt), the pointer is also confined, which keeps Meta + drag
 * for the guest (KWin runs them on the window otherwise).
 */
class DisplayInput
{
public:
    /* QEMU's side: DBusDisplay's input calls */
    class Sink
    {
    public:
        virtual ~Sink() = default;
        virtual void keyPress(uint32_t qnum) = 0;
        virtual void keyRelease(uint32_t qnum) = 0;
        virtual void mouseAbs(uint32_t x, uint32_t y) = 0;
        virtual void mouseRel(int dx, int dy) = 0;
        virtual void mouseButton(uint32_t button, bool down) = 0;
        virtual bool mouseIsAbsolute() const = 0;
    };
    /* The compositor's side: what it is asked for the window */
    class Compositor
    {
    public:
        virtual ~Compositor() = default;
        virtual void setShortcutsInhibited(bool on) = 0;
        virtual void setPointerLocked(bool on) = 0;
        virtual void setPointerConfined(bool on) = 0;
        /* Asked for the shortcuts, and no answer yet: GNOME asks the user
           in a dialog of its own, which takes the window's keyboard */
        virtual bool shortcutsInhibitPending() const { return false; }
    };

    /* QEMU's mouse buttons (Mouse.Press); 7 and 8 reach qemu_input_queue_btn()
       as the wheel's left and right though the interface does not name them */
    enum Button : uint32_t {
        Left = 0, Middle = 1, Right = 2, WheelUp = 3, WheelDown = 4, Side = 5, Extra = 6,
        WheelLeft = 7, WheelRight = 8,
    };
    /* The Ctrl+Alt hotkeys of QEMU's own displays */
    enum class Hotkey { None, Grab, FullScreen };

    DisplayInput(Sink *sink, Compositor *compositor);

    /* grabbed() or grabState() changed */
    std::function<void()> grabChanged;
    /* cursorHidden() changed */
    std::function<void()> cursorChanged;

    /* Ctrl+Alt+G, Ctrl+Alt+F: @keyval lower case (GDK_KEY_g = 'g') */
    static Hotkey hotkey(uint32_t keyval, bool ctrl, bool alt, bool shift, bool super);
    /* A key down or up; false: not the guest's (no qnum code, or a repeat) */
    bool key(uint32_t evdev, bool down);
    bool keyDown(uint32_t evdev) const;

    /* The pointer, in the view's logical pixels */
    void pointerEnter(double x, double y);
    void pointerLeave();
    void motion(double x, double y);
    /* A button down at (x, y): true if it went to the guest (with a
       relative mouse the first click grabs instead) */
    bool press(uint32_t button, double x, double y);
    void release(uint32_t button);
    /* The wheel, in 120ths of a notch: QEMU's wheel is in notches */
    void scroll(double dx, double dy);
    /* The compositor's relative motion, and whether it locked the pointer */
    void relativeMotion(double dx, double dy);
    void setPointerLockActive(bool active);

    /* Whether an absolute pointer over the focused view grabs on its own (as
       QEMU's SDL display), or only Ctrl+Alt+G grabs: on GNOME for an app it
       does not know, whose every new window it asks the user about
       (ShortcutsInhibit::automatic()) */
    void setAutoGrab(bool allowed);
    /* The window around the view has the keyboard on it */
    void setHostActive(bool active);
    bool hostActive() const { return m_hostActive; }
    void setGrab(bool on);
    bool grabbed() const { return m_grab || m_autoGrab; }
    QString grabState() const;
    /* QEMU's mouse switched between absolute and relative */
    void mouseModeChanged();

    /* The guest's screen; the view's size in buffer pixels and its scale */
    void setGuestSize(QSize size) { m_guestSize = size; }
    QSize guestSize() const { return m_guestSize; }
    void setView(QSize physical, double scale);
    /* (x, y) in the view's logical pixels, in the guest's: false if it has none */
    bool guestPos(double x, double y, uint32_t *gx, uint32_t *gy) const;

    /* The guest's cursor hidden (MouseSet), or hidden as the pointer is locked */
    void setGuestCursorVisible(bool visible);
    bool cursorHidden() const;

    void releaseAllKeys();
    void releaseAllButtons();
    uint32_t buttonsDown() const { return m_buttons; }

private:
    void updateGrab();
    bool windowActionModifierHeld() const;

    Sink *m_sink;
    Compositor *m_compositor;
    QSize m_guestSize;
    QSize m_view;
    double m_scale = 1;
    std::set<uint32_t> m_pressed;   // qnum codes
    uint32_t m_buttons = 0;         // the guest's mouse buttons down, 1 << QEMU's button
    bool m_lockActive = false;      // the compositor locked the pointer (relative motion)
    bool m_cursorVisible = true;
    bool m_cursorHidden = false;    // as last told
    bool m_grab = false;            // Ctrl+Alt+G, or a click with a relative mouse
    bool m_autoGrab = false;        // absolute pointer over the display
    bool m_autoGrabAllowed = true;  // setAutoGrab()
    bool m_suppressAuto = false;    // Ctrl+Alt+G released it until the pointer leaves
    bool m_pointerInside = false;
    bool m_hostActive = false;
    bool m_inhibited = false, m_locked = false, m_confined = false; // what the compositor was asked
    double m_relX = 0, m_relY = 0;
    double m_wheelAccum = 0, m_wheelAccumX = 0;
};
