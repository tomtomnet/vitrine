// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <cstring>

#include "core/firmwarefiles.h"

using FirmwareFiles::File;

/* qemu-img next to $QGM_TEST_QEMU, else in PATH */
static QString testQemuImg()
{
    const QString qemu = qEnvironmentVariable("QGM_TEST_QEMU");
    const QFileInfo sibling(QFileInfo(qemu).dir(), "qemu-img");

    return !qemu.isEmpty() && sibling.isExecutable() ? sibling.filePath()
                                                    : QStandardPaths::findExecutable("qemu-img");
}

/*
 * A flash image as EDK2 builds them: a firmware volume header, with its
 * checksum, then for a variable store the store's header
 */
static QByteArray flashImage(int size, bool vars)
{
    QByteArray image(size, vars ? '\xff' : '\x90');
    uchar *p = reinterpret_cast<uchar *>(image.data());
    quint16 sum = 0;

    std::memset(p, 0, 0x48);
    std::memcpy(p + 16, vars ? "NVRAM-FS-GUID..." : "FFS2-FS-GUID....", 16);
    qToLittleEndian<quint64>(quint64(size), p + 32);
    std::memcpy(p + 40, "_FVH", 4);
    qToLittleEndian<quint32>(0x4feff, p + 44);
    qToLittleEndian<quint16>(0x48, p + 48);
    p[55] = 2;
    qToLittleEndian<quint32>(quint32(size / 4096), p + 56);
    qToLittleEndian<quint32>(4096, p + 60);
    for (int i = 0; i < 0x48; i += 2) {
        sum += qFromLittleEndian<quint16>(p + i);
    }
    qToLittleEndian<quint16>(quint16(-sum), p + 50);
    if (vars) {
        std::memcpy(p + 0x48, "AUTH-VAR-GUID...", 16);
        qToLittleEndian<quint32>(quint32(size / 2 - 0x48), p + 0x58);
        p[0x5c] = 0x5a;
        p[0x5d] = 0xfe;
        std::memset(p + 0x5e, 0, 6);
    }
    return image;
}

class TestFirmwareFiles : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir tmp;
    QStringList dirs;
    QString qemuImg;

    QString write(const QString &path, const QByteArray &data)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(data);
        }
        return path;
    }

    QByteArray read(const QString &path)
    {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }

    void descriptor(const QString &name, const QString &description, const QString &code,
                    const QString &vars, const QString &format)
    {
        const QJsonObject root{
            {"description", description},
            {"interface-types", QJsonArray{"uefi"}},
            {"mapping", QJsonObject{{"device", "flash"},
                                    {"mode", "split"},
                                    {"executable",
                                     QJsonObject{{"filename", code}, {"format", format}}},
                                    {"nvram-template",
                                     QJsonObject{{"filename", vars}, {"format", format}}}}},
            {"targets", QJsonArray{QJsonObject{{"architecture", "x86_64"},
                                               {"machines", QJsonArray{"pc-q35-*"}}}}},
            {"features", QJsonArray{"secure-boot", "enrolled-keys", "requires-smm"}},
        };
        write(tmp.filePath("share/" + name), QJsonDocument(root).toJson());
    }

    QString ovmf(const QString &name) const { return tmp.filePath("ovmf/" + name); }

    /* A VM folder with copies of the qcow2 templates, and its arguments */
    QString vm(const QString &name, ArgsFile *args, const QString &vars = "VARS.qcow2")
    {
        const QString dir = tmp.filePath(name);
        QDir().mkpath(dir);
        QFile::copy(ovmf("CODE.qcow2"), dir + "/CODE.qcow2");
        QFile::copy(ovmf("VARS.qcow2"), dir + '/' + vars);
        *args = ArgsFile::parse("-machine q35\n"
                                "-drive if=pflash,format=qcow2,unit=0,readonly=on,file=CODE.qcow2\n"
                                "-drive if=pflash,format=qcow2,unit=1,file=" + vars + "\n");
        return dir;
    }

    /* Where the first cluster of a qcow2 image is in the file */
    qint64 dataOffset(const QString &path)
    {
        QProcess p;
        p.start(qemuImg, {"map", "--output=json", path});
        p.waitForFinished();
        const QJsonArray map = QJsonDocument::fromJson(p.readAllStandardOutput()).array();
        return map.isEmpty() ? -1 : map.first()["offset"].toInteger(-1);
    }

    void patch(const QString &path, qint64 offset, const QByteArray &data)
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadWrite));
        QVERIFY(f.seek(offset));
        f.write(data);
    }

    File find(const QList<File> &files, File::Role role)
    {
        for (const File &f : files) {
            if (f.role == role) {
                return f;
            }
        }
        return {};
    }

