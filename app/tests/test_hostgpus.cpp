// SPDX-License-Identifier: GPL-2.0-or-later
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include "core/hostgpus.h"
#include "fakegpus.h"

#include <sys/sysmacros.h>

/* The host's GPUs from a fake sysfs and /dev (fakegpus.h) */
class TestHostGpus : public QObject
{
    Q_OBJECT

    static QStringList nodes(const QList<HostGpu> &gpus)
    {
        QStringList out;
        for (const HostGpu &g : gpus) {
            out << g.node;
        }
        return out;
    }

private slots:
    void init()
    {
        HostGpus::setDesktopDevice(std::nullopt);
    }

    void cleanupTestCase()
    {
        HostGpus::setRoots("/sys", "/dev");
        HostGpus::setDesktopDevice(std::nullopt);
    }

    /* One GPU: the node, udev's link, what the PCI device says */
    void oneGpu()
    {
        QTemporaryDir tmp;
        FakeGpus host(tmp.path());
        host.laptop();
        host.use();

        const QList<HostGpu> gpus = HostGpus::list();
        QCOMPARE(gpus.size(), 1);
        const HostGpu &g = gpus.first();
        QCOMPARE(g.node, "/dev/dri/renderD128");
        QCOMPARE(g.byPath, "/dev/dri/by-path/pci-0000:65:00.0-render");
        QCOMPARE(g.path(), g.byPath);
        QCOMPARE(g.address, "0000:65:00.0");
        QCOMPARE(g.shortAddress(), "65:00.0");
        QCOMPARE(g.driver, "amdgpu");
        QCOMPARE(g.vendorId, 0x1002);
        QCOMPARE(g.deviceId, 0x1900);
        QVERIFY(g.bootVga);
        QVERIFY(g.hasNativeContext());
        if (QFileInfo::exists("/usr/share/hwdata/pci.ids")) {
            QVERIFY2(g.name().startsWith("AMD "), qPrintable(g.name()));
        }

        /* the desktop's, by boot_vga; Automatic adds nothing with one GPU, nor the display */
        QCOMPARE(HostGpus::desktop(gpus)->node, "/dev/dri/renderD128");
        QVERIFY(!HostGpus::automatic(gpus));
        QCOMPARE(HostGpus::automaticNode(), "");
        QCOMPARE(HostGpus::displayNode(), "");
        QCOMPARE(HostGpus::rendererChoice(gpus)->node, "/dev/dri/renderD128");
    }

    /*
     * Two GPUs: in the order of their addresses; the desktop's is the
     * compositor's main device, whether it names a card or a render node,
     * else the firmware's screen; Automatic takes it, and so does the
     * display
     */
    void twoGpus()
    {
        QTemporaryDir tmp;
        FakeGpus host(tmp.path());
        host.desktop();
        host.use();

        const QList<HostGpu> gpus = HostGpus::list();
        QCOMPARE(nodes(gpus), QStringList({"/dev/dri/renderD129", "/dev/dri/renderD128"}));
        QCOMPARE(gpus[0].driver, "xe");
        QCOMPARE(gpus[0].byPath, "/dev/dri/by-path/pci-0000:03:00.0-render");
        QVERIFY(!gpus[0].bootVga);
        QVERIFY(gpus[1].bootVga);

        /* not known: boot_vga's */
        QCOMPARE(HostGpus::desktop(gpus)->address, "0000:7a:00.0");
        QCOMPARE(HostGpus::automaticNode(), "/dev/dri/renderD128");
        QCOMPARE(HostGpus::displayNode(), "/dev/dri/renderD128");
        /* the compositor's main device, a primary node or a render node */
        HostGpus::setDesktopDevice(makedev(226, 0));
        QCOMPARE(HostGpus::desktop(gpus)->address, "0000:03:00.0");
        QCOMPARE(HostGpus::automatic(gpus)->node, "/dev/dri/renderD129");
        QCOMPARE(HostGpus::automaticNode(), "/dev/dri/renderD129");
        QCOMPARE(HostGpus::displayNode(), "/dev/dri/renderD129");
        HostGpus::setDesktopDevice(makedev(226, 129));
        QCOMPARE(HostGpus::desktop(gpus)->address, "0000:03:00.0");
        /* one that is no GPU of the list: boot_vga's again */
        HostGpus::setDesktopDevice(makedev(226, 77));
        QCOMPARE(HostGpus::desktop(gpus)->address, "0000:7a:00.0");

        /* virglrenderer by itself: amdgpu before xe, whatever the desktop's */
        QCOMPARE(HostGpus::rendererChoice(gpus)->address, "0000:7a:00.0");
    }

