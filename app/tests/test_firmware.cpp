// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include "core/firmware.h"

class TestFirmware : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir tmp;
    QStringList dirs;

    QString touch(const QString &name, const QByteArray &data = "x")
    {
        const QString path = tmp.filePath(name);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(data);
        }
        return path;
    }

    void descriptor(const QString &dir, const QString &name, const QString &description,
                    const QString &device, const QStringList &machines,
                    const QStringList &features, const QString &code = {},
                    const QString &vars = {}, const QString &format = "qcow2",
                    const QString &arch = "x86_64")
    {
        QJsonObject mapping{{"device", device}};
        if (device == "flash") {
            mapping["mode"] = vars.isEmpty() ? "stateless" : "split";
            mapping["executable"] = QJsonObject{{"filename", code}, {"format", format}};
            if (!vars.isEmpty()) {
                mapping["nvram-template"] = QJsonObject{{"filename", vars}, {"format", format}};
            }
        } else {
            mapping["filename"] = code;
        }
        const QJsonObject root{
            {"description", description},
            {"interface-types", QJsonArray{"uefi"}},
            {"mapping", mapping},
            {"targets", QJsonArray{QJsonObject{{"architecture", arch},
                                               {"machines", QJsonArray::fromStringList(machines)}}}},
            {"features", QJsonArray::fromStringList(features)},
        };
        touch(dir + '/' + name, QJsonDocument(root).toJson());
    }

