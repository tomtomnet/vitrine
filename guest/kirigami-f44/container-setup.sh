#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Run by build-rpms.sh in the build's container, before rpm-build-inside.sh.
# KDE Frameworks' specs build their API documentation (%cmake_build_kf6)
# from kde-qdoc-common's files under /usr/share/doc, which the image's dnf
# leaves out (tsflags=nodocs): it goes in first, with them.
set -eu
echo "vitrine-build: phase: installing the API documentation's tools"
dnf -y --setopt=keepcache=True --setopt=install_weak_deps=False --setopt=tsflags= \
	install kde-qdoc-common
