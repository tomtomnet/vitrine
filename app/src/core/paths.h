// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>

/*
 * Where things live: the VMs in ~/.local/share/qemu-gui-manager/vms/<id>/,
 * the sockets of running VMs in $XDG_RUNTIME_DIR/qemu-gui-manager/<id>/,
 * the settings in ~/.config/qemu-gui-manager/settings.conf, and the tools
 * wherever the settings or PATH say.
 */
namespace Paths {

QString dataDir();
QString vmsDir();
QString cacheDir();
/* Private to the user (0700), created on demand */
QString runtimeDir();
QString vmRuntimeDir(const QString &id);
/* For QSettings(settingsPath(), QSettings::IniFormat) */
QString settingsPath();

/* QEMU's name for the architecture of this machine: x86_64, aarch64... */
QString hostArch();
/* qemu-system-x86_64 on x86-64, qemu-system-aarch64 on ARM */
QString qemuSystemName();
/* The QEMU the VMs run with: the configured one, else qemuSystemName() */
QString qemuBinary();
void setQemuBinary(const QString &path);
/* qemu-img next to qemuBinary(), as in a build tree, else in PATH */
QString qemuImg();
/* The configured virtiofsd, else /usr/libexec/virtiofsd, else in PATH */
QString virtiofsd();
void setVirtiofsd(const QString &path);

}