private slots:
    void initTestCase()
    {
        const QString code = touch("ovmf/CODE.qcow2");
        const QString sbCode = touch("ovmf/CODE.secboot.qcow2");
        const QString vars = touch("ovmf/VARS.qcow2", "vars");
        const QString sbVars = touch("ovmf/VARS.secboot.qcow2", "secure vars");

        descriptor("share", "30-sb-enrolled.json", "SB enrolled", "flash", {"pc-q35-*"},
                   {"acpi-s3", "enrolled-keys", "requires-smm", "secure-boot"}, sbCode, sbVars);
        descriptor("share", "40-sb.json", "SB without keys", "flash", {"pc-q35-*"},
                   {"requires-smm", "secure-boot"}, sbCode, sbVars);
        descriptor("share", "50-nosb.json", "plain, overridden", "flash",
                   {"pc-i440fx-*", "pc-q35-*"}, {"amd-sev"}, code, vars);
        descriptor("etc", "50-nosb.json", "plain", "flash", {"pc-i440fx-*", "pc-q35-*"},
                   {"acpi-s3", "amd-sev", "amd-sev-es"}, code, vars);
        descriptor("share", "55-microvm.json", "microvm flash", "flash", {"microvm"}, {},
                   code, vars);
        descriptor("share", "60-stateless.json", "in memory", "memory", {"pc-q35-*"}, {},
                   code);
        descriptor("share", "70-masked.json", "masked", "flash", {"pc-q35-*"}, {}, code,
                   vars);
        touch("user/70-masked.json", "");
        dirs = {tmp.filePath("share"), tmp.filePath("etc"), tmp.filePath("user")};
    }

    void listByNameWithOverrides()
    {
        const QList<Firmware> list = FirmwareDb::list(dirs);
        QStringList names;

        for (const Firmware &fw : list) {
            names << QFileInfo(fw.descriptorPath).fileName();
        }
        QCOMPARE(names, QStringList({"30-sb-enrolled.json", "40-sb.json", "50-nosb.json",
                                     "55-microvm.json"}));
        QCOMPARE(list[2].description, "plain");
        QVERIFY(list[2].descriptorPath.contains("/etc/"));
        QCOMPARE(list[2].format, "qcow2");
        QCOMPARE(list[2].mode, "split");
        QCOMPARE(list[2].code, tmp.filePath("ovmf/CODE.qcow2"));
        QCOMPARE(list[2].varsTemplate, tmp.filePath("ovmf/VARS.qcow2"));
        QCOMPARE(list[2].machines, QStringList({"pc-i440fx-*", "pc-q35-*"}));
        QVERIFY(list[2].isUefi());
        QVERIFY(!list[2].hasSecureBoot());
        QVERIFY(list[0].hasSecureBoot());
        QVERIFY(list[0].requiresSmm());
        QVERIFY(!list[1].hasSecureBoot());     /* no keys enrolled */
    }

    void find()
    {
        std::optional<Firmware> fw = FirmwareDb::find(false, "pc-q35-10.0", dirs);
        QVERIFY(fw);
        QCOMPARE(QFileInfo(fw->descriptorPath).fileName(), "50-nosb.json");

        fw = FirmwareDb::find(true, "q35", dirs);
        QVERIFY(fw);
        QCOMPARE(QFileInfo(fw->descriptorPath).fileName(), "30-sb-enrolled.json");

        fw = FirmwareDb::find(false, "pc", dirs);
        QVERIFY(fw);
        QCOMPARE(QFileInfo(fw->descriptorPath).fileName(), "50-nosb.json");

        fw = FirmwareDb::find(false, "microvm", dirs);
        QVERIFY(fw);
        QCOMPARE(QFileInfo(fw->descriptorPath).fileName(), "55-microvm.json");

        QVERIFY(!FirmwareDb::find(true, "pc-i440fx-9.2", dirs));
    }

    void applySecureBoot()
    {
        QTemporaryDir vm;
        ArgsFile args = ArgsFile::parse("-name test\n-machine q35,accel=kvm\n-m 4G\n");
        const Firmware fw = *FirmwareDb::find(true, "q35", dirs);
        QString error;

        QVERIFY2(FirmwareDb::apply(args, fw, vm.path(), &error), qPrintable(error));
        QCOMPARE(args.toText(),
                 "-name test\n"
                 "-machine q35,accel=kvm,smm=on\n"
                 "-m 4G\n"
                 "-drive if=pflash,format=qcow2,unit=0,readonly=on,file=CODE.secboot.qcow2\n"
                 "-drive if=pflash,format=qcow2,unit=1,file=VARS.secboot.qcow2\n"
                 "-global driver=cfi.pflash01,property=secure,value=on\n");
        /* the firmware goes with the VM folder, as its variables do */
        QVERIFY(QFileInfo::exists(vm.filePath("CODE.secboot.qcow2")));

        QFile copy(vm.filePath("VARS.secboot.qcow2"));
        QVERIFY(copy.open(QIODevice::ReadWrite));
        QCOMPARE(copy.readAll(), "secure vars");
        copy.close();

        /* switching firmware replaces the drives, keeps the variables */
        const Firmware plain = *FirmwareDb::find(false, "q35", dirs);
        QVERIFY(FirmwareDb::apply(args, plain, vm.path(), &error));
        QCOMPARE(args.toText(),
                 "-name test\n"
                 "-machine q35,accel=kvm,smm=on\n"
                 "-m 4G\n"
                 "-drive if=pflash,format=qcow2,unit=0,readonly=on,file=CODE.qcow2\n"
                 "-drive if=pflash,format=qcow2,unit=1,file=VARS.qcow2\n");
        QVERIFY(QFileInfo::exists(vm.filePath("VARS.secboot.qcow2")));
        QVERIFY(QFileInfo::exists(vm.filePath("VARS.qcow2")));
    }

    void applyKeepsExistingVarsAndOtherDrives()
    {
        QTemporaryDir vm;
        ArgsFile args = ArgsFile::parse("-drive file=disk.qcow2,if=virtio\n"
                                        "-bios /old/bios.bin\n");
        const Firmware fw = *FirmwareDb::find(true, "q35", dirs);
        QFile vars(vm.filePath("VARS.secboot.qcow2"));

        QVERIFY(vars.open(QIODevice::WriteOnly));
        vars.write("my keys");
        vars.close();
        QVERIFY(FirmwareDb::apply(args, fw, vm.path()));
        QVERIFY(vars.open(QIODevice::ReadOnly));
        QCOMPARE(vars.readAll(), "my keys");
        QCOMPARE(args.indexOf("bios"), -1);
        QCOMPARE(args.lines[0].value, "file=disk.qcow2,if=virtio");
        QCOMPARE(args.indexesOf("drive").size(), 3);
        /* no -machine: q35 is what secure boot descriptors are for */
        QCOMPARE(args.lines[args.indexOf("machine")].value, "q35,smm=on");
    }

    void applyEscapesCommas()
    {
        QTemporaryDir vm;
        ArgsFile args;
        Firmware fw;

        fw.interfaceTypes = {"uefi"};
        fw.code = touch("fw,1/CODE,x.fd");
        fw.format = "raw";
        fw.mode = "stateless";
        fw.machines = {"pc-q35-*"};
        QVERIFY(FirmwareDb::apply(args, fw, vm.path()));
        QCOMPARE(args.toText(),
                 "-drive if=pflash,format=raw,unit=0,readonly=on,file=CODE,,x.fd\n");
    }

    /* Asahi and other ARM hosts: virt machines, no SMM */
    void aarch64()
    {
        const QString code = touch("aavmf/QEMU_EFI.qcow2");
        const QString vars = touch("aavmf/QEMU_VARS.qcow2");
        const QStringList armDirs{tmp.filePath("arm")};
        QTemporaryDir vm;
        ArgsFile args = ArgsFile::parse("-machine virt,gic-version=max\n");

        descriptor("arm", "60-edk2-aarch64.json", "AAVMF", "flash", {"virt-*"}, {}, code, vars,
                   "qcow2", "aarch64");
        descriptor("arm", "40-edk2-aarch64-sb.json", "AAVMF SB", "flash", {"virt-*"},
                   {"enrolled-keys", "secure-boot"}, code, vars, "qcow2", "aarch64");
        QCOMPARE(FirmwareDb::defaultMachine("aarch64"), "virt");
        QCOMPARE(FirmwareDb::list(armDirs, "x86_64").size(), 0);
        QCOMPARE(FirmwareDb::list(armDirs, "aarch64").size(), 2);
        QVERIFY(!FirmwareDb::find(false, {}, dirs, "aarch64"));

        const std::optional<Firmware> fw = FirmwareDb::find(true, {}, armDirs, "aarch64");
        QVERIFY(fw && fw->description == "AAVMF SB");
        QVERIFY(FirmwareDb::apply(args, *fw, vm.path()));
        QCOMPARE(args.toText(), "-machine virt,gic-version=max\n"
                                "-drive if=pflash,format=qcow2,unit=0,readonly=on,file=QEMU_EFI.qcow2\n"
                                "-drive if=pflash,format=qcow2,unit=1,file=QEMU_VARS.qcow2\n");
    }

    /* an imported VM gets copies of its firmware, not of its disks */
    void copyIntoVm()
    {
        QTemporaryDir vm;
        const QString code = touch("script/OVMF_CODE.fd", "code");
        const QString vars = touch("script/OVMF_VARS.fd", "vars");
        const QString other = touch("elsewhere/OVMF_CODE.fd", "other code");
        ArgsFile args = ArgsFile::parse(
            "-drive if=pflash,format=raw,readonly=on,file=" + code + "\n"
            "-drive if=pflash,format=raw,file=" + vars + "\n"
            "-hda " + tmp.filePath("script/disk.qcow2") + "\n"
            "-bios " + other + "\n"
            "-drive if=pflash,format=raw,file=missing.fd\n");
        QStringList copied;
        QString error;

        QVERIFY2(FirmwareDb::copyIntoVm(args, vm.path(), &copied, &error), qPrintable(error));
        QCOMPARE(copied, QStringList({code, vars, other}));
        QCOMPARE(args.lines[0].value, "if=pflash,format=raw,readonly=on,file=OVMF_CODE.fd");
        QCOMPARE(args.lines[1].value, "if=pflash,format=raw,file=OVMF_VARS.fd");
        QCOMPARE(args.lines[2].value, tmp.filePath("script/disk.qcow2"));
        QCOMPARE(args.lines[3].value, "OVMF_CODE-2.fd");
        QCOMPARE(args.lines[4].value, "if=pflash,format=raw,file=missing.fd");
        QFile copy(vm.filePath("OVMF_CODE-2.fd"));
        QVERIFY(copy.open(QIODevice::ReadOnly));
        QCOMPARE(copy.readAll(), "other code");

        /* once in the VM folder, nothing more to copy */
        copied.clear();
        QVERIFY(FirmwareDb::copyIntoVm(args, vm.path(), &copied));
        QVERIFY(copied.isEmpty());
    }

    void missingTemplate()
    {
        QTemporaryDir vm;
        ArgsFile args;
        Firmware fw = *FirmwareDb::find(false, "q35", dirs);
        QString error;

        fw.varsTemplate = tmp.filePath("ovmf/nothing.qcow2");
        QVERIFY(!FirmwareDb::apply(args, fw, vm.path(), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(args.lines.isEmpty());
    }

    void system()
    {
        if (!QFileInfo::exists("/usr/share/qemu/firmware")) {
            QSKIP("no firmware descriptors installed");
        }
        QVERIFY(!FirmwareDb::list().isEmpty());
        const std::optional<Firmware> plain = FirmwareDb::find(false);
        const std::optional<Firmware> secure = FirmwareDb::find(true);
        QVERIFY(plain && plain->isUefi() && !plain->features.contains("secure-boot"));
        QVERIFY(QFileInfo::exists(plain->code));
        QVERIFY(QFileInfo::exists(plain->varsTemplate));
        QVERIFY(secure && secure->hasSecureBoot());
    }
};

QTEST_APPLESS_MAIN(TestFirmware)
#include "test_firmware.moc"
