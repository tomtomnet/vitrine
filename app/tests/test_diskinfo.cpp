// SPDX-License-Identifier: GPL-2.0-or-later
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include "core/diskinfo.h"

class TestDiskInfo : public QObject
{
    Q_OBJECT

private slots:
    void qcow2()
    {
        QTemporaryDir tmp;
        QFile f(tmp.filePath("disk.qcow2"));
        QByteArray header(512, '\0');

        /* the header of a 64 GiB qcow2 v3, nothing else written */
        header.replace(0, 4, "QFI\xfb");
        qToBigEndian<quint32>(3, header.data() + 4);
        qToBigEndian<quint64>(64ULL << 30, header.data() + 24);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(header);
        f.close();

        const DiskInfo::Usage u = DiskInfo::usage(f.fileName());
        QCOMPARE(u.format, "qcow2");
        QCOMPARE(u.capacity, 64LL << 30);
        QVERIFY(u.used > 0 && u.used < (1 << 20));
    }

    void sparseRaw()
    {
        QTemporaryDir tmp;
        QFile f(tmp.filePath("disk.img"));

        QVERIFY(f.open(QIODevice::WriteOnly));
        QVERIFY(f.resize(100 << 20));       // a hole
        f.seek(50 << 20);
        f.write(QByteArray(1 << 20, 'x'));  // and 1 MiB written in it
        f.close();

        const DiskInfo::Usage u = DiskInfo::usage(f.fileName());
        QCOMPARE(u.format, "raw");
        QCOMPARE(u.capacity, 100LL << 20);
        QVERIFY2(u.used >= (1 << 20) && u.used < (4 << 20), qPrintable(QString::number(u.used)));
    }

    void unknownFormat()
    {
        QTemporaryDir tmp;
        QFile f(tmp.filePath("disk.vdi"));

        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(4096, 'v'));
        f.close();

        const DiskInfo::Usage u = DiskInfo::usage(f.fileName());
        QVERIFY(u.format.isEmpty());
        QCOMPARE(u.capacity, -1);
        QVERIFY(u.used >= 4096);
    }

    void missing()
    {
        const DiskInfo::Usage u = DiskInfo::usage("/nonexistent/disk.qcow2");
        QCOMPARE(u.used, -1);
        QCOMPARE(u.capacity, -1);
    }
};

QTEST_GUILESS_MAIN(TestDiskInfo)
#include "test_diskinfo.moc"
