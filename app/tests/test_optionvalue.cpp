// SPDX-License-Identifier: GPL-2.0-or-later
#include <QTest>

#include "core/optionvalue.h"

class TestOptionValue : public QObject
{
    Q_OBJECT

private slots:
    void implied()
    {
        OptionValue v("virtio-gpu-gl,hostmem=8G,blob=on");

        QCOMPARE(v.implied(), "virtio-gpu-gl");
        QCOMPARE(v.get("hostmem"), "8G");
        QVERIFY(v.flag("blob"));
        QCOMPARE(v.toString(), "virtio-gpu-gl,hostmem=8G,blob=on");
    }

    void noImplied()
    {
        OptionValue v("file=disk.qcow2,if=virtio");

        QCOMPARE(v.implied(), "");
        QCOMPARE(v.get("file"), "disk.qcow2");
        v.setImplied("x");
        QCOMPARE(v.toString(), "x,file=disk.qcow2,if=virtio");
    }

    void escapedCommas()
    {
        OptionValue v("file=/a,,b/disk.qcow2,format=qcow2");

        QCOMPARE(v.get("file"), "/a,b/disk.qcow2");
        QCOMPARE(v.get("format"), "qcow2");
        QCOMPARE(v.toString(), "file=/a,,b/disk.qcow2,format=qcow2");
        v.set("file", "/c,d");
        QCOMPARE(v.toString(), "file=/c,,d,format=qcow2");
    }

    void cpuFlags()
    {
        OptionValue v("host,+avx,-sse4a,topoext=on");

        QCOMPARE(v.implied(), "host");
        QVERIFY(v.has("+avx"));
        QVERIFY(v.has("-sse4a"));
        QCOMPARE(v.get("topoext"), "on");
        v.setImplied("max");
        QCOMPARE(v.toString(), "max,+avx,-sse4a,topoext=on");
    }

    void editKeepsOrderAndUnknownKeys()
    {
        OptionValue v("16,sockets=1,cores=8,threads=2,x-weird=1");

        v.set("cores", "4");
        v.setImplied("8");
        v.remove("x-weird");
        v.set("maxcpus", "8");
        QCOMPARE(v.toString(), "8,sockets=1,cores=4,threads=2,maxcpus=8");
    }

    void empty()
    {
        OptionValue v("");

        QVERIFY(v.isEmpty());
        QCOMPARE(v.toString(), "");
        v.set("a", "1");
        QCOMPARE(v.toString(), "a=1");
    }

    void flags()
    {
        OptionValue v("x,a=on,b=off,c=yes,d=junk");

        QVERIFY(v.flag("a"));
        QVERIFY(!v.flag("b", true));
        QVERIFY(v.flag("c"));
        QVERIFY(v.flag("d", true));
        QVERIFY(!v.flag("missing"));
        v.setFlag("b", true);
        QCOMPARE(v.get("b"), "on");
    }
};

QTEST_APPLESS_MAIN(TestOptionValue)
#include "test_optionvalue.moc"
