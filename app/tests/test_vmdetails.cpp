// SPDX-License-Identifier: GPL-2.0-or-later
#include <QAbstractTextDocumentLayout>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextTable>
#include <QPushButton>

#include "core/paths.h"
#include "core/settingsupdates.h"
#include "core/vmstore.h"
#include "ui/settingsupdates.h"
#include "ui/vmdetails.h"

/* The Details tab as the pane shows it, offscreen */
class TestVmDetails : public QObject
{
    Q_OBJECT

    /* The cell of the details' table whose text holds @text */
    static QTextTableCell cellWith(QTextTable *table, const QString &text)
    {
        for (int row = 0; row < table->rows(); row++) {
            for (int column = 0; column < table->columns(); column++) {
                const QTextTableCell cell = table->cellAt(row, column);
                QTextCursor cursor = cell.firstCursorPosition();
                cursor.setPosition(cell.lastCursorPosition().position(), QTextCursor::KeepAnchor);
                if (cursor.selectedText().contains(text)) {
                    return cell;
                }
            }
        }
        return {};
    }

private slots:
    void initTestCase()
    {
        /* settings of their own, and no QEMU to read the documentation of */
        QStandardPaths::setTestModeEnabled(true);
        Paths::setQemuBinary("/nonexistent/qemu-system-x86_64");
    }

    void cleanupTestCase()
    {
        Paths::setQemuBinary({});
    }

    /* The values of every section in one column, in line from one to the next */
    void valuesInOneColumn()
    {
        QTemporaryDir dir;
        QFile args(dir.filePath("vm.args"));
        QVERIFY(args.open(QIODevice::WriteOnly));
        args.write("-name Details\n-m 8G\n-device virtio-vga-gl\n-display dbus,p2p=yes,gl=on\n"
                   "-netdev user,id=net0\n-device virtio-net-pci,netdev=net0\n");
        args.close();
        Vm vm(dir.path());
        VmDetails details;
        details.setVm(&vm);
        details.resize(900, 700);
        details.show();
        auto *text = details.findChild<QTextBrowser *>("details");
        QVERIFY(text);
        QTextDocument *document = text->document();

        QList<QTextTable *> tables;
        for (QTextFrame *frame : document->rootFrame()->childFrames()) {
            if (auto *table = qobject_cast<QTextTable *>(frame)) {
                tables << table;
            }
        }
        QCOMPARE(tables.size(), 1);
        const QTextTableCell memory = cellWith(tables[0], "8 GiB");
        const QTextTableCell graphics = cellWith(tables[0], "virtio-vga-gl");
        QVERIFY(memory.isValid() && graphics.isValid());
        QCOMPARE(memory.column(), 1);
        QCOMPARE(graphics.column(), 1);
        /* the System and Display sections' values start at the same x */
        QAbstractTextDocumentLayout *layout = document->documentLayout();
        const qreal x1 = layout->blockBoundingRect(memory.firstCursorPosition().block()).x();
        const qreal x2 = layout->blockBoundingRect(graphics.firstCursorPosition().block()).x();
        QCOMPARE(x1, x2);
    }

    /*
     * The settings newer VMs get, on a VM of October: the Details' banner
     * and the Console's, Review with Don't Ask Again and Ask Again, Apply,
     * the update with Undo, Not Now
     */
    void newerSettings()
    {
        if (Paths::hostArch() != "x86_64") {
            QSKIP("the changes for a PC");
        }
        QTemporaryDir dir;
        QVERIFY(QDir().mkpath(dir.filePath("newer")));
        QFile args(dir.filePath("newer/vm.args"));
        QVERIFY(args.open(QIODevice::WriteOnly));
        args.write("-name Newer,debug-threads=on\n"
                   "#guest linux,desktop=kde\n"
                   "-machine q35,memory-backend=mem,dump-guest-core=off\n"
                   "-accel kvm,honor-guest-pat=auto\n"
                   "-object memory-backend-memfd,id=mem,size=2G,share=on\n"
                   "-device virtio-vga-gl,hostmem=4G,blob=on,drm_native_context=on,"
                   "x-host-vblank=on,x-vblank-lead=3000,x-vblank-lead-auto=on\n"
                   "-display dbus,p2p=yes,gl=on\n");
        args.close();
        VmStore store(dir.path());
        Vm *vm = store.find("newer");
        QVERIFY(vm);
        SettingsUpdatesMonitor *monitor = SettingsUpdatesMonitor::of(vm);
        SettingsUpdatesBanner details(SettingsUpdatesBanner::Place::Details);
        SettingsUpdatesBanner home(SettingsUpdatesBanner::Place::Home);
        details.setVm(vm);
        home.setVm(vm);
        QCOMPARE(details.offerText(), "This VM lacks 2 settings that new VMs get: No HPET timer, "
                                      "A 16 GiB window for GPU memory.");
        QCOMPARE(home.offerText(), "This VM lacks 2 settings that new VMs get.");
        QVERIFY(home.notNowButton());
        QVERIFY(!details.notNowButton());
        QVERIFY(details.updateText().isEmpty());
        QVERIFY(details.problemText().isEmpty());

        SettingsUpdatesDialog dialog(vm);
        QCOMPARE(dialog.offered(), QStringList({"hpet-off", "hostmem-16g"}));
        QVERIFY(dialog.applyButton()->isEnabled());
        dialog.checkBox("hpet-off")->setChecked(false);
        dialog.decline("hostmem-16g");
        QCOMPARE(dialog.declined(), QStringList{"hostmem-16g"});
        /* nothing checked to take */
        QVERIFY(!dialog.applyButton()->isEnabled());
        QCOMPARE(details.offerText(), "This VM lacks a setting that new VMs get: No HPET timer.");
        dialog.askAgain("hostmem-16g");
        QCOMPARE(dialog.offered(), QStringList({"hpet-off", "hostmem-16g"}));
        QVERIFY(!dialog.checkBox("hpet-off")->isChecked());
        QVERIFY(dialog.checkBox("hostmem-16g")->isChecked());
        dialog.checkBox("hpet-off")->setChecked(true);
        QVERIFY(dialog.note().isEmpty());
        dialog.accept();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QString text;
        QString error;
        QVERIFY(SettingsUpdates::readText(vm->argsPath(), &text, &error));
        QVERIFY(text.contains("dump-guest-core=off,hpet=off\n"));
        QVERIFY(text.contains(",hostmem=16G,"));
        QVERIFY(details.offerText().isEmpty());
        QVERIFY(home.offerText().isEmpty());
        QVERIFY(details.updateText().startsWith("Settings updated on "));
        QVERIFY(details.updateText().contains("No HPET timer, A 16 GiB window for GPU memory"));
        QVERIFY(details.undoButton()->isEnabled());

        /* undone: offered again; Not Now hides the Console's banner, not the Details' */
        QStringList failed;
        QVERIFY(monitor->undo(&failed, &error));
        QCOMPARE(home.offerText(), "This VM lacks 2 settings that new VMs get.");
        monitor->setNotNow();
        QVERIFY(home.offerText().isEmpty());
        QVERIFY(!details.offerText().isEmpty());

        /* on the Details page */
        VmDetails page;
        page.setVm(vm);
        auto *banner = page.findChild<SettingsUpdatesBanner *>();
        QVERIFY(banner);
        QCOMPARE(banner->offerText(), details.offerText());
    }
};

QTEST_MAIN(TestVmDetails)
#include "test_vmdetails.moc"
