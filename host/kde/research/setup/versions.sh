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
QEMU_BASE=c61ea024f6d58261394b1b408e0f220e30c136a4            # upstream QEMU our series applies to (master 2026-10-14;
                                                              # 3876503faff5 until 2026-10-15)
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
# HOST_KSVG_K2 [1] / HOST_KIRIGAMI_F1 [1] (2026-10-23, the Arc Pro B50 desktop): the host's Plasma popups re-uploaded
# their frames at each opening too (popup-frame-reuploads) - on the B50 a ~7 ms GPU job per launcher opening that KWin
# waited for: with KSvg k2 + Kirigami f1 the launcher 72-78 -> 61-69 ms at 4K 120, Super 8/s repeats 6-9 -> 0.2 a
# second (findings/b50-vs-amd-plan-2026-10-21.md); =0 keeps Fedora's
[ "${HOST_KSVG_K2:-1}" = 1 ] && HOST_KDE_PACKAGES+=" kf6-ksvg"
[ "${HOST_KIRIGAMI_F1:-1}" = 1 ] && HOST_KDE_PACKAGES+=" kf6-kirigami"
# HOST_QT_Q4 [1] (2026-10-24): Qt's Wayland shm pool reuse on the host too - the guests' rule applies to the host since
# 2026-10-20 (a measured gain: bare 4K resizes Konsole 85-88 -> 111-114, Dolphin 62 -> 99-102 images a second,
# findings/qt-shm-pool-reuse-2026-10-09.md); the desktop has run it since 10-20 22:27, setup did not build it. Every
# installed qtbase subpackage is replaced from the same build (host-kde.sh): never a part of the set. =0 keeps Fedora's.
[ "${HOST_QT_Q4:-1}" = 1 ] && HOST_KDE_PACKAGES+=" qt6-qtbase"
# GUEST_QT_Q2 [1]: Qt's Wayland shm pool reuse (qtbase, findings/qt-shm-pool-reuse-2026-10-09.md) - the
# guests' default since 2026-10-14: the user saw resizes smoother with it side by side (window title 230-240
# against 190-200 fps with QT_WAYLAND_SHM_NO_REUSE=1); GUEST_QT_Q2=0 keeps Fedora's Qt. Set q4 (patch v3) since
# 2026-10-14 03:30: q2 kept a shrunk window's largest pools (84 MiB against 7.3 stock), q4 bounds them at twice
# the window's buffers with q2's speed (bare metal too: Konsole +33 %, Dolphin +65 % sizes shown per second)
[ "${GUEST_QT_Q2:-1}" = 1 ] && GUEST_KDE_PACKAGES+=" qt6-qtbase"
# GUEST_KSVG_K1 [1]: KSvg k1 (patches/ksvg, findings/popup-frame-reuploads-2026-10-15.md) - the guests' default
# since 2026-10-15: Plasma's popups no longer upload their themed frames again at each opening (the launcher:
# 141-407 uploads -> 1, render -30 %, the same pixels); GUEST_KSVG_K1=0 keeps Fedora's kf6-ksvg. Set k2 since
# 2026-10-15 05:10: k1's kept copies bounded in bytes (kcp2: the same 0 copies by a third tour of the popups)
[ "${GUEST_KSVG_K1:-1}" = 1 ] && GUEST_KDE_PACKAGES+=" kf6-ksvg"
# GUEST_KIRIGAMI_F1 [1]: Kirigami f1 (patches/kirigami) - the guests' default since 2026-10-15: frameContrastChanged() only
# when the value changes (Plasma's style set it again at every color sync; a popup's items reacted ~1,600 times per
# opening and closing): the launcher's slots 19 -> 6.6 ms of the GUI thread, sync -1.4 ms, the same pixels (kca1)
[ "${GUEST_KIRIGAMI_F1:-1}" = 1 ] && GUEST_KDE_PACKAGES+=" kf6-kirigami"
# GUEST_PLASMA_W1 [0 until checked]: plasma-workspace w1 (patches/plasma-workspace, 2026-10-17): the task manager's
# Wayland model batches window geometry changes - with a 4000 Hz mouse, every step of a window drag was a dataChanged
# through the proxy models into QML, and the guest's panel froze for seconds behind them
# (findings/plasmashell-freeze-hunt-2026-10-17.md)
[ "${GUEST_PLASMA_W1:-0}" = 1 ] && GUEST_KDE_PACKAGES+=" plasma-workspace"
# a package's folder under patches/
kde_patch_dir() { case $1 in qt6-qtbase) echo qtbase ;; kf6-ksvg) echo ksvg ;; kf6-kirigami) echo kirigami ;; *) echo "$1" ;; esac; }
kde_set_for() {
	case $1/$2/$3 in
	host/kwin/6.7.91) echo "${VGD_HOST_KWIN_SET:-hl39y} ${VGD_HOST_KWIN_SET:-hl39y}" ;;   # hl39y (2026-10-24) = hl39x + 0082 (a layer with no damage keeps its buffer, on with $XDG_RUNTIME_DIR/kwin-skip-empty-layer-render: host-session); hl39x (2026-10-24) = hl39w + 0081 (an empty render is not content, on with $XDG_RUNTIME_DIR/kwin-empty-render-not-content: host-session); hl39w = hl39u + 0079 (the scanout slack at run time, inert) + 0080 (the active window a plane candidate, on with $XDG_RUNTIME_DIR/kwin-active-window-candidate: host-session); hl39u = hl39t + 0078 (the direct-scanout journal's variance cap, on with $XDG_RUNTIME_DIR/kwin-scanout-variance-cap: host-session); hl39t (2026-10-23) = hl39j + 0074 (lone-frame lead, off) + 0070 (layer surfaces) + 0075 (no udmabuf import on xe) + 0077 (effects start one refresh in, on with $XDG_RUNTIME_DIR/kwin-animation-start-step: host-session); before:   # hl + 0039 (pointer locks on the subsurface under the pointer: vmview's grab); hl39j (2026-10-20) = hl39i + 0068.hl (upstream e3f5caef: pointer constraints without a commit)
	host/kwin/6.8.0) echo "hl39-68 h18" ;;   # the same on the v6.8.0 tag (2026-10-08): the patches as on 6.7.91
	host/kwin/6.8.*) echo "${VGD_HOST_KWIN_SET_68:-h681a} ${VGD_HOST_KWIN_SET_68:-h681a}" ;;  # h681a (2026-10-25) = hl39y on 6.8.1: 0068 dropped (upstream), 0003/0039/0060/0077 in .68 versions (findings/kwin-6.8.1-port-notes-2026-10-24.md); before: hl39-681 (an older set): 6.8.1 on: Plasma/6.8 after 2026-10-08 (VRR repaint driver): 0003/0039 .68
	guest/kwin/6.7.91) echo "${VGD_GUEST_KWIN_SET:-g21imc12} ${VGD_GUEST_KWIN_SET:-g21imc12}" ;;   # g21imc12 (2026-10-24) = g21imc11 + 0073 (KWIN_WAYLAND_SCANOUT_BUFFERS: the nested output's buffers made for scanout - an Intel xe host scans only those out; setup's customize.sh sets the variable for GUEST_MESA_INTEL=1 guests since the same day - before, setup's guests had neither). g21imc11 (2026-10-20) = g21imc7 + 0068 (upstream e3f5caef: pointer constraints without a commit). Before:   # g21i + 0060 v2/0061/0062/0063 (2026-10-18: drags, cursor-plane tests and pointer motion to clients bounded; 0062 acts in main-line guests only) + 0064 (wl_output follows the host refresh learned from presentation feedback)
	guest/kwin/6.8.0) echo "g21j73 g23 0033" ;;   # g21j73 (2026-10-24) = g21j + 0073 (applies, checked on Plasma/6.8).   # lean (g21b + 0051): without the Overview experiments 0047/0048/0050
	guest/kwin/6.8.*) echo "g21j73-681 g23 0033" ;;   # g21j73-681 (2026-10-24) = g21j-681 + 0073.   # 6.8.1 on: 0003/0035/0042 in their .68 versions; 2026-10-25: 0024/0028/0037 too (the 6.8.1 release: its cursor timer and timeline changes; checked at fuzz 0 on the tarball)
	guest/kpipewire/6.7.91 | guest/kpipewire/6.8.*) echo "k2 k2" ;;
	guest/qt6-qtbase/6.11.2) echo "${VGD_GUEST_QT_SET:-q4} ${VGD_GUEST_QT_SET:-q4}" ;;   # q7 = q4 + pointer motion at most once per ms (2026-10-17)
	guest/kf6-ksvg/6.31.0) echo "${VGD_GUEST_KSVG_SET:-k2} ${VGD_GUEST_KSVG_SET:-k2}" ;;   # k2 = k1 with the kept copies bounded (LRU, as KImageCache keeps at 100 %); k1 kept up to 1024 of any size
	guest/kf6-kirigami/6.31.0) echo "f1 f1" ;;
	host/kf6-ksvg/6.31.0) echo "${VGD_HOST_KSVG_SET:-k2} ${VGD_HOST_KSVG_SET:-k2}" ;;
	host/kf6-kirigami/6.31.0) echo "f1 f1" ;;
	host/qt6-qtbase/6.11.2) echo "${VGD_HOST_QT_SET:-q4} ${VGD_HOST_QT_SET:-q4}" ;;   # as the guests' (2026-10-24)
	guest/plasma-workspace/6.7.* | guest/plasma-workspace/6.8.*) echo "${VGD_GUEST_PW_SET:-w1} ${VGD_GUEST_PW_SET:-w1}" ;;   # 6.7.5: Fedora 44's own Plasma (vitrine's guests); w2 = w1 + every 8 ms
	*) return 1 ;;
	esac
}
