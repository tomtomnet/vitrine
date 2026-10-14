# Upstream's commit c6ce1ca89fb4, where the research's work starts, with that
# work as one patch, taken at the research's commit b973b6b (the patch's
# description has what it adds).  Literal, without macros: vitrine's Builds
# window reads Version and Source0 as they are written.
Name:           wl-cross-domain-proxy
Version:        0.1.0^gitc6ce1ca.rb973b6b
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
settled resizes and statistics.

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
* Wed Oct 14 2026 vitrine <noreply@anthropic.com> - 0.1.0^gitc6ce1ca.rb973b6b-1
- First version: upstream c6ce1ca with the research's work at b973b6b, for
  the nested desktop; the tree GPL-2.0-or-later AND MIT
