// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 tomtomnet
// The protocols the forwarder knows (gen_protocols.py makes the table from protocols.txt).
#pragma once
#include <cstdint>

struct wl_interface;

struct vmview_iface {
    const char *name;
    const struct wl_interface *iface;
    uint32_t version;               // the version this table knows: binds are clamped to it
    uint64_t destructor_requests;   // bit N: request N destroys the object
    uint64_t destructor_events;     // bit N: event N destroys the object
};

extern const vmview_iface vmview_ifaces[];
extern const unsigned vmview_iface_count;
