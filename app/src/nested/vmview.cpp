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
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
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
constexpr uint16_t kPointerButton = 3;
constexpr uint16_t kKeyboardEnter = 1, kKeyboardLeave = 2, kKeyboardKey = 3, kKeyboardModifiers = 4;
constexpr uint16_t kDataDeviceEnter = 1, kDataDeviceLeave = 2, kDataDeviceMotion = 3, kDataDeviceDrop = 4;
constexpr uint16_t kToplevelConfigure = 0, kToplevelClose = 1, kSurfaceConfigure = 0, kPopupDone = 1;
constexpr uint32_t kStateFullscreen = 2, kStateActivated = 4;
constexpr uint32_t kServerIdStart = 0xff000000;
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

enum class Kind : uint8_t { Forwarded, Display, Registry, WmBase, Positioner, XdgSurface, Toplevel, Popup };
enum class Role : uint8_t { None, Pointer, Keyboard, Surface, DataDevice, RelPointer, Gesture, TabletTool, TextInput };
// the role the host gave a guest wl_surface: a second one would be a protocol error on the app's connection
enum class SurfaceRole : uint8_t { None, Subsurface, Cursor, DragIcon, Toplevel };

Role roleOf(const char *name)
{
    static const std::unordered_map<std::string, Role> roles = {
        {"wl_pointer", Role::Pointer},
        {"wl_keyboard", Role::Keyboard},
        {"wl_surface", Role::Surface},
        {"wl_data_device", Role::DataDevice},
        {"zwp_relative_pointer_v1", Role::RelPointer},
        {"zwp_pointer_gesture_swipe_v1", Role::Gesture},
        {"zwp_pointer_gesture_pinch_v1", Role::Gesture},
        {"zwp_pointer_gesture_hold_v1", Role::Gesture},
        {"zwp_tablet_tool_v2", Role::TabletTool},
        {"zwp_text_input_v3", Role::TextInput},
    };
    auto it = roles.find(name);
    return it == roles.end() ? Role::None : it->second;
}

struct Conn;
struct View;

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
    // xdg
    int32_t geom[4] = {0, 0, 0, 0};
    bool hasGeom = false;
    SurfaceRole surfaceRole = SurfaceRole::None; // wl_surface
    bool onAppWindow = false;     // a shortcuts inhibitor moved to the app's window
    wl_subsurface *subsurface = nullptr; // toplevel: the host subsurface of its wl_surface
    bool bufferAttached = false;  // wl_surface: a buffer attached since the last commit (VMVIEW_STATS)
    uint32_t configureSerial = 0;
};

struct Conn {
    View *view;
    int fd;
    bool dead = false;
    bool wantWrite = false;
    std::vector<uint8_t> in;
    std::vector<int> inFds;
    std::vector<uint8_t> out;
    std::vector<int> outFds;
    std::unordered_map<uint32_t, std::unique_ptr<Object>> objects;
    std::vector<uint32_t> freeServerIds;
    uint32_t nextServerId = kServerIdStart;
    Object *toplevel = nullptr;
    Conn(View *v, int f) : view(v), fd(f) {}
};

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
    // host events
    int event(wl_proxy *p, uint32_t opcode, const wl_message *m, wl_argument *args);
    void keyboardFocus(Conn &c);
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
    conns.push_back(std::make_unique<Conn>(this, fd));
    // the guest's wl_display
    add(*conns.back(), 1, ifaceByName("wl_display"), 1, Kind::Display);
    logf("guest connected (%zu connection%s)", conns.size(), conns.size() == 1 ? "" : "s");
    if (cb.connection)
        cb.connection(cb.data, int(conns.size()));
}

void View::send(Conn &c, OutMsg &m)
{
    if (c.dead) {
        for (int f : m.fds)
            close(f);
        return;
    }
    m.finish();
    const size_t at = c.out.size();
    c.out.resize(at + m.w.size() * 4);
    memcpy(&c.out[at], m.w.data(), m.w.size() * 4);
    c.outFds.insert(c.outFds.end(), m.fds.begin(), m.fds.end());
    m.fds.clear();
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
    for (;;) {
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
            handle(c, hdr[0], uint16_t(hdr[1] & 0xffff), &c.in[pos + 8], size - 8);
            pos += size;
        }
        if (c.dead)
            return;
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
    Object *p = o.get();
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
    if (o->subsurface) {
        byProxy.erase(reinterpret_cast<wl_proxy *>(o->subsurface));
        wl_subsurface_destroy(o->subsurface);
        o->subsurface = nullptr;
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
    hostDestroy(o);
    c.objects.erase(id);
    if (id >= kServerIdStart)
        c.freeServerIds.push_back(id);
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

void View::handle(Conn &c, uint32_t id, uint16_t opcode, const uint8_t *body, size_t len)
{
    auto it = c.objects.find(id);
    if (it == c.objects.end()) {
        // a request to an object we already destroyed (its destructor event crossed it): ignored, as
        // libwayland-server does for zombies
        if (id >= kServerIdStart)
            return;
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
        if (!strcmp(o->info->name, "wl_subcompositor") && opcode == 1) { // get_subsurface(id, surface, parent)
            if (objArgs[1] == objArgs[2] || objArgs[1]->surfaceRole != SurfaceRole::None)
                return fail("the surface has a role already, or is its own parent");
            objArgs[1]->surfaceRole = SurfaceRole::Subsurface;
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
                                                             reinterpret_cast<wl_surface *>(surface->proxy), parent);
            wl_subsurface_set_desync(tl->subsurface);
            placeToplevel(c);
            configureToplevel(c);
            keyboardFocus(c);
            setMapped(true);
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
            if (c.toplevel && c.toplevel->link == o)
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
    if (changed)
        configureToplevel(c);
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
    // an object the guest does not know: one of the app's, or one destroyed on the app's side, which
    // libwayland passes as NULL where the protocol allows none (the keyboard's leave of a window the
    // app just closed): the guest takes a NULL there for a protocol error
    auto foreign = [&](int i) {
        if (specs[i].type != 'o')
            return false;
        if (!args[i].o)
            return !specs[i].nullable;
        return !byProxy.count(reinterpret_cast<wl_proxy *>(args[i].o));
    };
    int foreignArg = -1;
    for (int i = 0; i < n; i++)
        if (foreign(i))
            foreignArg = i;

    // focus: events about the app's own surfaces stay with the app
    switch (o->role) {
    case Role::Pointer:
        if (opcode == kPointerEnter) {
            o->focus = foreignArg < 0;
        } else if (opcode == kPointerLeave) {
            if (foreignArg >= 0 || !o->focus) {
                closeFds();
                return 0;
            }
            o->focus = false;
            o->pendingFrame = true;
            break;
        } else if (opcode == kPointerFrame) {
            if (!o->pendingFrame)
                return 0;
            o->pendingFrame = false;
            break;
        } else if (!o->focus) {
            return 0;
        }
        if (foreignArg < 0)
            o->pendingFrame = true;
        if (opcode == kPointerButton && args[3].u == WL_POINTER_BUTTON_STATE_PRESSED && cb.pressed)
            cb.pressed(cb.data);
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
        if (opcode == kDataDeviceEnter)
            o->focus = foreignArg < 0;
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
    {
        std::lock_guard<std::mutex> lock(mutex);
        state = pending;
    }
    for (auto &c : conns) {
        if (c->dead)
            continue;
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
        const int r = poll(pf.data(), pf.size(), 1000);
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
