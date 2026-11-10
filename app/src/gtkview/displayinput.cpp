// SPDX-License-Identifier: GPL-2.0-or-later
#include "displayinput.h"

#include "vmview/keymap_linux_to_qnum.h"
#include "vmview/renderer.h"

#include <QRect>

#include <algorithm>
#include <chrono>
#include <utility>

Q_LOGGING_CATEGORY(lcGrab, "vitrine.grab", QtWarningMsg)

namespace {

/* The keys of KWin's modifier + button window actions ([MouseBindings]
   CommandAllKey: Meta, the default, or Alt), as qnum codes */
bool isWindowActionModifier(uint32_t qnum)
{
    return qnum == 0xdb || qnum == 0xdc || qnum == 0x38; // KEY_LEFTMETA, KEY_RIGHTMETA, KEY_LEFTALT
}

} // namespace

DisplayInput::DisplayInput(Sink *sink, Compositor *compositor)
    : m_sink(sink), m_compositor(compositor)
{
}

DisplayInput::Hotkey DisplayInput::hotkey(uint32_t keyval, bool ctrl, bool alt, bool shift,
                                          bool super)
{
    if (!ctrl || !alt || shift || super) {
        return Hotkey::None;
    }
    if (keyval == 'g') {
        return Hotkey::Grab;
    }
    if (keyval == 'f') {
        return Hotkey::FullScreen;
    }
    return Hotkey::None;
}

bool DisplayInput::key(uint32_t evdev, bool down)
{
    if (evdev >= linux_to_qnum.size() || !linux_to_qnum[evdev]) {
        return false;
    }
    const uint32_t qnum = linux_to_qnum[evdev];
    if (down) {
        if (!m_pressed.insert(qnum).second) {
            return false;   // the toolkit's repeat: the guest repeats on its own
        }
        if (isWindowActionModifier(qnum)) {
            updateGrab();   // the confinement first: the button press may follow at once
        }
        m_sink->keyPress(qnum);
    } else {
        if (!m_pressed.erase(qnum)) {
            return false;   // pressed before the view had the keyboard
        }
        if (isWindowActionModifier(qnum)) {
            updateGrab();
        }
        m_sink->keyRelease(qnum);
    }
    return true;
}

bool DisplayInput::keyDown(uint32_t evdev) const
{
    return evdev < linux_to_qnum.size() && linux_to_qnum[evdev] &&
           m_pressed.count(linux_to_qnum[evdev]);
}

bool DisplayInput::windowActionModifierHeld() const
{
    return std::any_of(m_pressed.cbegin(), m_pressed.cend(), isWindowActionModifier);
}

void DisplayInput::releaseAllKeys()
{
    for (uint32_t qnum : std::exchange(m_pressed, {})) {
        m_sink->keyRelease(qnum);
    }
}

void DisplayInput::pointerEnter(double x, double y)
{
    m_pointerInside = true;
    updateGrab();
    motion(x, y);
}

void DisplayInput::pointerLeave()
{
    /* with a button down only when the compositor took the pointer away (a
       screen lock, its own grab): no release follows */
    releaseAllButtons();
    m_pointerInside = false;
    m_suppressAuto = false;
    updateGrab();
}

void DisplayInput::setView(QSize physical, double scale)
{
    m_view = physical;
    m_scale = scale > 0 ? scale : 1;
}

bool DisplayInput::guestPos(double x, double y, uint32_t *gx, uint32_t *gy) const
{
    if (m_guestSize.isEmpty()) {
        return false;
    }
    const QRect r = fitRect(m_view, m_guestSize);
    if (r.isEmpty()) {
        return false;
    }
    const double px = (x * m_scale - r.x()) * m_guestSize.width() / r.width();
    const double py = (y * m_scale - r.y()) * m_guestSize.height() / r.height();
    *gx = uint32_t(std::clamp(px, 0.0, double(m_guestSize.width() - 1)));
    *gy = uint32_t(std::clamp(py, 0.0, double(m_guestSize.height() - 1)));
    return true;
}

void DisplayInput::motion(double x, double y)
{
    uint32_t gx, gy;
    if (m_sink->mouseIsAbsolute() && guestPos(x, y, &gx, &gy)) {
        m_sink->mouseAbs(gx, gy);
    }
}

bool DisplayInput::press(uint32_t button, double x, double y)
{
    if (!m_sink->mouseIsAbsolute() && !m_grab) {
        setGrab(true); // relative guests need the pointer locked
        return false;
    }
    motion(x, y);
    m_sink->mouseButton(button, true);
    m_buttons |= 1u << button;
    return true;
}

void DisplayInput::release(uint32_t button)
{
    m_sink->mouseButton(button, false);
    m_buttons &= ~(1u << button);
}

/* The buttons still down in the guest: up, or it drags on with the next motion */
void DisplayInput::releaseAllButtons()
{
    for (uint32_t b = 0; m_buttons; b++) {
        if (m_buttons & (1u << b)) {
            m_sink->mouseButton(b, false);
            m_buttons &= ~(1u << b);
        }
    }
}

void DisplayInput::scroll(double dx, double dy)
{
    auto clicks = [this](double &accum, double delta, uint32_t pos, uint32_t neg) {
        accum += delta;
        while (accum >= 120 || accum <= -120) {
            const uint32_t b = accum > 0 ? pos : neg;
            m_sink->mouseButton(b, true);
            m_sink->mouseButton(b, false);
            accum += accum > 0 ? -120 : 120;
        }
    };
    clicks(m_wheelAccum, dy, WheelUp, WheelDown);
    clicks(m_wheelAccumX, dx, WheelLeft, WheelRight);
}

