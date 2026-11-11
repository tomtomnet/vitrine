// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <map>

/*
 * Who each key held down belongs to: the guest, when the keys went to it at
 * the key's press, or the window.  A key keeps its owner until it is
 * released, so its repeats (the toolkit's) and its release go where its
 * press went, whichever way the keys go meanwhile: a key the guest had when
 * the pointer left the view is released in the guest at once, and its
 * later repeats and release go nowhere; a key pressed for the window never
 * reaches the guest, even if the pointer comes onto the view before it is
 * let go (src/gtkview/README.md).  No toolkit: the views' widgets and
 * DisplayInput feed it their key codes.
 */
class KeyOwners
{
public:
    enum class Owner { None, Guest, Window };

    /* A press of @code: its owner, the guest's when @toGuest.  A key down
       already keeps the owner of its press (@repeat set: the toolkit's repeat) */
    Owner press(uint32_t code, bool toGuest, bool *repeat = nullptr)
    {
        const auto [it, added] = m_down.try_emplace(code, toGuest ? Owner::Guest : Owner::Window);
        if (repeat) {
            *repeat = !added;
        }
        return it->second;
    }
    /* Its release: the owner it had (None: its press was not seen), forgotten */
    Owner release(uint32_t code)
    {
        const auto it = m_down.find(code);
        if (it == m_down.end()) {
            return Owner::None;
        }
        const Owner owner = it->second;
        m_down.erase(it);
        return owner;
    }
    /* The view lost the keyboard: the releases of the keys down will not come */
    void clear() { m_down.clear(); }
    bool isDown(uint32_t code) const { return m_down.count(code) != 0; }

private:
    std::map<uint32_t, Owner> m_down;
};