    /* The GPU of a DRM device number, as the compositor gives its main device */
    void ofDevice()
    {
        QTemporaryDir tmp;
        FakeGpus host(tmp.path());
        host.desktop();
        host.use();

        QCOMPARE(HostGpus::ofDevice(makedev(226, 0))->node, "/dev/dri/renderD129");
        QCOMPARE(HostGpus::ofDevice(makedev(226, 129))->node, "/dev/dri/renderD129");
        QCOMPARE(HostGpus::ofDevice(makedev(226, 1))->node, "/dev/dri/renderD128");
        QCOMPARE(HostGpus::ofDevice(makedev(226, 128))->address, "0000:7a:00.0");
        QVERIFY(!HostGpus::ofDevice(makedev(226, 5)));
        QVERIFY(!HostGpus::ofDevice(makedev(8, 0)));
    }

    /* Without udev's link the node itself is kept; a link to nothing is ignored */
    void missingByPath()
    {
        QTemporaryDir tmp;
        FakeGpus host(tmp.path());
        host.gpu("0000:03:00.0", "0x8086", "0xe212", "xe", false, 0, 129, false);
        host.laptop();
        FakeGpus::link("../renderD140", host.dev + "/dri/by-path/pci-0000:04:00.0-render");
        host.use();

        const QList<HostGpu> gpus = HostGpus::list();
        QCOMPARE(gpus.size(), 2);
        QCOMPARE(gpus[0].byPath, "");
        QCOMPARE(gpus[0].path(), "/dev/dri/renderD129");
        QCOMPARE(gpus[1].path(), "/dev/dri/by-path/pci-0000:65:00.0-render");
    }

    /* vgem's render node, and a PCI device that is no display controller, are no GPUs */
    void notGpus()
    {
        QTemporaryDir tmp;
        FakeGpus host(tmp.path());
        host.laptop();
        host.vgem(130);
        host.gpu("0000:c4:00.1", "0x1022", "0x17f0", "amdxdna", false, 3, 131, true, "0x118000");
        host.use();

        QCOMPARE(nodes(HostGpus::list()), QStringList({"/dev/dri/renderD128"}));
        QCOMPARE(HostGpus::automaticNode(), "");
        QCOMPARE(HostGpus::displayNode(), "");
    }

    /* A host without GPUs, or without sysfs */
    void noGpus()
    {
        QTemporaryDir tmp;
        FakeGpus(tmp.path()).use();
        QVERIFY(HostGpus::list().isEmpty());
        QVERIFY(!HostGpus::desktop({}));
        QVERIFY(!HostGpus::rendererChoice({}));
        HostGpus::setRoots(tmp.filePath("nothing"), tmp.filePath("nothing"));
        QVERIFY(HostGpus::list().isEmpty());
        QCOMPARE(HostGpus::automaticNode(), "");
    }

    /*
     * A desktop on a GPU without a native context in vitrine's
     * virglrenderer: Automatic leaves the choice to it, which would not
     * start on that GPU; the display, which needs none, takes it
     */
    void desktopWithoutNativeContext()
    {
        QTemporaryDir tmp;
        FakeGpus host(tmp.path());
        host.gpu("0000:01:00.0", "0x10de", "0x2684", "nvidia", true, 0, 128);
        host.gpu("0000:00:02.0", "0x8086", "0xa780", "i915", false, 1, 129);
        host.use();

        const QList<HostGpu> gpus = HostGpus::list();
        QCOMPARE(HostGpus::desktop(gpus)->driver, "nvidia");
        QVERIFY(!HostGpus::desktop(gpus)->hasNativeContext());
        QVERIFY(!HostGpus::automatic(gpus));
        QCOMPARE(HostGpus::automaticNode(), "");
        QCOMPARE(HostGpus::displayNode(), "/dev/dri/renderD128");
        QCOMPARE(HostGpus::rendererChoice(gpus)->driver, "i915");
    }

    /* Several of one driver: virglrenderer takes the first node */
    void rendererChoiceOfOneDriver()
    {
        QTemporaryDir tmp;
        FakeGpus host(tmp.path());
        host.gpu("0000:03:00.0", "0x1002", "0x744c", "amdgpu", false, 1, 129);
        host.gpu("0000:08:00.0", "0x1002", "0x744c", "amdgpu", true, 0, 128);
        host.use();

        QCOMPARE(HostGpus::rendererChoice(HostGpus::list())->address, "0000:08:00.0");
    }

