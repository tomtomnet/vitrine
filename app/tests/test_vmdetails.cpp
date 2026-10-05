// SPDX-License-Identifier: GPL-2.0-or-later
#include <QAbstractTextDocumentLayout>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextTable>

#include "core/paths.h"
#include "core/vmstore.h"
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
};

QTEST_MAIN(TestVmDetails)
#include "test_vmdetails.moc"