private slots:
    void initTestCase()
    {
        qemuImg = testQemuImg();
        dirs = {tmp.filePath("share")};
        write(ovmf("CODE.fd"), flashImage(64 * 1024, false));
        write(ovmf("VARS.fd"), flashImage(32 * 1024, true));
        write(ovmf("VARS.secboot.fd"), flashImage(32 * 1024, true));
        descriptor("30-raw-enrolled.json", "raw, keys enrolled", ovmf("CODE.fd"),
                   ovmf("VARS.secboot.fd"), "raw");
        descriptor("40-raw.json", "raw, no keys", ovmf("CODE.fd"), ovmf("VARS.fd"), "raw");
        if (!qemuImg.isEmpty()) {
            for (const QString &name : {"CODE", "VARS"}) {
                QCOMPARE(QProcess::execute(qemuImg, {"convert", "-q", "-f", "raw", "-O", "qcow2",
                                                     ovmf(name + ".fd"), ovmf(name + ".qcow2")}),
                         0);
            }
            descriptor("50-qcow2.json", "qcow2", ovmf("CODE.qcow2"), ovmf("VARS.qcow2"),
                       "qcow2");
        }
    }

    void listsTheVmsCopies()
    {
        const QString dir = tmp.filePath("list");
        QDir().mkpath(dir);
        write(dir + "/CODE.fd", read(ovmf("CODE.fd")));
        write(dir + "/guest_VARS.fd", read(ovmf("VARS.fd")));
        const ArgsFile args = ArgsFile::parse(
            "-drive if=pflash,format=raw,unit=0,readonly=on,file=CODE.fd\n"
            "-drive if=pflash,format=raw,unit=1,file=guest_VARS.fd\n"
            "-drive file=disk.qcow2,if=virtio\n");
        const QList<File> files = FirmwareFiles::list(args, dir, dirs, "x86_64");

        QCOMPARE(files.size(), 2);
        QCOMPARE(files[0].role, File::Role::Code);
        QCOMPARE(files[0].line, 0);
        QCOMPARE(files[0].path, dir + "/CODE.fd");
        QCOMPARE(files[0].format, QString("raw"));
        QCOMPARE(files[0].templatePath, ovmf("CODE.fd"));
        /* a store of another name, as libvirt's: the first of the code's, by priority */
        QCOMPARE(files[1].role, File::Role::Vars);
        QCOMPARE(files[1].templatePath, ovmf("VARS.secboot.fd"));
        QCOMPARE(files[1].templateDescription, QString("raw, keys enrolled"));
    }

    void listsTheStoreByItsName()
    {
        const QString dir = tmp.filePath("byname");
        QDir().mkpath(dir);
        /* the code stays the system's; -pflash takes units in turn */
        const ArgsFile args = ArgsFile::parse("-pflash " + ovmf("CODE.fd") + "\n" +
                                              "-pflash VARS.fd\n");
        const QList<File> files = FirmwareFiles::list(args, dir, dirs, "x86_64");

        QCOMPARE(files.size(), 1);
        QCOMPARE(files[0].role, File::Role::Vars);
        QCOMPARE(files[0].line, 1);
        QCOMPARE(files[0].templatePath, ovmf("VARS.fd"));
        QCOMPARE(files[0].templateDescription, QString("raw, no keys"));
        /* missing, so from its name */
        QCOMPARE(files[0].format, QString("raw"));
    }

    void leavesOtherFilesAlone()
    {
        const QString dir = tmp.filePath("others");
        QDir().mkpath(dir);
        QFile::link(ovmf("VARS.fd"), dir + "/VARS.fd");
        const ArgsFile args = ArgsFile::parse(
            "-drive if=pflash,format=raw,unit=0,readonly=on,file=../ovmf/CODE.fd\n"
            "-drive if=pflash,format=raw,unit=1,file=VARS.fd\n"
            "-bios bios.bin\n");

        QVERIFY(FirmwareFiles::list(args, dir, dirs, "x86_64").isEmpty());
    }

    void intactCopiesAreFine()
    {
        const QString dir = tmp.filePath("intact");
        QDir().mkpath(dir);
        write(dir + "/CODE.fd", read(ovmf("CODE.fd")));
        write(dir + "/VARS.fd", read(ovmf("VARS.fd")));
        const ArgsFile args = ArgsFile::parse(
            "-drive if=pflash,format=raw,unit=0,readonly=on,file=CODE.fd\n"
            "-drive if=pflash,format=raw,unit=1,file=VARS.fd\n");
        for (const File &f : FirmwareFiles::list(args, dir, dirs, "x86_64")) {
            QCOMPARE(FirmwareFiles::problem(f), QString());
        }
    }

    void findsRawDamage()
    {
        const QString dir = tmp.filePath("raw");
        QDir().mkpath(dir);
        const ArgsFile args = ArgsFile::parse(
            "-drive if=pflash,format=raw,unit=0,readonly=on,file=CODE.fd\n"
            "-drive if=pflash,format=raw,unit=1,file=VARS.fd\n");
        write(dir + "/CODE.fd", read(ovmf("CODE.fd")));
        const QList<File> files = FirmwareFiles::list(args, dir, dirs, "x86_64");
        const File code = find(files, File::Role::Code);
        const File vars = find(files, File::Role::Vars);
        QByteArray garbled = read(ovmf("VARS.fd"));

        QCOMPARE(FirmwareFiles::problem(vars), QString("missing"));
        write(vars.path, {});
        QCOMPARE(FirmwareFiles::problem(vars), QString("empty"));
        write(vars.path, read(ovmf("VARS.fd")).left(16 * 1024));
        QCOMPARE(FirmwareFiles::problem(vars), QString("of the wrong size"));
        garbled[0x58] = 0x12;
        write(vars.path, garbled);
        QCOMPARE(FirmwareFiles::problem(vars), QString("its variable store is damaged"));
        /* blank: UEFI formats it */
        write(vars.path, QByteArray(32 * 1024, '\0'));
        QCOMPARE(FirmwareFiles::problem(vars), QString());
        write(vars.path, QByteArray(32 * 1024, '\xff'));
        QCOMPARE(FirmwareFiles::problem(vars), QString());

        QCOMPARE(FirmwareFiles::problem(code), QString());
        write(code.path, QByteArray(64 * 1024, '\0'));
        QCOMPARE(FirmwareFiles::problem(code), QString("its firmware is damaged"));
    }

    void findsQcow2Damage()
    {
        if (qemuImg.isEmpty()) {
            QSKIP("no qemu-img, set QGM_TEST_QEMU");
        }
        ArgsFile args;
        const QString dir = vm("qcow2", &args);
        const QList<File> files = FirmwareFiles::list(args, dir, dirs, "x86_64");
        const File vars = find(files, File::Role::Vars);
        const File code = find(files, File::Role::Code);
        const QByteArray intact = read(vars.path);
        const qint64 data = dataOffset(vars.path);

        QCOMPARE(files.size(), 2);
        QCOMPARE(vars.templatePath, ovmf("VARS.qcow2"));
        QCOMPARE(code.templatePath, ovmf("CODE.qcow2"));
        QCOMPARE(FirmwareFiles::problem(vars), QString());
        QCOMPARE(FirmwareFiles::problem(code), QString());

        /* as QEMU says it: "Image is not in qcow2 format" */
        write(vars.path, QByteArray(intact.size(), '\0'));
        QCOMPARE(FirmwareFiles::problem(vars), QString("not a qcow2 image"));

        write(vars.path, intact.left(int(data) + 100));
        QCOMPARE(FirmwareFiles::problem(vars), QString("cut short"));

        QByteArray corrupt = intact;
        corrupt[79] = char(corrupt[79] | 2);
        write(vars.path, corrupt);
        QCOMPARE(FirmwareFiles::problem(vars), QString("marked as corrupt by QEMU"));

        /* sound tables, garbled data: the case where OVMF hangs at an ASSERT */
        write(vars.path, intact);
        QVERIFY(data > 0);
        patch(vars.path, data + 0x58, "\x12\x34");
        QCOMPARE(FirmwareFiles::problem(vars), QString("its variable store is damaged"));

        /* used as raw, QEMU hands the guest the qcow2 header */
        write(vars.path, intact);
        File asRaw = vars;
        asRaw.format = "raw";
        QCOMPARE(FirmwareFiles::problem(asRaw), QString("a qcow2 image used as raw"));
    }

    /* The distribution's OVMF: what OVMF hangs on is found, what it formats again is not */
    void realOvmf()
    {
        const QString code = "/usr/share/edk2/ovmf/OVMF_CODE_4M.qcow2";
        const QString model = "/usr/share/edk2/ovmf/OVMF_VARS_4M.qcow2";
        if (!QFileInfo::exists(code) || !QFileInfo::exists(model) || qemuImg.isEmpty()) {
            QSKIP("no edk2-ovmf with 4M qcow2 images, or no qemu-img");
        }
        const QString dir = tmp.filePath("real");
        QDir().mkpath(dir);
        QVERIFY(QFile::copy(code, dir + "/OVMF_CODE_4M.qcow2"));
        QVERIFY(QFile::copy(model, dir + "/OVMF_VARS_4M.qcow2"));
        const ArgsFile args = ArgsFile::parse(
            "-drive if=pflash,format=qcow2,unit=0,readonly=on,file=OVMF_CODE_4M.qcow2\n"
            "-drive if=pflash,format=qcow2,unit=1,file=OVMF_VARS_4M.qcow2\n");
        /* the system's descriptors */
        const QList<File> files = FirmwareFiles::list(args, dir, {}, "x86_64");
        const File vars = find(files, File::Role::Vars);
        const QByteArray intact = read(vars.path);
        const qint64 data = dataOffset(vars.path);
        QByteArray garbage(8192, '\0');

        QCOMPARE(files.size(), 2);
        QCOMPARE(vars.templatePath, model);
        for (const File &f : files) {
            QCOMPARE(FirmwareFiles::problem(f), QString());
        }
        for (int i = 0; i < garbage.size(); i++) {
            garbage[i] = char(i * 131 + 7);
        }
        patch(vars.path, data, garbage);
        QCOMPARE(FirmwareFiles::problem(vars), QString("its variable store is damaged"));
        write(vars.path, intact);
        patch(vars.path, data, QByteArray(540672, '\0'));
        QCOMPARE(FirmwareFiles::problem(vars), QString());
        write(vars.path, intact.left(100000));
        QCOMPARE(FirmwareFiles::problem(vars), QString("cut short"));
    }

    void recreatesMissingCopies()
    {
        const QString dir = tmp.filePath("recreate");
        QDir().mkpath(dir);
        const ArgsFile args = ArgsFile::parse(
            "-drive if=pflash,format=raw,unit=0,readonly=on,file=CODE.fd\n"
            "-drive if=pflash,format=raw,unit=1,file=VARS.fd\n");
        QString error;

        for (const File &f : FirmwareFiles::list(args, dir, dirs, "x86_64")) {
            QVERIFY2(FirmwareFiles::recreate(f, &error), qPrintable(error));
            QCOMPARE(read(f.path), read(f.templatePath));
            QVERIFY(QFileInfo(f.path).isWritable());
            QCOMPARE(FirmwareFiles::problem(f), QString());
        }
        /* nothing to make it from */
        File unknown;
        unknown.path = dir + "/other.fd";
        QVERIFY(!FirmwareFiles::recreate(unknown, &error));
        QVERIFY(error.contains("other.fd"));
    }

    void resetKeepsTheOldFile()
    {
        const QString dir = tmp.filePath("reset");
        QDir().mkpath(dir);
        const ArgsFile args = ArgsFile::parse(
            "-drive if=pflash,format=raw,unit=0,readonly=on,file=CODE.fd\n"
            "-drive if=pflash,format=raw,unit=1,file=VARS.fd\n");
        const File vars = find(FirmwareFiles::list(args, dir, dirs, "x86_64"), File::Role::Vars);
        const QByteArray old = QByteArray(32 * 1024, 'x');
        QString backup, error;
        bool kept = true;

        write(vars.path, old);
        QVERIFY2(FirmwareFiles::reset(vars, qemuImg, &backup, &kept, &error), qPrintable(error));
        QVERIFY(!kept);
        QCOMPARE(read(vars.path), read(ovmf("VARS.fd")));
        QVERIFY(backup.startsWith(vars.path + '.'));
        QVERIFY(backup.endsWith(".bak"));
        QCOMPARE(read(backup), old);

        /* twice in a second: another backup */
        QString second;
        QVERIFY(FirmwareFiles::reset(vars, qemuImg, &second, &kept, &error));
        QVERIFY(second != backup);
        QVERIFY(QFileInfo::exists(backup));
        QVERIFY(QFileInfo::exists(second));
    }

    void resetKeepsSnapshots()
    {
        if (qemuImg.isEmpty()) {
            QSKIP("no qemu-img, set QGM_TEST_QEMU");
        }
        ArgsFile args;
        const QString dir = vm("snapshots", &args);
        const File vars = find(FirmwareFiles::list(args, dir, dirs, "x86_64"), File::Role::Vars);
        QString backup, error;
        bool kept = false;

        /* changed variables, and a snapshot of them */
        patch(vars.path, dataOffset(vars.path) + 0x1000, "changed");
        QCOMPARE(QProcess::execute(qemuImg, {"snapshot", "-c", "before", vars.path}), 0);
        QCOMPARE(FirmwareFiles::snapshotCount(vars.path), 1);

        QVERIFY2(FirmwareFiles::reset(vars, qemuImg, &backup, &kept, &error), qPrintable(error));
        QVERIFY(kept);
        QCOMPARE(FirmwareFiles::snapshotCount(vars.path), 1);
        QCOMPARE(FirmwareFiles::snapshotCount(backup), 1);
        QCOMPARE(QProcess::execute(qemuImg, {"compare", "-q", ovmf("VARS.qcow2"), vars.path}), 0);
        QCOMPARE(FirmwareFiles::problem(vars), QString());
        /* the snapshot still has the variables it was taken with */
        QCOMPARE(QProcess::execute(qemuImg, {"snapshot", "-a", "before", vars.path}), 0);
        QVERIFY(QProcess::execute(qemuImg, {"compare", "-q", ovmf("VARS.qcow2"), vars.path}) != 0);
    }

    void inUseIsNotDamage()
    {
        /* what QEMU says when the VM already runs elsewhere */
        QVERIFY(FirmwareFiles::inUse(
            "qemu-system-x86_64: -drive if=pflash,format=qcow2,file=OVMF_VARS_4M.qcow2: "
            "Failed to get \"write\" lock\nIs another process using the image "
            "[OVMF_VARS_4M.qcow2]?"));
        QVERIFY(FirmwareFiles::inUse("Failed to get shared \"write\" lock"));
        QVERIFY(FirmwareFiles::inUse("Failed to lock byte 100"));
        QVERIFY(!FirmwareFiles::inUse(
            "qemu-system-x86_64: -drive if=pflash,format=qcow2,file=OVMF_VARS_4M.qcow2: "
            "Image is not in qcow2 format"));
        QVERIFY(!FirmwareFiles::inUse("Could not open 'OVMF_VARS_4M.qcow2': No such file"));
    }

    void resetReplacesAnUnreadableFile()
    {
        if (qemuImg.isEmpty()) {
            QSKIP("no qemu-img, set QGM_TEST_QEMU");
        }
        ArgsFile args;
        const QString dir = vm("unreadable", &args);
        const File vars = find(FirmwareFiles::list(args, dir, dirs, "x86_64"), File::Role::Vars);
        QString backup, error;
        bool kept = true;

        QCOMPARE(QProcess::execute(qemuImg, {"snapshot", "-c", "s1", vars.path}), 0);
        /* the header says it has snapshots, the tables are gone */
        QByteArray broken = read(vars.path);
        broken.truncate(512);
        write(vars.path, broken);
        QVERIFY(!FirmwareFiles::problem(vars).isEmpty());

        QVERIFY2(FirmwareFiles::reset(vars, qemuImg, &backup, &kept, &error), qPrintable(error));
        QVERIFY(!kept);
        QCOMPARE(read(vars.path), read(ovmf("VARS.qcow2")));
        QCOMPARE(read(backup), broken);
    }
};

QTEST_GUILESS_MAIN(TestFirmwareFiles)
#include "test_firmwarefiles.moc"