    /* A VM's x-render-node: the GPU it names, by its node or any link to it */
    void find()
    {
        QTemporaryDir tmp;
        FakeGpus host(tmp.path());
        host.desktop();
        FakeGpus::link("dri/renderD128", host.dev + "/gpu-link");
        host.use();
        const QList<HostGpu> gpus = HostGpus::list();

        QCOMPARE(HostGpus::find(gpus, "/dev/dri/by-path/pci-0000:03:00.0-render")->driver, "xe");
        QCOMPARE(HostGpus::find(gpus, "/dev/dri/renderD129")->driver, "xe");
        QCOMPARE(HostGpus::find(gpus, "/dev/gpu-link")->driver, "amdgpu");
        QVERIFY(!HostGpus::find(gpus, "/dev/dri/by-path/pci-0000:04:00.0-render"));
        QVERIFY(!HostGpus::find(gpus, "/dev/dri/card0"));
        QVERIFY(!HostGpus::find(gpus, ""));
    }

    /* What QEMU would make of a path, for the runner's check before it */
    void pathState()
    {
        QTemporaryDir tmp;
        FakeGpus host(tmp.path());
        host.laptop();
        FakeGpus::write(host.dev + "/null", "");
        host.use();

        using S = HostGpus::PathState;
        QCOMPARE(HostGpus::pathState("/dev/dri/renderD128"), S::RenderNode);
        QCOMPARE(HostGpus::pathState("/dev/dri/by-path/pci-0000:65:00.0-render"), S::RenderNode);
        QCOMPARE(HostGpus::pathState("/dev/dri/by-path/pci-0000:03:00.0-render"), S::Missing);
        QCOMPARE(HostGpus::pathState("/dev/dri/renderD200"), S::Missing);
        QCOMPARE(HostGpus::pathState("/dev/dri/card1"), S::NotRenderNode);
        QCOMPARE(HostGpus::pathState("/dev/dri/by-path/pci-0000:65:00.0-card"), S::NotRenderNode);
        QCOMPARE(HostGpus::pathState("/dev/null"), S::NotRenderNode);
    }

    /* A VM's choice as the user reads it: a by-path link by its PCI address */
    void describe()
    {
        QCOMPARE(HostGpus::describe("/dev/dri/by-path/pci-0000:03:00.0-render"), "PCI 03:00.0");
        QCOMPARE(HostGpus::describe("/dev/dri/by-path/pci-0001:c4:00.1-render"),
                 "PCI 0001:c4:00.1");
        QCOMPARE(HostGpus::describe("/dev/dri/renderD129"), "/dev/dri/renderD129");
        QCOMPARE(HostGpus::describe("/dev/dri/by-path/platform-fd4a0000.gpu-render"),
                 "/dev/dri/by-path/platform-fd4a0000.gpu-render");
        QCOMPARE(HostGpus::describe("/dev/dri/by-path/pci-0000:03:00.0-card"),
                 "/dev/dri/by-path/pci-0000:03:00.0-card");
    }

    /* The names: the vendor's short one, and the model people know */
    void names()
    {
        const auto named = [](const QString &vendor, const QString &device, quint16 v = 1,
                              quint16 d = 2) {
            HostGpu g;
            g.vendorName = vendor;
            g.deviceName = device;
            g.vendorId = v;
            g.deviceId = d;
            return g.name();
        };
        QCOMPARE(named("Intel Corporation", "Battlemage G21 [Arc Pro B50]"), "Intel Arc Pro B50");
        QCOMPARE(named("Advanced Micro Devices, Inc. [AMD/ATI]", "HawkPoint1"), "AMD HawkPoint1");
        QCOMPARE(named("NVIDIA Corporation", "AD102 [GeForce RTX 4090]"),
                 "NVIDIA GeForce RTX 4090");
        QCOMPARE(named("Red Hat, Inc.", "Virtio 1.0 GPU"), "Red Hat Virtio 1.0 GPU");
        /* what pci.ids does not know: the IDs, as PciDevice gives them */
        QCOMPARE(named("8086", "e2ff", 0x8086, 0xe2ff), "GPU 8086:e2ff");
        QCOMPARE(named("Intel Corporation", "e2ff", 0x8086, 0xe2ff), "Intel 8086:e2ff");
    }
};

QTEST_GUILESS_MAIN(TestHostGpus)
#include "test_hostgpus.moc"
