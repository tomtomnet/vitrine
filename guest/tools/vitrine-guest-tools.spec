# Bump Release for any change in guest/tools: the installer installs this
# exact version, and dnf takes an installed one of the same version as done.
Name:           vitrine-guest-tools
Version:        0.1.0
Release:        41%{?dist}
Summary:        vitrine guest tools: patched virtio-gpu driver, settings and agent

# the driver's sources (dkms/vendor, dkms/patches) are the kernel's: MIT
License:        GPL-2.0-or-later AND MIT
URL:            https://github.com/tomtomnet/vitrine
Source0:        %{name}-%{version}.tar.gz
BuildArch:      noarch

BuildRequires:  systemd-rpm-macros
# the driver, rebuilt for every kernel (kernel-devel comes with dkms)
Requires:       dkms
Requires:       curl
Requires:       patch
Requires:       dracut
Requires:       kmod
# the agent
Requires:       python3
Requires:       util-linux
# the nested desktop's session: the host's screens through its proxy
Requires:       wayland-utils
# the clipboard shared with the host: spice-vdagent talks to QEMU's
# qemu-vdagent, and the bridge (clipboard/) passes Wayland copies on to it
Requires:       spice-vdagent
Requires:       wl-clipboard
Requires:       xclip
Requires:       libX11
Requires:       libXfixes
%{?systemd_requires}

%global dkms_name vitrine-virtio-gpu
%global libexec %{_libexecdir}/%{name}

%description
What a Fedora guest needs to run well in vitrine: the virtio-gpu driver with
vitrine's patches (host vblank, zero-copy display) as a DKMS module, its
module options, the KWin settings for vitrine's KWin, a fix-up that makes the
GPU's shared memory cacheable, full kernel preemption, a boot-time check of
the driver and of the host's GPU behind the native context (on an Intel xe
host it gives the guest's own sessions KWin's shorter lone-frame lead), the
nested desktop's session (a boot with the VM's Nested desktop box logs
Plasma into a window of the host's desktop, with KWin's settings for the
host's GPU), and the vitrine agent, which tells vitrine the state of the
guest tools and installs their updates.  vitrine's Mesa, KWin with the
KWayland it is built against, KPipeWire, the nested desktop's proxy, Qt Base,
KSvg, Kirigami and Mutter come from the same tools medium.

%prep
%autosetup

%build

