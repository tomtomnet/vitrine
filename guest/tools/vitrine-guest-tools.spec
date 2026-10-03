# Bump Release for any change in guest/tools: the installer installs this
# exact version, and dnf takes an installed one of the same version as done.
Name:           vitrine-guest-tools
Version:        0.1.0
Release:        12%{?dist}
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
the driver, and the vitrine agent, which tells vitrine the state of the
guest tools and installs their updates.  vitrine's Mesa and KWin come from
the same tools medium.

%prep
%autosetup

%build

%install
# the driver's DKMS tree
install -d %{buildroot}%{_usrsrc}/%{dkms_name}-%{version}
cp -r dkms/patches dkms/vendor %{buildroot}%{_usrsrc}/%{dkms_name}-%{version}/
install -m 0755 dkms/prepare.sh %{buildroot}%{_usrsrc}/%{dkms_name}-%{version}/
sed 's/@VERSION@/%{version}/' dkms/dkms.conf > %{buildroot}%{_usrsrc}/%{dkms_name}-%{version}/dkms.conf
install -Dm 0644 depmod.d/vitrine-virtio-gpu.conf %{buildroot}%{_prefix}/lib/depmod.d/vitrine-virtio-gpu.conf
install -Dm 0644 modprobe.d/vitrine-virtio-gpu.conf %{buildroot}%{_prefix}/lib/modprobe.d/vitrine-virtio-gpu.conf
install -Dm 0644 environment.d/60-vitrine-kwin.conf %{buildroot}%{_prefix}/lib/environment.d/60-vitrine-kwin.conf
install -Dm 0644 udev/90-vitrine-agent.rules %{buildroot}%{_udevrulesdir}/90-vitrine-agent.rules
for f in systemd/*.service; do
	sed 's/@VERSION@/%{version}/' "$f" > tmp && install -Dm 0644 tmp %{buildroot}%{_unitdir}/$(basename "$f")
done
install -Dm 0644 systemd/80-vitrine-guest-tools.preset %{buildroot}%{_presetdir}/80-vitrine-guest-tools.preset
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

%global units vitrine-agent.service vitrine-mtrr-hostmem-wb.service vitrine-virtio-gpu-check.service vitrine-kernel-settings.service

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
	for v in $(dkms status -m %{dkms_name} 2> /dev/null | sed -n 's|^%{dkms_name}/\([^,:]*\)[,:].*|\1|p' | sort -u); do
		dkms remove -m %{dkms_name} -v "$v" --all > /dev/null 2>&1 || :
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
# (kernel-devel included) is in; the installer makes the initramfs itself
rebuild=
[ -e /run/vitrine-guest-tools/rebuild-driver ] && rebuild=--rebuild
rm -f /run/vitrine-guest-tools/rebuild-driver
if [ -e /run/vitrine-guest-tools/installer-active ]; then
	%{libexec}/dkms-sync $rebuild --no-initramfs || :
else
	%{libexec}/dkms-sync $rebuild --all-initramfs || :
fi

%files
%{_usrsrc}/%{dkms_name}-%{version}
%{_prefix}/lib/depmod.d/vitrine-virtio-gpu.conf
%{_prefix}/lib/modprobe.d/vitrine-virtio-gpu.conf
%ghost %config(noreplace) %{_sysconfdir}/modprobe.d/vitrine-virtio-gpu.conf
%{_prefix}/lib/environment.d/60-vitrine-kwin.conf
%{_udevrulesdir}/90-vitrine-agent.rules
%{_unitdir}/vitrine-agent.service
%{_unitdir}/vitrine-mtrr-hostmem-wb.service
%{_unitdir}/vitrine-virtio-gpu-check.service
%{_unitdir}/vitrine-kernel-settings.service
%{_presetdir}/80-vitrine-guest-tools.preset
%{_sysconfdir}/xdg/autostart/vitrine-clipboard-bridge.desktop
%{libexec}
%dir %{_sharedstatedir}/%{name}

%changelog
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
