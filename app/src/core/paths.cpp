// SPDX-License-Identifier: GPL-2.0-or-later
#include "paths.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>

namespace Paths {

static const char kApp[] = "/qemu-gui-manager";

static QString privateDir(const QString &path)
{
    QDir().mkpath(path);
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                    QFileDevice::ExeOwner);
    return path;
}

static QString setting(const QString &key)
{
    return QSettings(settingsPath(), QSettings::IniFormat).value(key).toString();
}

static void setSetting(const QString &key, const QString &value)
{
    QSettings s(settingsPath(), QSettings::IniFormat);
    if (value.isEmpty()) {
        s.remove(key);
    } else {
        s.setValue(key, value);
    }
}

QString dataDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + kApp;
}

QString vmsDir()
{
    return dataDir() + "/vms";
}

QString cacheDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + kApp;
}

QString runtimeDir()
{
    return privateDir(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + kApp);
}

QString vmRuntimeDir(const QString &id)
{
    return privateDir(runtimeDir() + '/' + id);
}

QString settingsPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + kApp +
           "/settings.conf";
}

QString hostArch()
{
    const QString arch = QSysInfo::currentCpuArchitecture();

    if (arch == "arm64") {
        return "aarch64";
    }
    if (arch.startsWith("power")) {
        return arch == "power64" ? "ppc64" : "ppc";
    }
    return arch;
}

QString qemuSystemName()
{
    return "qemu-system-" + hostArch();
}

QString qemuBinary()
{
    const QString configured = setting("qemu/binary");
    return configured.isEmpty() ? QStandardPaths::findExecutable(qemuSystemName())
                                : configured;
}

void setQemuBinary(const QString &path)
{
    setSetting("qemu/binary", path);
}

QString qemuImg()
{
    const QString qemu = qemuBinary();
    if (!qemu.isEmpty()) {
        const QFileInfo sibling(QFileInfo(qemu).dir(), "qemu-img");
        if (sibling.isExecutable()) {
            return sibling.filePath();
        }
    }
    return QStandardPaths::findExecutable("qemu-img");
}

QString virtiofsd()
{
    const QString configured = setting("virtiofsd/binary");
    if (!configured.isEmpty()) {
        return configured;
    }
    if (QFileInfo("/usr/libexec/virtiofsd").isExecutable()) {
        return "/usr/libexec/virtiofsd";
    }
    return QStandardPaths::findExecutable("virtiofsd");
}

void setVirtiofsd(const QString &path)
{
    setSetting("virtiofsd/binary", path);
}

}
