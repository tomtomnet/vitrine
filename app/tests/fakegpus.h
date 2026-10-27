// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTest>

#include <unistd.h>

#include "core/hostgpus.h"

/*
 * The GPUs of a fake host, in folder @root: its sysfs and /dev as the
 * kernel and udev lay them out (/sys/class/drm, the PCI devices,
 * /sys/dev/char, /dev/dri and its by-path links), for HostGpus to read
 * once use() points it there.  Empty, it is a host without GPUs, which
 * keeps tests from reading this computer's.
 */
class FakeGpus
{
public:
    explicit FakeGpus(const QString &root) : sys(root + "/sys"), dev(root + "/dev")
    {
        QDir().mkpath(sys);
        QDir().mkpath(dev);
    }

    const QString sys;
    const QString dev;

    /* HostGpus reads this host from now on */
    void use() const { HostGpus::setRoots(sys, dev); }

    static void write(const QString &path, const QByteArray &data)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(data);
    }

    /* A symlink at @path to @target as given, relative or not */
    static void link(const QString &target, const QString &path)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile::remove(path);
        QVERIFY(::symlink(QFile::encodeName(target).constData(),
                          QFile::encodeName(path).constData()) == 0);
    }

    /* A DRM node of the device in @device: its sysfs entries and /dev/dri's */
    void node(const QString &device, const QString &name, int minor) const
    {
        const QString dir = device + "/drm/" + name;
        write(dir + "/dev", "226:" + QByteArray::number(minor) + "\n");
        link(device, dir + "/device");
        link(dir, sys + "/class/drm/" + name);
        link(dir, sys + "/dev/char/226:" + QString::number(minor));
        write(dev + "/dri/" + name, "");
    }

    /*
     * A PCI GPU at @address, its driver @driver, with a primary node
     * card@card and a render node renderD@render, and udev's by-path link
     * to each if @byPath
     */
    void gpu(const QString &address, const QByteArray &vendor, const QByteArray &device,
             const QString &driver, bool bootVga, int card, int render, bool byPath = true,
             const QByteArray &cls = "0x030000") const
    {
        const QString dir = sys + "/devices/pci0000:00/" + address;
        write(dir + "/vendor", vendor + "\n");
        write(dir + "/device", device + "\n");
        write(dir + "/class", cls + "\n");
        write(dir + "/boot_vga", bootVga ? "1\n" : "0\n");
        QDir().mkpath(sys + "/bus/pci/drivers/" + driver);
        link(sys + "/bus/pci/drivers/" + driver, dir + "/driver");
        link(sys + "/bus/pci", dir + "/subsystem");
        link(dir, sys + "/bus/pci/devices/" + address);
        node(dir, "card" + QString::number(card), card);
        node(dir, "renderD" + QString::number(render), render);
        if (byPath) {
            link("../renderD" + QString::number(render),
                 dev + "/dri/by-path/pci-" + address + "-render");
            link("../card" + QString::number(card), dev + "/dri/by-path/pci-" + address + "-card");
        }
    }

    /* vgem: a render node of a device on no bus of hardware */
    void vgem(int render) const
    {
        const QString dir = sys + "/devices/faux/vgem";
        QDir().mkpath(sys + "/bus/faux");
        link(sys + "/bus/faux", dir + "/subsystem");
        node(dir, "renderD" + QString::number(render), render);
        node(dir, "card" + QString::number(render - 128), render - 128);
    }

    /* This laptop: the Radeon 780M alone, card1 and renderD128 */
    void laptop() const { gpu("0000:65:00.0", "0x1002", "0x1900", "amdgpu", true, 1, 128); }

    /*
     * The user's desktop with its integrated GPU on: an Arc Pro B50 (xe,
     * card0, renderD129) beside an AMD iGPU (amdgpu, card1, renderD128),
     * whose screen the firmware used
     */
    void desktop() const
    {
        gpu("0000:03:00.0", "0x8086", "0xe212", "xe", false, 0, 129);
        gpu("0000:7a:00.0", "0x1002", "0x13c0", "amdgpu", true, 1, 128);
    }
};
