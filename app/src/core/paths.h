// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>

/*
 * Where things live: the VMs in ~/.local/share/vitrine/vms/<id>/,
 * Vitrine's QEMU in ~/.local/share/vitrine/stack/ (see StackBuilder),
 * the sockets of running VMs in $XDG_RUNTIME_DIR/vitrine/<id>/,
 * the settings in ~/.config/vitrine/settings.conf, and the other tools
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
/* Vitrine's QEMU builds: a prefix per build, and `current` */
QString stackDir();
/*
 * qemuSystemName() of the current build of the stack, the link resolved:
 * a build that replaces it has another path; empty if none is built
 */
QString stackQemu();
/* The QEMU the VMs run with: customQemuBinary() if set, else defaultQemuBinary() */
QString qemuBinary();
/* stackQemu(), else qemuSystemName() in PATH */
QString defaultQemuBinary();
/* Another QEMU chosen in the preferences; empty if none */
QString customQemuBinary();
/* Empty for the default */
void setQemuBinary(const QString &path);
/* qemu-img next to qemuBinary(), as in the stack or a build tree, else in PATH */
QString qemuImg();
/* The configured virtiofsd, else /usr/libexec/virtiofsd, else in PATH */
QString virtiofsd();
void setVirtiofsd(const QString &path);

}
