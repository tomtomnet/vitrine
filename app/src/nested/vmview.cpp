// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 tomtomnet
// vmview: the guest's Wayland protocol, from QEMU's cross-domain socket, on the app's own Wayland
// connection; the guest's toplevel as a subsurface of the app's window (vmview.h). 2026-10-12.
//
// QEMU side: a small Wayland server of our own (wire format, fds over SCM_RIGHTS, object ids), so that
// wl_display.sync goes to the host compositor and comes back in order, as through a plain proxy.
// Host side: libwayland-client on the app's wl_display, an event queue of ours, generic marshaling
// (wl_proxy_marshal_array_flags) and dispatchers (wl_proxy_add_dispatcher), with the message
// signatures of protocols.txt's interfaces. Answered here instead of passed on: wl_registry (the
// host's globals through an allowlist, versions clamped to what this file knows) and xdg-shell (the
// toplevel becomes a desync wl_subsurface of the app's surface, configured from the app's geometry).
// Events naming the app's own objects (Qt's surfaces, outputs) are dropped or, for the keyboard focus
// on the app's window, given to the guest's toplevel.
#include "vmview.h"
#include "protocols.h"

#include <wayland-client.h>

// wayland-scanner's private code for presentation-time (built with this library): the forwarder's own
// feedback for VMVIEW_STATS
extern "C" const struct wl_interface wp_presentation_interface;
extern "C" const struct wl_interface wp_presentation_feedback_interface;

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