%install
# the driver's DKMS tree
install -d %{buildroot}%{_usrsrc}/%{dkms_name}-%{version}
cp -r dkms/patches dkms/vendor %{buildroot}%{_usrsrc}/%{dkms_name}-%{version}/
install -m 0755 dkms/prepare.sh %{buildroot}%{_usrsrc}/%{dkms_name}-%{version}/
sed 's/@VERSION@/%{version}/' dkms/dkms.conf > %{buildroot}%{_usrsrc}/%{dkms_name}-%{version}/dkms.conf
install -Dm 0644 dkms/framework.conf.d/vitrine-guest-tools.conf %{buildroot}%{_sysconfdir}/dkms/framework.conf.d/vitrine-guest-tools.conf
install -Dm 0644 depmod.d/vitrine-virtio-gpu.conf %{buildroot}%{_prefix}/lib/depmod.d/vitrine-virtio-gpu.conf
install -Dm 0644 modprobe.d/vitrine-virtio-gpu.conf %{buildroot}%{_prefix}/lib/modprobe.d/vitrine-virtio-gpu.conf
install -Dm 0644 environment.d/60-vitrine-kwin.conf %{buildroot}%{_prefix}/lib/environment.d/60-vitrine-kwin.conf
install -Dm 0644 udev/90-vitrine-agent.rules %{buildroot}%{_udevrulesdir}/90-vitrine-agent.rules
for f in systemd/*.service; do
	sed 's/@VERSION@/%{version}/' "$f" > tmp && install -Dm 0644 tmp %{buildroot}%{_unitdir}/$(basename "$f")
done
install -Dm 0644 systemd/80-vitrine-guest-tools.preset %{buildroot}%{_presetdir}/80-vitrine-guest-tools.preset
# the nested desktop (nested/: the research's session, nested/README.md): the
# session, its settings, its entry for the login screen (found only in a
# nested-desktop boot, vitrine-nested-boot.service), plasmalogin's and SDDM's
# side of it, and no spice-vdagent clipboard beside the proxy's
install -d %{buildroot}%{libexec}/nested/bin
install -m 0755 nested/session %{buildroot}%{libexec}/nested/
install -m 0755 nested/kwin_wayland_wrapper %{buildroot}%{libexec}/nested/bin/
install -m 0644 nested/stats-overlay.qml %{buildroot}%{libexec}/nested/
install -Dm 0644 nested/plasma.desktop %{buildroot}%{_datadir}/%{name}/wayland-sessions/plasma.desktop
install -Dm 0644 nested/nested.conf %{buildroot}%{_sysconfdir}/%{name}/nested.conf
install -Dm 0644 nested/README.md %{buildroot}%{_docdir}/%{name}/nested.md
install -Dm 0644 systemd/plasmalogin.service.d/vitrine-nested.conf %{buildroot}%{_unitdir}/plasmalogin.service.d/vitrine-nested.conf
for u in spice-vdagent.service 'app-vitrine\x2dclipboard\x2dbridge@autostart.service'; do
	install -Dm 0644 systemd/user/vitrine-nested-no-vdagent.conf "%{buildroot}%{_userunitdir}/$u.d/vitrine-nested.conf"
done
install -Dm 0644 systemd/user/vitrine-nested-greeter.conf %{buildroot}%{_userunitdir}/plasma-login-kwin_wayland.service.d/vitrine-nested.conf
install -d %{buildroot}%{_prefix}/lib/sddm/sddm.conf.d
ln -s /run/vitrine-nested/sddm.conf %{buildroot}%{_prefix}/lib/sddm/sddm.conf.d/vitrine-nested.conf
install -d %{buildroot}%{libexec}
for f in libexec/* agent/vitrine-agent install; do
	sed 's/@VERSION@/%{version}/' "$f" > tmp && install -m 0755 tmp %{buildroot}%{libexec}/$(basename "$f")
done
# spice-vdagent only sees the X11 clipboard, and KWin passes Wayland copies
# on to X11 only for a focused X11 window: the bridge, from the qemu-gui
# fork (contrib/vdagent-clipboard-bridge), copies them, in each session
install -m 0755 clipboard/vdagent-clipboard-bridge %{buildroot}%{libexec}/vdagent-clipboard-bridge
install -Dm 0644 clipboard/vitrine-clipboard-bridge.desktop %{buildroot}%{_sysconfdir}/xdg/autostart/vitrine-clipboard-bridge.desktop
install -d %{buildroot}%{_sharedstatedir}/%{name}
# written by the installer: blob_flush_fence=3 with vitrine's KWin
install -d %{buildroot}%{_sysconfdir}/modprobe.d
touch %{buildroot}%{_sysconfdir}/modprobe.d/vitrine-virtio-gpu.conf

%global units vitrine-agent.service vitrine-mtrr-hostmem-wb.service vitrine-virtio-gpu-check.service vitrine-kernel-settings.service vitrine-nested-boot.service

%post
%systemd_post %{units}
# %%systemd_post applies the presets on the first install only: a unit an
# update brings would stay off
if [ $1 -gt 1 ]; then
	systemctl --no-reload preset %{units} > /dev/null 2>&1 || :
	# new driver sources: %%posttrans builds them where the driver is built
	mkdir -p /run/vitrine-guest-tools && touch /run/vitrine-guest-tools/rebuild-driver
fi
%udev_rules_update

%preun
%systemd_preun %{units}
# removed: the driver out of every kernel, all its versions; on an update
# the new package's %%posttrans builds over it, keeping it if that fails
if [ $1 -eq 0 ]; then
	# the initramfs once, in %%postun, not after each version (dkms's
	# post-transaction command, which the package's dkms config skips)
	for v in $(dkms status -m %{dkms_name} 2> /dev/null | sed -n 's|^%{dkms_name}/\([^,:]*\)[,:].*|\1|p' | sort -u); do
		VITRINE_DKMS_NO_POST_TRANSACTION=1 dkms remove -m %{dkms_name} -v "$v" --all > /dev/null 2>&1 || :
	done
fi

%postun
# no restart on update: the agent runs the installer that updates it, and
# takes the new version over by itself once done
%systemd_postun %{units}
%udev_rules_update
if [ $1 -eq 0 ]; then
	# the stock driver, without the options, from every kernel's initramfs
	dracut -f --regenerate-all > /dev/null 2>&1 || :
fi

%posttrans
# the driver for every kernel with headers, now that the whole transaction
# (kernel-devel included) is in.  Under the installer, its own step builds
# it, which says so kernel by kernel: here, in dnf's transaction, the
# builds showed nothing but dnf's "Upgrading" line (2026-10-30).
# The rebuild marker stays for that step.
if [ ! -e /run/vitrine-guest-tools/installer-active ]; then
	rebuild=
	[ -e /run/vitrine-guest-tools/rebuild-driver ] && rebuild=--rebuild
	rm -f /run/vitrine-guest-tools/rebuild-driver
	%{libexec}/dkms-sync $rebuild --all-initramfs || :
fi

%files
%{_usrsrc}/%{dkms_name}-%{version}
%config(noreplace) %{_sysconfdir}/dkms/framework.conf.d/vitrine-guest-tools.conf
%{_prefix}/lib/depmod.d/vitrine-virtio-gpu.conf
%{_prefix}/lib/modprobe.d/vitrine-virtio-gpu.conf
%ghost %config(noreplace) %{_sysconfdir}/modprobe.d/vitrine-virtio-gpu.conf
%{_prefix}/lib/environment.d/60-vitrine-kwin.conf
%{_udevrulesdir}/90-vitrine-agent.rules
%{_unitdir}/vitrine-agent.service
%{_unitdir}/vitrine-mtrr-hostmem-wb.service
%{_unitdir}/vitrine-virtio-gpu-check.service
%{_unitdir}/vitrine-kernel-settings.service
%{_unitdir}/vitrine-nested-boot.service
%dir %{_unitdir}/plasmalogin.service.d
%{_unitdir}/plasmalogin.service.d/vitrine-nested.conf
%dir %{_userunitdir}/spice-vdagent.service.d
%{_userunitdir}/spice-vdagent.service.d/vitrine-nested.conf
%dir %{_userunitdir}/app-vitrine*clipboard*bridge@autostart.service.d
%{_userunitdir}/app-vitrine*clipboard*bridge@autostart.service.d/vitrine-nested.conf
%dir %{_userunitdir}/plasma-login-kwin_wayland.service.d
%{_userunitdir}/plasma-login-kwin_wayland.service.d/vitrine-nested.conf
%dir %{_prefix}/lib/sddm
%dir %{_prefix}/lib/sddm/sddm.conf.d
%{_prefix}/lib/sddm/sddm.conf.d/vitrine-nested.conf
%{_datadir}/%{name}
%dir %{_sysconfdir}/%{name}
%config(noreplace) %{_sysconfdir}/%{name}/nested.conf
%doc %{_docdir}/%{name}/nested.md
%{_presetdir}/80-vitrine-guest-tools.preset
%{_sysconfdir}/xdg/autostart/vitrine-clipboard-bridge.desktop
%{libexec}
%dir %{_sharedstatedir}/%{name}

%changelog
* Mon Nov 09 2026 vitrine <noreply@anthropic.com> - 0.1.0-41
- with Mutter 51.1-1.1.m1 on Fedora 45's medium, the research's set m1
  (83db562): a new frame clock seeds its update-duration estimates with
  at most one refresh, not with its first frame, often a mode set's.  After
  a full-screen switch the guest's compositor no longer runs up to three
  refreshes ahead for seconds of motion (the window after the switch, on
  the research's Radeon 780M laptop: 0.21-0.31 stutters a second under a
  compile, against 2.37-2.41)
- the installer puts it in place of the guest's Mutter where the guest has
  51.1 and no newer build, every installed Mutter package in the same
  transaction, and does not lock it: Fedora updates Mutter together with
  GNOME Shell, and a lock would keep Mutter and its fixes at 51.1 while
  the Shell moved on, a pair GNOME does not release.  Fedora's next Mutter
  takes its place until the next guest tools; --uninstall brings Fedora's
  back
- the agent reports the installed mutter, and whether it is vitrine's
  build

* Sun Nov 08 2026 vitrine <noreply@anthropic.com> - 0.1.0-40
- the driver's flip grace holds only for a tick that has no flip yet (the
  research's a5e7488): a flip committed in the grace of a tick that had
  one already was completed for that same tick, so the compositor drew its
  next frame at once and the host showed only one of the two.  A GNOME
  guest in vitrine's window fell into spells of half rate for seconds at a
  time (the research's gdl13, gdl17/18: in those spells 46 % of its
  flushes fell in the grace, 6 % outside them).  Such a flip now waits for
  the next tick: module parameter flip_grace_once, default 1 (0: the grace
  for any flip in it, as before); flip_grace_us stays 1200
- debugfs virtio-gpu-flip-gate counts the flips completed in a grace and
  the second flips in the grace of a tick that had one.  On the research's
  laptop (Radeon 780M, GNOME host and guest, 6 boots) the fixed driver
  sent every second flip to the next tick (0, 81 and 26 in its boots; the
  old one completed 13, 1 and 1,252 for the same tick), at no cost seen

* Sun Nov 08 2026 vitrine <noreply@anthropic.com> - 0.1.0-39
- with KWin 6.8.2-3.3.g28 on Fedora 44's medium (the research's guest set
  g28: the nested desktop binds the host's seat at version 9 and hands a
  scroll's source, wheel steps and direction on, so that a high-resolution
  wheel scrolls smoothly on a KDE host) and KWayland 6.8.2-1.1.w1, which
  that KWin is built against and does not start without
- the installer puts vitrine's KWayland in with vitrine's KWin, every
  installed package of it, in the same transaction, and locks it with that
  KWin; a guest with 32-bit KWayland keeps its KWin, and says why.
  --no-kwin keeps the guest's KWayland, --uninstall brings Fedora's back,
  and an earlier tools' KWayland that no longer matches the guest's Plasma
  goes back to Fedora's with their KWin
- the agent reports the installed kwayland too
- the nested desktop's notes name the guest set g28

* Sat Nov 07 2026 vitrine <noreply@anthropic.com> - 0.1.0-38
- on an Intel xe host, KWin's lone-frame lead goes to 1000 µs instead of
  1500 in the nested desktop, and to 1000 µs in a KDE guest's own
  session, where it was unset (the research's e152b8a and ef7974d, on its
  Arc Pro B50 under KDE: the nested desktop's keys 7.8-10.2 -> 7.4-7.6 ms,
  bare KDE's 7.6-7.7, the launcher's start still without a repeated
  refresh; the guest's own session in QEMU's window 11.01 -> 10.29 ms,
  where its KWin held a key's frame up to ~2 ms for its own vblank).
  Nothing changes on other hosts
- the nested desktop: a 1500 in nested.conf's CROSVM_DESKTOP_KWIN_ENV, the
  xe value its notes gave until now, is replaced too, and the session's
  journal says so
- the guest's own session: the boot check writes
  /run/environment.d/45-vitrine-lone-frame.conf at each boot, from the
  native context alone, before any user's systemd starts; a file of the
  same name in /etc/environment.d turns it off (nested.md: "KWin's
  settings on Intel xe hosts")
- the boot check gives up after a minute: now that user logins wait for it,
  a hung check would keep them all out, ssh's too.  Stopped, it leaves its
  files in /run whole or absent

* Thu Nov 05 2026 vitrine <noreply@anthropic.com> - 0.1.0-37
- on an Intel host the VM's screen can show the guest's own buffers
  instead of copying each frame (the research's 4ba00a2 and 420b32d, 7.2
  and 7.3 versions): the driver tells the host each scanout's layout, its
  format modifier in SET_SCANOUT_BLOB's padding word (scanout_modifier, on
  by default), as an Intel GPU keeps none in the buffer, and the primary
  planes' tiled_scanout list offers Intel's Tile4 too, before linear
  (intel_scanout=1, set by modprobe.d; with tiled_scanout=0 it changes
  nothing: alone it made an AMD guest's buffers linear, where planes
  without a list leave them AMD-tiled).  Both only when the host offers
  VIRTIO_GPU_F_SCANOUT_MODIFIER_VGD (bit 23: vitrine's QEMU with
  virtio-gpu-scanout-modifier; the probe line says "+scanout_modifier"):
  under another QEMU nothing changes.  AMD's tiled layouts do not fit the
  word and are not sent; the host reads them from the buffer as before

* Thu Nov 05 2026 vitrine <noreply@anthropic.com> - 0.1.0-36
- the nested desktop's statistics go to the guest's journal by default
  (CROSVM_DESKTOP_STATS=journal in nested.conf; the research's 5eee06a
  and fd4db41, with wl-cross-domain-proxy at fd4db41 on Fedora 44's
  medium): every minute with frames, the frames presented and dropped
  and the latencies (journalctl --user -t crosvm-desktop-proxy), no
  numbers in the window title.  Up to 0.1.0-35 they were off, and
  nothing showed, for weeks, that a GNOME host discarded every frame's
  feedback

* Thu Nov 05 2026 vitrine <noreply@anthropic.com> - 0.1.0-35
- with KWin 6.8.2-3.2.g27 and wl-cross-domain-proxy at the research's
  fe4eacc on Fedora 44's medium: on a GNOME host the nested desktop's KWin
  learns again when the host shows its frames (it asked for its frame
  callbacks and presentation feedback on a surface GNOME treats as
  covered, and got none), and the proxy's statistics count that feedback.
  The tools' new version lets vitrine see the update for a guest that is
  not running, and name it

* Thu Nov 05 2026 vitrine <noreply@anthropic.com> - 0.1.0-34
- with KWin 6.8.2-3.1.g26 on Fedora 44's medium (the research's guest set g26
  on Fedora's 6.8.2-3): the nested desktop's cursor reaches the host as shared
  memory, which a GNOME host puts on its cursor plane, keeping the full-screen
  VM scanned out; the Overview keeps its views between openings; the GPU's
  clock is read once a second. The tools' new version lets vitrine see the
  update for a guest that is not running, and name it

* Thu Nov 05 2026 vitrine <noreply@anthropic.com> - 0.1.0-33
- the nested desktop's KWin runs without an input method (the research's
  97e4f0d): Fedora's kde-settings make plasma-keyboard every KDE session's
  input method, and with it the nested KWin passed a batch's keys to the
  app before their modifiers, which came back from the input method a round
  trip later: Shift and a letter sent together came out unshifted ("Ab!"
  typed as "ab1"). An input method set in ~/.config/kwinrc is kept;
  CROSVM_DESKTOP_INPUT_METHOD=1 in nested.conf keeps the system's

* Wed Nov 04 2026 vitrine <noreply@anthropic.com> - 0.1.0-32
- built for each Fedora release the tools are made for, 44 and 45, on a
  medium of its own; Fedora 45's has the tools and Mesa, no KDE packages yet
- the installer read rpm -q's "package ... is not installed", which it prints
  on its standard output, as a version (since 0.1.0-29): a guest without Qt
  Base, KSvg or Kirigami was told "the installed one stays", or "vitrine's
  ..., 0 packages" by a medium that has them (nothing went in wrongly); and
  as the Plasma version of a guest without Plasma
- on a guest without Plasma (GNOME), no word of the nested desktop's proxy;
  "Mesa: none on this medium" for a medium without Mesa, not "none
  installed (no desktop?)"

* Tue Nov 03 2026 vitrine <noreply@anthropic.com> - 0.1.0-31
- vitrine's KWin and KPipeWire for Fedora 44's Plasma 6.8.2 (the research's
  sets g24 and k2), in place of 6.7.5's
- a guest with Fedora's older Plasma (6.7.5, which tools 0.1.0-30 held)
  moves to Fedora's Plasma 6.8.2 in the transaction that installs them, where
  Fedora's repositories still have it: Plasma with a KWin of another version
  has no session at all; else the installed KWin stays, as before
- on a guest whose update already brought Plasma 6.8.2 around vitrine's KWin
  6.7.5, what it left at 6.7.5 (xdg-desktop-portal-kde, which waits for
  KPipeWire 6.8.2) comes in the same transaction as KWin and KPipeWire

* Sun Nov 01 2026 vitrine <noreply@anthropic.com> - 0.1.0-30
- with vitrine's KWin, the installer keeps Fedora's Plasma at the version
  that KWin is built for (dnf versionlock on the packages installed at that
  version): Fedora 44's Plasma 6.8.2 requires an unversioned kwin, and dnf
  updated the rest of Plasma around the locked KWin 6.7.5
- where vitrine's KWin or KPipeWire of earlier tools no longer matches the
  guest's Plasma (updated before these tools), Fedora's go back in (dnf
  distro-sync), until guest tools built for that Plasma

* Sat Oct 31 2026 vitrine <noreply@anthropic.com> - 0.1.0-29
- the installer puts vitrine's Qt Base (the research's q4: Wayland windows
  being resized reuse their shared memory), KSvg (k2) and Kirigami (f1:
  Plasma's popups no longer upload their frames again at each opening) in
  place of the guest's, each where the guest has the version it is built
  from and no newer build, all of its installed packages in one go (none if
  one of them has no build on the medium, a 32-bit one say)
- Qt Base is version-locked with vitrine's KWin, which is built against its
  private API; KSvg, Kirigami, and Qt Base without vitrine's KWin, are not:
  Fedora's next build of them takes their place, consistent with the rest of
  KDE's frameworks or of Qt, until the next guest tools
- --uninstall puts Fedora's builds back by the exact names of the installed
  packages of each source package (mesa, kwin, kpipewire, qt6-qtbase,
  kf6-ksvg, kf6-kirigami): the globs mesa-*, kwin* and kpipewire* took other
  packages along (kwin-x11, mesa-libGLU)
- the agent also reports Qt Base, KSvg and Kirigami, and whether each is
  vitrine's build

* Fri Oct 30 2026 vitrine <noreply@anthropic.com> - 0.1.0-28
- the tools' own dkms runs skip dkms's post-transaction command (a dkms
  config of the package): on Fedora it made every kernel's initramfs again
  after each install and uninstall of the driver, some twenty with three
  kernels, minutes; the initramfs are made once, after all the builds
- under the installer, the package leaves the driver's build to the
  installer's step "Building the graphics driver", which names each kernel
  as it builds: in dnf's transaction the builds showed nothing but dnf's
  "Upgrading vitrine-guest-tools" line
- the installer makes the initramfs kernel by kernel, naming each, and also
  when only the driver's files changed

* Fri Oct 30 2026 vitrine <noreply@anthropic.com> - 0.1.0-27
- the installer locks vitrine's builds again when it finds the medium's
  packages installed and has nothing else to do: locks lost some other way
  (an older installer's failure, dnf remove --duplicates, a hand edit) stayed
  lost at every retry

* Fri Oct 30 2026 vitrine <noreply@anthropic.com> - 0.1.0-26
- the installer locks vitrine's Mesa, KWin, KPipeWire and proxy right after
  installing them, before the driver and the initramfs: an install that
  failed at dracut left them unlocked, and the next dnf upgrade offered
  Fedora's Mesa in their place.  A run that ends before that (dnf failed,
  or it was stopped) puts the locks back as they were
- it stops at once, with the commands to fix it, on a guest with packages
  installed twice (an update cut short), where it failed half way through
- dracut's own last line in its error, as "No space left on device"

* Fri Oct 30 2026 vitrine <noreply@anthropic.com> - 0.1.0-25
- the nested desktop's session puts its VT in VT_AUTO and KD_TEXT at its
  start: the login manager left it in VT_PROCESS with the session's script
  to answer a switch, which nothing did, so a switch to another VT
  (Ctrl+Alt+F3 from vitrine's Send Key menu, chvt) ended the session.  The
  kernel now switches by itself, and the session runs on in the
  background, as any session switched away from; the VT's cursor is hidden,
  which the framebuffer console would blink 5 times a second once the VT
  has been switched away from and back

* Tue Oct 27 2026 vitrine <noreply@anthropic.com> - 0.1.0-24
- the driver: a DIRTYFB without clips, which means the whole framebuffer,
  is flushed as one full-size clip instead of being skipped as a commit
  that changes nothing (the research's skip-unchanged-flush patch)

* Mon Oct 26 2026 vitrine <noreply@anthropic.com> - 0.1.0-23
- the driver: a deferred blob map that cannot be queued (no room left in
  the host-visible window) fails the mmap instead of hanging the app in
  uninterruptible sleep; vitrine's Mesa 2.xe defers the map of every xe
  buffer to its first mmap (the research's patch)
- the driver on Linux 7.3: the research's 7.3 versions of host-vblank,
  linear-scanout and tiled-scanout.  Of a patch's versions the first that
  applies without fuzz is taken; when none does, the build stops and says
  which patch needs a rebase.  host-blob-damage is left out where the
  kernel has its change (7.4).  tiled-scanout offers GFX10.3's layout too
  (the research's, for a host with an RX 6000 card)
- the host's GPU, as the virtio-gpu native context reports it (xe, amdgpu,
  ...), asked at boot by the driver check, its finding in that unit's
  journal and in /run/vitrine-guest-tools/host-gpu; the agent reports it
- the nested desktop on an Intel xe host: its KWin gets
  KWIN_DISABLE_UDMABUF_IMPORT=1, KWIN_WAYLAND_SCANOUT_BUFFERS=1 and the
  lone-frame lead at 1500 us instead of 2500 (the research's settings for
  its Arc Pro B50); CROSVM_DESKTOP_HOST_GPU in nested.conf names the GPU
  instead.  The session's journal says what it found and set

* Thu Oct 15 2026 vitrine <noreply@anthropic.com> - 0.1.0-22
- the nested session also ends at once when the host's display hangs up
  without a word (vitrine not running, with vitrine's QEMU of 2026-10-15):
  no more KWin restarts and crashing clients before the login screen

* Thu Oct 15 2026 vitrine <noreply@anthropic.com> - 0.1.0-21
- the nested session ends at once when the host's display does not answer
  (vitrine not running): it no longer hangs without a window, the guest goes
  back to its login screen

* Wed Oct 14 2026 vitrine <noreply@anthropic.com> - 0.1.0-20
- the agent tells what the VM's own screen (the virtio card) shows: a login
  screen, the text console, or neither (protocol 3), in its status and as it
  changes; vitrine's console shows the nested desktop's Screen view by it,
  not as soon as an agent answers (before the login screen, during the
  install at boot)

* Wed Oct 14 2026 vitrine <noreply@anthropic.com> - 0.1.0-19
- plasmalogin's XDG_DATA_DIRS in a nested-desktop boot: the session entry's
  folder in front of the service manager's, which keeps flatpak's exports
  (the research's finding)

* Wed Oct 14 2026 vitrine <noreply@anthropic.com> - 0.1.0-18
- in a nested-desktop boot the login screen draws on the VM's own screen
  only (plasmalogin's KWin with KWIN_DRM_DEVICES): it drew on QEMU's VGA too,
  and put its login form there, on the Boot view, while vitrine showed the
  Screen view after a logout

* Wed Oct 14 2026 vitrine <noreply@anthropic.com> - 0.1.0-17
- the nested desktop's boot script runs: an apostrophe in a message broke it
  (bash took it for a quote); the agent finds the proxy's process

* Wed Oct 14 2026 vitrine <noreply@anthropic.com> - 0.1.0-16
- the nested desktop: the research's session (its KWin nested on
  wl-cross-domain-proxy), offered as "Plasma (nested desktop)" in place of
  Plasma by plasmalogin and SDDM in the boots vitrine marks as nested (fw_cfg
  crosvm_desktop=1, vitrine-nested-boot.service), where the guest's own
  automatic login into Plasma logs into it; none of it in other boots
- the driver's hide_virgl (the research's patch), set in nested-desktop
  boots with blob_flush_fence 0; no spice-vdagent clipboard there (the proxy
  shares it)
- the agent reports hide_virgl and the nested desktop's state

* Mon Oct 05 2026 vitrine <noreply@anthropic.com> - 0.1.0-15
- the agent answers stats (protocol 2): the guest's network cards' byte
  counters and its memory, for vitrine's status bar; QEMU counts no network
  traffic when passt moves the packets
* Sun Oct 04 2026 vitrine <noreply@anthropic.com> - 0.1.0-14
- the driver on Linux 7.2.9 and 7.3: they refuse again the import of other
  devices' dma-bufs on a 3D device, which native contexts need (KWin's zero
  copy of wl_shm buffers); a patch allows it again where the host offers
  native contexts (parameter import_3d), applied to sources with that revert
- the driver's patches apply without fuzz, and what kernel.org sends must be
  kernel sources: a page answered with status 200 (a bot check) is refused,
  and a cache holding one is fetched again
- the boot check and the agent recognize the driver by its own parameter,
  not only by the out-of-tree taint
* Sun Oct 04 2026 vitrine <noreply@anthropic.com> - 0.1.0-13
- the driver's sources fetched from kernel.org with retries (three more
  tries, 2 s apart, on any error), and a failed fetch says which file and
  why
* Sun Oct 04 2026 vitrine <noreply@anthropic.com> - 0.1.0-12
- clipboard sharing with the host: spice-vdagent, and the bridge that passes
  Wayland copies on to its X11 clipboard (from the qemu-gui fork), started
  with each desktop session but GNOME's
* Sat Oct 03 2026 vitrine <noreply@anthropic.com> - 0.1.0-11
- the installer's error messages without their exit code
* Sat Oct 03 2026 vitrine <noreply@anthropic.com> - 0.1.0-10
- Secure Boot read from EFI's global variable (its GUID was wrong: the
  installer did not refuse a Secure Boot guest), mokutil as a fallback;
  the driver check also when no virtio-gpu driver loaded at all (the
  kernel refused the unsigned one); the agent says whether the VM has a
  virtio GPU
* Sat Oct 03 2026 vitrine <noreply@anthropic.com> - 0.1.0-9
- the agent finds the virtio-gpu render node under a PCI device too (it
  reported no capability sets)
* Sat Oct 03 2026 vitrine <noreply@anthropic.com> - 0.1.0-8
- blob_flush_fence set at boot by the driver check: dracut does not copy
  /etc/modprobe.d into the initramfs, where the driver loads
* Sat Oct 03 2026 vitrine <noreply@anthropic.com> - 0.1.0-7
- the newest kernel's headers by name, in a step of their own (dnf took
  kernel-devel as installed with an older version); the agent reports the
  state of the install vitrine asked for at boot
* Sat Oct 03 2026 vitrine <noreply@anthropic.com> - 0.1.0-6
- the installer brings the newest kernel's headers (a guest with an older
  kernel-devel-matched got the new kernel without them), says when the
  kernel booted next has no driver, and leaves Mesa alone on a guest with
  32-bit Mesa (dnf wants both architectures at one version)
* Sat Oct 03 2026 vitrine <noreply@anthropic.com> - 0.1.0-5
- the driver's source cache readable by all, as the other caches
* Sat Oct 03 2026 vitrine <noreply@anthropic.com> - 0.1.0-4
- an update builds the driver over the one in place and keeps it if the
  build fails, instead of removing it first; the source cache is written
  atomically and checked before use (a guest killed after writing it kept
  empty files)
* Sat Oct 03 2026 vitrine <noreply@anthropic.com> - 0.1.0-3
- the installer: a repository id per medium (dnf kept the metadata of the
  previous one), its last result in /var/lib/vitrine-guest-tools for the agent
* Sat Oct 03 2026 vitrine <noreply@anthropic.com> - 0.1.0-2
- drm.vblankoffdelay=0 at run time (drm is built in), with full preemption
  in vitrine-kernel-settings.service; the agent's shutdown command
* Sat Oct 03 2026 vitrine <noreply@anthropic.com> - 0.1.0-1
- First version: the research guest setup as a package
