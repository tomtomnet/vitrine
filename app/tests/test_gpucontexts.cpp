// SPDX-License-Identifier: GPL-2.0-or-later
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "core/gpucontexts.h"
#include "core/qmpclient.h"

static QJsonArray array(const char *json)
{
    return QJsonDocument::fromJson(json).array();
}

class TestGpuContexts : public QObject
{
    Q_OBJECT

private slots:
    void findGpu()
    {
        /* qom-list /machine/peripheral-anon of a VM, as QEMU answers */
        QCOMPARE(GpuContexts::findGpu(array(R"([{"name": "type", "type": "string"},
                                               {"name": "device[0]", "type": "child<vhost-user-fs-pci>"},
                                               {"name": "device[1]", "type": "child<virtio-vga-gl>"},
                                               {"name": "device[2]", "type": "child<virtio-blk-pci>"}])"),
                                         "/machine/peripheral-anon"),
                 "/machine/peripheral-anon/device[1]");
        QCOMPARE(GpuContexts::findGpu(array(R"([{"name": "gpu0", "type": "child<virtio-gpu-gl-pci>"}])"),
                                         "/machine/peripheral"),
                 "/machine/peripheral/gpu0");
        /* 2D */
        QCOMPARE(GpuContexts::findGpu(array(R"([{"name": "device[0]", "type": "child<virtio-vga>"}])"),
                                         "/machine/peripheral-anon"),
                 QString());
    }

    void status()
    {
        using Status = GpuContexts::Status;

        QCOMPARE(GpuContexts::statusOf(false, 0, 3), Status::NotOffered);
        QCOMPARE(GpuContexts::statusOf(true, 2, 5), Status::InUse);
        QCOMPARE(GpuContexts::statusOf(true, 0, 5), Status::Virgl);
        QCOMPARE(GpuContexts::statusOf(true, 0, 0), Status::Waiting);
    }

    /* A real QEMU: the qemu-gui build has the counts, others not */
    void realQemu()
    {
        const QString qemu = qEnvironmentVariable("QGM_TEST_QEMU");
        QTemporaryDir tmp;
        const QString socket = tmp.filePath("qmp.sock");
        QProcess help, vm;

        if (qemu.isEmpty()) {
            QSKIP("QGM_TEST_QEMU is not set");
        }
        help.start(qemu, {"-device", "virtio-gpu-gl-device,help"});
        help.waitForFinished();
        const bool counts = help.readAllStandardOutput().contains("x-drm-offered");

        vm.start(qemu, {"-machine", "q35", "-m", "256", "-nodefaults", "-S", "-display",
                        "egl-headless", "-device", "virtio-vga-gl,blob=on,hostmem=64M", "-qmp",
                        "unix:" + socket + ",server=on,wait=off"});
        vm.waitForStarted();
        for (int i = 0; i < 50 && !QFileInfo::exists(socket) && vm.state() == QProcess::Running;
             i++) {
            QTest::qWait(100);
        }
        if (!QFileInfo::exists(socket)) {
            QSKIP(qPrintable("QEMU does not run with virgl here: " +
                             QString::fromLocal8Bit(vm.readAllStandardError())));
        }

        QmpClient client;
        GpuContexts contexts;
        QSignalSpy changed(&contexts, &GpuContexts::changed);
        client.connectToSocket(socket);
        QTRY_VERIFY(client.isReady());
        contexts.update(&client);
        if (counts) {
            /* drm_native_context is off: not offered */
            QVERIFY(changed.wait(5000));
            QCOMPARE(contexts.status(), GpuContexts::Status::NotOffered);
        } else {
            QTest::qWait(1000);
            QCOMPARE(contexts.status(), GpuContexts::Status::Unknown);
        }
        client.execute("quit");
        QTRY_COMPARE_WITH_TIMEOUT(vm.state(), QProcess::NotRunning, 5000);
    }
};

QTEST_GUILESS_MAIN(TestGpuContexts)
#include "test_gpucontexts.moc"
