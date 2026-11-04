// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/gtkbase.h"

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#include "gtk/widgets.h"

namespace Gtk4 {

/*
 * For a C++ object that shows GTK widgets which may outlive it a little:
 * the connections it makes to their signals through on(), and the
 * callbacks it hands out through guarded(), do nothing once it is gone
 * (or once it says so, stopCallbacks()), whatever the widgets emit as
 * they go.
 */
class Guarded
{
public:
    Guarded() : m_alive(std::make_shared<bool>(true)) {}
    ~Guarded() { *m_alive = false; }
    Guarded(const Guarded &) = delete;
    Guarded &operator=(const Guarded &) = delete;

protected:
    /* Gtk4::connect(), for as long as this object lives */
    template <typename Signature, typename F>
    gulong on(gpointer instance, const char *signal, F &&function)
    {
        return Gtk4::connect<Signature>(instance, signal,
                                        guarded<Signature>(std::forward<F>(function)));
    }
    /* @function, called only while this object lives: for a file chooser's answer */
    template <typename Signature, typename F>
    std::function<Signature> guarded(F &&function)
    {
        return Guard<Signature>::wrap(m_alive, std::forward<F>(function));
    }
    /* Nothing of on() and guarded() runs from now on: before a destructor lets its widgets go */
    void stopCallbacks() { *m_alive = false; }

private:
    template <typename Signature>
    struct Guard;

    template <typename R, typename... Args>
    struct Guard<R(Args...)> {
        template <typename F>
        static std::function<R(Args...)> wrap(std::shared_ptr<bool> alive, F function)
        {
            return [alive = std::move(alive), function = std::move(function)](Args... args) -> R {
                if (!*alive) {
                    if constexpr (std::is_void_v<R>) {
                        return;
                    } else {
                        return R();
                    }
                }
                return function(args...);
            };
        }
    };

    std::shared_ptr<bool> m_alive;
};

}