void DisplayInput::relativeMotion(double dx, double dy)
{
    /* relative motion goes on while the compositor unlocked the pointer (it
       does when the window loses focus, say): only while locked */
    if (!m_grab || m_sink->mouseIsAbsolute() || !m_lockActive) {
        return;
    }
    m_relX += dx;
    m_relY += dy;
    const int ix = int(m_relX), iy = int(m_relY);
    if (ix || iy) {
        m_relX -= ix;
        m_relY -= iy;
        m_sink->mouseRel(ix, iy);
    }
}

void DisplayInput::setPointerLockActive(bool active)
{
    m_lockActive = active;
}

void DisplayInput::setAutoGrab(bool allowed)
{
    m_autoGrabAllowed = allowed;
    updateGrab();
}

int64_t DisplayInput::now() const
{
    if (m_clock) {
        return m_clock();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void DisplayInput::setHostActive(bool active)
{
    if (active == m_hostActive) {
        return;
    }
    m_hostActive = active;
    if (!active) {
        releaseAllKeys();
        releaseAllButtons();
        /* the user's grab, unless this is the loss of the keyboard that the
           compositor's question about it causes: GNOME shows its "allow"
           dialog at once, the dialog takes the keyboard, and its Allow must
           find the grab there (else the VM was left not grabbed).  Only a loss
           soon after the request, and once: the keyboard is back when the
           user answered, Allow or Deny, and after a Deny - or with a
           compositor that never answers - the request stays ungranted for
           good, while later losses (Alt+Tab away) release the grab */
        const bool askedLoss = m_grab && m_inhibited && m_askOpen &&
                               now() - m_askedMs <= kAskedLossMs && m_compositor &&
                               m_compositor->shortcutsInhibitPending();
        if (!askedLoss) {
            m_grab = false;
        }
        m_askOpen = false;
        m_suppressAuto = false;
    }
    updateGrab();
}

void DisplayInput::setGrab(bool on)
{
    m_grab = on;
    if (!on) {
        releaseAllButtons();
        /* releasing holds off the automatic grab until the pointer leaves */
        m_suppressAuto = m_pointerInside;
    }
    updateGrab();
}

void DisplayInput::mouseModeChanged()
{
    releaseAllButtons(); // pressed in the other mode
    updateGrab();
}

void DisplayInput::setGuestCursorVisible(bool visible)
{
    m_cursorVisible = visible;
    if (cursorHidden() != m_cursorHidden) {
        m_cursorHidden = cursorHidden();
        if (cursorChanged) {
            cursorChanged();
        }
    }
}

bool DisplayInput::cursorHidden() const
{
    return (m_grab && !m_sink->mouseIsAbsolute()) || !m_cursorVisible;
}

void DisplayInput::updateGrab()
{
    const bool absolute = m_sink->mouseIsAbsolute();
    const QString before = grabState();
    /* as QEMU's SDL display: an absolute pointer over the focused display
       grabs the keyboard */
    m_autoGrab = m_autoGrabAllowed && m_hostActive && m_pointerInside && absolute && !m_suppressAuto;
    const bool inhibit = m_grab || m_autoGrab;
    const bool lock = m_grab && !absolute;
    /* KWin runs its modifier + button window actions (Meta + left: move,
       Meta + right: resize, Meta + middle: raise/lower) on the window and
       keeps the press from it unless the pointer is constrained; the
       shortcuts inhibitor does not count (input.cpp
       windowActionForPointerButtonPress).  An absolute pointer is not
       locked, so while the grab holds and such a modifier is down, it is
       confined to the window: Meta + drag then reaches the guest, as with
       QEMU's SDL display, whose grab confines the pointer. */
    const bool confine = inhibit && absolute && windowActionModifierHeld();
    if (inhibit != m_inhibited || lock != m_locked || confine != m_confined) {
        /* a new request: its question's loss of the keyboard may come */
        if (inhibit != m_inhibited) {
            m_askOpen = inhibit;
            m_askedMs = inhibit ? now() : 0;
            qCDebug(lcGrab).nospace() << "grab " << (inhibit ? "on" : "off") << ": keyboard "
                                      << m_hostActive << ", pointer on the screen " << m_pointerInside
                                      << ", absolute " << absolute << ", Ctrl+Alt+G grab " << m_grab
                                      << ", held off " << m_suppressAuto << ", automatic "
                                      << m_autoGrabAllowed;
        }
        m_inhibited = inhibit;
        m_locked = lock;
        m_confined = confine;
        if (m_compositor) {
            m_compositor->setShortcutsInhibited(inhibit);
            /* one constraint per surface: the one going away goes first */
            if (!lock) {
                m_compositor->setPointerLocked(false);
            }
            if (!confine) {
                m_compositor->setPointerConfined(false);
            }
            if (lock) {
                m_compositor->setPointerLocked(true);
            }
            if (confine) {
                m_compositor->setPointerConfined(true);
            }
        }
    }
    if (cursorHidden() != m_cursorHidden) {
        m_cursorHidden = cursorHidden();
        if (cursorChanged) {
            cursorChanged();
        }
    }
    if (grabState() != before && grabChanged) {
        grabChanged();
    }
}

QString DisplayInput::grabState() const
{
    if (m_grab) {
        return QStringLiteral("grabbed (Ctrl+Alt+G releases)");
    }
    if (m_autoGrab) {
        return QStringLiteral("grabbed while the pointer is on the VM (Ctrl+Alt+G releases)");
    }
    if (!m_autoGrabAllowed) {
        return QStringLiteral("not grabbed (Ctrl+Alt+G grabs; GNOME asks first)");
    }
    return QStringLiteral("not grabbed (Ctrl+Alt+G grabs)");
}
