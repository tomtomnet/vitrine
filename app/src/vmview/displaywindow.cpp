#include "displaywindow.h"
#include "dbusdisplay.h"
#include "keymap_linux_to_qnum.h"
#include "renderer.h"
#include "waylandextras.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QPixmap>
#include <QScreen>
#include <QSurfaceFormat>
#include <algorithm>

namespace {
// QEMU D-Bus mouse buttons
constexpr uint32_t kLeft = 0, kMiddle = 1, kRight = 2, kWheelUp = 3, kWheelDown = 4,
                   kSide = 5, kExtra = 6;
// Not in dbus-display1.xml, but Mouse.Press hands the value to
// qemu_input_queue_btn(): InputButton wheel-left / wheel-right.
constexpr uint32_t kWheelLeft = 7, kWheelRight = 8;

// The keys of KWin's modifier + button window actions ([MouseBindings]
// CommandAllKey: Meta, the default, or Alt), as qnum codes.
bool isWindowActionModifier(uint32_t qnum)
{
    return qnum == 0xdb || qnum == 0xdc || qnum == 0x38; // KEY_LEFTMETA, KEY_RIGHTMETA, KEY_LEFTALT
}

bool qtButton(Qt::MouseButton b, uint32_t *out)
{
    switch (b) {
    case Qt::LeftButton: *out = kLeft; return true;
    case Qt::MiddleButton: *out = kMiddle; return true;
    case Qt::RightButton: *out = kRight; return true;
    case Qt::BackButton: *out = kSide; return true;
    case Qt::ForwardButton: *out = kExtra; return true;
    default: return false;
    }
}
} // namespace

DisplayWindow::DisplayWindow(DBusDisplay *display, WaylandExtras *wayland, const Options &opts)
    : m_display(display), m_wayland(wayland), m_opts(opts)
{
    setSurfaceType(QSurface::OpenGLSurface);
    QSurfaceFormat fmt;
    fmt.setAlphaBufferSize(0); // opaque: the compositor needs no blending
    fmt.setDepthBufferSize(0);
    fmt.setStencilBufferSize(0);
    fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    // 0 = present each guest frame at once, like QEMU's SDL display
    fmt.setSwapInterval(opts.vsync ? 1 : 0);
    setFormat(fmt);
    setFlag(Qt::WindowDoesNotAcceptFocus, false);

    m_uiInfoTimer.setSingleShot(true);
    m_uiInfoTimer.setInterval(100); // resize storms: tell the guest once
    connect(&m_uiInfoTimer, &QTimer::timeout, this, &DisplayWindow::sendUiInfo);
    // the guest's USB tablet driver switches the mouse to absolute after boot
    connect(m_display, &DBusDisplay::mouseModeChanged, this, &DisplayWindow::updateGrab);
    if (m_wayland) {
        connect(m_wayland, &WaylandExtras::relativeMotion, this, [this](double dx, double dy) {
            if (!m_grab || m_display->mouseIsAbsolute()) {
                return;
            }
            m_relX += dx;
            m_relY += dy;
            int ix = int(m_relX), iy = int(m_relY);
            if (ix || iy) {
                m_relX -= ix;
                m_relY -= iy;
                m_display->mouseRel(ix, iy);
            }
        });
    }
}

QSize DisplayWindow::physicalSize() const
{
    const qreal dpr = devicePixelRatio();
    return QSize(qRound(width() * dpr), qRound(height() * dpr));
}

double DisplayWindow::refreshRateHz() const
{
    if (m_opts.refreshOverride > 0) {
        return m_opts.refreshOverride;
    }
    return screen() ? screen()->refreshRate() : 60.0;
}

void DisplayWindow::sendUiInfo()
{
    QSize size = m_opts.guestWidth ? QSize(m_opts.guestWidth, m_opts.guestHeight) : physicalSize();
    if (size.isEmpty()) {
        return;
    }
    uint32_t mhz = m_opts.sendRefreshRate ? uint32_t(qRound(refreshRateHz() * 1000)) : 0;
    m_display->applyUiInfo(size.width(), size.height(), mhz, 0, 0);
}

void DisplayWindow::setGuestSize(uint32_t w, uint32_t h)
{
    m_guestSize = QSize(int(w), int(h));
}

void DisplayWindow::setGuestCursor(const QImage &image, int hotX, int hotY)
{
    QPixmap pm = QPixmap::fromImage(image);
    // guest pixels are physical pixels when the guest mode matches the window
    pm.setDevicePixelRatio(devicePixelRatio());
    m_guestCursor = QCursor(pm, int(hotX / devicePixelRatio()), int(hotY / devicePixelRatio()));
    updateCursor();
}

