// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include "core/importer.h"
#include "core/vmconfig.h"
#include "core/vmhardware.h"

using namespace Importer;

class TestImporter : public QObject
{
    Q_OBJECT

private slots:
    void split()
    {
        QStringList notes;
        const QList<QStringList> commands = splitCommands(
            "#!/bin/sh\n"
            "# a comment\n"
            "DISK=\"my disk.qcow2\"; export SIZE=4G\n"
            "cd ~/vms && qemu-system-x86_64 \\\n"
            "    -trace 'vdagent*' -hda \"$DISK\" -m ${SIZE} \\\n"
            "    -drive file=\"OVMF \\\"x\\\".fd\",if=pflash -append a\\ b # tail\n"
            "echo $NOPE_NOT_SET $(date)\n",
            &notes);

        QCOMPARE(commands.size(), 5);
        QCOMPARE(commands[0], QStringList({"DISK=my disk.qcow2"}));
        QCOMPARE(commands[1], QStringList({"export", "SIZE=4G"}));
        QCOMPARE(commands[2], QStringList({"cd", QDir::homePath() + "/vms"}));
        QCOMPARE(commands[3], QStringList({"qemu-system-x86_64", "-trace", "vdagent*",
                                           "-hda", "my disk.qcow2", "-m", "4G", "-drive",
                                           "file=OVMF \"x\".fd,if=pflash", "-append", "a b"}));
        QCOMPARE(commands[4].first(), "echo");
        QCOMPARE(notes.size(), 2);
    }

    void import()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        for (const char *name : {"OVMF_CODE.fd", "disk.qcow2"}) {
            QFile f(dir.filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
        }

        std::optional<Result> r = importScript(
            "#!/bin/sh\n"
            "virtiofsd --socket-path=/tmp/fs.sock &\n"
            "SDL_VIDEODRIVER=wayland ./build/qemu-system-x86_64 \\\n"
            "    -accel kvm,honor-guest-pat=on \\\n"
            "    -enable-kvm -m 16G -daemonize \\\n"
            "    -drive if=pflash,format=raw,readonly=on,file=\"OVMF_CODE.fd\"  \\\n"
            "    -drive if=pflash,format=raw,file=\"OVMF_VARS.fd\"  \\\n"
            "    -fsdev local,id=fsdev0,path=\"/home/user/Public\",security_model=mapped-xattr \\\n"
            "    -trace 'vdagent*' \\\n"
            "    disk.qcow2\n",
            dir.path());

        QVERIFY(r);
        QCOMPARE(r->args.toText(),
                 "# The script also ran:\n"
                 "#   virtiofsd --socket-path=/tmp/fs.sock\n"
                 "#qemu " + dir.path() + "/build/qemu-system-x86_64\n"
                 "-accel kvm,honor-guest-pat=on\n"
                 "-enable-kvm\n"
                 "-m 16G\n"
                 "-drive if=pflash,format=raw,readonly=on,file=" + dir.path() + "/OVMF_CODE.fd\n"
                 "-drive if=pflash,format=raw,file=OVMF_VARS.fd\n"
                 "-fsdev local,id=fsdev0,path=/home/user/Public,security_model=mapped-xattr\n"
                 "-trace vdagent*\n"
                 "-hda " + dir.path() + "/disk.qcow2\n");
        QCOMPARE(VmConfig::qemuBinary(r->args), dir.path() + "/build/qemu-system-x86_64");
        QCOMPARE(r->notes.size(), 2);
        QVERIFY(r->notes[0].contains("SDL_VIDEODRIVER=wayland"));
        QVERIFY(r->notes[1].contains("-daemonize"));
        QCOMPARE(r->missing, QStringList{"OVMF_VARS.fd"});
    }

    void takesValue()
    {
        /* -snapshot is a flag: disk.img is the disk, not its value */
        std::optional<Result> r = importScript(
            "qemu-system-x86_64 -snapshot disk.img -m 1G", "/",
            [](const QString &name) { return name == "m"; });

        QVERIFY(r);
        QCOMPARE(r->args.toText(), "-snapshot\n-hda disk.img\n-m 1G\n");
        QVERIFY(VmConfig::qemuBinary(r->args).isEmpty());
    }

    /* Native context with KVM gets honor-guest-pat=on, as from the Display page */
    void nativeContextPat()
    {
        const QString card = "-device virtio-vga-gl,blob=on,hostmem=4G,drm_native_context=on";
        std::optional<Result> r = importScript("qemu-system-x86_64 -accel kvm -m 4G " + card, "/");

        QVERIFY(r);
        QCOMPARE(r->args.toText(), "-accel kvm,honor-guest-pat=on\n-m 4G\n"
                                   "-device virtio-vga-gl,blob=on,hostmem=4G,drm_native_context=on\n");
        QVERIFY(r->notes.join('\n').contains("honor-guest-pat=on was added"));

        /* -enable-kvm moves to -accel, which alone takes properties */
        r = importScript("qemu-system-x86_64 -enable-kvm " + card, "/");
        QVERIFY(r);
        QCOMPARE(VmConfig::accelProperty(r->args, "honor-guest-pat"), "on");

        /* set by hand, TCG, or no native context: as it is */
        for (const QString &script : {"qemu-system-x86_64 -accel kvm,honor-guest-pat=off " + card,
                                      "qemu-system-x86_64 -accel tcg " + card,
                                      QString("qemu-system-x86_64 -accel kvm -device virtio-vga-gl")}) {
            r = importScript(script, "/");
            QVERIFY(r);
            QVERIFY2(!r->args.toText().contains("honor-guest-pat=on"), qPrintable(script));
            QVERIFY(!r->notes.join('\n').contains("honor-guest-pat"));
        }
    }

    void noQemu()
    {
        QVERIFY(!importScript("echo hello\n", "/"));
    }

    void qemuDirective()
    {
        ArgsFile a = ArgsFile::parse("# top\n-m 1G\n");

        VmConfig::setQemuBinary(a, "/opt/q/qemu-system-x86_64");
        QCOMPARE(a.toText(), "# top\n#qemu /opt/q/qemu-system-x86_64\n-m 1G\n");
        QCOMPARE(a.argv(), QStringList({"-m", "1G"}));
        a = ArgsFile::parse(a.toText());
        QCOMPARE(VmConfig::qemuBinary(a), "/opt/q/qemu-system-x86_64");
        VmConfig::setQemuBinary(a, "~/b/qemu");
        QCOMPARE(VmConfig::qemuBinary(a), QDir::homePath() + "/b/qemu");
        VmConfig::setQemuBinary(a, {});
        QCOMPARE(a.toText(), "# top\n-m 1G\n");
    }
};

QTEST_APPLESS_MAIN(TestImporter)
#include "test_importer.moc"
