// SPDX-License-Identifier: GPL-2.0-or-later
#include <QTest>

#include "core/argsfile.h"

static const char *kSample =
    "# my VM\n"
    "-name fedora\n"
    "-M q35,memory-backend=mem\n"
    "\n"
    "-object memory-backend-memfd,id=mem,size=16G,share=on\n"
    "-drive file=/vms/my disk.qcow2,if=virtio\n"
    "--nodefaults\n"
    "-device virtio-gpu-gl,hostmem=8G\n"
    "#share tag=host_share,path=/home/bobby/Public,cache=auto\n"
    "-device qemu-xhci\n";

class TestArgsFile : public QObject
{
    Q_OBJECT

private slots:
    void parseAndWriteBack()
    {
        ArgsFile f = ArgsFile::parse(kSample);

        QCOMPARE(f.lines.size(), 10);
        QCOMPARE(f.lines[0].kind, ArgsFile::Line::Comment);
        QCOMPARE(f.lines[3].kind, ArgsFile::Line::Blank);
        QCOMPARE(f.lines[8].kind, ArgsFile::Line::Directive);
        QCOMPARE(f.lines[8].name, "share");
        /* -- options come back with one dash, the rest as written */
        QString expected = QString(kSample).replace("--nodefaults", "-nodefaults");
        QCOMPARE(f.toText(), expected);
    }

    void argv()
    {
        ArgsFile f = ArgsFile::parse(kSample);
        const QStringList expected = {
            "-name", "fedora",
            "-M", "q35,memory-backend=mem",
            "-object", "memory-backend-memfd,id=mem,size=16G,share=on",
            "-drive", "file=/vms/my disk.qcow2,if=virtio",
            "-nodefaults",
            "-device", "virtio-gpu-gl,hostmem=8G",
            "-device", "qemu-xhci",
        };

        QCOMPARE(f.argv(), expected);
    }

    void find()
    {
        ArgsFile f = ArgsFile::parse(kSample);

        QCOMPARE(f.indexOf("machine"), 2);      /* -M */
        QCOMPARE(f.indexesOf("device"), QList<int>({7, 9}));
        QCOMPARE(f.indexOf("share", ArgsFile::Line::Directive), 8);
        QCOMPARE(f.indexOfDevice([](const QString &d) {
                     return d.startsWith("virtio-gpu");
                 }), 7);
        QCOMPARE(f.indexOf("missing"), -1);
    }

    void edit()
    {
        ArgsFile f = ArgsFile::parse(kSample);
        OptionValue gpu = f.valueAt(7);

        gpu.set("blob", "on");
        f.setValueAt(7, gpu);
        QCOMPARE(f.lines[7].value, "virtio-gpu-gl,hostmem=8G,blob=on");

        /* after the last -device */
        QCOMPARE(f.add("device", "usb-tablet"), 10);
        /* a new option at the end */
        QCOMPARE(f.add("smp", "8"), 11);
        f.removeAll("device");
        QVERIFY(f.indexesOf("device").isEmpty());
        QCOMPARE(f.lines.last().name, "smp");
    }

    void fromArgv()
    {
        const QStringList argv = {"qemu-system-x86_64", "-m", "4G",
                                  "-enable-kvm", "-drive", "file=a.img",
                                  "disk.img", "-S"};
        ArgsFile f = ArgsFile::fromArgv(argv, [](const QString &o) {
            return o == "m" || o == "drive";
        });

        QCOMPARE(f.toText(), "-m 4G\n-enable-kvm\n-drive file=a.img\n-S\n");
    }

    void strayTextBecomesComment()
    {
        ArgsFile f = ArgsFile::parse("disk.img\n-m 1G\n");

        QCOMPARE(f.lines[0].kind, ArgsFile::Line::Comment);
        QCOMPARE(f.argv(), QStringList({"-m", "1G"}));
    }
};

QTEST_APPLESS_MAIN(TestArgsFile)
#include "test_argsfile.moc"
