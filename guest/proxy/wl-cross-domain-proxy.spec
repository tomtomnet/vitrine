# Upstream's commit c6ce1ca89fb4, where the research's work starts, with that
# work as one patch, taken at the research's commit a56b97b (the patch's
# description has what it adds).  Literal, without macros: vitrine's Builds
# window reads Version and Source0 as they are written.  The date the patch
# was taken comes first, so that each version sorts after the one before
# (rpm compares commit hashes as words: rb973b6b sorts after r8ecffaf), and
# a counter after it for another one on the same day: 20261015.1 sorts after
# 20261015, .2 after .1, the next day's date after both.
Name:           wl-cross-domain-proxy
Version:        0.1.0^20261031.3gitc6ce1ca.ra56b97b
Release:        1%{?dist}
Summary:        Wayland proxy over virtio-gpu cross-domain, for vitrine's nested desktop

# The tree: GPL-2.0-or-later AND MIT - the research's changes GPL-2.0-or-later,
# Victoria Brekenfeld's proxy MIT (her notice in LICENSE); in it, the
# ChromiumOS part (src/cross_domain) BSD-3-Clause.  The 83 crates it is built
# from (Cargo.lock): MIT, or a choice that includes MIT, and unicode-ident's
# Unicode-3.0.
License:        GPL-2.0-or-later AND MIT AND BSD-3-Clause AND Unicode-3.0
URL:            https://codeberg.org/drakulix/wl-cross-domain-proxy
Source0:        https://codeberg.org/drakulix/wl-cross-domain-proxy/archive/c6ce1ca89fb4d6f4f18d3aaf88324d40d4589177.tar.gz#/wl-cross-domain-proxy-c6ce1ca.tar.gz
# the GPL's text, which the tree's README names (the research's LICENSE)
Source1:        GPL-2.0.txt
Patch0:         wl-cross-domain-proxy-research.patch

ExclusiveArch:  x86_64
BuildRequires:  cargo
BuildRequires:  rust
BuildRequires:  gcc

%description
The guest side of vitrine's nested desktop: a Wayland compositor's socket in
the guest whose clients - the nested KWin of the "Plasma (nested desktop)"
session - reach the host's compositor through virtio-gpu's cross-domain
context, their GPU buffers shared without a copy.  Upstream's proxy with the
research's work for a whole desktop: the presentation clock in the guest's
time, fence waits, the pointer grab and shortcut capture, the clipboard,
settled resizes, statistics, the events of a guest client that stops
reading for a while kept until it reads again, and the session's end when
the host's side of the connection goes away, its windows closed first; AMD
DCC and Intel CCS modifiers left out, so that the host can put the
desktop's frames on a plane.

%prep
%autosetup -p1 -n %{name}

%build
# the crates of Cargo.lock, from crates.io (the build's container has the
# network), checked against the lock's sums
export CARGO_HOME=$PWD/.cargo-home
cargo build --release --locked %{?_smp_build_ncpus:-j %{_smp_build_ncpus}}

%install
install -Dm 0755 target/release/%{name} %{buildroot}%{_bindir}/%{name}
install -Dm 0644 src/cross_domain/LICENSE %{buildroot}%{_licensedir}/%{name}/LICENSE.cross_domain
install -Dm 0644 %{SOURCE1} %{buildroot}%{_licensedir}/%{name}/GPL-2.0.txt

%files
%license LICENSE
%license %{_licensedir}/%{name}/LICENSE.cross_domain
%license %{_licensedir}/%{name}/GPL-2.0.txt
%doc README.md
%{_bindir}/%{name}