void DisplayWindow::setGuestCursorVisible(bool visible)
{
    m_cursorVisible = visible;
    updateCursor();
}

void DisplayWindow::updateCursor()
{
    bool locked = m_grab && !m_display->mouseIsAbsolute();
    setCursor(locked || !m_cursorVisible ? QCursor(Qt::BlankCursor) : m_guestCursor);
}

bool DisplayWindow::event(QEvent *e)
{
    switch (e->type()) {
    case QEvent::ShortcutOverride:
        e->accept(); // every key goes to the guest
        return true;
    case QEvent::Enter:
        m_pointerInside = true;
        updateGrab();
        break;
    case QEvent::Leave:
        m_pointerInside = false;
        m_suppressAuto = false;
        updateGrab();
        break;
    case QEvent::Close:
        if (!parent()) {
            // In its own window (--toplevel) the view is the app's last window:
            // closing it would quit the app and kill QEMU. QEMU quits first.
            e->ignore();
            Q_EMIT closeRequested();
            return true;
        }
        break;
    default:
        break;
    }
    return QWindow::event(e);
}

void DisplayWindow::exposeEvent(QExposeEvent *)
{
    if (!m_renderer) {
        return;
    }
    if (isExposed() && !m_waylandSet && m_wayland && m_wayland->display()) {
        m_renderer->setWayland(m_wayland->display(), WaylandExtras::surfaceOf(this));
        m_waylandSet = true;
    }
    m_renderer->setExposed(isExposed());
}

void DisplayWindow::resizeEvent(QResizeEvent *)
{
    if (m_renderer) {
        m_renderer->requestRedraw();
    }
    if (!m_opts.guestWidth) {
        m_uiInfoTimer.start();
    }
}

bool DisplayWindow::guestPos(QPointF pos, uint32_t *x, uint32_t *y) const
{
    if (m_guestSize.isEmpty()) {
        return false;
    }
    const qreal dpr = devicePixelRatio();
    QRect r = fitRect(physicalSize(), m_guestSize);
    if (r.isEmpty()) {
        return false;
    }
    double gx = (pos.x() * dpr - r.x()) * m_guestSize.width() / r.width();
    double gy = (pos.y() * dpr - r.y()) * m_guestSize.height() / r.height();
    *x = uint32_t(std::clamp(gx, 0.0, double(m_guestSize.width() - 1)));
    *y = uint32_t(std::clamp(gy, 0.0, double(m_guestSize.height() - 1)));
    return true;
}

void DisplayWindow::mouseMoveEvent(QMouseEvent *e)
{
    uint32_t x, y;
    if (m_display->mouseIsAbsolute() && guestPos(e->position(), &x, &y)) {
        m_display->mouseAbs(x, y);
    }
}

void DisplayWindow::mousePressEvent(QMouseEvent *e)
{
    requestActivate();
    Q_EMIT pressed();
    if (!m_display->mouseIsAbsolute() && !m_grab) {
        setGrab(true); // relative guests need the pointer locked
        return;
    }
    uint32_t b;
    if (qtButton(e->button(), &b)) {
        mouseMoveEvent(e);
        m_display->mouseButton(b, true);
    }
}

void DisplayWindow::mouseReleaseEvent(QMouseEvent *e)
{
    uint32_t b;
    if (qtButton(e->button(), &b)) {
        m_display->mouseButton(b, false);
    }
}

void DisplayWindow::wheelEvent(QWheelEvent *e)
{
    // QEMU's wheel is in notches: one click per 120 units
    auto clicks = [this](int &accum, int delta, uint32_t pos, uint32_t neg) {
        accum += delta;
        while (accum >= 120 || accum <= -120) {
            uint32_t b = accum > 0 ? pos : neg;
            m_display->mouseButton(b, true);
            m_display->mouseButton(b, false);
            accum += accum > 0 ? -120 : 120;
        }
    };
    clicks(m_wheelAccum, e->angleDelta().y(), kWheelUp, kWheelDown);
    clicks(m_wheelAccumX, e->angleDelta().x(), kWheelLeft, kWheelRight);
}

void DisplayWindow::keyPressEvent(QKeyEvent *e)
{
    handleKey(e, true);
}

void DisplayWindow::keyReleaseEvent(QKeyEvent *e)
{
    handleKey(e, false);
}

void DisplayWindow::handleKey(QKeyEvent *e, bool down)
{
    // the hotkeys of QEMU's SDL and GTK displays: Ctrl+Alt+G grab, Ctrl+Alt+F
    // full screen. Ctrl and Alt still reach the guest, the letter does not.
    const Qt::KeyboardModifiers mods = e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier |
                                                         Qt::AltModifier | Qt::MetaModifier);
    if (down && mods == (Qt::ControlModifier | Qt::AltModifier) &&
        (e->key() == Qt::Key_G || e->key() == Qt::Key_F)) {
        if (!e->isAutoRepeat()) {
            if (e->key() == Qt::Key_G) {
                setGrab(!grabbed());
            } else {
                Q_EMIT fullScreenToggled();
            }
        }
        return;
    }
    key(e, down);
}