namespace {

void logf(const char *fmt, ...)
{
    static const bool quiet = getenv("VMVIEW_QUIET") != nullptr;
    if (quiet)
        return;
    va_list ap;
    va_start(ap, fmt);
    fputs("vmview: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
const bool g_debug = getenv("VMVIEW_DEBUG") != nullptr;
// VMVIEW_STATS=1: every 5 s, the guest's frames the host presented and how many of them went straight
// to the screen (presentation feedback's ZERO_COPY: direct scanout), and the discarded ones
const bool g_stats = getenv("VMVIEW_STATS") != nullptr;
// VMVIEW_STANDALONE=1 (for comparisons): the guest's xdg-shell goes to the host as well, so its window is
// an ordinary window of the app's connection instead of a subsurface of the app's window
const bool g_standalone = getenv("VMVIEW_STANDALONE") != nullptr;
// the guest's surfaces clipped to the view (2026-10-31); VMVIEW_NO_CLIP=1: as before, for comparisons
const bool g_clip = !g_standalone && getenv("VMVIEW_NO_CLIP") == nullptr;

// Host globals the guest may see: what the nested desktop and its proxy use, nothing that lets a guest
// watch or drive the host (data control, virtual input, window management, outputs, layer shell...).
// xdg_wm_base is answered here (the guest's toplevel becomes a subsurface); decorations, toplevel
// icons, exporters and the like would name that toplevel to the host and are left out.
const std::unordered_set<std::string> kAllowed = {
    "wl_compositor", "wl_subcompositor", "wl_shm", "wl_seat", "wl_output", "wl_data_device_manager",
    "wl_fixes", "zwp_linux_dmabuf_v1", "wp_presentation", "wp_viewporter", "wp_fractional_scale_manager_v1",
    "wp_cursor_shape_manager_v1", "wp_commit_timing_manager_v1", "wp_fifo_manager_v1",
    "wp_tearing_control_manager_v1", "wp_single_pixel_buffer_manager_v1", "wp_alpha_modifier_v1",
    "wp_content_type_manager_v1", "xdg_activation_v1", "zxdg_output_manager_v1", "zwp_pointer_constraints_v1",
    "zwp_relative_pointer_manager_v1", "zwp_pointer_gestures_v1", "zwp_keyboard_shortcuts_inhibit_manager_v1",
    "zwp_primary_selection_device_manager_v1", "zwp_text_input_manager_v3", "zwp_idle_inhibit_manager_v1",
    "wp_color_manager_v1", "wp_color_representation_manager_v1", "zwp_tablet_manager_v2",
    "xdg_system_bell_v1", "wp_pointer_warp_v1",
};
constexpr uint32_t kWmBaseName = 0xfff00001; // our xdg_wm_base in the guest's registry
// event opcodes (wayland's client headers name the requests only)
constexpr uint16_t kDisplayError = 0, kDisplayDeleteId = 1;
constexpr uint16_t kRegistryGlobal = 0, kRegistryGlobalRemove = 1;
constexpr uint16_t kPointerEnter = 0, kPointerLeave = 1, kPointerFrame = 5;
constexpr uint16_t kPointerMotion = 2, kPointerButton = 3;
constexpr uint16_t kKeyboardEnter = 1, kKeyboardLeave = 2, kKeyboardKey = 3, kKeyboardModifiers = 4;
constexpr uint16_t kDataDeviceEnter = 1, kDataDeviceLeave = 2, kDataDeviceMotion = 3, kDataDeviceDrop = 4;
constexpr uint16_t kDataDeviceSelection = 5, kPrimarySelection = 1;
constexpr uint16_t kToplevelConfigure = 0, kToplevelClose = 1, kSurfaceConfigure = 0, kPopupDone = 1;
constexpr uint32_t kStateFullscreen = 2, kStateActivated = 4;
constexpr uint32_t kServerIdStart = 0xff000000;
// fds a guest connection may have queued that no complete request has taken yet (a request's fds come
// with its first bytes; more than this means fds sent for nothing: the connection is closed)
constexpr size_t kMaxPendingFds = 128;
// What a guest can make vmview hold or do at once (2026-10-31 review, M2): beyond, its connection goes (or a new one
// is refused), as a compositor drops a client that does not read its events
constexpr size_t kMaxBacklog = size_t(16) << 20; // bytes of events not read yet
constexpr size_t kMaxObjects = 65536;             // objects of one connection
constexpr size_t kMaxConns = 64;                  // guest connections
constexpr size_t kCoalesceAbove = size_t(64) << 10; // unread bytes beyond which pointer motion is held
constexpr int kMaxReadsPerPass = 64;              // 16 KiB reads of one connection per pass of the loop (a flood
                                                  // kept the loop there: vmview_destroy waited, the app's GUI too)
constexpr int kMaxArgs = 20;
constexpr int kMaxFdsOut = 28;

const vmview_iface *ifaceByName(const char *name)
{
    static const std::unordered_map<std::string, const vmview_iface *> map = [] {
        std::unordered_map<std::string, const vmview_iface *> m;
        for (unsigned i = 0; i < vmview_iface_count; i++)
            m.emplace(vmview_ifaces[i].name, &vmview_ifaces[i]);
        return m;
    }();
    auto it = map.find(name);
    return it == map.end() ? nullptr : it->second;
}

struct ArgSpec {
    char type;
    bool nullable;
};
int parseSignature(const char *sig, ArgSpec *out)
{
    int n = 0;
    bool nullable = false;
    for (; *sig; sig++) {
        if (*sig >= '0' && *sig <= '9')
            continue;
        if (*sig == '?') {
            nullable = true;
            continue;
        }
        if (n == kMaxArgs)
            return -1;
        out[n++] = {*sig, nullable};
        nullable = false;
    }
    return n;
}
uint32_t sinceOf(const char *sig)
{
    uint32_t v = 0;
    while (*sig >= '0' && *sig <= '9')
        v = v * 10 + uint32_t(*sig++ - '0');
    return v ? v : 1;
}

// One message for the guest, as words.
struct OutMsg {
    std::vector<uint32_t> w;
    std::vector<int> fds;
    uint16_t opcode;
    OutMsg(uint32_t id, uint16_t op) : opcode(op) { w = {id, 0}; }
    void u(uint32_t v) { w.push_back(v); }
    void bytes(const void *p, size_t n)
    {
        const size_t at = w.size();
        w.resize(at + (n + 3) / 4, 0);
        memcpy(&w[at], p, n);
    }
    void s(const char *str)
    {
        if (!str) {
            u(0);
            return;
        }
        const size_t n = strlen(str) + 1;
        u(uint32_t(n));
        bytes(str, n);
    }
    void a(const wl_array *arr)
    {
        u(uint32_t(arr ? arr->size : 0));
        if (arr && arr->size)
            bytes(arr->data, arr->size);
    }
    void finish() { w[1] = uint32_t(w.size() * 4) << 16 | opcode; }
};

enum class Kind : uint8_t { Forwarded, Display, Registry, WmBase, Positioner, XdgSurface, Toplevel, Popup, Viewport };
enum class Role : uint8_t {
    None, Pointer, Keyboard, Surface, DataDevice, PrimaryDevice, RelPointer, Gesture, TabletTool, TextInput, Touch
};
// the role the host gave a guest wl_surface: a second one would be a protocol error on the app's connection
enum class SurfaceRole : uint8_t { None, Subsurface, Cursor, DragIcon, Toplevel };

// Objects a surface may have one of: a second one is the host's protocol error (raised on the app's own
// connection, which ends the app), refused here on the guest's (2026-10-31 review, H1). The surface is
// each request's second argument, after the new id. Pointer constraints and shortcut inhibitors are per
// surface and pointer or seat in the protocols: one seat here.
enum : uint32_t {
    kExtViewport = 1u << 0, kExtFractional = 1u << 1, kExtCommitTimer = 1u << 2, kExtFifo = 1u << 3,
    kExtTearing = 1u << 4, kExtAlpha = 1u << 5, kExtContentType = 1u << 6, kExtColor = 1u << 7,
    kExtColorRepr = 1u << 8, kExtConstraint = 1u << 9, kExtInhibitor = 1u << 10, kExtSubsurface = 1u << 11,
};
struct PerSurfaceRule {
    const char *iface;
    uint16_t opcode;
    uint32_t bit;
    uint32_t code; // the interface's error
    const char *what;
};
const PerSurfaceRule kPerSurface[] = {
    {"wp_viewporter", 1, kExtViewport, 0, "the surface has a viewport already"},
    {"wp_fractional_scale_manager_v1", 1, kExtFractional, 0, "the surface has a fractional scale object already"},
    {"wp_commit_timing_manager_v1", 1, kExtCommitTimer, 0, "the surface has a commit timer already"},
    {"wp_fifo_manager_v1", 1, kExtFifo, 0, "the surface has a fifo object already"},
    {"wp_tearing_control_manager_v1", 1, kExtTearing, 0, "the surface has a tearing control object already"},
    {"wp_alpha_modifier_v1", 1, kExtAlpha, 0, "the surface has an alpha modifier already"},
    {"wp_content_type_manager_v1", 1, kExtContentType, 0, "the surface has a content type object already"},
    {"wp_color_manager_v1", 2, kExtColor, 1, "the surface has a color management surface already"},
    {"wp_color_representation_manager_v1", 1, kExtColorRepr, 1, "the surface has a color representation already"},
    {"zwp_pointer_constraints_v1", 1, kExtConstraint, 1, "the surface has a pointer constraint already"},
    {"zwp_pointer_constraints_v1", 2, kExtConstraint, 1, "the surface has a pointer constraint already"},
    {"zwp_keyboard_shortcuts_inhibit_manager_v1", 1, kExtInhibitor, 0, "the surface has a shortcuts inhibitor already"},
};
const PerSurfaceRule *perSurfaceRule(const char *iface, uint16_t opcode)
{
    for (const auto &r : kPerSurface)
        if (r.opcode == opcode && !strcmp(r.iface, iface))
            return &r;
    return nullptr;
}

Role roleOf(const char *name)
{
    static const std::unordered_map<std::string, Role> roles = {
        {"wl_pointer", Role::Pointer},
        {"wl_keyboard", Role::Keyboard},
        {"wl_surface", Role::Surface},
        {"wl_data_device", Role::DataDevice},
        {"zwp_primary_selection_device_v1", Role::PrimaryDevice},
        {"zwp_relative_pointer_v1", Role::RelPointer},
        {"zwp_pointer_gesture_swipe_v1", Role::Gesture},
        {"zwp_pointer_gesture_pinch_v1", Role::Gesture},
        {"zwp_pointer_gesture_hold_v1", Role::Gesture},
        {"zwp_tablet_tool_v2", Role::TabletTool},
        {"zwp_text_input_v3", Role::TextInput},
        {"wl_touch", Role::Touch},
    };
    auto it = roles.find(name);
    return it == roles.end() ? Role::None : it->second;
}

struct Conn;
struct View;
struct Object;

// ---- clipping the guest's surfaces to the view: the types (2026-10-31) ----
//
// Wayland clips no subsurface to its parent. vitrine's sandbox (2026-10-31): a guest subsurface at (-400, -400) was
// drawn over the app's menu bar, VM list and toolbar beside the view and took the clicks there; a buffer or a
// viewport bigger than the view does the same on the right and below. Each surface of the guest's toplevel tree is
// now cropped to the view by a viewport of ours (the guest's own composed into it) and moved by what is cut on its
// left and top; a compositor clips input regions to the surface, so input stays in the view too. Coordinates in
// events and requests are moved back by that cut. A surface all inside the view (the nested desktop's whole tree,
// normally) goes to the host exactly as the guest made it.
//
// Timing: the guest's subsurfaces are all synchronized on the host, so what the host shows of the tree changes only
// with a commit of the toplevel's surface (the root): the guest's, or one of ours standing for a commit the guest made
// in desynchronized mode or for a change of the view. Before each, the whole tree is checked against what the host
// will apply, and a surface that needs other values gets a commit of ours first (cached, applied with the root's):
// what the host shows is always checked.

constexpr size_t kMaxRegionOps = 256; // a region's rectangles kept; beyond, the bounding box of its adds
constexpr int kMaxDepth = 32;         // subsurface levels below a surface without a parent
constexpr size_t kMaxSurfaces = 1024; // wl_surfaces of one connection

// a wl_region as the guest made it
struct Region {
    std::vector<std::array<int32_t, 5>> ops; // {0 add | 1 subtract, x, y, width, height}
    bool overflow = false;
    int64_t box[4] = {INT64_MAX, INT64_MAX, INT64_MIN, INT64_MIN}; // the adds' bounding box: x0, y0, x1, y1
    void op(int32_t kind, int32_t x, int32_t y, int32_t w, int32_t h)
    {
        if (kind == 0 && w > 0 && h > 0) {
            box[0] = std::min<int64_t>(box[0], x);
            box[1] = std::min<int64_t>(box[1], y);
            box[2] = std::max<int64_t>(box[2], int64_t(x) + w);
            box[3] = std::max<int64_t>(box[3], int64_t(y) + h);
        }
        if (ops.size() < kMaxRegionOps)
            ops.push_back({kind, x, y, w, h});
        else
            overflow = true;
    }
};
using RegionRef = std::shared_ptr<const Region>;

// The double-buffered state that sizes a surface, as the guest set it (pending and cached: the fields in SET only)
enum : uint32_t {
    kStBuffer = 1u << 0, kStScale = 1u << 1, kStTransform = 1u << 2, kStSrc = 1u << 3, kStDst = 1u << 4,
    kStInput = 1u << 5, kStOpaque = 1u << 6,
};
struct SurfState {
    uint32_t set = 0;
    bool buffer = false; // a buffer attached (else the surface is unmapped)
    int32_t bw = 0, bh = 0;
    int32_t scale = 1, transform = 0;
    wl_fixed_t src[4] = {-256, -256, -256, -256}; // wp_viewport's source, unset: -1
    int32_t dst[2] = {-1, -1};                     // and destination
    RegionRef input;                               // null: infinite
    RegionRef opaque;                              // null: none
    void merge(const SurfState &o)
    {
        if (o.set & kStBuffer)
            buffer = o.buffer, bw = o.bw, bh = o.bh;
        if (o.set & kStScale)
            scale = o.scale;
        if (o.set & kStTransform)
            transform = o.transform;
        if (o.set & kStSrc)
            memcpy(src, o.src, sizeof src);
        if (o.set & kStDst)
            dst[0] = o.dst[0], dst[1] = o.dst[1];
        if (o.set & kStInput)
            input = o.input;
        if (o.set & kStOpaque)
            opaque = o.opaque;
        set |= o.set;
    }
};
struct Pos {
    bool set = false;
    int32_t x = 0, y = 0;
};
// a region as the host has it, or is to have it
struct HostRegion {
    uint8_t mode = 0; // 0: the guest's (null: an infinite input region, no opaque region) moved by -(dx, dy);
                      // 1: empty; 2: the guest's within clip (no viewports on the host: input only)
    RegionRef r;
    int64_t dx = 0, dy = 0;
    int32_t clip[4] = {0, 0, 0, 0};
    bool operator==(const HostRegion &o) const
    {
        return mode == o.mode && r == o.r && dx == o.dx && dy == o.dy && !memcmp(clip, o.clip, sizeof clip);
    }
};
// what the host has, or is to have, of a guest surface's own state: our viewport's values, the cut, the regions
struct HostVals {
    wl_fixed_t src[4] = {-256, -256, -256, -256};
    int32_t dst[2] = {-1, -1};
    int64_t ox = 0, oy = 0; // where the host's surface starts, in the guest's surface coordinates (the cut)
    HostRegion input, opaque;
    bool sameViewport(const HostVals &o) const
    {
        return !memcmp(src, o.src, sizeof src) && dst[0] == o.dst[0] && dst[1] == o.dst[1];
    }
    bool operator==(const HostVals &o) const
    {
        return sameViewport(o) && ox == o.ox && oy == o.oy && input == o.input && opaque == o.opaque;
    }
};
// a guest wl_surface's tree, state and what the host has of it. The host side runs synchronized for every guest
// subsurface: pending (requests since its commit), cached (committed, not applied), current (applied).
struct Geo {
    SurfState pend, cached, cur;
    bool hasCache = false;                // committed since the host last applied its state
    Object *parent = nullptr;             // through its live wl_subsurface
    Object *subsurface = nullptr;         // that wl_subsurface (the guest's object, its host proxy)
    std::vector<Object *> children;       // surfaces with a live wl_subsurface on it
    bool guestSync = true;                // the mode the guest set (the host's is always synchronized)
    Pos posPend, posCached;               // its position, as part of its parent's pending and cached state
    int32_t px = 0, py = 0;               // and as applied
    HostVals hostWill, hostCur;           // what the host will apply with its last commit; what it has
    int32_t hostPosWill[2] = {0, 0}, hostPosCur[2] = {0, 0}; // the host's position of it (sent; applied)
    wl_proxy *viewport = nullptr;         // our host viewport, made when first needed
    uint32_t viewportId = 0;              // the guest's wp_viewport (its errors)
    uint64_t viewportSeq = 0;
};

struct Object {
    uint32_t id = 0;
    const vmview_iface *info = nullptr;
    uint32_t version = 1;
    Kind kind = Kind::Forwarded;
    Role role = Role::None;
    uint64_t seq = 0;             // creation order (cleanup goes backwards)
    wl_proxy *proxy = nullptr;    // the host object (Forwarded)
    Conn *conn = nullptr;
    Object *link = nullptr;       // relative pointer -> its pointer; xdg_surface -> wl_surface; toplevel -> xdg_surface
    // input focus as the guest knows it
    bool focus = false;           // pointer over a guest surface / keyboard given to the guest / DnD on it
    bool pendingFrame = false;    // pointer events sent since the last wl_pointer.frame
    bool hostFocus = false;       // keyboard: the host gave the app's window the keyboard
    uint32_t lastSerial = 0;      // keyboard: the serial of the host's last enter
    // pointer: the host keeps the pointer on the app's window while a button is down, but moves it
    // between the window's surfaces, the app's and the guest's (KWin 6.7, a button held or not).
    // A drag begun on the guest's surface goes on there as the guest would have it of a compositor
    // of its own: over the app's surfaces the events go to that surface, moved by grabOffset, and
    // the guest's leave comes after the last button is let go (vitrine, 2026-10-30: a window dragged
    // past the view's edge was never let go in the guest, nor tiled or maximized there). The host
    // groups such a move in one frame (leave + enter); a leave whose frame brings no enter means the
    // pointer left the app (another window, an effect's grab, Alt+Tab): no release will come, so the
    // guest gets its leave at that frame and the held buttons are forgotten (2026-10-31 review: the
    // drag stayed on, and the app's own widgets' clicks went to the guest afterwards)
    uint32_t pointerSurface = 0;  // the guest's surface it knows the pointer over
    std::set<uint32_t> buttonsHeld; // buttons the guest saw pressed and not released
    bool grabOutside = false;     // meanwhile over one of the app's surfaces
    bool leaveUnmatched = false;  // a leave during a drag, no enter yet in its frame
    uint32_t leaveSerial = 0;     // that leave's serial
    double lastX = 0, lastY = 0;  // the last position the guest got, on pointerSurface
    double grabOffset[2] = {0, 0};
    uint32_t lastTime = 0;        // the last motion's time
    // data devices: the host's last selection, given to the guest only while it has the keyboard
    wl_proxy *selection = nullptr;
    bool selectionKnown = false;
    // wp_color_manager_v1: the features the host announced (supported_feature), as bits
    uint32_t colorFeatures = 0;
    // zwp_linux_buffer_params_v1: a guest's create_immed sent as create, its answer awaited (handle())
    uint32_t immedFor = 0;
    bool immedDone = false, immedFailed = false;
    wl_proxy *immedProxy = nullptr;
    // xdg
    int32_t geom[4] = {0, 0, 0, 0};
    bool hasGeom = false;
    SurfaceRole surfaceRole = SurfaceRole::None; // wl_surface
    // per-surface rules (2026-10-31 review, H1): one object of each kind per surface, and the subsurface
    // tree; the host would raise a second one or a cycle on the app's own connection
    uint32_t extensions = 0;      // wl_surface: the kinds of objects it has (kExt*)
    uint32_t parentSurface = 0;   // wl_surface with a live wl_subsurface: its parent (guest id)...
    uint64_t parentSeq = 0;       // ...and that parent's seq (ids are reused)
    uint32_t extSurface = 0;      // an object of one of those kinds (wl_subsurface too): its surface...
    uint64_t extSurfaceSeq = 0;   // ...that surface's seq...
    uint32_t extBit = 0;          // ...and the kind
    bool onAppWindow = false;     // a shortcuts inhibitor moved to the app's window
    wl_subsurface *subsurface = nullptr; // toplevel: the host subsurface of its wl_surface
    bool bufferAttached = false;  // wl_surface: a buffer attached since the last commit (VMVIEW_STATS)
    // pointer events held while the guest does not read (View::coalesce)
    std::vector<uint32_t> heldMotion; // wl_pointer: its latest motion, not sent yet
    std::vector<uint32_t> heldRel;    // zwp_relative_pointer_v1: its relative motion, summed, not sent yet
    bool heldFrame = false;           // wl_pointer: the frame of a group made only of held motion
    bool relHeld = false;             // wl_pointer: a relative pointer of it holds motion (a locked pointer's)
    bool groupSent = false;           // wl_pointer: an event of the current frame has gone out
    uint32_t configureSerial = 0;
    // clipping to the view
    int32_t bufW = 0, bufH = 0;       // wl_buffer: its size; zwp_linux_buffer_params_v1: its create's
    std::shared_ptr<Region> region;   // wl_region: what the guest made it of
    std::unique_ptr<Geo> geo;         // wl_surface (made on first use)
    std::vector<std::array<uint32_t, 2>> touches; // wl_touch: {touch id, guest surface} of the points down on the guest
};

struct Conn {
    View *view;
    int fd;
    bool dead = false;
    bool wantWrite = false;
    bool overflow = false; // its events backed up beyond kMaxBacklog: closed by the loop
    bool anyHeld = false;  // a pointer of it holds motion (View::coalesce)
    std::vector<uint8_t> in;
    std::vector<int> inFds;
    std::vector<uint8_t> out;
    std::vector<int> outFds;
    std::unordered_map<uint32_t, std::unique_ptr<Object>> objects;
    std::vector<uint32_t> freeServerIds;
    uint32_t nextServerId = kServerIdStart;
    // server-created objects removed, until their id is used again: a request the guest sent before it
    // knew takes its fds, as libwayland-server's zombies do (2026-10-31 review: they stayed queued and
    // the next request with an fd got them)
    std::unordered_map<uint32_t, const wl_interface *> zombies;
    Object *toplevel = nullptr;
    size_t surfaces = 0;   // wl_surfaces (kMaxSurfaces)
    int64_t clipDue = 0;   // the view changed: the tree is checked by then (CLOCK_MONOTONIC ns) if the guest has
                           // not committed its root meanwhile; 0: nothing due
    Conn(View *v, int f) : view(v), fd(f) {}
};

int32_t clamp32(int64_t v)
{
    return int32_t(std::clamp<int64_t>(v, INT32_MIN, INT32_MAX));
}

wl_fixed_t moveFixed(wl_fixed_t f, int64_t d)
{
    return clamp32(int64_t(f) + d * 256);
}

// the object behind a guest id, if it is still the one created with seq (ids are reused)
Object *objectAt(Conn &c, uint32_t id, uint64_t seq)
{
    if (!id)
        return nullptr;
    auto it = c.objects.find(id);
    return it != c.objects.end() && it->second->seq == seq ? it->second.get() : nullptr;
}

struct Global {
    std::string iface;
    uint32_t version;   // host's
    const vmview_iface *info;
    bool shown;
    uint32_t advertised; // to the guest: min(host's, ours)
};

struct Pending {
    int x = 0, y = 0, w = 640, h = 480;
    bool fullscreen = false;
    bool keyboard = true;
    bool stop = false;
};

struct View {
    wl_display *display = nullptr;
    wl_surface *parent = nullptr;
    wl_event_queue *queue = nullptr;
    wl_display *wrapper = nullptr;   // the display on our queue
    wl_registry *registry = nullptr; // ours, on our queue
    wl_subcompositor *subcompositor = nullptr;
    vmview_callbacks cb{};
    std::string socketPath;
    int listenFd = -1;
    int wakeFd = -1;
    std::thread thread;
    std::mutex mutex;
    Pending pending; // under mutex
    Pending state;   // the thread's copy
    std::map<uint32_t, Global> globals;
    std::vector<std::unique_ptr<Conn>> conns;
    std::unordered_map<wl_proxy *, Object *> byProxy; // our host objects
    uint64_t seq = 0;
    bool mapped = false;
    bool shortcutsCaptured = false; // a guest's shortcuts inhibitor moved to the app's window exists
    // clipping to the view: our own compositor (regions) and viewporter (crops); without a viewporter, input only
    wl_proxy *ownCompositor = nullptr, *ownViewporter = nullptr;
    // the anchor: a transparent pixel of ours, a subsurface of the app's PARENT at the view's top-left. The guest's
    // toplevels are subsurfaces of it, so their positions apply with commits of ours: under PARENT, a new toplevel sat
    // at PARENT's (0, 0) until the app's next commit (the clip's tests, 2026-10-31), over the app's own widgets
    wl_surface *anchor = nullptr;
    wl_subsurface *anchorSub = nullptr;
    wl_buffer *anchorBuffer = nullptr;
    // VMVIEW_STATS
    unsigned presented = 0, zeroCopy = 0, discarded = 0, pointerEvents = 0, keyEvents = 0;
    // the forwarder's own feedback on every guest commit with a new buffer: the guest asks for feedback
    // on the surface it chooses (the nested desktop: one that is not the one scanned out), this one is
    // on the surface the buffer is on, so its ZERO_COPY says whether that buffer went to the screen
    wl_proxy *ownPresentation = nullptr;
    std::unordered_set<wl_proxy *> ownFeedback;
    unsigned ownPresented = 0, ownZeroCopy = 0, ownDiscarded = 0;
    timespec statsSince{};
    void countFeedback(const Object *o, uint32_t opcode, const wl_argument *args);
    void ownFeedbackFor(Object *surface);
    void printStats();

    bool start(wl_display *d, wl_surface *p, const char *path, const vmview_callbacks *callbacks);
    void run();
    void stopAll();
    void applyPending();
    // guest connections
    void accept();
    void readConn(Conn &c);
    void flushConn(Conn &c);
    void closeConn(Conn &c, const char *why);
    void protocolError(Conn &c, uint32_t objectId, uint32_t code, const char *fmt, ...);
    void send(Conn &c, OutMsg &m);
    void append(Conn &c, OutMsg &m);
    bool coalesce(Conn &c, OutMsg &m);
    void releaseHeld(Conn &c, Object *pointer, bool withFrame);
    void releaseAllHeld(Conn &c);
    void handle(Conn &c, uint32_t id, uint16_t opcode, const uint8_t *body, size_t len);
    Object *add(Conn &c, uint32_t id, const vmview_iface *info, uint32_t version, Kind kind);
    uint32_t allocServerId(Conn &c);
    void remove(Conn &c, Object *o, bool sendDeleteId);
    void hostDestroy(Object *o);
    // the guest's registries and globals
    void sendGlobals(Conn &c, Object *registry);
    void hostGlobal(uint32_t name, const char *iface, uint32_t version);
    void hostGlobalRemove(uint32_t name);
    // xdg-shell, answered here
    void xdgRequest(Conn &c, Object *o, uint16_t opcode, const wl_argument *args, Object **objArgs);
    void configureToplevel(Conn &c);
    void placeToplevel(Conn &c);
    void setMapped(bool on);
    void setShortcutsCaptured(bool on);
    // clipping the guest's surfaces to the view
    Geo &geoOf(Object *s);
    Object *rootOf(Conn &c);
    void viewRect(Conn &c, int32_t vr[4]);
    bool wantFor(const SurfState &st, bool inView, int64_t ax, int64_t ay, const int32_t vr[4], bool hide, HostVals &w,
                 uint32_t *code, const char **why);
    void sendRegion(Object *s, bool input, const HostRegion &r);
    void sendVals(Object *s, const HostVals &w);
    void sendPos(Object *child, int64_t x, int64_t y);
    bool expectedAbs(Conn &c, Object *s, int64_t &ax, int64_t &ay);
    bool guestDesync(Object *s, Object **top);
    void guestCommit(Conn &c, Object *s);
    void rootCommit(Conn &c, bool guest);
    void topCommit(Conn &c, Object *top);
    void applied(Object *s, bool withPending);
    void cached(Object *s);
    bool viewNeedsFix(Conn &c);
    void detach(Object *s);
    bool cutOf(Conn &c, uint32_t surfaceId, int32_t &ox, int32_t &oy);
    // host events
    int event(wl_proxy *p, uint32_t opcode, const wl_message *m, wl_argument *args);
    void keyboardFocus(Conn &c);
    bool guestHasKeyboard(Conn &c);   // a guest keyboard has the focus
    bool guestHasInput(Conn &c);      // that, or a drag is over the guest
    static int dispatch(const void *data, void *target, uint32_t opcode, const wl_message *m, wl_argument *args)
    {
        return static_cast<View *>(const_cast<void *>(data))->event(static_cast<wl_proxy *>(target), opcode, m, args);
    }
    void watch(wl_proxy *p, Object *o)
    {
        wl_proxy_add_dispatcher(p, &View::dispatch, this, o);
        byProxy[p] = o;
    }
};

// ---- the guest's connection ----

void View::accept()
{
    const int fd = accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0)
        return;
    if (conns.size() >= kMaxConns) {
        logf("guest connection refused: %zu connections already", conns.size());
        close(fd);
        return;
    }
    conns.push_back(std::make_unique<Conn>(this, fd));
    // the guest's wl_display
    add(*conns.back(), 1, ifaceByName("wl_display"), 1, Kind::Display);
    logf("guest connected (%zu connection%s)", conns.size(), conns.size() == 1 ? "" : "s");
    if (cb.connection)
        cb.connection(cb.data, int(conns.size()));
}

void View::send(Conn &c, OutMsg &m)
{
    if (c.dead || c.overflow) {
        for (int f : m.fds)
            close(f);
        return;
    }
    if (c.out.size() > kMaxBacklog) {
        // the guest does not read its events: dropped from here, its connection closed by the loop (here, in an
        // event's handler, its objects may still be in use)
        c.overflow = true;
        for (int f : m.fds)
            close(f);
        return;
    }
    if (coalesce(c, m))
        return;
    append(c, m);
}

void View::append(Conn &c, OutMsg &m)
{
    m.finish();
    const size_t at = c.out.size();
    c.out.resize(at + m.w.size() * 4);
    memcpy(&c.out[at], m.w.data(), m.w.size() * 4);
    c.outFds.insert(c.outFds.end(), m.fds.begin(), m.fds.end());
    m.fds.clear();
}

// While the guest does not read (a paused VM, savevm, a stalled guest), its pointers' motion is held, the latest
// kept, instead of queued without end: 16 MiB of it ended the connection (kMaxBacklog). vitrine 2026-10-31: a paused
// VM's session would have ended on resume after ~4 min of 1000 Hz motion over its view (~33 s at 8 kHz). Relative
// motion is summed. A frame closing a group of held motion only is held too; any other event of the pointer sends
// the held motion first, so the order stays. releaseAllHeld sends what is held once the guest reads again.
// Returns true when m was held.
bool View::coalesce(Conn &c, OutMsg &m)
{
    auto it = c.objects.find(m.w[0]);
    if (it == c.objects.end())
        return false;
    Object *o = it->second.get();
    const bool backedUp = c.out.size() > kCoalesceAbove;
    if (o->role == Role::Pointer) {
        if (m.opcode == kPointerMotion) {
            if (!backedUp && o->heldMotion.empty()) {
                o->groupSent = true;
                return false;
            }
            o->heldMotion = m.w; // the words before finish(): id, 0, time, x, y
            c.anyHeld = true;
            return true;
        }
        if (m.opcode == kPointerFrame) {
            if (backedUp && (!o->heldMotion.empty() || o->relHeld) && !o->groupSent) {
                o->heldFrame = true;
                return true;
            }
            releaseHeld(c, o, false);
            o->groupSent = false;
            return false;
        }
        releaseHeld(c, o, false);
        o->groupSent = true;
        return false;
    }
    if (o->role == Role::RelPointer && m.opcode == 0 && m.w.size() == 8) { // relative_motion
        Object *pointer = o->link;
        if (!pointer || (!backedUp && pointer->heldMotion.empty() && !pointer->relHeld))
            return false;
        if (o->heldRel.empty()) {
            o->heldRel = m.w;
        } else {
            o->heldRel[2] = m.w[2]; // the latest time
            o->heldRel[3] = m.w[3];
            for (int k = 4; k < 8; k++) // dx, dy, dx_unaccel, dy_unaccel (wl_fixed_t) summed
                o->heldRel[k] = uint32_t(int32_t(o->heldRel[k]) + int32_t(m.w[k]));
        }
        pointer->relHeld = true;
        c.anyHeld = true;
        return true;
    }
    return false;
}

void View::releaseHeld(Conn &c, Object *pointer, bool withFrame)
{
    // nothing held: the usual case, called for every pointer event (no scan of the objects)
    if (pointer->heldMotion.empty() && !pointer->relHeld && !pointer->heldFrame)
        return;
    if (!pointer->heldMotion.empty()) {
        OutMsg m(pointer->id, kPointerMotion);
        m.w = std::move(pointer->heldMotion);
        pointer->heldMotion.clear();
        append(c, m);
    }
    if (pointer->relHeld) {
        for (auto &kv : c.objects) {
            Object *r = kv.second.get();
            if (r->role == Role::RelPointer && r->link == pointer && !r->heldRel.empty()) {
                OutMsg m(r->id, 0);
                m.w = std::move(r->heldRel);
                r->heldRel.clear();
                append(c, m);
            }
        }
        pointer->relHeld = false;
    }
    if (withFrame) {
        OutMsg f(pointer->id, kPointerFrame);
        append(c, f);
        pointer->groupSent = false;
    }
    pointer->heldFrame = false;
}

void View::releaseAllHeld(Conn &c)
{
    if (!c.anyHeld)
        return;
    c.anyHeld = false;
    for (auto &kv : c.objects)
        if (kv.second->role == Role::Pointer)
            releaseHeld(c, kv.second.get(), true);
}

void View::flushConn(Conn &c)
{
    size_t done = 0;
    while (!c.dead && done < c.out.size()) {
        iovec iov{c.out.data() + done, c.out.size() - done};
        msghdr mh{};
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        alignas(cmsghdr) char cbuf[CMSG_SPACE(sizeof(int) * kMaxFdsOut)];
        const size_t nfds = std::min<size_t>(c.outFds.size(), kMaxFdsOut);
        if (nfds) {
            mh.msg_control = cbuf;
            mh.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
            cmsghdr *cm = CMSG_FIRSTHDR(&mh);
            cm->cmsg_level = SOL_SOCKET;
            cm->cmsg_type = SCM_RIGHTS;
            cm->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
            memcpy(CMSG_DATA(cm), c.outFds.data(), sizeof(int) * nfds);
        }
        const ssize_t n = sendmsg(c.fd, &mh, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            closeConn(c, strerror(errno));
            return;
        }
        // fds go with the first bytes after them (early is fine: the reader queues them)
        for (size_t i = 0; i < nfds; i++)
            close(c.outFds[i]);
        c.outFds.erase(c.outFds.begin(), c.outFds.begin() + long(nfds));
        done += size_t(n);
    }
    c.out.erase(c.out.begin(), c.out.begin() + long(done));
    c.wantWrite = !c.out.empty();
}

void View::readConn(Conn &c)
{
    uint8_t buf[16384];
    alignas(cmsghdr) char cbuf[CMSG_SPACE(sizeof(int) * 64)];
    for (int reads = 0; reads < kMaxReadsPerPass; reads++) {
        iovec iov{buf, sizeof buf};
        msghdr mh{};
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        mh.msg_control = cbuf;
        mh.msg_controllen = sizeof cbuf;
        const ssize_t n = recvmsg(c.fd, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            closeConn(c, strerror(errno));
            return;
        }
        for (cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
                const size_t k = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                const int *fds = reinterpret_cast<const int *>(CMSG_DATA(cm));
                c.inFds.insert(c.inFds.end(), fds, fds + k);
            }
        }
        if (mh.msg_flags & MSG_CTRUNC) {
            closeConn(c, "too many fds in one message");
            return;
        }
        if (n == 0) {
            closeConn(c, "the guest closed the connection");
            return;
        }
        c.in.insert(c.in.end(), buf, buf + n);
        size_t pos = 0;
        while (!c.dead && c.in.size() - pos >= 8) {
            uint32_t hdr[2];
            memcpy(hdr, &c.in[pos], 8);
            const uint32_t size = hdr[1] >> 16;
            if (size < 8 || size % 4) {
                protocolError(c, 1, WL_DISPLAY_ERROR_INVALID_METHOD, "bad message size %u", size);
                return;
            }
            if (c.in.size() - pos < size)
                break;
            // data() + offset, not &c.in[pos + 8]: an argument-less request (wl_surface.commit) at the end of
            // the buffer indexes one past it, which aborts builds with _GLIBCXX_ASSERTIONS (2026-10-31 review)
            handle(c, hdr[0], uint16_t(hdr[1] & 0xffff), c.in.data() + pos + 8, size - 8);
            pos += size;
        }
        if (c.dead)
            return;
        // fds wait here only until the requests that carry them are complete: a guest that sends fds no
        // request takes would otherwise fill the app's fd table (2026-10-31 review)
        if (c.inFds.size() > kMaxPendingFds) {
            closeConn(c, "too many file descriptors not taken by any request");
            return;
        }
        c.in.erase(c.in.begin(), c.in.begin() + long(pos));
        if (size_t(n) < sizeof buf)
            break;
    }
}

void View::protocolError(Conn &c, uint32_t objectId, uint32_t code, const char *fmt, ...)
{
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    logf("guest protocol error on object %u: %s", objectId, text);
    OutMsg m(1, kDisplayError);
    m.u(objectId);
    m.u(code);
    m.s(text);
    send(c, m);
    flushConn(c);
    closeConn(c, "protocol error");
}

Object *View::add(Conn &c, uint32_t id, const vmview_iface *info, uint32_t version, Kind kind)
{
    auto o = std::make_unique<Object>();
    o->id = id;
    o->info = info;
    o->version = version;
    o->kind = kind;
    o->role = roleOf(info->name);
    o->seq = ++seq;
    o->conn = &c;
    if (o->role == Role::Surface)
        c.surfaces++;
    Object *p = o.get();
    c.zombies.erase(id);
    c.objects[id] = std::move(o);
    return p;
}

uint32_t View::allocServerId(Conn &c)
{
    if (!c.freeServerIds.empty()) {
        // the lowest free one: libwayland's object map only takes ids next to the ones it has
        auto it = std::min_element(c.freeServerIds.begin(), c.freeServerIds.end());
        const uint32_t id = *it;
        c.freeServerIds.erase(it);
        return id;
    }
    return c.nextServerId++;
}

// Destroys our host object of O: its destructor request when it has one without arguments that its
// version allows, else just our proxy (the host keeps its side until the app's connection closes).
void View::hostDestroy(Object *o)
{
    // the guest's shortcuts capture goes with its inhibitor, however it goes (destroyed, or its
    // connection closed)
    if (o->onAppWindow) {
        o->onAppWindow = false;
        setShortcutsCaptured(false);
    }
    if (o->subsurface) {
        byProxy.erase(reinterpret_cast<wl_proxy *>(o->subsurface));
        wl_subsurface_destroy(o->subsurface);
        o->subsurface = nullptr;
    }
    // our viewport of a guest surface (clipping), before the surface
    if (o->geo && o->geo->viewport) {
        wl_proxy_marshal_flags(o->geo->viewport, 0, nullptr, 1, WL_MARSHAL_FLAG_DESTROY); // destroy
        o->geo->viewport = nullptr;
    }
    if (!o->proxy)
        return;
    byProxy.erase(o->proxy);
    const wl_interface *wi = o->info->iface;
    for (int op = 0; op < wi->method_count; op++) {
        if (!(o->info->destructor_requests & (1ull << op)))
            continue;
        ArgSpec specs[kMaxArgs];
        if (parseSignature(wi->methods[op].signature, specs) == 0 && sinceOf(wi->methods[op].signature) <= o->version) {
            wl_proxy_marshal_array_flags(o->proxy, uint32_t(op), nullptr, o->version, WL_MARSHAL_FLAG_DESTROY, nullptr);
            o->proxy = nullptr;
            return;
        }
    }
    wl_proxy_destroy(o->proxy);
    o->proxy = nullptr;
}

void View::remove(Conn &c, Object *o, bool sendDeleteId)
{
    const uint32_t id = o->id;
    if (o == c.toplevel) {
        c.toplevel = nullptr;
        setMapped(false);
    }
    // links point at pointers (relative pointers, gestures), wl_surfaces and xdg_surfaces only: the
    // frequent removals (frame callbacks, presentation feedback) skip the scan
    if (o->role == Role::Pointer || o->role == Role::Surface || o->kind == Kind::XdgSurface)
        for (auto &kv : c.objects)
            if (kv.second->link == o)
                kv.second->link = nullptr;
    if (o->extBit) {
        if (Object *sf = objectAt(c, o->extSurface, o->extSurfaceSeq)) {
            sf->extensions &= ~o->extBit;
            if (o->extBit == kExtSubsurface)
                sf->parentSurface = 0, sf->parentSeq = 0;
            // clipping: the surface leaves its parent's tree with its wl_subsurface
            if (o->extBit == kExtSubsurface && sf->geo && sf->geo->subsurface == o)
                detach(sf);
            if (o->extBit == kExtViewport && sf->geo && sf->geo->viewportId == id)
                sf->geo->viewportId = 0;
        }
    }
    if (o->role == Role::Surface) {
        c.surfaces--;
        if (o->geo) {
            // clipping: out of its parent's tree; its children are unmapped, theirs no longer
            detach(o);
            for (Object *ch : o->geo->children)
                if (ch->geo)
                    ch->geo->parent = nullptr;
            o->geo->children.clear();
        }
    }
    const wl_interface *iface = o->info->iface;
    hostDestroy(o);
    c.objects.erase(id);
    if (id >= kServerIdStart) {
        c.freeServerIds.push_back(id);
        if (c.zombies.size() > 4096)
            c.zombies.clear();
        c.zombies[id] = iface;
    }
    else if (sendDeleteId) {
        OutMsg m(1, kDisplayDeleteId);
        m.u(id);
        send(c, m);
    }
}

void View::closeConn(Conn &c, const char *why)
{
    if (c.dead)
        return;
    c.dead = true;
    logf("guest connection closed: %s", why);
    // the host objects, newest first (children before what they were made from)
    std::vector<Object *> all;
    for (auto &kv : c.objects)
        all.push_back(kv.second.get());
    std::sort(all.begin(), all.end(), [](Object *a, Object *b) { return a->seq > b->seq; });
    for (Object *o : all)
        hostDestroy(o);
    if (c.toplevel) {
        c.toplevel = nullptr;
        setMapped(false);
    }
    c.objects.clear();
    for (int f : c.inFds)
        close(f);
    for (int f : c.outFds)
        close(f);
    c.inFds.clear();
    c.outFds.clear();
    close(c.fd);
    c.fd = -1;
    wl_display_flush(display);
}

// ---- registry ----

void View::sendGlobals(Conn &c, Object *reg)
{
    for (auto &kv : globals) {
        if (!kv.second.shown)
            continue;
        OutMsg m(reg->id, kRegistryGlobal);
        m.u(kv.first);
        m.s(kv.second.iface.c_str());
        m.u(kv.second.advertised);
        send(c, m);
    }
    if (g_standalone)
        return;
    const vmview_iface *wm = ifaceByName("xdg_wm_base");
    OutMsg m(reg->id, kRegistryGlobal);
    m.u(kWmBaseName);
    m.s("xdg_wm_base");
    m.u(wm->version);
    send(c, m);
}

void View::hostGlobal(uint32_t name, const char *iface, uint32_t version)
{
    Global g{iface, version, ifaceByName(iface), false, 0};
    g.shown = g.info && (kAllowed.count(g.iface) ||
                         (g_standalone && (g.iface == "xdg_wm_base" || g.iface == "zxdg_decoration_manager_v1")));
    g.advertised = g.info ? std::min(version, g.info->version) : 0;
    globals[name] = g;
    if (g_debug)
        logf("host global %u %s v%u: %s", name, iface, version, g.shown ? "shown" : "hidden");
    if (!g.shown)
        return;
    for (auto &c : conns)
        for (auto &kv : c->objects)
            if (kv.second->kind == Kind::Registry) {
                OutMsg m(kv.first, kRegistryGlobal);
                m.u(name);
                m.s(iface);
                m.u(g.advertised);
                send(*c, m);
            }
}

void View::hostGlobalRemove(uint32_t name)
{
    auto it = globals.find(name);
    if (it == globals.end())
        return;
    const bool shown = it->second.shown;
    globals.erase(it);
    if (!shown)
        return;
    for (auto &c : conns)
        for (auto &kv : c->objects)
            if (kv.second->kind == Kind::Registry) {
                OutMsg m(kv.first, kRegistryGlobalRemove);
                m.u(name);
                send(*c, m);
            }
}

// ---- guest requests ----

// A request the host compositor would refuse with a protocol error, which it raises on the app's own
// connection: the app's whole Wayland connection then ends (the app quits). The one-message cases are
// refused here instead, with the same error on the guest's connection (2026-10-31 review). The rules that
// need per-surface state (one viewport and the like per surface, the subsurface tree) are in handle().
static bool refusedByHost(const char *iface, uint16_t opcode, const wl_argument *a, uint32_t *code, const char **why)
{
    if (!strcmp(iface, "wl_surface")) {
        if (opcode == 8 && a[0].i <= 0) { // set_buffer_scale: invalid_scale
            *code = 0;
            *why = "the buffer scale must be positive";
            return true;
        }
        if (opcode == 7 && (a[0].i < 0 || a[0].i > 7)) { // set_buffer_transform: invalid_transform
            *code = 1;
            *why = "invalid buffer transform";
            return true;
        }
    } else if (!strcmp(iface, "wl_shm")) {
        if (opcode == 0 && a[2].i <= 0) { // create_pool(id, fd, size): invalid_stride, as libwayland
            *code = 1;
            *why = "invalid pool size";
            return true;
        }
    } else if (!strcmp(iface, "wl_shm_pool")) {
        // create_buffer(id, offset, width, height, stride, format): invalid_stride
        if (opcode == 0 && (a[1].i < 0 || a[2].i <= 0 || a[3].i <= 0 || a[4].i <= 0)) {
            *code = 1;
            *why = "invalid buffer offset, size or stride";
            return true;
        }
    } else if (!strcmp(iface, "zwp_linux_buffer_params_v1")) {
        if (opcode == 1 && a[1].u > 3) { // add(fd, plane_idx, ...): plane_idx
            *code = 1;
            *why = "plane index out of bounds";
            return true;
        }
        // create(width, height, format, flags), create_immed(buffer_id, width, height, ...): invalid_dimensions
        if ((opcode == 2 && (a[0].i <= 0 || a[1].i <= 0)) || (opcode == 3 && (a[1].i <= 0 || a[2].i <= 0))) {
            *code = 5;
            *why = "invalid buffer size";
            return true;
        }
    }
    return false;
}

void View::handle(Conn &c, uint32_t id, uint16_t opcode, const uint8_t *body, size_t len)
{
    auto it = c.objects.find(id);
    if (it == c.objects.end()) {
        // a request to an object we already destroyed (its destructor event crossed it): ignored, as
        // libwayland-server does for zombies, its fds taken and closed
        if (id >= kServerIdStart) {
            auto z = c.zombies.find(id);
            if (z != c.zombies.end() && opcode < z->second->method_count) {
                for (const char *t = z->second->methods[opcode].signature; *t; t++) {
                    if (*t == 'h' && !c.inFds.empty()) {
                        close(c.inFds.front());
                        c.inFds.erase(c.inFds.begin());
                    }
                }
            }
            return;
        }
        protocolError(c, 1, WL_DISPLAY_ERROR_INVALID_OBJECT, "invalid object %u", id);
        return;
    }
    Object *o = it->second.get();
    const wl_interface *wi = o->info->iface;
    if (opcode >= wi->method_count) {
        protocolError(c, id, WL_DISPLAY_ERROR_INVALID_METHOD, "%s has no request %u", wi->name, opcode);
        return;
    }
    const wl_message &msg = wi->methods[opcode];
    if (sinceOf(msg.signature) > o->version) {
        protocolError(c, id, WL_DISPLAY_ERROR_INVALID_METHOD, "%s.%s needs version %u (bound %u)", wi->name,
                      msg.name, sinceOf(msg.signature), o->version);
        return;
    }
    ArgSpec specs[kMaxArgs];
    const int n = parseSignature(msg.signature, specs);
    wl_argument args[kMaxArgs]{};
    Object *objArgs[kMaxArgs]{};
    wl_array arrays[kMaxArgs]{};
    int newArg = -1;
    uint32_t newId = 0;
    const vmview_iface *newInfo = nullptr;
    std::vector<int> fds;
    auto fail = [&](const char *what) {
        for (int f : fds)
            close(f);
        protocolError(c, id, WL_DISPLAY_ERROR_INVALID_METHOD, "%s.%s: %s", wi->name, msg.name, what);
    };
    size_t off = 0;
    auto word = [&](uint32_t &v) {
        if (len - off < 4)
            return false;
        memcpy(&v, body + off, 4);
        off += 4;
        return true;
    };
    for (int i = 0; i < n; i++) {
        uint32_t v = 0;
        switch (specs[i].type) {
        case 'i':
        case 'u':
        case 'f':
            if (!word(v))
                return fail("message too short");
            args[i].u = v;
            break;
        case 's':
        case 'a': {
            if (!word(v))
                return fail("message too short");
            const size_t padded = (size_t(v) + 3) & ~size_t(3);
            if (len - off < padded)
                return fail("message too short");
            if (specs[i].type == 's') {
                if (v == 0) {
                    if (!specs[i].nullable)
                        return fail("null string");
                    args[i].s = nullptr;
                } else {
                    if (body[off + v - 1] != 0)
                        return fail("string without its NUL");
                    args[i].s = reinterpret_cast<const char *>(body + off);
                }
            } else {
                arrays[i].size = arrays[i].alloc = v;
                arrays[i].data = const_cast<uint8_t *>(body + off);
                args[i].a = &arrays[i];
            }
            off += padded;
            break;
        }
        case 'o': {
            if (!word(v))
                return fail("message too short");
            if (v == 0) {
                if (!specs[i].nullable)
                    return fail("null object");
                args[i].o = nullptr;
                break;
            }
            auto ot = c.objects.find(v);
            if (ot == c.objects.end())
                return fail("unknown object argument");
            Object *a = ot->second.get();
            if (msg.types[i] && strcmp(msg.types[i]->name, a->info->name) != 0)
                return fail("object argument of the wrong interface");
            objArgs[i] = a;
            args[i].o = reinterpret_cast<wl_object *>(a->proxy);
            break;
        }
        case 'n': {
            if (!word(v))
                return fail("message too short");
            if (v == 0 || v >= kServerIdStart || c.objects.count(v))
                return fail("invalid new id");
            if (c.objects.size() >= kMaxObjects)
                return fail("too many objects");
            if (msg.types[i] && !strcmp(msg.types[i]->name, "wl_surface") && c.surfaces >= kMaxSurfaces)
                return fail("too many surfaces");
            newArg = i;
            newId = v;
            if (msg.types[i]) {
                newInfo = ifaceByName(msg.types[i]->name);
            } else if (i >= 2 && specs[i - 2].type == 's' && args[i - 2].s) {
                newInfo = ifaceByName(args[i - 2].s); // wl_registry.bind: interface, version, id
            }
            if (!newInfo)
                return fail("new object of an interface this forwarder does not know");
            args[i].n = 0;
            break;
        }
        case 'h':
            if (c.inFds.empty())
                return fail("fd missing");
            args[i].h = c.inFds.front();
            fds.push_back(c.inFds.front());
            c.inFds.erase(c.inFds.begin());
            break;
        default:
            return fail("unknown argument type");
        }
    }
    if (off != len)
        return fail("message longer than its signature");
    if (g_debug)
        logf("-> %s@%u.%s", wi->name, id, msg.name);
    // wp_color_manager_v1's creators for a feature the host did not announce: its unsupported_feature
    // error, which it raises on the app's own connection (vitrine 2026-10-31: create_icc_creator from a
    // raw client of the guest's proxy ended vitrine). create_icc_creator 4, create_parametric_creator 5,
    // create_windows_scrgb 6, create_windows_bt2100 8 - the features icc_v2_v4 0, parametric 1,
    // windows_scrgb 7, windows_bt2100 8
    if (!strcmp(wi->name, "wp_color_manager_v1")) {
        const int need = opcode == 4 ? 0 : opcode == 5 ? 1 : opcode == 6 ? 7 : opcode == 8 ? 8 : -1;
        if (need >= 0 && !(o->colorFeatures & (1u << need))) {
            for (int f : fds)
                close(f);
            protocolError(c, id, 0, "wp_color_manager_v1.%s: the host does not support this feature", msg.name);
            return;
        }
    }
    uint32_t refusedCode = 0;
    const char *refusedWhy = nullptr;
    if (refusedByHost(wi->name, opcode, args, &refusedCode, &refusedWhy)) {
        for (int f : fds)
            close(f);
        protocolError(c, id, refusedCode, "%s.%s: %s", wi->name, msg.name, refusedWhy);
        return;
    }
    // the host's clipboard is read only by a guest that has the keyboard, or that a drag is over (a
    // drag's wl_data_offer): in the background (another VM's view in front, the app's own widgets
    // focused) it got the host's selection and could read it at any time (2026-10-31 review). Refused,
    // the fd closed: the guest reads nothing.
    if (((!strcmp(wi->name, "wl_data_offer") && opcode == 1) ||
         (!strcmp(wi->name, "zwp_primary_selection_offer_v1") && opcode == 0)) &&
        !guestHasInput(c)) {
        if (g_debug)
            logf("   refused: the guest has neither the keyboard nor a drag over it");
        for (int f : fds)
            close(f);
        return;
    }

    switch (o->kind) {
    case Kind::Display:
        if (opcode == WL_DISPLAY_SYNC) {
            auto *cbk = reinterpret_cast<wl_proxy *>(wl_display_sync(wrapper));
            Object *no = add(c, newId, ifaceByName("wl_callback"), 1, Kind::Forwarded);
            no->proxy = cbk;
            watch(cbk, no);
        } else if (opcode == WL_DISPLAY_GET_REGISTRY) {
            Object *reg = add(c, newId, ifaceByName("wl_registry"), 1, Kind::Registry);
            sendGlobals(c, reg);
        }
        break;
    case Kind::Registry: {
        // bind(name, interface, version, id)
        const uint32_t name = args[0].u, version = args[2].u;
        if (name == kWmBaseName && !g_standalone) {
            if (strcmp(args[1].s, "xdg_wm_base") || version < 1 || version > ifaceByName("xdg_wm_base")->version)
                return fail("bad xdg_wm_base bind");
            add(c, newId, ifaceByName("xdg_wm_base"), version, Kind::WmBase);
            break;
        }
        auto g = globals.find(name);
        if (g == globals.end() || !g->second.shown || g->second.iface != args[1].s || version < 1 ||
            version > g->second.advertised)
            return fail("bind of a global that was not offered, or above its version");
        auto *p = static_cast<wl_proxy *>(wl_registry_bind(registry, name, newInfo->iface, version));
        Object *no = add(c, newId, newInfo, version, Kind::Forwarded);
        no->proxy = p;
        watch(p, no);
        break;
    }
    case Kind::Viewport: {
        // the guest's wp_viewport (clipping to the view): its values are the surface's pending state here, the
        // host's viewport of the surface is ours. The checks are the host's: bad_value here, bad_size and
        // out_of_buffer with the commit (wantFor), no_surface
        Object *sf = objectAt(c, o->extSurface, o->extSurfaceSeq);
        if (opcode == 0) { // destroy: the surface's source and destination unset with its next commit
            if (sf) {
                Geo &g = geoOf(sf);
                for (auto &v : g.pend.src)
                    v = -256;
                g.pend.dst[0] = g.pend.dst[1] = -1;
                g.pend.set |= kStSrc | kStDst;
                g.viewportId = 0;
            }
            remove(c, o, true);
            break;
        }
        if (!sf)
            return protocolError(c, id, 3, "wp_viewport.%s: the surface was destroyed", msg.name); // no_surface
        Geo &g = geoOf(sf);
        if (opcode == 1) { // set_source(x, y, width, height)
            const bool unset = args[0].i == -256 && args[1].i == -256 && args[2].i == -256 && args[3].i == -256;
            if (!unset && (args[0].i < 0 || args[1].i < 0 || args[2].i <= 0 || args[3].i <= 0))
                return protocolError(c, id, 0, "wp_viewport.set_source: invalid rectangle"); // bad_value
            for (int i = 0; i < 4; i++)
                g.pend.src[i] = args[i].i;
            g.pend.set |= kStSrc;
        } else if (opcode == 2) { // set_destination(width, height)
            const bool unset = args[0].i == -1 && args[1].i == -1;
            if (!unset && (args[0].i <= 0 || args[1].i <= 0))
                return protocolError(c, id, 0, "wp_viewport.set_destination: invalid size"); // bad_value
            g.pend.dst[0] = args[0].i;
            g.pend.dst[1] = args[1].i;
            g.pend.set |= kStDst;
        }
        break;
    }
    case Kind::WmBase:
    case Kind::Positioner:
    case Kind::XdgSurface:
    case Kind::Toplevel:
    case Kind::Popup:
        if (newArg >= 0) {
            Kind k = Kind::Positioner;
            if (o->kind == Kind::WmBase && opcode == 2)
                k = Kind::XdgSurface; // get_xdg_surface
            else if (o->kind == Kind::XdgSurface && opcode == 1)
                k = Kind::Toplevel; // get_toplevel
            else if (o->kind == Kind::XdgSurface && opcode == 2)
                k = Kind::Popup; // get_popup
            Object *no = add(c, newId, newInfo, o->version, k);
            args[newArg].n = newId;
            objArgs[newArg] = no;
        }
        xdgRequest(c, o, opcode, args, objArgs);
        for (int f : fds)
            close(f);
        break;
    case Kind::Forwarded: {
        if (!o->proxy)
            return fail("object without a host object");
        for (int i = 0; i < n; i++)
            if (specs[i].type == 'o' && objArgs[i] && !objArgs[i]->proxy)
                return fail("an object of the guest's window (xdg-shell) cannot go to the host");
        // requests that act on the window with the keyboard focus name the app's window instead of
        // the guest's toplevel surface (a subsurface of it, which never has the focus): the host's
        // shortcuts are captured while the app's window has the keyboard, activation activates it
        Object *tlSurface = c.toplevel && c.toplevel->link ? c.toplevel->link->link : nullptr;
        bool movedInhibitor = false;
        if (tlSurface && ((!strcmp(o->info->name, "zwp_keyboard_shortcuts_inhibit_manager_v1") && opcode == 1) ||
                          (!strcmp(o->info->name, "xdg_activation_v1") && opcode == 2) ||        // activate
                          (!strcmp(o->info->name, "xdg_activation_token_v1") && opcode == 2))) { // set_surface
            for (int i = 0; i < n; i++)
                if (specs[i].type == 'o' && objArgs[i] == tlSurface) {
                    args[i].o = reinterpret_cast<wl_object *>(parent);
                    movedInhibitor = opcode == 1 && o->info->name[0] == 'z';
                }
            // one inhibitor per surface and seat, or the host ends the connection (already_inhibited)
            if (movedInhibitor)
                for (auto &kv : c.objects)
                    if (kv.second->onAppWindow)
                        return fail("a second shortcuts inhibitor for the guest's window");
        }
        // roles: a surface the host already gave a role must not get another one (the host's protocol
        // error would end the app's whole connection): refused here, the guest's connection only
        auto takeRole = [&](Object *sf, SurfaceRole role) {
            if (!sf)
                return true;
            if (sf->surfaceRole != SurfaceRole::None && sf->surfaceRole != role)
                return false;
            sf->surfaceRole = role;
            return true;
        };
        // the error a request is refused with, on the guest's connection
        auto refuse = [&](uint32_t code, const char *what) {
            for (int f : fds)
                close(f);
            protocolError(c, id, code, "%s.%s: %s", wi->name, msg.name, what);
        };
        Object *subsurfaceOf = nullptr; // get_subsurface: the surface it makes a subsurface
        if (!strcmp(o->info->name, "wl_subcompositor") && opcode == 1) { // get_subsurface(id, surface, parent)
            Object *sf = objArgs[1], *parentSf = objArgs[2];
            // bad_surface: its own parent, another role, or a live wl_subsurface already (one whose
            // wl_subsurface was destroyed may get a new one)
            if (sf == parentSf || (sf->surfaceRole != SurfaceRole::None && sf->surfaceRole != SurfaceRole::Subsurface) ||
                (sf->extensions & kExtSubsurface))
                return refuse(0, "the surface has another role or a subsurface already, or is its own parent");
            // bad_parent: the parent is one of the surface's descendants (a cycle)
            int depth = 0;
            for (Object *p = parentSf; p && depth < 4096; p = objectAt(c, p->parentSurface, p->parentSeq), depth++)
                if (p == sf)
                    return refuse(1, "the parent is a descendant of the surface");
            // the tree's depth: the parent's levels, and the surface's own subtree below it (clipping walks it)
            if (g_clip) {
                int below = 0;
                std::vector<std::pair<Object *, int>> todo{{sf, 0}};
                for (size_t k = 0; k < todo.size() && k <= kMaxSurfaces; k++) {
                    below = std::max(below, todo[k].second);
                    if (todo[k].first->geo)
                        for (Object *ch : todo[k].first->geo->children)
                            todo.push_back({ch, todo[k].second + 1});
                }
                if (depth + below > kMaxDepth)
                    return refuse(1, "the subsurface tree would be deeper than 32 levels");
            }
            sf->surfaceRole = SurfaceRole::Subsurface;
            subsurfaceOf = sf;
        } else if (!strcmp(o->info->name, "wl_subsurface") && (opcode == 2 || opcode == 3)) {
            // place_above/place_below(sibling): bad_surface unless the sibling is the parent or another
            // subsurface of it (not checked once the surface or its parent is gone)
            Object *sf = objectAt(c, o->extSurface, o->extSurfaceSeq);
            Object *parentSf = sf ? objectAt(c, sf->parentSurface, sf->parentSeq) : nullptr;
            Object *sib = objArgs[0];
            if (parentSf && sib != parentSf &&
                (sib == sf || !(sib->extensions & kExtSubsurface) || objectAt(c, sib->parentSurface, sib->parentSeq) != parentSf))
                return refuse(0, "the sibling is neither the parent nor a subsurface of it");
        } else if (!strcmp(o->info->name, "wl_pointer") && opcode == 0) { // set_cursor(serial, surface, x, y)
            if (!takeRole(objArgs[1], SurfaceRole::Cursor))
                return fail("cursor surface with another role");
        } else if (!strcmp(o->info->name, "wl_data_device") && opcode == 0) { // start_drag(source, origin, icon, serial)
            if (!takeRole(objArgs[2], SurfaceRole::DragIcon))
                return fail("drag icon with another role");
        }
        if (g_stats && o->role == Role::Surface) {
            if (opcode == 1) // attach(buffer, x, y)
                o->bufferAttached = objArgs[0] != nullptr;
            else if (opcode == 6 && o->bufferAttached) { // commit: the feedback goes with it
                ownFeedbackFor(o);
                o->bufferAttached = false;
            }
        }
        if (!strcmp(o->info->name, "zwp_linux_buffer_params_v1") && opcode == 2) // create(width, height, ...)
            o->bufW = args[0].i, o->bufH = args[1].i;
        if (!strcmp(o->info->name, "zwp_linux_buffer_params_v1") && opcode == 3) {
            // create_immed(buffer, width, height, format, flags): a buffer the host cannot import would end the
            // app's connection (KWin raises invalid_wl_buffer on it). Sent as create and waited for here: created
            // gives the guest its buffer, failed an error on the guest's connection only (2026-10-31 review, H1).
            // Guest input is read after the display's read is done or cancelled (run()): a roundtrip is safe here.
            wl_argument cargs[4];
            for (int i = 0; i < 4; i++)
                cargs[i] = args[i + 1];
            o->immedFor = newId;
            o->immedDone = o->immedFailed = false;
            o->immedProxy = nullptr;
            wl_proxy_marshal_array_flags(o->proxy, 2, nullptr, o->version, 0, cargs);
            // c.dead first: a connection closed meanwhile has freed its objects, o included
            while (!c.dead && !o->immedDone && wl_display_roundtrip_queue(display, queue) >= 0) {
            }
            if (c.dead)
                return;
            wl_proxy *bp = o->immedProxy;
            const bool ok = o->immedDone && !o->immedFailed && bp;
            o->immedFor = 0;
            o->immedProxy = nullptr;
            if (!ok) {
                if (bp)
                    wl_proxy_destroy(bp);
                protocolError(c, id, 7, "zwp_linux_buffer_params_v1.create_immed: the host could not import the buffer");
                return;
            }
            Object *no = add(c, newId, newInfo, o->version, Kind::Forwarded);
            no->proxy = bp;
            no->bufW = args[1].i, no->bufH = args[2].i;
            watch(bp, no);
            break;
        }
        // one object of a kind per surface (the inhibitor moved to the app's window has its own check)
        const PerSurfaceRule *rule = movedInhibitor ? nullptr : perSurfaceRule(o->info->name, opcode);
        Object *ruleSurface = rule ? objArgs[1] : nullptr;
        if (ruleSurface && (ruleSurface->extensions & rule->bit))
            return refuse(rule->code, rule->what);
        // wl_surface.attach with an offset from version 5 on: invalid_offset, which the host raises on the app's
        // connection (2026-10-31: it ended vmview-demo)
        if (o->role == Role::Surface && opcode == 1 && o->version >= 5 && (args[1].i || args[2].i))
            return refuse(3, "a buffer offset in attach, from version 5 on");
        // clipping to the view: what sizes and places the guest's surfaces is kept here; the host gets our values
        // with each commit ("clipping the guest's surfaces to the view")
        if (g_clip) {
            const char *in = o->info->name;
            if (o->role == Role::Surface) {
                Geo &g = geoOf(o);
                switch (opcode) {
                case 1: { // attach(buffer, x, y)
                    Object *b = objArgs[0];
                    g.pend.buffer = b != nullptr;
                    g.pend.bw = b ? std::max(1, b->bufW) : 0;
                    g.pend.bh = b ? std::max(1, b->bufH) : 0;
                    g.pend.set |= kStBuffer;
                    // hosts that move a subsurface by an offset would move it out of our clip
                    if ((args[1].i || args[2].i) && o->surfaceRole != SurfaceRole::Cursor &&
                        o->surfaceRole != SurfaceRole::DragIcon)
                        args[1].i = args[2].i = 0;
                    break;
                }
                case 10: // offset(x, y): as attach's
                    if (o->surfaceRole != SurfaceRole::Cursor && o->surfaceRole != SurfaceRole::DragIcon)
                        return;
                    break;
                case 2: // damage(x, y, w, h), surface coordinates: a cut surface's moved, so all of it
                    if (g.hostWill.ox || g.hostWill.oy)
                        args[0].i = args[1].i = 0, args[2].i = args[3].i = INT32_MAX;
                    break;
                case 4: // set_opaque_region(region)
                case 5: // set_input_region(region)
                {
                    RegionRef r;
                    if (objArgs[0])
                        r = objArgs[0]->region ? std::make_shared<const Region>(*objArgs[0]->region)
                                               : std::make_shared<const Region>();
                    (opcode == 5 ? g.pend.input : g.pend.opaque) = r;
                    g.pend.set |= opcode == 5 ? kStInput : kStOpaque;
                    return; // ours go with the commit
                }
                case 6: // commit
                    guestCommit(c, o);
                    return;
                case 7: // set_buffer_transform(transform)
                    g.pend.transform = args[0].i;
                    g.pend.set |= kStTransform;
                    break;
                case 8: // set_buffer_scale(scale)
                    g.pend.scale = args[0].i;
                    g.pend.set |= kStScale;
                    break;
                default:
                    break;
                }
            } else if (!strcmp(in, "wl_region") && (opcode == 1 || opcode == 2)) { // add, subtract(x, y, w, h)
                if (!o->region)
                    o->region = std::make_shared<Region>();
                o->region->op(opcode == 1 ? 0 : 1, args[0].i, args[1].i, args[2].i, args[3].i);
            } else if (!strcmp(in, "wl_subsurface") && (opcode == 1 || opcode == 4 || opcode == 5)) {
                Object *sf = objectAt(c, o->extSurface, o->extSurfaceSeq);
                if (sf && sf->geo && sf->geo->subsurface == o) {
                    if (opcode == 1) // set_position(x, y): part of the parent's state, the host's sent with its commit
                        sf->geo->posPend = Pos{true, args[0].i, args[1].i};
                    else // set_sync, set_desync: the host's stays synchronized
                        sf->geo->guestSync = opcode == 4;
                }
                return;
            } else if (!strcmp(in, "wp_viewporter") && opcode == 1 && ruleSurface) {
                // get_viewport(id, surface): answered here, the surface's viewport on the host is ours
                Object *no = add(c, newId, newInfo, o->version, Kind::Viewport);
                no->extBit = kExtViewport;
                no->extSurface = ruleSurface->id;
                no->extSurfaceSeq = ruleSurface->seq;
                ruleSurface->extensions |= kExtViewport;
                Geo &g = geoOf(ruleSurface);
                g.viewportId = newId;
                g.viewportSeq = no->seq;
                return;
            } else if (!strcmp(in, "wp_pointer_warp_v1") && opcode == 1 && objArgs[0]) {
                // warp_pointer(surface, pointer, x, y, serial): on the host's surface, which starts at the cut
                int32_t ox = 0, oy = 0;
                if (cutOf(c, objArgs[0]->id, ox, oy)) {
                    args[2].f = moveFixed(args[2].f, -int64_t(ox));
                    args[3].f = moveFixed(args[3].f, -int64_t(oy));
                }
            }
        }
        const bool destructor = o->info->destructor_requests & (1ull << opcode);
        const uint32_t version = o->version;
        wl_proxy *np = wl_proxy_marshal_array_flags(o->proxy, opcode, newInfo ? newInfo->iface : nullptr,
                                                    version, destructor ? WL_MARSHAL_FLAG_DESTROY : 0, args);
        for (int f : fds)
            close(f); // libwayland duplicated them
        if (destructor) {
            byProxy.erase(o->proxy);
            o->proxy = nullptr;
            remove(c, o, true);
        }
        if (newArg >= 0) {
            Object *no = add(c, newId, newInfo, version, Kind::Forwarded);
            no->proxy = np;
            no->onAppWindow = movedInhibitor;
            Object *owner = ruleSurface ? ruleSurface : subsurfaceOf;
            if (owner) {
                no->extBit = ruleSurface ? rule->bit : kExtSubsurface;
                no->extSurface = owner->id;
                no->extSurfaceSeq = owner->seq;
                owner->extensions |= no->extBit;
                if (subsurfaceOf)
                    subsurfaceOf->parentSurface = objArgs[2]->id, subsurfaceOf->parentSeq = objArgs[2]->seq;
            }
            if (g_clip) {
                const char *in = o->info->name;
                if (subsurfaceOf) {
                    // the tree: at (0, 0) of its parent, synchronized (the host's stays so)
                    Geo &g = geoOf(subsurfaceOf);
                    g.parent = objArgs[2];
                    g.subsurface = no;
                    g.guestSync = true;
                    g.posPend = g.posCached = Pos{};
                    g.px = g.py = 0;
                    g.hostPosWill[0] = g.hostPosWill[1] = g.hostPosCur[0] = g.hostPosCur[1] = 0;
                    geoOf(objArgs[2]).children.push_back(subsurfaceOf);
                } else if (!strcmp(in, "wl_compositor") && opcode == 1) { // create_region
                    no->region = std::make_shared<Region>();
                } else if (!strcmp(in, "wl_shm_pool") && opcode == 0) { // create_buffer(id, offset, w, h, ...)
                    no->bufW = args[2].i, no->bufH = args[3].i;
                } else if (!strcmp(in, "wp_single_pixel_buffer_manager_v1") && opcode == 1) {
                    no->bufW = no->bufH = 1;
                }
            }
            if (movedInhibitor)
                setShortcutsCaptured(true);
            watch(np, no);
            for (int i = 0; i < n; i++)
                if (specs[i].type == 'o' && objArgs[i] && objArgs[i]->role == Role::Pointer)
                    no->link = objArgs[i]; // relative pointer, gestures: follow this pointer's focus
            if (no->role == Role::Keyboard)
                keyboardFocus(c);
        }
        break;
    }
    }
}

// ---- xdg-shell: the guest's toplevel as a subsurface of the app's window ----

void View::xdgRequest(Conn &c, Object *o, uint16_t opcode, const wl_argument *args, Object **objArgs)
{
    switch (o->kind) {
    case Kind::WmBase:
        if (opcode == 0) // destroy
            remove(c, o, true);
        else if (opcode == 2) { // get_xdg_surface(id, surface)
            Object *xs = objArgs[0];
            Object *surface = objArgs[1];
            if (!surface || !surface->proxy)
                return protocolError(c, o->id, 0, "get_xdg_surface without a surface");
            if (surface->surfaceRole != SurfaceRole::None && surface->surfaceRole != SurfaceRole::Toplevel)
                return protocolError(c, o->id, 0, "get_xdg_surface on a surface with another role"); // XDG_WM_BASE_ERROR_ROLE
            xs->link = surface;
        }
        // create_positioner, pong: nothing to do
        break;
    case Kind::Positioner:
        if (opcode == 0)
            remove(c, o, true);
        break;
    case Kind::XdgSurface:
        if (opcode == 0) { // destroy
            for (auto &kv : c.objects)
                if (kv.second->kind == Kind::Toplevel && kv.second->link == o)
                    return protocolError(c, o->id, 1, "xdg_surface destroyed before its toplevel");
            remove(c, o, true);
        } else if (opcode == 1) { // get_toplevel(id)
            Object *tl = objArgs[0];
            tl->link = o;
            if (c.toplevel) {
                logf("a second toplevel from the guest: only the first one is shown");
                break;
            }
            Object *surface = o->link;
            if (!surface || (surface->surfaceRole != SurfaceRole::None && surface->surfaceRole != SurfaceRole::Toplevel))
                return protocolError(c, o->id, 0, "get_toplevel on a surface with another role");
            surface->surfaceRole = SurfaceRole::Toplevel;
            c.toplevel = tl;
            tl->subsurface = wl_subcompositor_get_subsurface(subcompositor,
                                                             reinterpret_cast<wl_surface *>(surface->proxy),
                                                             anchor ? anchor : parent);
            wl_subsurface_set_desync(tl->subsurface);
            placeToplevel(c);
            configureToplevel(c);
            keyboardFocus(c);
            setMapped(true);
            // what the surface already shows (a buffer before the first configure, subsurfaces) clipped before the
            // app's next commit shows it
            if (viewNeedsFix(c))
                rootCommit(c, false);
            logf("guest window shown as a subsurface of the app's window");
        } else if (opcode == 2) { // get_popup: not shown; dismissed at once
            OutMsg m(objArgs[0]->id, kPopupDone);
            send(c, m);
        } else if (opcode == 3) { // set_window_geometry(x, y, w, h)
            o->geom[0] = args[0].i;
            o->geom[1] = args[1].i;
            o->geom[2] = args[2].i;
            o->geom[3] = args[3].i;
            o->hasGeom = true;
            // with crops, the geometry's offset is cut instead (at the root's next commit, as xdg-shell applies it)
            if (c.toplevel && c.toplevel->link == o && !(anchor && ownViewporter))
                placeToplevel(c);
        }
        // ack_configure: nothing to do
        break;
    case Kind::Toplevel:
        switch (opcode) {
        case 0: // destroy
            remove(c, o, true);
            break;
        case 2: // set_title
            if (o == c.toplevel && cb.title && args[0].s)
                cb.title(cb.data, args[0].s);
            break;
        case 11: // set_fullscreen
        case 12: // unset_fullscreen
            if (o == c.toplevel && cb.fullscreen)
                cb.fullscreen(cb.data, opcode == 11);
            break;
        default: // parent, app id, menus, move, resize, size limits, maximize, minimize: the app's business
            break;
        }
        break;
    case Kind::Popup:
        if (opcode == 0)
            remove(c, o, true);
        break;
    default:
        break;
    }
}

void View::placeToplevel(Conn &c)
{
    Object *tl = c.toplevel;
    if (!tl || !tl->subsurface)
        return;
    Object *xs = tl->link;
    const int gx = xs && xs->hasGeom ? xs->geom[0] : 0, gy = xs && xs->hasGeom ? xs->geom[1] : 0;
    if (anchor) {
        // under the anchor, at the view's top-left: with crops the root starts there too (what its window geometry
        // leaves out on the left and top is cut), without them it is moved by that offset. Applied by our commit
        if (ownViewporter)
            wl_subsurface_set_position(tl->subsurface, 0, 0);
        else
            wl_subsurface_set_position(tl->subsurface, -gx, -gy);
        wl_surface_commit(anchor);
        return;
    }
    wl_subsurface_set_position(tl->subsurface, state.x - gx, state.y - gy);
}

void View::configureToplevel(Conn &c)
{
    Object *tl = c.toplevel;
    if (!tl)
        return;
    std::vector<uint32_t> states;
    if (state.fullscreen)
        states.push_back(kStateFullscreen);
    bool active = false;
    for (auto &kv : c.objects)
        if (kv.second->role == Role::Keyboard && kv.second->focus)
            active = true;
    if (active)
        states.push_back(kStateActivated);
    OutMsg m(tl->id, kToplevelConfigure); // (width, height, states)
    m.u(uint32_t(state.w));
    m.u(uint32_t(state.h));
    wl_array arr{states.size() * 4, states.size() * 4, states.data()};
    m.a(&arr);
    send(c, m);
    OutMsg s(tl->link->id, kSurfaceConfigure); // (serial)
    s.u(++tl->configureSerial);
    send(c, s);
}

void View::setMapped(bool on)
{
    if (on == mapped)
        return;
    mapped = on;
    if (cb.mapped)
        cb.mapped(cb.data, on);
}

// The guest's shortcuts inhibitor names the app's PARENT surface (the request is moved there),
// which has the keyboard focus only if it is the app's window's main surface: the app hears of it
void View::setShortcutsCaptured(bool on)
{
    if (on == shortcutsCaptured)
        return;
    shortcutsCaptured = on;
    logf("the guest %s the host's keyboard shortcuts", on ? "captures" : "releases");
    if (cb.shortcuts)
        cb.shortcuts(cb.data, on);
}

// ---- clipping the guest's surfaces to the view (see the types above) ----

Geo &View::geoOf(Object *s)
{
    if (!s->geo)
        s->geo = std::make_unique<Geo>();
    return *s->geo;
}

// the guest's toplevel surface, a subsurface of the app's window: the root of what is clipped
Object *View::rootOf(Conn &c)
{
    Object *tl = c.toplevel;
    return !g_clip || !tl || !tl->subsurface || !tl->link || !tl->link->link ? nullptr : tl->link->link;
}

// the view in the root's coordinates: the origin of its window geometry (never left of or above the surface's
// own), the size the app gave
void View::viewRect(Conn &c, int32_t vr[4])
{
    Object *xs = c.toplevel ? c.toplevel->link : nullptr;
    vr[0] = xs && xs->hasGeom ? xs->geom[0] : 0;
    vr[1] = xs && xs->hasGeom ? xs->geom[1] : 0;
    if (ownViewporter) // the root's host surface starts at the view (placeToplevel)
        vr[0] = std::max(0, vr[0]), vr[1] = std::max(0, vr[1]);
    vr[2] = state.w;
    vr[3] = state.h;
}

// What the host is to have of a surface whose state is ST, at (AX, AY) in the root's coordinates, the view at VR
// there: the guest's own values when it is all in the view or not in the root's tree (IN_VIEW false), else a crop.
// False when the host would refuse the guest's state (CODE, WHY: wp_viewport's error) - unless HALF: a state that
// may be caught half made (a commit of ours): nothing of it is shown then.
bool View::wantFor(const SurfState &st, bool inView, int64_t ax, int64_t ay, const int32_t vr[4], bool half,
                   HostVals &w, uint32_t *code, const char **why)
{
    w = HostVals{};
    memcpy(w.src, st.src, sizeof w.src);
    w.dst[0] = st.dst[0];
    w.dst[1] = st.dst[1];
    w.input.r = st.input;
    w.opaque.r = st.opaque;
    const bool crop = ownViewporter != nullptr; // else input only
    // where a surface goes that shows nothing: in the view, so that its children's positions on the host (relative
    // to it) stay small
    auto placeInside = [&] {
        if (!crop)
            return;
        w.ox = std::clamp<int64_t>(ax, vr[0], int64_t(vr[0]) + std::max(1, vr[2]) - 1) - ax;
        w.oy = std::clamp<int64_t>(ay, vr[1], int64_t(vr[1]) + std::max(1, vr[3]) - 1) - ay;
        w.input.dx = w.opaque.dx = w.ox;
        w.input.dy = w.opaque.dy = w.oy;
    };
    // nothing of it in the view: all of its content in one pixel there, no input
    auto hidden = [&] {
        w.input = HostRegion{1, nullptr};
        w.opaque = HostRegion{};
        if (!crop)
            return;
        w.src[0] = w.src[1] = w.src[2] = w.src[3] = -256;
        w.dst[0] = w.dst[1] = 1;
        placeInside();
        w.input.dx = w.input.dy = w.opaque.dx = w.opaque.dy = 0;
    };
    if (!st.buffer) {
        if (inView)
            placeInside(); // unmapped: shows nothing
        return true;
    }
    int64_t bw = std::max(1, st.bw), bh = std::max(1, st.bh);
    if (st.transform & 1) // 90 or 270 degrees, flipped or not
        std::swap(bw, bh);
    const int64_t scale = std::max(1, st.scale);
    const bool hasSrc = !(st.src[0] == -256 && st.src[1] == -256 && st.src[2] == -256 && st.src[3] == -256);
    const bool hasDst = !(st.dst[0] == -1 && st.dst[1] == -1);
    // the content in wl_fixed_t units of surface coordinates (after the transform and the scale), and the source
    int64_t sx = 0, sy = 0, sw = bw * 256 / scale, sh = bh * 256 / scale;
    bool invalid = false;
    if (hasSrc) {
        sx = st.src[0], sy = st.src[1], sw = st.src[2], sh = st.src[3];
        if ((sx + sw) * scale > bw * 256 || (sy + sh) * scale > bh * 256) {
            *code = 2; // WP_VIEWPORT_ERROR_OUT_OF_BUFFER
            *why = "the source rectangle extends outside the buffer";
            invalid = true;
        }
    }
    int64_t dw, dh; // its size, surface coordinates
    if (hasDst) {
        dw = st.dst[0], dh = st.dst[1];
    } else if (hasSrc) {
        if (sw % 256 || sh % 256) {
            *code = 1; // WP_VIEWPORT_ERROR_BAD_SIZE
            *why = "a source size that is not integer, without a destination size";
            invalid = true;
        }
        dw = sw / 256, dh = sh / 256;
    } else {
        dw = (bw + scale - 1) / scale, dh = (bh + scale - 1) / scale;
    }
    if (invalid) {
        if (!half)
            return false;
        if (inView) {
            hidden();
        } else {
            w.src[0] = w.src[1] = w.src[2] = w.src[3] = -256; // no viewport: whatever the buffer is, valid
            w.dst[0] = w.dst[1] = -1;
        }
        return true;
    }
    if (!inView)
        return true;
    // the part in the view, surface coordinates
    const int64_t x0 = std::max<int64_t>(0, vr[0] - ax), y0 = std::max<int64_t>(0, vr[1] - ay);
    const int64_t x1 = std::min<int64_t>(dw, int64_t(vr[0]) + vr[2] - ax);
    const int64_t y1 = std::min<int64_t>(dh, int64_t(vr[1]) + vr[3] - ay);
    if (x0 == 0 && y0 == 0 && x1 == dw && y1 == dh)
        return true; // all in the view: the guest's own values
    if (x1 <= x0 || y1 <= y0 || dw <= 0 || dh <= 0 || sw < 1 || sh < 1) {
        hidden();
        return true;
    }
    if (!crop) {
        // no viewports on the host: no crop, the input region alone kept to the view
        w.input = HostRegion{2, st.input};
        w.input.clip[0] = clamp32(x0), w.input.clip[1] = clamp32(y0);
        w.input.clip[2] = clamp32(x1 - x0), w.input.clip[3] = clamp32(y1 - y0);
        return true;
    }
    // the guest's source rectangle cut as its surface is (sw / dw of content per unit of surface)
    int64_t cx0 = sx + sw * x0 / dw, cx1 = sx + sw * x1 / dw;
    int64_t cy0 = sy + sh * y0 / dh, cy1 = sy + sh * y1 / dh;
    cx0 = std::clamp(cx0, sx, sx + sw - 1);
    cx1 = std::clamp(cx1, cx0 + 1, sx + sw);
    cy0 = std::clamp(cy0, sy, sy + sh - 1);
    cy1 = std::clamp(cy1, cy0 + 1, sy + sh);
    w.src[0] = clamp32(cx0), w.src[1] = clamp32(cy0), w.src[2] = clamp32(cx1 - cx0), w.src[3] = clamp32(cy1 - cy0);
    w.dst[0] = clamp32(x1 - x0), w.dst[1] = clamp32(y1 - y0);
    w.ox = x0, w.oy = y0;
    w.input.dx = w.opaque.dx = x0;
    w.input.dy = w.opaque.dy = y0;
    if (st.opaque && st.opaque->overflow)
        w.opaque.r = nullptr; // its bounding box would claim more than it covers
    return true;
}

// a region of ours as the input (INPUT) or opaque region of the guest's surface S
void View::sendRegion(Object *s, bool input, const HostRegion &r)
{
    wl_proxy *reg = nullptr;
    if ((r.mode != 0 || r.r) && ownCompositor) {
        reg = wl_proxy_marshal_flags(ownCompositor, 1, &wl_region_interface, 1, 0, nullptr); // create_region
        auto rect = [&](uint32_t op, int64_t x, int64_t y, int64_t w, int64_t h) { // add 1, subtract 2
            if (w > 0 && h > 0)
                wl_proxy_marshal_flags(reg, op, nullptr, 1, 0, clamp32(x), clamp32(y), clamp32(w), clamp32(h));
        };
        if (r.mode == 0 || r.mode == 2) {
            if (!r.r) {
                rect(1, r.clip[0], r.clip[1], r.clip[2], r.clip[3]); // mode 2, an infinite region: the clip
            } else if (r.r->overflow) {
                rect(1, r.r->box[0] - r.dx, r.r->box[1] - r.dy, r.r->box[2] - r.r->box[0], r.r->box[3] - r.r->box[1]);
            } else {
                for (const auto &op : r.r->ops)
                    rect(op[0] == 0 ? 1 : 2, op[1] - r.dx, op[2] - r.dy, op[3], op[4]);
            }
            if (r.mode == 2 && r.r) {
                // all but the clip taken away
                const int64_t big = int64_t(1) << 29, x = r.clip[0], y = r.clip[1], w = r.clip[2], h = r.clip[3];
                rect(2, -big, -big, x + big, 2 * big);
                rect(2, x + w, -big, big, 2 * big);
                rect(2, x, -big, w, y + big);
                rect(2, x, y + h, w, big);
            }
        }
    }
    if (input)
        wl_surface_set_input_region(reinterpret_cast<wl_surface *>(s->proxy), reinterpret_cast<wl_region *>(reg));
    else
        wl_surface_set_opaque_region(reinterpret_cast<wl_surface *>(s->proxy), reinterpret_cast<wl_region *>(reg));
    if (reg)
        wl_proxy_marshal_flags(reg, 0, nullptr, 1, WL_MARSHAL_FLAG_DESTROY); // destroy
}

// the values W for the guest's surface S, where they differ from what the host will have; before a commit of it
void View::sendVals(Object *s, const HostVals &w)
{
    Geo &g = geoOf(s);
    HostVals &h = g.hostWill;
    if (!s->proxy)
        return;
    if (!w.sameViewport(h)) {
        if (!g.viewport && ownViewporter) // get_viewport(id, surface)
            g.viewport = wl_proxy_marshal_flags(ownViewporter, 1, ifaceByName("wp_viewport")->iface, 1, 0, nullptr,
                                                s->proxy);
        if (g.viewport) {
            if (memcmp(w.src, h.src, sizeof w.src)) // set_source
                wl_proxy_marshal_flags(g.viewport, 1, nullptr, 1, 0, w.src[0], w.src[1], w.src[2], w.src[3]);
            if (w.dst[0] != h.dst[0] || w.dst[1] != h.dst[1]) // set_destination
                wl_proxy_marshal_flags(g.viewport, 2, nullptr, 1, 0, w.dst[0], w.dst[1]);
        }
    }
    if (!(w.input == h.input))
        sendRegion(s, true, w.input);
    if (!(w.opaque == h.opaque))
        sendRegion(s, false, w.opaque);
    // the cut moved, or a cut surface's crop changed: its content moved on the host's surface
    if (w.ox != h.ox || w.oy != h.oy || ((w.ox || w.oy) && !w.sameViewport(h)))
        wl_surface_damage(reinterpret_cast<wl_surface *>(s->proxy), 0, 0, INT32_MAX, INT32_MAX);
    h = w;
}

// the host's position of the guest's subsurface CHILD, where it differs from what the host will have; before a
// commit of its parent
void View::sendPos(Object *child, int64_t x, int64_t y)
{
    Geo &g = geoOf(child);
    const int32_t px = clamp32(x), py = clamp32(y);
    if (g.hostPosWill[0] == px && g.hostPosWill[1] == py)
        return;
    if (g.subsurface && g.subsurface->proxy)
        wl_subsurface_set_position(reinterpret_cast<wl_subsurface *>(g.subsurface->proxy), px, py);
    g.hostPosWill[0] = px;
    g.hostPosWill[1] = py;
}

// S's position in the root's coordinates from the positions the guest set last, applied or not (as the root's next
// commit will most likely apply them); false when S is not in the root's tree
bool View::expectedAbs(Conn &c, Object *s, int64_t &ax, int64_t &ay)
{
    Object *root = rootOf(c);
    ax = ay = 0;
    for (int depth = 0; s && depth <= kMaxDepth + 1; depth++) {
        if (s == root)
            return true;
        Geo &g = geoOf(s);
        if (!g.parent)
            return false;
        const Pos p = g.posPend.set ? g.posPend : g.posCached.set ? g.posCached : Pos{true, g.px, g.py};
        ax += p.x;
        ay += p.y;
        s = g.parent;
    }
    return false;
}

// whether the guest's commit of S applies at once as the guest has it: desynchronized up to a surface without a
// parent, TOP
bool View::guestDesync(Object *s, Object **top)
{
    for (int depth = 0; s && depth <= kMaxDepth + 1; depth++) {
        Geo &g = geoOf(s);
        if (!g.parent) {
            *top = s;
            return true;
        }
        if (g.guestSync)
            return false;
        s = g.parent;
    }
    return false;
}

// S was committed while synchronized (on the host, every guest subsurface): its state and its children's positions
// are cached
void View::cached(Object *s)
{
    Geo &g = geoOf(s);
    g.cached.merge(g.pend);
    g.pend = SurfState{};
    g.hasCache = true;
    for (Object *ch : g.children) {
        Geo &cg = geoOf(ch);
        if (cg.posPend.set) {
            cg.posCached = cg.posPend;
            cg.posPend = Pos{};
        }
    }
}

// The host applied S's state (WITH_PENDING: a commit of S, desynchronized), and in turn its subtree's caches
void View::applied(Object *s, bool withPending)
{
    std::vector<Object *> todo{s};
    for (size_t i = 0; i < todo.size() && i <= kMaxSurfaces; i++) {
        Geo &g = geoOf(todo[i]);
        const bool pend = i == 0 && withPending;
        g.cur.merge(g.cached);
        g.cached = SurfState{};
        if (pend) {
            g.cur.merge(g.pend);
            g.pend = SurfState{};
        }
        g.hasCache = false;
        g.hostCur = g.hostWill;
        for (Object *ch : g.children) {
            Geo &cg = geoOf(ch);
            if (cg.posCached.set)
                cg.px = cg.posCached.x, cg.py = cg.posCached.y;
            if (pend && cg.posPend.set)
                cg.px = cg.posPend.x, cg.py = cg.posPend.y;
            cg.posCached = Pos{};
            if (pend)
                cg.posPend = Pos{};
            cg.hostPosCur[0] = cg.hostPosWill[0];
            cg.hostPosCur[1] = cg.hostPosWill[1];
            todo.push_back(ch);
        }
    }
}

// the guest's wl_surface.commit
void View::guestCommit(Conn &c, Object *s)
{
    if (s == rootOf(c)) {
        rootCommit(c, true);
        return;
    }
    Geo &g = geoOf(s);
    SurfState st = g.cur;
    st.merge(g.cached);
    st.merge(g.pend);
    int64_t ax = 0, ay = 0;
    const bool inView = expectedAbs(c, s, ax, ay);
    int32_t vr[4];
    viewRect(c, vr);
    HostVals w;
    uint32_t code = 0;
    const char *why = "";
    if (!wantFor(st, inView, ax, ay, vr, false, w, &code, &why)) {
        protocolError(c, objectAt(c, g.viewportId, g.viewportSeq) ? g.viewportId : s->id, code, "wp_viewport: %s", why);
        return;
    }
    sendVals(s, w);
    // its children's positions go with this commit: as the guest set them, moved by their cuts and its own (the
    // root's next commit checks them again)
    for (Object *ch : g.children) {
        Geo &cg = geoOf(ch);
        const Pos p = cg.posPend.set ? cg.posPend : cg.posCached.set ? cg.posCached : Pos{true, cg.px, cg.py};
        sendPos(ch, p.x + cg.hostWill.ox - w.ox, p.y + cg.hostWill.oy - w.oy);
    }
    wl_surface_commit(reinterpret_cast<wl_surface *>(s->proxy));
    if (!g.parent) {
        // no parent (no role, a cursor, a drag icon, a toplevel not shown): applied at once, not in the view
        applied(s, true);
        return;
    }
    cached(s);
    // desynchronized as the guest has it: applied now, with a commit of ours of what the host applies it with
    Object *top = nullptr;
    if (guestDesync(s, &top)) {
        if (top == rootOf(c))
            rootCommit(c, false);
        else
            topCommit(c, top);
    }
}

// a commit of ours of a surface without a parent that is not the root (not in the view): what the guest committed
// desynchronized below it applies
void View::topCommit(Conn &c, Object *top)
{
    Geo &g = geoOf(top);
    SurfState st = g.cur;
    st.merge(g.cached);
    st.merge(g.pend);
    int32_t vr[4];
    viewRect(c, vr);
    HostVals w;
    uint32_t code = 0;
    const char *why = "";
    wantFor(st, false, 0, 0, vr, true, w, &code, &why);
    sendVals(top, w);
    for (Object *ch : g.children) {
        Geo &cg = geoOf(ch);
        const Pos p = cg.posPend.set ? cg.posPend : cg.posCached.set ? cg.posCached : Pos{true, cg.px, cg.py};
        sendPos(ch, p.x + cg.hostWill.ox - w.ox, p.y + cg.hostWill.oy - w.oy);
    }
    wl_surface_commit(reinterpret_cast<wl_surface *>(top->proxy));
    applied(top, true);
}

// The root's state applies now: the guest's commit of it (GUEST), or ours. Every guest subsurface is synchronized on
// the host, so all the host shows of the tree changes here, at once. The crops, cuts and positions it is to apply
// are sent first; a surface whose cached values (or children's positions) would not do gets a commit of ours
// before, cached, applied with the root's. So does each parent of a surface with a cache: hosts differ on whether a
// cache applies under a parent without one.
void View::rootCommit(Conn &c, bool guest)
{
    Object *root = rootOf(c);
    if (!root)
        return;
    c.clipDue = 0;
    int32_t vr[4];
    viewRect(c, vr);
    struct Node {
        Object *s;
        int parent;
        int64_t ax, ay;   // in the root's coordinates, once this commit applies
        int32_t px, py;   // its position in its parent then
        size_t first, n;  // its children (contiguous: breadth first)
        HostVals w;
    };
    std::vector<Node> nodes;
    std::unordered_set<Object *> ours; // committed by us before the root
    for (int round = 0;; round++) {
        nodes.clear();
        nodes.push_back({root, -1, 0, 0, 0, 0, 0, 0, {}});
        for (size_t i = 0; i < nodes.size() && nodes.size() <= kMaxSurfaces + 1; i++) {
            Object *x = nodes[i].s;
            Geo &g = geoOf(x);
            const bool pend = i == 0 || ours.count(x);
            SurfState st = g.cur;
            st.merge(g.cached);
            if (pend)
                st.merge(g.pend);
            uint32_t code = 0;
            const char *why = "";
            HostVals w;
            if (!wantFor(st, true, nodes[i].ax, nodes[i].ay, vr, !(i == 0 && guest), w, &code, &why)) {
                // the guest's own commit of its root, refused as the host would refuse it
                protocolError(c, objectAt(c, g.viewportId, g.viewportSeq) ? g.viewportId : x->id, code,
                              "wp_viewport: %s", why);
                return;
            }
            nodes[i].w = w;
            nodes[i].first = nodes.size();
            nodes[i].n = g.children.size();
            const int64_t ax = nodes[i].ax, ay = nodes[i].ay;
            for (Object *ch : g.children) {
                Geo &cg = geoOf(ch);
                Pos p{true, cg.px, cg.py};
                if (cg.posCached.set)
                    p = cg.posCached;
                if (pend && cg.posPend.set)
                    p = cg.posPend;
                nodes.push_back({ch, int(i), ax + p.x, ay + p.y, p.x, p.y, 0, 0, {}});
            }
        }
        // which others need a commit of ours: values or children's positions the host would not apply as wanted
        std::vector<char> need(nodes.size(), 0);
        for (size_t i = 1; i < nodes.size(); i++) {
            Geo &g = geoOf(nodes[i].s);
            if (!(nodes[i].w == g.hostWill))
                need[i] = 1;
            const Node &p = nodes[size_t(nodes[i].parent)];
            const int32_t hx = clamp32(nodes[i].px + nodes[i].w.ox - p.w.ox), hy = clamp32(nodes[i].py + nodes[i].w.oy - p.w.oy);
            if (nodes[i].parent > 0 && (hx != g.hostPosWill[0] || hy != g.hostPosWill[1]))
                need[size_t(nodes[i].parent)] = 1;
        }
        // and the parents of a surface with a cache or a commit of ours, up to the root
        for (size_t i = nodes.size(); i-- > 1;) {
            Object *x = nodes[i].s;
            if (need[i] || ours.count(x) || geoOf(x).hasCache)
                for (int a = nodes[i].parent; a > 0; a = nodes[size_t(a)].parent)
                    if (!geoOf(nodes[size_t(a)].s).hasCache)
                        need[size_t(a)] = 1;
        }
        bool grew = false;
        for (size_t i = 1; i < nodes.size(); i++)
            if (need[i] && ours.insert(nodes[i].s).second)
                grew = true;
        if (!grew)
            break;
        if (round >= 8) // a tree that keeps changing what it needs: all of it committed
            for (size_t i = 1; i < nodes.size(); i++)
                ours.insert(nodes[i].s);
    }
    // our commits, cached; then the root's values and its children's positions, and its commit
    auto sendChildren = [&](const Node &n) {
        for (size_t k = 0; k < n.n; k++) {
            const Node &ch = nodes[n.first + k];
            sendPos(ch.s, ch.px + ch.w.ox - n.w.ox, ch.py + ch.w.oy - n.w.oy);
        }
    };
    for (size_t i = 1; i < nodes.size(); i++) {
        if (!ours.count(nodes[i].s))
            continue;
        sendVals(nodes[i].s, nodes[i].w);
        sendChildren(nodes[i]);
        wl_surface_commit(reinterpret_cast<wl_surface *>(nodes[i].s->proxy));
        cached(nodes[i].s);
    }
    sendVals(root, nodes[0].w);
    sendChildren(nodes[0]);
    wl_surface_commit(reinterpret_cast<wl_surface *>(root->proxy));
    if (g_debug && !ours.empty())
        logf("   the root's commit with %zu commits of ours before it (the view's clip)", ours.size());
    applied(root, true);
}

// whether what the host shows of the tree now is not what it should be for the current view (after a change of it)
bool View::viewNeedsFix(Conn &c)
{
    Object *root = rootOf(c);
    if (!root)
        return false;
    int32_t vr[4];
    viewRect(c, vr);
    struct Node {
        Object *s;
        int64_t ax, ay;
    };
    std::vector<Node> todo{{root, 0, 0}};
    for (size_t i = 0; i < todo.size() && i <= kMaxSurfaces; i++) {
        Geo &g = geoOf(todo[i].s);
        HostVals w;
        uint32_t code = 0;
        const char *why = "";
        wantFor(g.cur, true, todo[i].ax, todo[i].ay, vr, true, w, &code, &why);
        if (!(w == g.hostCur))
            return true;
        for (Object *ch : g.children) {
            Geo &cg = geoOf(ch);
            todo.push_back({ch, todo[i].ax + cg.px, todo[i].ay + cg.py});
            if (clamp32(cg.px + cg.hostCur.ox - w.ox) != cg.hostPosCur[0] ||
                clamp32(cg.py + cg.hostCur.oy - w.oy) != cg.hostPosCur[1])
                return true;
        }
    }
    return false;
}

// S leaves its parent (its wl_subsurface or the surface itself destroyed): the host unmaps it at once
void View::detach(Object *s)
{
    Geo &g = geoOf(s);
    if (g.parent && g.parent->geo) {
        auto &v = g.parent->geo->children;
        v.erase(std::remove(v.begin(), v.end(), s), v.end());
    }
    g.parent = nullptr;
    g.subsurface = nullptr;
    g.guestSync = true;
    g.posPend = g.posCached = Pos{};
    g.px = g.py = 0;
    g.hostPosWill[0] = g.hostPosWill[1] = g.hostPosCur[0] = g.hostPosCur[1] = 0;
    // its cache: applied as the surface is unmapped (wlroots), or kept with the same values; either way, as here
    g.cur.merge(g.cached);
    g.cached = SurfState{};
    g.hasCache = false;
    g.hostCur = g.hostWill;
}

// the cut of the guest's surface SURFACE_ID as the host has it: its events' coordinates are moved by it
bool View::cutOf(Conn &c, uint32_t surfaceId, int32_t &ox, int32_t &oy)
{
    auto it = c.objects.find(surfaceId);
    if (it == c.objects.end() || it->second->role != Role::Surface || !it->second->geo)
        return false;
    const HostVals &h = it->second->geo->hostCur;
    ox = clamp32(h.ox);
    oy = clamp32(h.oy);
    return ox || oy;
}

// The keyboard: the guest gets it while the host gives it to the app's window and the app lets it.
void View::keyboardFocus(Conn &c)
{
    Object *tl = c.toplevel;
    const uint32_t surfaceId = tl && tl->link && tl->link->link ? tl->link->link->id : 0;
    bool changed = false;
    for (auto &kv : c.objects) {
        Object *k = kv.second.get();
        if (k->role != Role::Keyboard)
            continue;
        const bool want = k->hostFocus && state.keyboard && surfaceId;
        if (want == k->focus)
            continue;
        if (want) {
            OutMsg m(k->id, kKeyboardEnter);
            m.u(k->lastSerial);
            m.u(surfaceId);
            m.a(nullptr); // no keys held (the host's array was for the app's window)
            send(c, m);
        } else if (surfaceId) {
            OutMsg m(k->id, kKeyboardLeave);
            m.u(k->lastSerial);
            m.u(surfaceId);
            send(c, m);
        }
        k->focus = want;
        changed = true;
    }
    if (changed && guestHasKeyboard(c)) {
        // the host's selection while the guest did not have the keyboard: the latest, now
        for (auto &kv : c.objects) {
            Object *d = kv.second.get();
            if ((d->role != Role::DataDevice && d->role != Role::PrimaryDevice) || !d->selectionKnown)
                continue;
            uint32_t offer = 0;
            if (d->selection) {
                auto bp = byProxy.find(d->selection);
                if (bp == byProxy.end() || bp->second->conn != &c)
                    continue;
                offer = bp->second->id;
            }
            OutMsg m(d->id, d->role == Role::DataDevice ? kDataDeviceSelection : kPrimarySelection);
            m.u(offer);
            send(c, m);
        }
    }
    if (changed)
        configureToplevel(c);
}

bool View::guestHasKeyboard(Conn &c)
{
    for (auto &kv : c.objects)
        if (kv.second->role == Role::Keyboard && kv.second->focus)
            return true;
    return false;
}

bool View::guestHasInput(Conn &c)
{
    for (auto &kv : c.objects)
        if ((kv.second->role == Role::Keyboard || kv.second->role == Role::DataDevice) && kv.second->focus)
            return true;
    return false;
}

// ---- host events ----

int View::event(wl_proxy *p, uint32_t opcode, const wl_message *m, wl_argument *args)
{
    ArgSpec specs[kMaxArgs];
    const int n = parseSignature(m->signature, specs);
    auto closeFds = [&] {
        for (int i = 0; i < n; i++)
            if (specs[i].type == 'h')
                close(args[i].h);
    };
    if (p == reinterpret_cast<wl_proxy *>(registry)) {
        if (opcode == kRegistryGlobal)
            hostGlobal(args[0].u, args[1].s, args[2].u);
        else
            hostGlobalRemove(args[0].u);
        return 0;
    }
    auto it = byProxy.find(p);
    if (it == byProxy.end()) {
        closeFds();
        return 0;
    }
    Object *o = it->second;
    Conn &c = *o->conn;
    if (c.dead) {
        closeFds();
        return 0;
    }
    if (opcode == 1 && args[0].u < 32 && !strcmp(o->info->name, "wp_color_manager_v1"))
        o->colorFeatures |= 1u << args[0].u; // supported_feature: passed on as well
    if (o->immedFor) {
        // the answer to the create that stands for a guest's create_immed (handle()): created(buffer) or failed,
        // neither passed on (the guest asked for an immediate buffer)
        if (opcode == 0)
            o->immedProxy = reinterpret_cast<wl_proxy *>(args[0].o);
        else
            o->immedFailed = true;
        o->immedDone = true;
        return 0;
    }
    // an object the guest does not know: one of the app's, one of another guest connection's (vitrine,
    // 2026-10-31: a test client's pointer got the enter of the nested KWin's surface, by that
    // connection's id), or one destroyed on the app's side, which libwayland passes as NULL where the
    // protocol allows none (the keyboard's leave of a window the app just closed): the guest takes a
    // NULL there for a protocol error
    auto foreign = [&](int i) {
        if (specs[i].type != 'o')
            return false;
        if (!args[i].o)
            return !specs[i].nullable;
        auto f = byProxy.find(reinterpret_cast<wl_proxy *>(args[i].o));
        return f == byProxy.end() || f->second->conn != &c;
    };
    int foreignArg = -1;
    for (int i = 0; i < n; i++)
        if (foreign(i))
            foreignArg = i;

    // the guest's leave of its surface, sent after this event (the end of a drag over the app's)
    uint32_t leaveAfter = 0, leaveSerial = 0;
    // focus: events about the app's own surfaces stay with the app
    switch (o->role) {
    case Role::Pointer:
        if (opcode == kPointerEnter) {
            // a guest surface the host shows from its cut on (clipping): its coordinates from the guest's origin
            int32_t ox = 0, oy = 0;
            if (foreignArg < 0 && cutOf(c, byProxy[reinterpret_cast<wl_proxy *>(args[1].o)]->id, ox, oy)) {
                args[2].f = moveFixed(args[2].f, ox);
                args[3].f = moveFixed(args[3].f, oy);
            }
            const double x = wl_fixed_to_double(args[2].f), y = wl_fixed_to_double(args[3].f);
            if (foreignArg >= 0) {
                // one of the app's surfaces: the guest's drag goes on, its position from here
                if (o->focus && !o->buttonsHeld.empty()) {
                    o->leaveUnmatched = false; // the leave's frame brought an enter: a move within the app
                    o->grabOutside = true;
                    o->grabOffset[0] = o->lastX - x;
                    o->grabOffset[1] = o->lastY - y;
                } else {
                    o->focus = false;
                }
                closeFds();
                return 0;
            }
            const uint32_t surface = byProxy[reinterpret_cast<wl_proxy *>(args[1].o)]->id;
            o->leaveUnmatched = false;
            if (o->grabOutside) {
                o->grabOutside = false;
                if (surface == o->pointerSurface) {
                    // back on the surface the guest never left: a move to here
                    OutMsg move(o->id, kPointerMotion);
                    move.u(o->lastTime);
                    move.u(uint32_t(args[2].f));
                    move.u(uint32_t(args[3].f));
                    send(c, move);
                    o->lastX = x;
                    o->lastY = y;
                    o->pendingFrame = true;
                    return 0;
                }
                // another of the guest's surfaces: it leaves the one it had
                if (c.objects.count(o->pointerSurface)) {
                    OutMsg leave(o->id, kPointerLeave);
                    leave.u(args[0].u);
                    leave.u(o->pointerSurface);
                    send(c, leave);
                }
                o->buttonsHeld.clear();
            }
            o->focus = true;
            o->pointerSurface = surface;
            o->lastX = x;
            o->lastY = y;
        } else if (opcode == kPointerLeave) {
            if (o->focus && !o->buttonsHeld.empty()) {
                // a button held: the guest keeps the pointer if the app's enter follows in this frame (it
                // says where); a frame without one ends the drag (kPointerFrame)
                o->grabOutside = true;
                o->leaveUnmatched = true;
                o->leaveSerial = args[0].u;
                closeFds();
                return 0;
            }
            if (foreignArg >= 0 || !o->focus) {
                closeFds();
                return 0;
            }
            o->focus = false;
            o->pendingFrame = true;
            break;
        } else if (opcode == kPointerFrame) {
            if (o->leaveUnmatched) {
                // a leave with no enter in its frame: the pointer left the app, the release will not come
                o->leaveUnmatched = false;
                o->grabOutside = false;
                o->buttonsHeld.clear();
                o->focus = false;
                if (c.objects.count(o->pointerSurface)) {
                    OutMsg leave(o->id, kPointerLeave);
                    leave.u(o->leaveSerial);
                    leave.u(o->pointerSurface);
                    send(c, leave);
                    o->pendingFrame = true;
                }
            }
            if (!o->pendingFrame)
                return 0;
            o->pendingFrame = false;
            break;
        } else if (!o->focus) {
            return 0;
        }
        if (opcode == kPointerMotion) {
            o->lastTime = args[0].u;
            int32_t ox = 0, oy = 0;
            if (o->grabOutside) {
                args[1].f = wl_fixed_from_double(wl_fixed_to_double(args[1].f) + o->grabOffset[0]);
                args[2].f = wl_fixed_from_double(wl_fixed_to_double(args[2].f) + o->grabOffset[1]);
            } else if (cutOf(c, o->pointerSurface, ox, oy)) {
                args[1].f = moveFixed(args[1].f, ox);
                args[2].f = moveFixed(args[2].f, oy);
            }
            o->lastX = wl_fixed_to_double(args[1].f);
            o->lastY = wl_fixed_to_double(args[2].f);
        }
        if (foreignArg < 0)
            o->pendingFrame = true;
        if (opcode == kPointerButton) {
            if (args[3].u == WL_POINTER_BUTTON_STATE_PRESSED) {
                o->buttonsHeld.insert(args[2].u);
                if (cb.pressed)
                    cb.pressed(cb.data);
            } else if (o->buttonsHeld.erase(args[2].u) > 0 && o->buttonsHeld.empty() && o->grabOutside) {
                // the last one let go over the app's surface: the guest's leave after its release
                o->grabOutside = false;
                o->focus = false;
                if (c.objects.count(o->pointerSurface)) {
                    leaveAfter = o->pointerSurface;
                    leaveSerial = args[0].u;
                }
            }
        }
        break;
    case Role::Keyboard:
        if ((opcode == kKeyboardEnter || opcode == kKeyboardLeave) && foreignArg < 0) {
            o->focus = opcode == kKeyboardEnter; // a surface of the guest's own (VMVIEW_STANDALONE): as it is
            break;
        }
        if (opcode == kKeyboardEnter || opcode == kKeyboardLeave) {
            o->hostFocus = opcode == kKeyboardEnter;
            o->lastSerial = args[0].u;
            keyboardFocus(c);
            return 0;
        }
        if ((opcode == kKeyboardKey || opcode == kKeyboardModifiers) && !o->focus)
            return 0;
        break;
    case Role::DataDevice:
    case Role::PrimaryDevice:
        // selection(offer): wl_data_device's event 5, zwp_primary_selection_device_v1's 1. Kept, and passed
        // on only while the guest has the keyboard; keyboardFocus gives it the latest when it gets it
        if ((o->role == Role::DataDevice && opcode == kDataDeviceSelection) ||
            (o->role == Role::PrimaryDevice && opcode == kPrimarySelection)) {
            o->selection = reinterpret_cast<wl_proxy *>(args[0].o);
            o->selectionKnown = true;
            if (!guestHasKeyboard(c)) {
                closeFds();
                return 0;
            }
            break;
        }
        if (o->role == Role::PrimaryDevice)
            break;
        if (opcode == kDataDeviceEnter) {
            o->focus = foreignArg < 0;
            if (o->focus) { // enter(serial, surface, x, y, offer)
                o->pointerSurface = byProxy[reinterpret_cast<wl_proxy *>(args[1].o)]->id;
                int32_t ox = 0, oy = 0;
                if (cutOf(c, o->pointerSurface, ox, oy)) {
                    args[2].f = moveFixed(args[2].f, ox);
                    args[3].f = moveFixed(args[3].f, oy);
                }
            }
        } else if (opcode == kDataDeviceMotion && o->focus) { // motion(time, x, y)
            int32_t ox = 0, oy = 0;
            if (cutOf(c, o->pointerSurface, ox, oy)) {
                args[1].f = moveFixed(args[1].f, ox);
                args[2].f = moveFixed(args[2].f, oy);
            }
        }
        else if ((opcode == kDataDeviceLeave || opcode == kDataDeviceMotion || opcode == kDataDeviceDrop) &&
                 !o->focus)
            return 0;
        if (opcode == kDataDeviceLeave || opcode == kDataDeviceDrop)
            o->focus = false;
        break;
    case Role::RelPointer:
        if (o->link && !o->link->focus)
            return 0;
        break;
    case Role::Touch: {
        // down(serial, time, surface, id, x, y) 0 on a guest surface: moved by its cut, the point remembered. up 1,
        // motion 2, shape 5 and orientation 6 of a point the guest never saw go down (on one of the app's surfaces)
        // are not its business; motion is moved by its surface's cut
        auto point = [&](int32_t tid) {
            return std::find_if(o->touches.begin(), o->touches.end(), [&](const auto &t) { return int32_t(t[0]) == tid; });
        };
        if (opcode == 0 && foreignArg < 0) {
            const uint32_t sid = byProxy[reinterpret_cast<wl_proxy *>(args[2].o)]->id;
            auto t = point(args[3].i);
            if (t != o->touches.end())
                (*t)[1] = sid;
            else if (o->touches.size() < 64)
                o->touches.push_back({uint32_t(args[3].i), sid});
            int32_t ox = 0, oy = 0;
            if (cutOf(c, sid, ox, oy)) {
                args[4].f = moveFixed(args[4].f, ox);
                args[5].f = moveFixed(args[5].f, oy);
            }
        } else if (opcode == 1 || opcode == 2 || opcode == 5 || opcode == 6) {
            auto t = point(opcode == 1 ? args[2].i : opcode == 2 ? args[1].i : args[0].i);
            if (t == o->touches.end())
                return 0;
            int32_t ox = 0, oy = 0;
            if (opcode == 2 && cutOf(c, (*t)[1], ox, oy)) {
                args[2].f = moveFixed(args[2].f, ox);
                args[3].f = moveFixed(args[3].f, oy);
            }
            if (opcode == 1)
                o->touches.erase(t);
        } else if (opcode == 4) { // cancel
            o->touches.clear();
        }
        break;
    }
    case Role::TabletTool:
        // proximity_in(serial, tablet, surface) 6 remembers the surface, motion(x, y) 10 is moved by its cut
        if (opcode == 6 && foreignArg < 0)
            o->pointerSurface = byProxy[reinterpret_cast<wl_proxy *>(args[2].o)]->id;
        if (opcode == 10) {
            int32_t ox = 0, oy = 0;
            if (cutOf(c, o->pointerSurface, ox, oy)) {
                args[0].f = moveFixed(args[0].f, ox);
                args[1].f = moveFixed(args[1].f, oy);
            }
        }
        break;
    case Role::Gesture:
        if (opcode == 0) // begin(serial, time, surface, ...)
            o->focus = foreignArg < 0;
        else if (!o->focus)
            return 0;
        break;
    default:
        break;
    }
    if (foreignArg >= 0) {
        if (g_debug)
            logf("<- %s.%s about one of the app's objects: kept from the guest", o->info->name, m->name);
        closeFds();
        return 0;
    }

    if (g_stats) {
        countFeedback(o, opcode, args);
        if (o->role == Role::Pointer && opcode != kPointerFrame)
            pointerEvents++;
        else if (o->role == Role::Keyboard && opcode == kKeyboardKey)
            keyEvents++;
    }
    OutMsg out(o->id, uint16_t(opcode));
    for (int i = 0; i < n; i++) {
        switch (specs[i].type) {
        case 'i':
        case 'u':
        case 'f':
            out.u(args[i].u);
            break;
        case 's':
            out.s(args[i].s);
            break;
        case 'a':
            out.a(args[i].a);
            break;
        case 'o':
            out.u(args[i].o ? byProxy[reinterpret_cast<wl_proxy *>(args[i].o)]->id : 0);
            break;
        case 'n': {
            auto *np = reinterpret_cast<wl_proxy *>(args[i].o);
            const vmview_iface *info = np ? ifaceByName(wl_proxy_get_class(np)) : nullptr;
            if (!np || !info) {
                // an object of an interface we do not know: impossible with our own message tables
                out.u(0);
                break;
            }
            Object *no = add(c, allocServerId(c), info, wl_proxy_get_version(np), Kind::Forwarded);
            no->proxy = np;
            no->bufW = o->bufW, no->bufH = o->bufH; // zwp_linux_buffer_params_v1.created: its create's size
            watch(np, no);
            out.u(no->id);
            break;
        }
        case 'h':
            out.fds.push_back(args[i].h);
            break;
        }
    }
    send(c, out);
    if (leaveAfter) {
        OutMsg leave(o->id, kPointerLeave);
        leave.u(leaveSerial);
        leave.u(leaveAfter);
        send(c, leave);
    }
    if (g_debug)
        logf("<- %s@%u.%s", o->info->name, o->id, m->name);
    if (o->info->destructor_events & (1ull << opcode)) {
        // the host destroyed it: our proxy goes, the guest's id is free again
        byProxy.erase(p);
        wl_proxy_destroy(p);
        o->proxy = nullptr;
        remove(c, o, true);
    }
    return 0;
}

// ---- VMVIEW_STATS ----

void View::countFeedback(const Object *o, uint32_t opcode, const wl_argument *args)
{
    if (strcmp(o->info->name, "wp_presentation_feedback") != 0)
        return;
    if (opcode == 1) { // presented(tv_sec_hi, tv_sec_lo, tv_nsec, refresh, seq_hi, seq_lo, flags)
        presented++;
        if (args[6].u & 0x8) // WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY
            zeroCopy++;
    } else if (opcode == 2) {
        discarded++;
    }
}

namespace {
struct OwnFeedbackListener {
    void (*sync_output)(void *, wl_proxy *, wl_proxy *);
    void (*presented)(void *, wl_proxy *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    void (*discarded)(void *, wl_proxy *);
};
const OwnFeedbackListener ownFeedbackListener = {
    [](void *, wl_proxy *, wl_proxy *) {},
    [](void *d, wl_proxy *f, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t flags) {
        auto *v = static_cast<View *>(d);
        v->ownPresented++;
        if (flags & 0x8) // WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY
            v->ownZeroCopy++;
        v->ownFeedback.erase(f);
        wl_proxy_destroy(f);
    },
    [](void *d, wl_proxy *f) {
        auto *v = static_cast<View *>(d);
        v->ownDiscarded++;
        v->ownFeedback.erase(f);
        wl_proxy_destroy(f);
    },
};
} // namespace

void View::ownFeedbackFor(Object *surface)
{
    // cursors and drag icons never go to the primary plane: only the window's surfaces count
    if (!ownPresentation || !surface->proxy || surface->surfaceRole == SurfaceRole::Cursor ||
        surface->surfaceRole == SurfaceRole::DragIcon || ownFeedback.size() > 256)
        return;
    wl_proxy *f = wl_proxy_marshal_flags(ownPresentation, 1, &wp_presentation_feedback_interface, 1, 0,
                                         surface->proxy, nullptr); // feedback(surface, callback)
    if (!f)
        return;
    wl_proxy_add_listener(f, reinterpret_cast<void (**)(void)>(const_cast<OwnFeedbackListener *>(&ownFeedbackListener)), this);
    ownFeedback.insert(f);
}

void View::printStats()
{
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!statsSince.tv_sec) {
        statsSince = now;
        return;
    }
    const double dt = double(now.tv_sec - statsSince.tv_sec) + double(now.tv_nsec - statsSince.tv_nsec) / 1e9;
    if (dt < 5)
        return;
    size_t objects = 0;
    for (auto &c : conns)
        objects += c->objects.size();
    logf("stats %.1f s: %u frames presented (%.1f/s), %u straight to the screen (%.0f %%), %u discarded; "
         "%u pointer and %u key events to the guest; %zu guest objects, %zu host objects; "
         "the guest's new buffers: %u presented, %u straight to the screen (%.0f %%), %u discarded",
         dt, presented, presented / dt, zeroCopy, presented ? 100.0 * zeroCopy / presented : 0.0, discarded,
         pointerEvents, keyEvents, objects, byProxy.size(), ownPresented, ownZeroCopy,
         ownPresented ? 100.0 * ownZeroCopy / ownPresented : 0.0, ownDiscarded);
    presented = zeroCopy = discarded = pointerEvents = keyEvents = 0;
    ownPresented = ownZeroCopy = ownDiscarded = 0;
    statsSince = now;
}

// ---- the thread ----

void View::applyPending()
{
    const int oldW = state.w, oldH = state.h;
    {
        std::lock_guard<std::mutex> lock(mutex);
        state = pending;
    }
    // the anchor at the view's top-left (applied with the app's next commit, as vmview_set_geometry asks)
    if (anchorSub)
        wl_subsurface_set_position(anchorSub, state.x, state.y);
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    for (auto &c : conns) {
        if (c->dead)
            continue;
        // the view's size changed: the guest's next commit of its root clips to it, or ours within 100 ms (a paused
        // or stalled guest)
        if ((state.w != oldW || state.h != oldH) && rootOf(*c) && !c->clipDue)
            c->clipDue = int64_t(now.tv_sec) * 1000000000 + now.tv_nsec + 100000000;
        placeToplevel(*c);
        keyboardFocus(*c);
        configureToplevel(*c);
    }
}

void View::run()
{
    const int dfd = wl_display_get_fd(display);
    std::vector<pollfd> pf;
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (pending.stop)
                break;
        }
        while (wl_display_prepare_read_queue(display, queue) != 0)
            wl_display_dispatch_queue_pending(display, queue);
        wl_display_flush(display);
        for (auto &c : conns)
            if (!c->dead && !c->out.empty())
                flushConn(*c);
        pf.clear();
        pf.push_back({dfd, POLLIN, 0});
        pf.push_back({wakeFd, POLLIN, 0});
        pf.push_back({listenFd, POLLIN, 0});
        for (auto &c : conns)
            pf.push_back({c->fd, short(POLLIN | (c->wantWrite ? POLLOUT : 0)), 0});
        int timeout = 1000;
        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        const int64_t nowNs = int64_t(now.tv_sec) * 1000000000 + now.tv_nsec;
        for (auto &c : conns)
            if (!c->dead && c->clipDue)
                timeout = int(std::clamp<int64_t>((c->clipDue - nowNs + 999999) / 1000000, 0, timeout));
        const int r = poll(pf.data(), pf.size(), timeout);
        if (r > 0 && (pf[0].revents & POLLIN))
            wl_display_read_events(display);
        else
            wl_display_cancel_read(display);
        if (wl_display_get_error(display)) {
            logf("the app's Wayland connection failed (%s): stopping", strerror(wl_display_get_error(display)));
            break;
        }
        wl_display_dispatch_queue_pending(display, queue);
        if (r > 0 && (pf[1].revents & POLLIN)) {
            uint64_t v;
            (void)!read(wakeFd, &v, sizeof v);
            applyPending();
        }
        if (r > 0 && (pf[2].revents & POLLIN))
            accept();
        for (size_t i = 3; i < pf.size() && i - 3 < conns.size(); i++) {
            Conn &c = *conns[i - 3];
            if (c.dead)
                continue;
            if (pf[i].revents & (POLLIN | POLLHUP | POLLERR))
                readConn(c);
            if (!c.dead && (pf[i].revents & POLLOUT))
                flushConn(c);
        }
        for (auto &c : conns)
            if (!c->dead && c->overflow)
                closeConn(*c, "the guest does not read its events");
        // a change of the view the guest has not committed its root for: clipped by a commit of ours
        clock_gettime(CLOCK_MONOTONIC, &now);
        for (auto &c : conns) {
            if (c->dead || !c->clipDue || int64_t(now.tv_sec) * 1000000000 + now.tv_nsec < c->clipDue)
                continue;
            c->clipDue = 0;
            if (viewNeedsFix(*c)) {
                if (g_debug)
                    logf("the view changed, the guest did not commit: clipped by a commit of ours");
                rootCommit(*c, false);
            }
        }
        // the guest reads again: its held pointer motion goes out (the latest position, its frame)
        for (auto &c : conns)
            if (!c->dead && c->anyHeld && c->out.size() <= kCoalesceAbove / 2)
                releaseAllHeld(*c);
        const size_t before = conns.size();
        conns.erase(std::remove_if(conns.begin(), conns.end(), [](auto &c) { return c->dead; }), conns.end());
        if (conns.size() != before && cb.connection)
            cb.connection(cb.data, int(conns.size()));
        for (auto &c : conns)
            flushConn(*c);
        wl_display_flush(display);
        if (g_stats)
            printStats();
    }
    stopAll();
}

