// SPDX-License-Identifier: GPL-2.0-or-later
#include "consoleviews.h"

#include <QKeyEvent>

/* XKB's keycodes, evdev's + 8: KEY_1..KEY_3 and KEY_KP1..KEY_KP3 */
static constexpr quint32 kKeycode1 = 2 + 8;
static constexpr quint32 kKeypad1 = 79 + 8;

ConsoleViews::ConsoleViews(QObject *parent) : QObject(parent) {}

ConsoleViews::View ConsoleViews::view() const
{
    return m_view;
}

int ConsoleViews::qemuConsole(View view)
{
    return view == View::Boot ? 0 : 1;
}

std::optional<ConsoleViews::View> ConsoleViews::viewForKey(const QKeyEvent *event)
{
    static const View views[] = {View::Boot, View::Screen, View::Desktop};
    const Qt::KeyboardModifiers mods =
        event->modifiers() &
        (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    const quint32 code = event->nativeScanCode();

    if (mods != (Qt::ControlModifier | Qt::AltModifier)) {
        return std::nullopt;
    }
    if (code >= kKeycode1 && code < kKeycode1 + 3) {
        return views[code - kKeycode1];
    }
    if (code >= kKeypad1 && code < kKeypad1 + 3) {
        return views[code - kKeypad1];
    }
    if (code == 0 && event->key() >= Qt::Key_1 && event->key() <= Qt::Key_3) {
        return views[event->key() - Qt::Key_1];
    }
    return std::nullopt;
}

void ConsoleViews::choose(View view)
{
    show(view);
}

void ConsoleViews::powerOn()
{
    m_up = false;
    m_screen = false;
    m_desktop = false;
    show(View::Boot);
}

void ConsoleViews::setGuest(bool up, bool screen)
{
    screen = screen && up;
    const bool shows = screen && !m_screen;
    const bool gone = !up && m_up;

    m_up = up;
    m_screen = screen;
    if (m_desktop) {
        return;
    }
    if (shows) {
        show(View::Screen);
    } else if (gone) {
        show(View::Boot);
    }
}

void ConsoleViews::setDesktop(bool shown)
{
    if (shown == m_desktop) {
        return;
    }
    m_desktop = shown;
    if (shown) {
        show(View::Desktop);
    } else if (m_screen) {
        /* the session ended, the login screen up already */
        show(View::Screen);
    } else if (!m_up) {
        /* the guest shuts down or restarts: its console, on the VGA (on its
           own screen only without the VGA's driver, which the agent no
           longer tells) */
        show(View::Boot);
    }
    /* else a logout, whose login screen comes, or a shutdown, whose agent
       goes: setGuest() tells which */
}

bool ConsoleViews::hasDesktop() const
{
    return m_desktop;
}

void ConsoleViews::show(View view)
{
    if (view != m_view) {
        m_view = view;
        emit viewChanged(view);
    }
}
