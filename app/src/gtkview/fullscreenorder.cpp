// SPDX-License-Identifier: GPL-2.0-or-later
#include "fullscreenorder.h"

#include <gtkmm/window.h>
#include <glibmm/main.h>

#include <utility>

namespace {

unsigned timeoutMs = 2000;

}

FullScreenOrder::~FullScreenOrder()
{
    cancel();
}

void FullScreenOrder::setTimeout(unsigned ms)
{
    timeoutMs = ms;
}

bool FullScreenOrder::ready(Gtk::Window *from, std::function<void()> retry)
{
    /* the retry of a wait that ended */
    if (std::exchange(m_go, false)) {
        return true;
    }
    if (m_waiting) {
        return false;
    }
    if (!from || !from->get_visible() || from->is_active()) {
        return true;
    }
    m_waiting = true;
    m_retry = std::move(retry);
    /* a window gone meanwhile takes its signal with it: the timeout then */
    m_active = from->property_is_active().signal_changed().connect([this, from]() {
        if (from->is_active()) {
            go();
        }
    });
    m_timeout = Glib::signal_timeout().connect([this]() {
        go();
        return false;
    }, timeoutMs);
    return false;
}

/* From an idle: GTK is done with the window's activation first */
void FullScreenOrder::go()
{
    m_active.disconnect();
    m_timeout.disconnect();
    if (m_idle.connected()) {
        return;
    }
    m_idle = Glib::signal_idle().connect([this]() {
        std::function<void()> retry = std::exchange(m_retry, nullptr);
        m_waiting = false;
        m_go = true;
        if (retry) {
            retry();
        }
        /* the retry may not have asked */
        m_go = false;
        return false;
    });
}

void FullScreenOrder::cancel()
{
    m_active.disconnect();
    m_timeout.disconnect();
    m_idle.disconnect();
    m_retry = nullptr;
    m_waiting = false;
    m_go = false;
}