void View::stopAll()
{
    // the guest's window closed first, as a compositor closes a window: its nested compositor quits on
    // its own and its session ends at the login screen (vitrine's change, 2026-10-15). Closing the
    // connection alone now ends the session too (rutabaga and the proxy pass the hang-up on since
    // 8ecffaf), but abruptly: KWin's wrapper restarts KWin a few times first. Up to ~50 ms for the
    // message to go out.
    for (auto &c : conns) {
        if (c->dead || !c->toplevel)
            continue;
        OutMsg m(c->toplevel->id, kToplevelClose);
        send(*c, m);
        flushConn(*c);
        for (int i = 0; i < 5 && !c->dead && !c->out.empty(); i++) {
            pollfd p{c->fd, POLLOUT, 0};
            poll(&p, 1, 10);
            flushConn(*c);
        }
        logf("guest window closed");
    }
    for (auto &c : conns)
        closeConn(*c, "the view is going away");
    conns.clear();
    if (subcompositor)
        wl_subcompositor_destroy(subcompositor);
    subcompositor = nullptr;
    if (anchorSub)
        wl_subsurface_destroy(anchorSub);
    if (anchor)
        wl_surface_destroy(anchor);
    if (anchorBuffer)
        wl_buffer_destroy(anchorBuffer);
    anchorSub = nullptr;
    anchor = nullptr;
    anchorBuffer = nullptr;
    if (ownViewporter)
        wl_proxy_marshal_flags(ownViewporter, 0, nullptr, 1, WL_MARSHAL_FLAG_DESTROY); // wp_viewporter.destroy
    ownViewporter = nullptr;
    if (ownCompositor)
        wl_proxy_destroy(ownCompositor);
    ownCompositor = nullptr;
    for (wl_proxy *f : ownFeedback)
        wl_proxy_destroy(f);
    ownFeedback.clear();
    if (ownPresentation)
        wl_proxy_marshal_flags(ownPresentation, 0, nullptr, 1, WL_MARSHAL_FLAG_DESTROY); // wp_presentation.destroy
    ownPresentation = nullptr;
    if (registry)
        wl_registry_destroy(registry);
    registry = nullptr;
    wl_display_flush(display);
}

