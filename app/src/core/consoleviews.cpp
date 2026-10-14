// SPDX-License-Identifier: GPL-2.0-or-later
#include "consoleviews.h"

#include <QKeyEvent>
#include <QTimer>

#include <utility>

/* XKB's keycodes, evdev's + 8: KEY_1..KEY_3 and KEY_KP1..KEY_KP3 */
static constexpr quint32 kKeycode1 = 2 + 8;
static constexpr quint32 kKeypad1 = 79 + 8;

/* the tests' wait for an agent that went, else kAgentWaitMs */
static int s_agentWaitMs = 0;

ConsoleViews::ConsoleViews(QObject *parent) : QObject(parent), m_agentWait(new QTimer(this))
{
    m_agentWait->setSingleShot(true);
    connect(m_agentWait, &QTimer::timeout, this, &ConsoleViews::agentGone);
}

void ConsoleViews::setAgentWait(int ms)
{
    s_agentWaitMs = ms;
}

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
    /* over the Boot that an agent's going would bring after its wait */
    m_bootAfter = false;
    show(view);
}

void ConsoleViews::powerOn()
{
    m_agentWait->stop();
    m_bootAfter = false;
    m_screenBefore = false;
    m_systemDown = false;
    m_up = false;
    m_screen = false;
    m_desktop = false;
    show(View::Boot);
}

void ConsoleViews::setGuest(bool up, bool screen)
{
    screen = screen && up;
    const bool gone = !up && m_up;
    /* an agent back within the wait says again what the guest's screen
       showed before it went: only what it did not show then is new */
    const bool back = up && m_agentWait->isActive();
    const bool shows = screen && !(back ? m_screenBefore : m_screen);

    if (gone) {
        m_screenBefore = m_screen;
    }
    m_up = up;
    m_screen = screen;
    if (up) {
        m_agentWait->stop();
        m_bootAfter = false;
    }
    if (m_desktop) {
        return;
    }
    if (shows) {
        show(View::Screen);
    } else if (gone && m_systemDown) {
        /* the guest's system goes down, QEMU's guest agent gone already */
        show(View::Boot);
    } else if (gone) {
        /* the guest shuts down or restarts, or the agent restarts itself
           (after a tools update it ran, after a crash): Boot once it has
           been gone for the wait */
        m_bootAfter = true;
        m_agentWait->start(s_agentWaitMs > 0 ? s_agentWaitMs : kAgentWaitMs);
    }
}

void ConsoleViews::setQemuAgent(bool up)
{
    m_systemDown = !up;
    if (!up && m_agentWait->isActive()) {
        /* the guest's system goes down: the agent does not come back */
        m_agentWait->stop();
        agentGone();
    }
}

void ConsoleViews::agentGone()
{
    m_screenBefore = false;
    if (std::exchange(m_bootAfter, false) && !m_desktop) {
        show(View::Boot);
    }
}

void ConsoleViews::setDesktop(bool shown)
{
    if (shown == m_desktop) {
        return;
    }
    m_desktop = shown;
    /* what the guest's screen showed before the agent went: not what the
       view showed last any more */
    m_screenBefore = false;
    if (shown) {
        m_bootAfter = false;
        show(View::Desktop);
    } else if (m_screen) {
        /* the session ended, the login screen up already */
        show(View::Screen);
    } else if (!m_up && m_agentWait->isActive()) {
        /* the agent went a moment ago: Boot if it does not come back */
        m_bootAfter = true;
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
