// SPDX-License-Identifier: GPL-2.0-or-later
#include "consoleviews.h"

#include <QKeyEvent>

#include <iterator>

/* XKB's keycodes, evdev's + 8: KEY_1, KEY_2 and KEY_KP1, KEY_KP2 */
static constexpr quint32 kKeycode1 = 2 + 8;
static constexpr quint32 kKeypad1 = 79 + 8;
static constexpr ConsoleViews::View kViews[] = {ConsoleViews::View::Screen,
                                                ConsoleViews::View::Desktop};
static constexpr quint32 kViewCount = std::size(kViews);

ConsoleViews::ConsoleViews(QObject *parent) : QObject(parent) {}

ConsoleViews::View ConsoleViews::view() const
{
    return m_view;
}

std::optional<ConsoleViews::View> ConsoleViews::viewForKey(const QKeyEvent *event)
{
    const Qt::KeyboardModifiers mods =
        event->modifiers() &
        (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    const quint32 code = event->nativeScanCode();

    if (mods != (Qt::ControlModifier | Qt::AltModifier)) {
        return std::nullopt;
    }
    if (code >= kKeycode1 && code < kKeycode1 + kViewCount) {
        return kViews[code - kKeycode1];
    }
    if (code >= kKeypad1 && code < kKeypad1 + kViewCount) {
        return kViews[code - kKeypad1];
    }
    if (code == 0 && event->key() >= Qt::Key_1 && event->key() < Qt::Key_1 + int(kViewCount)) {
        return kViews[event->key() - Qt::Key_1];
    }
    return std::nullopt;
}

void ConsoleViews::choose(View view)
{
    show(view);
}

void ConsoleViews::powerOn()
{
    m_desktop = false;
    show(View::Screen);
}

void ConsoleViews::setDesktop(bool shown)
{
    if (shown == m_desktop) {
        return;
    }
    m_desktop = shown;
    show(shown ? View::Desktop : View::Screen);
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
