// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QString>

#include <optional>

#include <sys/types.h>

/*
 * The host's GPUs, by their DRM render nodes, for the GPU VMs render on.
 * The nested desktop's card: virtio-gpu-rutabaga's x-render-node, which
 * hands the node to rutabaga and virglrenderer (vrend's GBM device and the
 * native context alike); without it, virglrenderer's native context opens
 * the first render node of its own table (amdgpu, then i915, then xe),
 * whatever GPU this computer's desktop runs on.  The other 3D cards
 * (virtio-vga-gl): virglrenderer, which QEMU gives its EGL display but no
 * get_drm_fd, renders where QEMU's EGL runs, vrend and the native context
 * alike (virgl_egl_open_drm_fd()), and so does the VM's screen in vitrine's
 * window: the D-Bus display's rendernode= (displayNode()).
 *
 * Read from sysfs: the render nodes of the PCI display controllers, named
 * from pci.ids as HostDevices names PCI devices.  Other render nodes are no
 * GPUs (vgem and the like) and are left out.
 */
struct HostGpu
{
    QString node;           // /dev/dri/renderD128
    /*
     * udev's stable link to it, /dev/dri/by-path/pci-0000:03:00.0-render:
     * with several GPUs the numbers of the nodes can change from one boot
     * to the next.  Empty when udev made none.
     */
    QString byPath;
    QString address;        // 0000:03:00.0
    QString driver;         // amdgpu, i915, xe...
    quint16 vendorId = 0;
    quint16 deviceId = 0;
    QString vendorName;     // from pci.ids, else the ID in hex
    QString deviceName;
    /* The firmware's screen was on it (boot_vga) */
    bool bootVga = false;

    /* What a VM keeps when it is chosen: byPath, else node */
    QString path() const;
    /*
     * Short: the vendor's name and the model's, e.g. "Intel Arc Pro B50"
     * from Intel Corporation's "Battlemage G21 [Arc Pro B50]", "AMD
     * HawkPoint1"; the IDs for what pci.ids does not know
     */
    QString name() const;
    /* 03:00.0 */
    QString shortAddress() const;
    /*
     * Vitrine's virglrenderer has a native context for its driver (amdgpu,
     * i915, xe): the nested desktop's card (x-drm=on) renders on no other,
     * as virglrenderer then fails to start
     */
    bool hasNativeContext() const;
};

namespace HostGpus {

/* The GPUs, in the order of their PCI addresses */
QList<HostGpu> list();
/*
 * The GPU of the DRM device numbered @device: a primary node (card*) or a
 * render node of it, as a compositor names its main device
 */
std::optional<HostGpu> ofDevice(dev_t device);

/*
 * The DRM device this computer's desktop runs on: the main device its
 * compositor names (zwp_linux_dmabuf_v1's default feedback), which the
 * windows' side learns over its Wayland connection and hands over at
 * start.  None: not known (X11, an older compositor).
 */
void setDesktopDevice(std::optional<dev_t> device);
/*
 * The GPU of @gpus this computer's desktop runs on: the one of
 * setDesktopDevice(), else the one the firmware's screen was on
 * (boot_vga), else none
 */
std::optional<HostGpu> desktop(const QList<HostGpu> &gpus);
/*
 * The GPU of @gpus virglrenderer's native context takes by itself (no
 * x-render-node): the first render node of amdgpu, else of i915, else of
 * xe
 */
std::optional<HostGpu> rendererChoice(const QList<HostGpu> &gpus);
/*
 * Automatic, a VM's choice when it makes none: on a host with several
 * GPUs, the desktop's, so that the guest's frames stay on the GPU that
 * shows them, if it has a native context; else none, the renderer's own
 * choice, as with one GPU
 */
std::optional<HostGpu> automatic(const QList<HostGpu> &gpus);
/* The x-render-node the runner adds for automatic(), on this host now; empty for none */
QString automaticNode();
/*
 * The rendernode= the runner gives QEMU's D-Bus display with OpenGL, on
 * this host now: on a host with several GPUs, the desktop's.  QEMU's EGL
 * runs there: it draws the VM's screen into the textures vitrine's window
 * imports, which must come from the GPU that window draws on, and the 3D of
 * a card without x-render-node follows it (virtio-vga-gl: vrend and the
 * native context alike), so that VMs render on the desktop's GPU; the
 * nested desktop's card renders where its own x-render-node says.  The
 * same GPU as automatic()'s, so only one with a native context: on another
 * (NVIDIA's), a main-line card with the native context would follow the
 * display there and not start.  Empty otherwise: QEMU's own choice, the
 * first render node, as before.
 */
QString displayNode();
/*
 * The GPU of @gpus that @path names, a VM's x-render-node: its node, its
 * by-path link, or any link to them
 */
std::optional<HostGpu> find(const QList<HostGpu> &gpus, const QString &path);

/*
 * @path, a VM's x-render-node, as the user reads it: "PCI 03:00.0" for
 * udev's by-path link of a PCI device, which outlives the device, else
 * the path itself
 */
QString describe(const QString &path);

/* What a VM's x-render-node names on this host */
enum class PathState {
    RenderNode,     // a render node, as QEMU wants it
    Missing,        // nothing (a GPU taken out, a link udev no longer makes)
    NotRenderNode,  // something else, e.g. a primary node, /dev/dri/card1
};
PathState pathState(const QString &path);

/* "/sys" and "/dev" elsewhere: a fake host, for tests */
void setRoots(const QString &sysfs, const QString &dev);

}