void DisplayWindow::key(QKeyEvent *e, bool down)
{
    if (e->isAutoRepeat()) {
        return; // the guest repeats on its own
    }
    // Wayland and X11 give XKB keycodes: evdev + 8
    const uint32_t evdev = e->nativeScanCode() >= 8 ? e->nativeScanCode() - 8 : 0;
    if (evdev >= linux_to_qnum.size() || !linux_to_qnum[evdev]) {
        return;
    }
    const uint32_t qnum = linux_to_qnum[evdev];
    if (down) {
        m_pressed.insert(qnum);
        if (isWindowActionModifier(qnum)) {
            updateGrab(); // the confinement first: the button press may follow at once
        }
        m_display->keyPress(qnum);
    } else if (m_pressed.remove(qnum)) {
        if (isWindowActionModifier(qnum)) {
            updateGrab();
        }
        m_display->keyRelease(qnum);
    }
}

bool DisplayWindow::windowActionModifierHeld() const
{
    return std::any_of(m_pressed.cbegin(), m_pressed.cend(), isWindowActionModifier);
}

void DisplayWindow::releaseAllKeys()
{
    for (uint32_t q : std::as_const(m_pressed)) {
        m_display->keyRelease(q);
    }
    m_pressed.clear();
}

// Embedded, MainWindow tells whether the display has the keyboard; as a
// top-level window it is its own host.
void DisplayWindow::focusInEvent(QFocusEvent *)
{
    if (!parent()) {
        setHostActive(true);
    }
}

void DisplayWindow::focusOutEvent(QFocusEvent *)
{
    if (!parent()) {
        setHostActive(false);
    }
}

void DisplayWindow::setHostActive(bool active)
{
    if (active == m_hostActive) {
        return;
    }
    m_hostActive = active;
    if (!active) {
        releaseAllKeys();
        m_grab = false;
        m_suppressAuto = false;
    }
    updateGrab();
}

void DisplayWindow::setGrab(bool on)
{
    m_grab = on;
    if (!on) {
        // releasing holds off the automatic grab until the pointer leaves
        m_suppressAuto = m_pointerInside;
    }
    updateGrab();
}

void DisplayWindow::updateGrab()
{
    const bool absolute = m_display->mouseIsAbsolute();
    const QString before = grabState();
    // as QEMU's SDL display: an absolute pointer over the focused display grabs the keyboard
    m_autoGrab = m_hostActive && m_pointerInside && absolute && !m_suppressAuto;
    const bool inhibit = m_grab || m_autoGrab;
    const bool lock = m_grab && !absolute;
    // KWin runs its modifier + button window actions (Meta + left: move, Meta +
    // right: resize, Meta + middle: raise/lower) on our window and keeps the press
    // from us unless the pointer is constrained; the shortcuts inhibitor does not
    // count (input.cpp windowActionForPointerButtonPress). An absolute pointer is not
    // locked, so while the grab holds and such a modifier is down, confine it to the
    // window: Meta+drag then reaches the guest, as with QEMU's SDL display, whose grab
    // confines the pointer.
    const bool confine = inhibit && absolute && windowActionModifierHeld();
    if (inhibit != m_inhibited || lock != m_locked || confine != m_confined) {
        m_inhibited = inhibit;
        m_locked = lock;
        m_confined = confine;
        if (m_wayland) {
            QWindow *top = this;
            while (top->parent()) {
                top = top->parent();
            }
            m_wayland->setShortcutsInhibited(top, inhibit);
            // one constraint per surface: the one going away goes first
            if (!lock) {
                m_wayland->setPointerLocked(top, false);
            }
            if (!confine) {
                m_wayland->setPointerConfined(top, false);
            }
            if (lock) {
                m_wayland->setPointerLocked(top, true); // as the confinement: the main surface
            }
            if (confine) {
                m_wayland->setPointerConfined(top, true);
            }
        }
    }
    updateCursor();
    if (grabState() != before) {
        Q_EMIT grabChanged();
    }
}

QString DisplayWindow::grabState() const
{
    if (m_grab) {
        return QStringLiteral("grabbed (Ctrl+Alt+G releases)");
    }
    if (m_autoGrab) {
        return QStringLiteral("grabbed while the pointer is on the VM (Ctrl+Alt+G releases)");
    }
    return QStringLiteral("not grabbed (Ctrl+Alt+G grabs)");
}