bool View::start(wl_display *d, wl_surface *p, const char *path, const vmview_callbacks *callbacks)
{
    display = d;
    parent = p;
    if (callbacks)
        cb = *callbacks;
    socketPath = path;
    queue = wl_display_create_queue(display);
    wrapper = static_cast<wl_display *>(wl_proxy_create_wrapper(display));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapper), queue);
    registry = wl_display_get_registry(wrapper);
    wl_proxy_add_dispatcher(reinterpret_cast<wl_proxy *>(registry), &View::dispatch, this, nullptr);
    if (wl_display_roundtrip_queue(display, queue) < 0) {
        logf("no answer from the compositor");
        return false;
    }
    for (auto &kv : globals)
        if (kv.second.iface == "wl_subcompositor")
            subcompositor = static_cast<wl_subcompositor *>(
                wl_registry_bind(registry, kv.first, &wl_subcompositor_interface, 1));
    if (!subcompositor) {
        logf("the compositor has no wl_subcompositor");
        return false;
    }
    // clipping to the view: regions of ours, and crops (without wp_viewporter, the input regions only)
    if (g_clip)
        for (auto &kv : globals) {
            if (kv.second.iface == "wl_compositor" && !ownCompositor)
                ownCompositor = static_cast<wl_proxy *>(wl_registry_bind(registry, kv.first, &wl_compositor_interface, 1));
            if (kv.second.iface == "wp_viewporter" && !ownViewporter && ifaceByName("wp_viewporter"))
                ownViewporter = static_cast<wl_proxy *>(
                    wl_registry_bind(registry, kv.first, ifaceByName("wp_viewporter")->iface, 1));
        }
    if (g_clip && !ownViewporter)
        logf("the compositor has no wp_viewporter: the guest's surfaces are kept to the view for input only");
    // the anchor: a transparent pixel without input, at the view's top-left (state: the defaults until the app's
    // vmview_set_geometry)
    wl_shm *shm = nullptr;
    if (g_clip)
        for (auto &kv : globals)
            if (kv.second.iface == "wl_shm" && !shm)
                shm = static_cast<wl_shm *>(wl_registry_bind(registry, kv.first, &wl_shm_interface, 1));
    const int afd = shm && ownCompositor ? memfd_create("vmview-anchor", MFD_CLOEXEC) : -1;
    if (afd >= 0 && ftruncate(afd, 4) == 0) {
        auto *comp = reinterpret_cast<wl_compositor *>(ownCompositor);
        wl_shm_pool *pool = wl_shm_create_pool(shm, afd, 4);
        anchorBuffer = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4, WL_SHM_FORMAT_ARGB8888); // all zero: transparent
        wl_shm_pool_destroy(pool);
        anchor = wl_compositor_create_surface(comp);
        wl_region *empty = wl_compositor_create_region(comp);
        wl_surface_set_input_region(anchor, empty);
        wl_region_destroy(empty);
        wl_surface_attach(anchor, anchorBuffer, 0, 0);
        wl_surface_commit(anchor);
        anchorSub = wl_subcompositor_get_subsurface(subcompositor, anchor, parent);
        wl_subsurface_set_desync(anchorSub);
        wl_subsurface_set_position(anchorSub, pending.x, pending.y);
    }
    if (afd >= 0)
        close(afd);
    if (shm)
        wl_proxy_destroy(reinterpret_cast<wl_proxy *>(shm));
    if (g_stats)
        for (auto &kv : globals)
            if (kv.second.iface == "wp_presentation")
                ownPresentation = static_cast<wl_proxy *>(wl_registry_bind(registry, kv.first, &wp_presentation_interface, 1));
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    if (socketPath.size() >= sizeof sa.sun_path) {
        errno = ENAMETOOLONG;
        logf("socket path too long: %s", path);
        return false;
    }
    memcpy(sa.sun_path, socketPath.c_str(), socketPath.size() + 1);
    listenFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    unlink(socketPath.c_str());
    if (listenFd < 0 || bind(listenFd, reinterpret_cast<sockaddr *>(&sa), sizeof sa) < 0 || listen(listenFd, 4) < 0) {
        logf("cannot serve %s: %s", path, strerror(errno));
        return false;
    }
    wakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    state = pending;
    unsigned shown = 0;
    for (auto &kv : globals)
        shown += kv.second.shown;
    logf("serving %s; %u of the compositor's %zu globals go to the guest", path, shown, globals.size());
    thread = std::thread([this] { run(); });
    return true;
}

} // namespace

