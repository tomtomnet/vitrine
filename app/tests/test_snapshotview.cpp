// SPDX-License-Identifier: GPL-2.0-or-later
#include <QFile>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include "core/paths.h"
#include "core/vmstore.h"
#include "ui/snapshotview.h"

/* The Snapshots tab as the pane shows it, offscreen, with qemu-img's snapshots */
class TestSnapshotView : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        Paths::setQemuBinary("/nonexistent/qemu-system-x86_64");
        if (Paths::qemuImg().isEmpty()) {
            QSKIP("no qemu-img");
        }
    }

    void cleanupTestCase()
    {
        Paths::setQemuBinary({});
    }

    /*
     * Start From It with a snapshot of the disks only (QEMU saves no running
     * state for VMs with vhost-user or 3D blobs): the disks go back to it,
     * then the VM starts as it is, not with -loadvm
     */
    void startFromDisksOnly()
    {
        QTemporaryDir dir;
        QFile args(dir.filePath("vm.args"));
        QVERIFY(args.open(QIODevice::WriteOnly));
        args.write("-name Snap\n-drive file=disk.qcow2,format=qcow2,if=virtio\n");
        args.close();
        QCOMPARE(QProcess::execute(Paths::qemuImg(), {"create", "-q", "-f", "qcow2",
                                                      dir.filePath("disk.qcow2"), "1M"}), 0);
        QCOMPARE(QProcess::execute(Paths::qemuImg(), {"snapshot", "-c", "base",
                                                      dir.filePath("disk.qcow2")}), 0);
        Vm vm(dir.path());
        SnapshotView view;
        view.setVm(&vm);
        view.show();
        auto *table = view.findChild<QTableWidget *>();
        auto *start = view.findChild<QPushButton *>("startFrom");
        QVERIFY(table && start);
        QTRY_COMPARE(table->rowCount(), 1);
        table->selectRow(0);

        QTRY_VERIFY(start->isEnabled());
        QVERIFY2(start->toolTip().contains("disks only"), qPrintable(start->toolTip()));

        QSignalSpy requested(&view, &SnapshotView::startRequested);
        /* the confirmation: Start */
        QTimer::singleShot(0, this, []() {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(box);
            for (QAbstractButton *button : box->buttons()) {
                if (button->text().remove('&') == "Start") {
                    button->click();
                }
            }
        });
        start->click();
        QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 1, 15000);
        /* as it is, its disks back: no -loadvm of a state it does not have */
        QCOMPARE(requested[0][0].toString(), QString());
    }
};

QTEST_MAIN(TestSnapshotView)
#include "test_snapshotview.moc"
