// SPDX-License-Identifier: GPL-2.0-or-later
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include "core/hostmemory.h"

using namespace HostMemory;

class TestHostMemory : public QObject
{
    Q_OBJECT

private slots:
    void read()
    {
        QTemporaryDir tmp;
        QFile f(tmp.filePath("meminfo"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("MemTotal:       32768000 kB\n"
                "MemFree:         1000000 kB\n"
                "MemAvailable:    8192000 kB\n"
                "SwapTotal:       8388604 kB\n"
                "SwapFree:        4194302 kB\n");
        f.close();

        const Info info = HostMemory::read(f.fileName());
        QCOMPARE(info.totalMiB, 32000);
        QCOMPARE(info.availableMiB, 8000);
        QCOMPARE(info.swapFreeMiB, 4095);
        QCOMPARE(reserveMiB(info), 1600);
        QVERIFY(HostMemory::read(tmp.filePath("none")).totalMiB == 0);
    }

    void resident()
    {
        QVERIFY(residentMiB(QCoreApplication::applicationPid()) > 0);
        QCOMPARE(residentMiB(0), 0);
    }

    void tight()
    {
        const Info host{32768, 12288, 0};

        /* a 16 GiB guest holding 4 GiB can take 12.9 GiB more: too much */
        QVERIFY(HostMemory::tight(host, {{16384, 4096}}));
        /* holding nearly all of it, it takes little more */
        QVERIFY(!HostMemory::tight(host, {{16384, 16384}}));
        /* two small ones */
        QVERIFY(!HostMemory::tight(host, {{2048, 512}, {2048, 1024}}));
        QVERIFY(!HostMemory::tight(host, {}));
        QVERIFY(HostMemory::tight({32768, 900, 0}, {}));
        QVERIFY(!HostMemory::tight({}, {{16384, 0}}));
    }
};

QTEST_GUILESS_MAIN(TestHostMemory)
#include "test_hostmemory.moc"