struct vmview {
    View view;
};

extern "C" {

vmview *vmview_new(wl_display *display, wl_surface *parent, const char *socket_path, const vmview_callbacks *callbacks)
{
    auto *v = new vmview;
    if (!v->view.start(display, parent, socket_path, callbacks)) {
        const int e = errno;
        if (v->view.listenFd >= 0)
            close(v->view.listenFd);
        delete v;
        errno = e;
        return nullptr;
    }
    return v;
}

static void wake(vmview *v)
{
    const uint64_t one = 1;
    (void)!write(v->view.wakeFd, &one, sizeof one);
}

void vmview_set_geometry(vmview *v, int x, int y, int width, int height)
{
    {
        std::lock_guard<std::mutex> lock(v->view.mutex);
        v->view.pending.x = x;
        v->view.pending.y = y;
        v->view.pending.w = std::max(1, width);
        v->view.pending.h = std::max(1, height);
    }
    wake(v);
}

void vmview_set_fullscreen(vmview *v, bool fullscreen)
{
    {
        std::lock_guard<std::mutex> lock(v->view.mutex);
        v->view.pending.fullscreen = fullscreen;
    }
    wake(v);
}

void vmview_set_keyboard_focus(vmview *v, bool focus)
{
    {
        std::lock_guard<std::mutex> lock(v->view.mutex);
        v->view.pending.keyboard = focus;
    }
    wake(v);
}

void vmview_destroy(vmview *v)
{
    if (!v)
        return;
    {
        std::lock_guard<std::mutex> lock(v->view.mutex);
        v->view.pending.stop = true;
    }
    wake(v);
    if (v->view.thread.joinable())
        v->view.thread.join();
    close(v->view.listenFd);
    close(v->view.wakeFd);
    unlink(v->view.socketPath.c_str());
    wl_proxy_wrapper_destroy(v->view.wrapper);
    wl_event_queue_destroy(v->view.queue);
    delete v;
}

} // extern "C"
