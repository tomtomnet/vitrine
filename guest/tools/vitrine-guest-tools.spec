Name:           vitrine-guest-tools
Version:        0.1.0
Release:        1%{?dist}
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
install -d %{buildroot}%{_sharedstatedir}/%{name}
# written by the installer: blob_flush_fence=3 with vitrine's KWin
install -d %{buildroot}%{_sysconfdir}/modprobe.d
touch %{buildroot}%{_sysconfdir}/modprobe.d/vitrine-virtio-gpu.conf

%post
%systemd_post vitrine-agent.service vitrine-mtrr-hostmem-wb.service vitrine-virtio-gpu-check.service vitrine-preempt-full.service
%udev_rules_update

%preun
%systemd_preun vitrine-agent.service vitrine-mtrr-hostmem-wb.service vitrine-virtio-gpu-check.service vitrine-preempt-full.service
# this version's driver out of every kernel (an update builds its own after)
dkms remove -m %{dkms_name} -v %{version} --all > /dev/null 2>&1 || :

%postun
# no restart on update: the agent runs the installer that updates it, and
# takes the new version over by itself once done
%systemd_postun vitrine-agent.service vitrine-mtrr-hostmem-wb.service vitrine-virtio-gpu-check.service vitrine-preempt-full.service
%udev_rules_update
if [ $1 -eq 0 ]; then
	# the stock driver, without the options, from every kernel's initramfs
	dracut -f --regenerate-all > /dev/null 2>&1 || :
fi

%posttrans
# the driver for every kernel with headers, now that the whole transaction
# (kernel-devel included) is in; the installer makes the initramfs itself
if [ -e /run/vitrine-guest-tools/installer-active ]; then
	%{libexec}/dkms-sync --no-initramfs || :
else
	%{libexec}/dkms-sync --all-initramfs || :
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
%{_unitdir}/vitrine-preempt-full.service
%{_presetdir}/80-vitrine-guest-tools.preset
%{libexec}
%dir %{_sharedstatedir}/%{name}

%changelog
* Sat Oct 03 2026 vitrine <noreply@anthropic.com> - 0.1.0-1
- First version: the research guest setup as a package
