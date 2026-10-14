# shellcheck shell=bash
# SPDX-License-Identifier: GPL-2.0-or-later
# What the setup builds: the upstream sources and the commits our patches apply to (sourced).
FEDORA=${VGD_FEDORA:-44}

# host
VIRGL_URL=https://gitlab.freedesktop.org/virgl/virglrenderer.git
VIRGL_SPEC=patches/virglrenderer/main/virglrenderer.spec      # its %global commit, its patches beside it
RUTABAGA_URL=https://github.com/magma-gpu/rutabaga_gfx.git
RUTABAGA_BASE=4fb9f32055d8e001cc9450e92db76fc6fc16804e        # v0.1.85 + patches/rutabaga_gfx/changes.diff
QEMU_URL=https://gitlab.com/qemu-project/qemu.git
QEMU_BASE=3876503faff51ce3a132dbbd0ab42c10f2269319            # upstream QEMU our series applies to
QEMU_SERIES=patches/qemu/series                               # our QEMU: QEMU_BASE + these patches (git am), the
                                                              # source of truth since 2026-10-14 (not the qemu-gui fork)

# guest
CLOUD_IMAGE=Fedora-Cloud-Base-Generic-44-1.7.x86_64.qcow2
CLOUD_CHECKSUM=Fedora-Cloud-44-1.7-x86_64-CHECKSUM
CLOUD_URL=https://download.fedoraproject.org/pub/fedora/linux/releases/44/Cloud/x86_64/images
# Plasma in the guest: "beta" = the KDE SIG's COPR below (6.7.91 Plasma 6.8 beta, what the desktop's
# guests run since 2026-10-06), "fedora" = Fedora's own packages. GUEST_PLASMA= overrides.
GUEST_PLASMA_DEFAULT=beta
GUEST_PLASMA_COPR=@kdesig/kde-beta

# Our KDE builds, per machine: the packages patched, and the patch set for each version of them
# (patches/PACKAGE/sets/SERIES.series): kde_set_for ROLE PACKAGE VERSION -> "SERIES TAG [EXCLUDED...]".
# TAG goes into the RPM release (N.TAG); the excluded patches' names start with these numbers. 6.8.0
# drops guest KWin's 0033, fixed upstream (findings/host-set-6.8.0-plan.md); the host's hl becomes h18.
# The desktop (2026-10-12): host KWin hl on Plasma 6.7.91; guests KWin g21a + KPipeWire k2. Since
# 2026-10-13 setup builds guest KWin g21b = g21a + 0046 (the nested window follows the host's scale,
# findings/laptop-screen-checks-2026-10-13.md); since 19:30 g21g = g21b + 0047 (QML effect swapchains
# pooled, opt-in) + 0048 v3 (their scanout buffers in the layers' format, opt-in) + 0050 (QML effects
# composited, opt-in: KWIN_QUICKEFFECT_COMPOSITE=1 in session.conf - better on the laptop at 1200p, worse on
# the desktop at 4K, 2026-10-13 night); older sets: VGD_GUEST_KWIN_SET=g21b. For 6.8.0 the guest set is g22 =
# g21b without 0033: the three Overview experiments (0047 pool, 0048 format, 0050 composite) gave the desktop
# nothing (night 2026-10-13/14) and the user wants little code (2026-10-14) - they stay in patches/kwin.
# 2026-10-14 02:10: + 0051 (the nested grab, Ctrl+Alt+G, takes effect at once - it waited for the guest's next
# repaint): g21i = g21g + 0051 (6.7.91, the desktop's guests since 02:12), g21j = g21b + 0051 (6.8.0's base).
HOST_KDE_PACKAGES="kwin"
GUEST_KDE_PACKAGES="kwin kpipewire"
# GUEST_QT_Q2 [1]: Qt's Wayland shm pool reuse (qtbase, findings/qt-shm-pool-reuse-2026-10-09.md) - the
# guests' default since 2026-10-14: the user saw resizes smoother with it side by side (window title 230-240
# against 190-200 fps with QT_WAYLAND_SHM_NO_REUSE=1); GUEST_QT_Q2=0 keeps Fedora's Qt. Set q4 (patch v3) since
# 2026-10-14 03:30: q2 kept a shrunk window's largest pools (84 MiB against 7.3 stock), q4 bounds them at twice
# the window's buffers with q2's speed (bare metal too: Konsole +33 %, Dolphin +65 % sizes shown per second)
[ "${GUEST_QT_Q2:-1}" = 1 ] && GUEST_KDE_PACKAGES+=" qt6-qtbase"
# a package's folder under patches/
kde_patch_dir() { case $1 in qt6-qtbase) echo qtbase ;; *) echo "$1" ;; esac; }
kde_set_for() {
	case $1/$2/$3 in
	host/kwin/6.7.91) echo "hl39 hl39" ;;   # hl + 0039 (pointer locks on the subsurface under the pointer: vmview's grab)
	host/kwin/6.8.0) echo "hl39-68 h18" ;;   # the same on the v6.8.0 tag (2026-10-08): the patches as on 6.7.91
	host/kwin/6.8.*) echo "hl39-681 h18" ;;  # 6.8.1 on: Plasma/6.8 after 2026-10-08 (VRR repaint driver): 0003/0039 .68
	guest/kwin/6.7.91) echo "${VGD_GUEST_KWIN_SET:-g21i} ${VGD_GUEST_KWIN_SET:-g21i}" ;;
	guest/kwin/6.8.0) echo "g21j g22 0033" ;;   # lean (g21b + 0051): without the Overview experiments 0047/0048/0050
	guest/kwin/6.8.*) echo "g21j-681 g22 0033" ;;   # 6.8.1 on: 0003/0035/0042 in their .68 versions
	guest/kpipewire/6.7.91 | guest/kpipewire/6.8.*) echo "k2 k2" ;;
	guest/qt6-qtbase/6.11.2) echo "q4 q4" ;;
	*) return 1 ;;
	esac
}
