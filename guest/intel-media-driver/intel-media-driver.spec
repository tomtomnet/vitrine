# Intel's media driver (iHD, VA-API) with cmspam's Xe native context, for guests
# of a host whose GPU is on the Xe driver.  Built as RPM Fusion's
# intel-media-driver is, under its name and in its folder, so that it takes that
# package's place; 64-bit only.
#
# Nothing uses it by itself: libva gives the guest's virtio_gpu Mesa's driver,
# and takes this one only with LIBVA_DRIVER_NAME=iHD (docs/guest-mesa.md).

# the driver gains nothing from it, and its link would take several GiB
%global _lto_cflags %{nil}

Name:           intel-media-driver
Version:        26.3.5
Release:        1.1.xe%{?dist}
Summary:        The Intel Media Driver for VAAPI, with the Xe native context
License:        MIT AND BSD-3-Clause
URL:            https://github.com/intel/media-driver
Source0:        %{url}/archive/intel-media-%{version}.tar.gz

ExclusiveArch:  x86_64

## cmspam's xe-native-context-enablement, unchanged: a client of the Xe native
## context inside the driver's Xe backend, used when the DRM device is virtio_gpu
Patch1000:      intel-media-driver-xe-native-context.patch

BuildRequires:  cmake >= 3.5
BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  pkgconfig(igdgmm) >= 11.2.0
# for the native context: virtgpu_drm.h and the DRM calls of the patch
BuildRequires:  pkgconfig(libdrm)
BuildRequires:  pkgconfig(libva) >= 1.6.0
BuildRequires:  pkgconfig(pciaccess)
BuildRequires:  pkgconfig(x11)

Provides:       bundled(libdrm)
Provides:       bundled(libcmrt)
Provides:       libva-intel-media-driver = %{version}-%{release}
# libva looks in dri-nonfree from this version on
Conflicts:      libva%{?_isa} < 1.20.0-5

%description
The Intel Media Driver for VAAPI: hardware accelerated video decoding, encoding
and post processing on Intel GPUs, here also through the Xe native context of a
virtual machine.

%prep
%autosetup -p1 -n media-driver-intel-media-%{version}
chmod -x LICENSE.md
# a prebuilt file nothing uses
rm -f Tools/MediaDriverTools/UMDPerfProfiler/MediaPerfParser
sed -e "/-Werror/d" -i media_driver/cmake/linux/media_compile_flags_linux.cmake
sed -e "/-Werror/d" -i media_driver/media_top_cmake.cmake

%build
%cmake \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DBUILD_CMRTLIB:BOOL=False \
  -DMEDIA_RUN_TEST_SUITE:BOOL=False \
  -DRUN_TEST_SUITE:BOOL=False
%cmake_build

%install
%cmake_install
chmod +x %{buildroot}%{_libdir}/dri/iHD_drv_video.so
rm -rf %{buildroot}%{_includedir}/igfxcmrt %{buildroot}%{_libdir}/pkgconfig
# where libva looks first, and RPM Fusion's package has it
mv %{buildroot}%{_libdir}/dri{,-nonfree}

%files
%license LICENSE.md
%{_libdir}/dri-nonfree/iHD_drv_video.so

%changelog
* Wed Nov 18 2026 vitrine <noreply@anthropic.com> - 26.3.5-1.1.xe
- Intel's 26.3.5, built as RPM Fusion's 26.3.5-1, with cmspam's
  intel-media-driver-xe-native-context.patch