%changelog
* Sat Oct 31 2026 vitrine <noreply@anthropic.com> - 0.1.0^20261031.3gitc6ce1ca.ra56b97b-1
- the research's a56b97b: the message parser takes a message's file
  descriptors only once all its arguments parse (a message refused
  halfway left the next one on the connection with the wrong ones), and a
  complete message too short for its signature is malformed instead of
  waited for (the client's connection hung)

* Sat Oct 31 2026 vitrine <noreply@anthropic.com> - 0.1.0^20261031.2gitc6ce1ca.rf79a9bc-1
- the research's f79a9bc: the clipboard's connection to the nested
  compositor skips a guest selection already replaced in the same batch
  (two at login, Klipper's: reading the first one's offer, gone by then,
  closed the connection, and the clipboard stayed off for the session),
  and connects again by itself when it is lost

* Sat Oct 31 2026 vitrine <noreply@anthropic.com> - 0.1.0^20261031.1gitc6ce1ca.r4dd61fa-1
- the research's 4dd61fa: colour management creators the host would
  refuse on its own connection are refused on the client's instead:
  create_icc_creator always, the parametric, scRGB and BT.2100 creators
  unless the host announced the feature (a raw client's create_icc_creator
  reached the host and ended the app serving the VM)

* Sat Oct 31 2026 vitrine <noreply@anthropic.com> - 0.1.0^20261031gitc6ce1ca.rac787d1-1
- the research's ac787d1 (its review of 2026-10-31): input the proxy cannot
  carry ends the client that sent it with a protocol error instead of
  panicking the proxy, which ended the guest's session: a dma-buf plane
  that is not a virtio-gpu buffer, an ICC profile's file (the host's ICC
  support is no longer passed on) or any other unhandled file descriptor,
  shm buffers of negative sizes; cross-domain commands out of range are
  dropped; a message that does not fit the socket's buffer no longer
  leaves its file descriptors queued twice

* Fri Oct 30 2026 vitrine <noreply@anthropic.com> - 0.1.0^20261030.1gitc6ce1ca.r0c56aa7-1
- the research's 0c56aa7: a guest client's stall is logged once when it
  begins and once when it ends ("guest client reads again after N s; its
  outgoing buffer peaked at N KiB"), not ~10 times a second while it lasts;
  the buffer's growth was logged where the default filter hid it

* Fri Oct 30 2026 vitrine <noreply@anthropic.com> - 0.1.0^20261030gitc6ce1ca.rcf498ad-1
- the research's work at cf498ad, one patch again (d81b32d's Intel CCS
  change is in it)
- a guest client that stops reading for a few seconds no longer ends the
  proxy: the host's events for it wait in its outgoing buffer, which grows
  from 4 KiB up to 16 MiB instead of failing with E2BIG, and go out as soon
  as its socket takes more.  The nested KWin, busy during a dnf update in
  the guest with a 1000 Hz mouse over its window, filled the 4 KiB: the
  proxy exited, the session ended, and then again at each login
- on a KWin 6.8 host, which applies a pointer constraint at the window's
  next commit, the proxy commits the window after each grab change when the
  guest has nothing pending on it: on an idle desktop Meta + right click
  resized the VM's window, and a released grab kept the pointer in it
- out of full screen (Ctrl+Alt+F) the shortcut capture keeps following the
  pointer, as QEMU's SDL window keeps its grab
- a window without a window geometry (Hyprland nested) takes its size for
  the grab from the host's last configure

* Mon Oct 26 2026 vitrine <noreply@anthropic.com> - 0.1.0^20261026gitc6ce1ca.r18cb8b2.d81b32d-1
- the research's d81b32d: --drop-dcc-modifiers (the nested desktop's
  default) also leaves Intel CCS modifiers out of the dma-buf feedback.  On
  an Intel xe host (Arc Pro B50) the nested KWin's output buffers had a CCS
  modifier, which the display takes only from 64 KiB-aligned buffers: every
  framebuffer failed, and the host composited every frame of the full-screen
  VM instead of putting it on a plane

* Thu Oct 15 2026 vitrine <noreply@anthropic.com> - 0.1.0^20261015.1gitc6ce1ca.r18cb8b2-1
- the research's work at 18cb8b2: when the host end of the connection
  closes (vitrine crashed or was killed), the guest client's windows get
  xdg_toplevel.close first, its requests are dropped, and its connection
  closes once it disconnects or 3 s later; the nested KWin then quits as
  after vitrine's own close, without its wrapper restarting it
- a counter after the date, for a second version on the same day

* Thu Oct 15 2026 vitrine <noreply@anthropic.com> - 0.1.0^20261015gitc6ce1ca.r8ecffaf-1
- the research's work at 8ecffaf: an empty receive (the host end of the
  connection closed: vitrine quit or crashed) closes that guest client
  instead of polling on, so the nested session ends instead of running on
  without a window while QEMU spins
- the date first in the version, which then sorts after the one before

* Wed Oct 14 2026 vitrine <noreply@anthropic.com> - 0.1.0^gitc6ce1ca.rb973b6b-1
- First version: upstream c6ce1ca with the research's work at b973b6b, for
  the nested desktop; the tree GPL-2.0-or-later AND MIT
