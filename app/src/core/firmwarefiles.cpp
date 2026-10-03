// SPDX-License-Identifier: GPL-2.0-or-later
#include "firmwarefiles.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QProcess>
#include <QRegularExpression>
#include <QtEndian>

#include <cstring>

#include "core/firmware.h"
#include "core/optionvalue.h"
#include "core/paths.h"
#include "core/vmconfig.h"

namespace FirmwareFiles {

/* The start of a flash image, which holds its headers */
static const int kHead = 512;

QString File::name() const
{
    return QFileInfo(path).fileName();
}

/* The architecture of the VM's QEMU: qemu-system-aarch64 runs aarch64 */
static QString archOf(const ArgsFile &args)
{
    static const QRegularExpression target("^qemu-system-([a-z0-9_]+)$");
    const QString own = VmConfig::qemuBinary(args);
    const QRegularExpressionMatch m =
        target.match(QFileInfo(own.isEmpty() ? Paths::qemuBinary() : own).fileName());

    return m.hasMatch() ? m.captured(1) : Paths::hostArch();
}

/* qcow2 by its first bytes, else raw */
static QString formatOf(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) && f.read(4) == "QFI\xfb" ? "qcow2" : "raw";
}

QList<File> list(const ArgsFile &args, const QString &vmDir, const QStringList &dirs,
                 const QString &arch)
{
    const QDir base(vmDir);
    QList<File> files;
    QString codeName;
    int next = 0;

    for (int i = 0; i < int(args.lines.size()); i++) {
        const ArgsFile::Line &line = args.lines[i];
        QString path, format;
        int unit = next;

        if (line.kind != ArgsFile::Line::Option) {
            continue;
        }
        if (line.name == "drive") {
            const OptionValue v = args.valueAt(i);
            if (v.get("if") != "pflash") {
                continue;
            }
            path = v.get("file");
            format = v.get("format");
            if (v.has("unit")) {
                unit = v.get("unit").toInt();
            }
        } else if (line.name == "pflash") {
            path = line.value;
        } else {
            continue;
        }
        /* without unit=, the next one */
        next = unit + 1;
        if (path.isEmpty()) {
            continue;
        }
        const QString absolute = QDir::cleanPath(base.absoluteFilePath(path));
        if (unit == 0) {
            codeName = QFileInfo(absolute).fileName();
        }
        /* the system's files, or links to them, are not the VM's to replace */
        if (base.relativeFilePath(absolute).startsWith("..") ||
            QFileInfo(absolute).isSymLink()) {
            continue;
        }
        File file;
        file.role = unit == 0 ? File::Role::Code : File::Role::Vars;
        file.line = i;
        file.path = absolute;
        file.format = !format.isEmpty()                ? format
                      : QFileInfo::exists(absolute)    ? formatOf(absolute)
                      : absolute.endsWith(".qcow2")    ? "qcow2"
                                                       : "raw";
        files << file;
    }
    if (files.isEmpty()) {
        return files;
    }

    /*
     * The template: the code of the same name; the variable store of the
     * same name, preferably one of this code, else the store of this code,
     * as for a store the importer copied from libvirt
     */
    const QList<Firmware> firmware = FirmwareDb::list(dirs, arch.isEmpty() ? archOf(args) : arch);
    for (File &file : files) {
        int rank = 0;

        for (const Firmware &fw : firmware) {
            const bool code = !codeName.isEmpty() && QFileInfo(fw.code).fileName() == codeName;
            QString candidate;
            int r = 0;

            if (file.role == File::Role::Code) {
                candidate = fw.code;
                r = QFileInfo(fw.code).fileName() == file.name() ? 3 : 0;
            } else if (!fw.varsTemplate.isEmpty()) {
                const bool vars = QFileInfo(fw.varsTemplate).fileName() == file.name();
                candidate = fw.varsTemplate;
                r = vars && code ? 3 : vars ? 2 : code && fw.format == file.format ? 1 : 0;
            }
            if (r > rank && QFileInfo::exists(candidate)) {
                rank = r;
                file.templatePath = candidate;
                file.templateDescription = fw.description;
            }
        }
    }
    return files;
}

/* What the start of an image shows */
struct Image
{
    QString problem;    // with the file itself
    qint64 size = -1;   // for the guest
    QByteArray head;    // the first bytes the guest reads; empty when unknown
};

static quint32 be32(const char *p)
{
    return qFromBigEndian<quint32>(p);
}

static quint64 be64(const char *p)
{
    return qFromBigEndian<quint64>(p);
}

/*
 * A qcow2 image: its header, then every cluster its tables map, which must
 * be in the file, then the first bytes of its data
 */
static Image readQcow2(QFile &f)
{
    static const quint64 kOffset = 0x00fffffffffffe00ULL;   // in L1 and L2 entries
    static const quint64 kCompressed = 1ULL << 62;
    const qint64 fileSize = f.size();
    const QByteArray h = f.read(104);
    Image img;

    if (h.size() < 72 || !h.startsWith("QFI\xfb")) {
        img.problem = QObject::tr("not a qcow2 image");
        return img;
    }
    const char *p = h.constData();
    const quint32 version = be32(p + 4);
    const quint32 clusterBits = be32(p + 20);
    const quint32 l1Size = be32(p + 36);
    const quint64 l1Offset = be64(p + 40);
    const quint64 incompatible = version >= 3 && h.size() >= 80 ? be64(p + 72) : 0;

    img.size = qint64(be64(p + 24));
    if (version < 2 || version > 3 || clusterBits < 9 || clusterBits > 21 || img.size < 0) {
        img.problem = QObject::tr("a damaged qcow2 header");
        return img;
    }
    if (incompatible & 2) {
        img.problem = QObject::tr("marked as corrupt by QEMU");
        return img;
    }
    /* a backing file, encryption, extended L2 tables...: not a firmware copy, cannot tell */
    if (be64(p + 8) != 0 || be32(p + 32) != 0 || (incompatible & ~quint64(1 | 8)) != 0 ||
        img.size > (qint64(256) << 20)) {
        return img;
    }

    const qint64 clusterSize = qint64(1) << clusterBits;
    const qint64 perTable = clusterSize / 8;
    const qint64 clusters = (img.size + clusterSize - 1) / clusterSize;
    const qint64 tables = (clusters + perTable - 1) / perTable;
    const qint64 wanted = qMin<qint64>(kHead, img.size);
    const QString cut = QObject::tr("cut short");
    enum { Zeros, Data, Unknown } first = Zeros;
    qint64 firstData = 0;

    if (tables > l1Size) {
        img.problem = QObject::tr("a damaged qcow2 header");
        return img;
    }
    if (qint64(l1Offset) + tables * 8 > fileSize || !f.seek(qint64(l1Offset))) {
        img.problem = cut;
        return img;
    }
    const QByteArray l1 = f.read(tables * 8);
    for (qint64 t = 0; t < tables; t++) {
        const quint64 l2Offset = be64(l1.constData() + t * 8) & kOffset;

        /* unallocated: zeros */
        if (l2Offset == 0) {
            continue;
        }
        if (qint64(l2Offset) + clusterSize > fileSize || !f.seek(qint64(l2Offset))) {
            img.problem = cut;
            return img;
        }
        const QByteArray l2 = f.read(clusterSize);
        for (qint64 c = 0; c < perTable && t * perTable + c < clusters; c++) {
            const quint64 entry = be64(l2.constData() + c * 8);
            const quint64 data = entry & kOffset;

            if (t == 0 && c == 0) {
                first = entry & kCompressed ? Unknown : data && !(entry & 1) ? Data : Zeros;
                firstData = qint64(data);
            }
            if (!(entry & kCompressed) && data != 0 && qint64(data) >= fileSize) {
                img.problem = cut;
                return img;
            }
        }
    }
    if (first == Zeros) {
        img.head = QByteArray(wanted, '\0');
    } else if (first == Data) {
        if (!f.seek(firstData)) {
            img.problem = cut;
            return img;
        }
        img.head = f.read(wanted);
        if (img.head.size() < wanted) {
            img.problem = cut;
        }
    }
    return img;
}

static Image inspect(const QString &path, const QString &format)
{
    QFile f(path);
    Image img;

    if (!f.open(QIODevice::ReadOnly)) {
        img.problem = QObject::tr("unreadable");
        return img;
    }
    const bool qcow2 = f.peek(4) == "QFI\xfb";
    if (format == "qcow2" || (format.isEmpty() && qcow2)) {
        return readQcow2(f);
    }
    /* QEMU would hand the guest the qcow2 header as firmware */
    if (qcow2) {
        img.problem = QObject::tr("a qcow2 image used as raw");
        return img;
    }
    img.size = f.size();
    img.head = f.read(kHead);
    return img;
}

/*
 * The firmware volume header at the start of an EDK2 flash image, and for
 * a variable store, the header of the store that follows: the part of them
 * that never changes.  Empty if there is none, or its checksum is wrong.
 */
static QByteArray volume(const QByteArray &head, bool vars)
{
    const uchar *p = reinterpret_cast<const uchar *>(head.constData());
    quint16 sum = 0;

    if (head.size() < 56 || std::memcmp(p + 40, "_FVH", 4) != 0) {
        return {};
    }
    const int length = qFromLittleEndian<quint16>(p + 48);
    if (length < 56 || length % 2 != 0 || length + (vars ? 22 : 0) > head.size()) {
        return {};
    }
    for (int i = 0; i < length; i += 2) {
        sum += qFromLittleEndian<quint16>(p + i);
    }
    if (sum != 0) {
        return {};
    }
    /* its type and length; the store's type, size, format and state */
    QByteArray key = head.mid(16, 24) + head.mid(48, 2);
    if (vars) {
        key += head.mid(length, 22);
    }
    return key;
}

QString problem(const File &file)
{
    const QFileInfo fi(file.path);
    const bool vars = file.role == File::Role::Vars;

    if (!fi.exists()) {
        return QObject::tr("missing");
    }
    if (fi.size() == 0) {
        return QObject::tr("empty");
    }
    const Image copy = inspect(file.path, file.format);
    if (!copy.problem.isEmpty()) {
        return copy.problem;
    }
    if (file.templatePath.isEmpty()) {
        return {};
    }
    const Image model = inspect(file.templatePath, {});
    if (!model.problem.isEmpty() || model.size < 0 || copy.size < 0) {
        return {};
    }
    if (copy.size != model.size) {
        return QObject::tr("of the wrong size");
    }
    /* checked only as far as the template passes: EDK2's, readable */
    const QByteArray expected = volume(model.head, vars);
    if (expected.isEmpty() || copy.head.isEmpty()) {
        return {};
    }
    const QByteArray found = volume(copy.head, vars);
    /* the code of an older release may differ; its header must hold */
    if (vars ? found == expected : !found.isEmpty()) {
        return {};
    }
    /* erased or zeroed: UEFI formats the store again */
    if (vars && (copy.head.count('\0') == copy.head.size() ||
                 copy.head.count('\xff') == copy.head.size())) {
        return {};
    }
    return vars ? QObject::tr("its variable store is damaged")
                : QObject::tr("its firmware is damaged");
}

int snapshotCount(const QString &path)
{
    QFile f(path);

    if (!f.open(QIODevice::ReadOnly)) {
        return 0;
    }
    const QByteArray h = f.read(64);
    return h.size() == 64 && h.startsWith("QFI\xfb") ? int(be32(h.constData() + 60)) : 0;
}

static bool copyTemplate(const File &file, QString *error)
{
    if (file.templatePath.isEmpty()) {
        if (error) {
            *error = QObject::tr("No firmware is known that %1 is a copy of").arg(file.name());
        }
        return false;
    }
    if (!QFile::copy(file.templatePath, file.path)) {
        if (error) {
            *error = QObject::tr("Cannot copy %1 to %2").arg(file.templatePath, file.path);
        }
        return false;
    }
    /* the system's are read-only: the copies are the user's */
    QFile::setPermissions(file.path, QFile::permissions(file.path) | QFile::ReadOwner |
                                         QFile::WriteOwner);
    return true;
}

bool recreate(const File &file, QString *error)
{
    return copyTemplate(file, error);
}

bool reset(const File &file, const QString &qemuImg, QString *backup, bool *keptSnapshots,
           QString *error)
{
    const QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
    QString saved = QString("%1.%2.bak").arg(file.path, stamp);
    bool kept = false;

    if (backup) {
        backup->clear();
    }
    if (keptSnapshots) {
        *keptSnapshots = false;
    }
    /* nothing to keep, or nothing to put in its place */
    if (!QFileInfo::exists(file.path) || file.templatePath.isEmpty()) {
        return copyTemplate(file, error);
    }
    for (int n = 2; QFileInfo::exists(saved); n++) {
        saved = QString("%1.%2-%3.bak").arg(file.path, stamp).arg(n);
    }

    if (file.format == "qcow2" && snapshotCount(file.path) > 0 && !qemuImg.isEmpty() &&
        QFile::copy(file.path, saved)) {
        /* the template written into the file: its snapshots stay */
        QProcess p;
        p.start(qemuImg, {"convert", "-n", "-f", formatOf(file.templatePath), "-O", "qcow2",
                          file.templatePath, file.path});
        kept = p.waitForFinished(60000) && p.exitStatus() == QProcess::NormalExit &&
               p.exitCode() == 0;
        if (!kept) {
            p.kill();
            p.waitForFinished(1000);
            /* half written, or unreadable: the copy is the backup */
            if (!QFile::remove(file.path)) {
                if (error) {
                    *error = QObject::tr("Cannot replace %1").arg(file.path);
                }
                return false;
            }
        }
    } else if (!QFile::rename(file.path, saved)) {
        if (error) {
            *error = QObject::tr("Cannot rename %1 to %2")
                         .arg(file.name(), QFileInfo(saved).fileName());
        }
        return false;
    }
    if (!kept && !copyTemplate(file, error)) {
        /* the old one back rather than none */
        QFile::rename(saved, file.path);
        return false;
    }
    if (backup) {
        *backup = saved;
    }
    if (keptSnapshots) {
        *keptSnapshots = kept;
    }
    return true;
}

QString qemuImg(const ArgsFile &args)
{
    const QString own = VmConfig::qemuBinary(args);

    if (!own.isEmpty()) {
        const QFileInfo sibling(QFileInfo(own).dir(), "qemu-img");
        if (sibling.isExecutable()) {
            return sibling.filePath();
        }
    }
    return Paths::qemuImg();
}

bool inUse(const QString &error)
{
    /* file-posix: 'Failed to get "write" lock', 'Failed to lock byte 100', and its hint */
    static const QRegularExpression locked(
        "Failed to get (shared )?\"[a-z ]+\" lock|Failed to lock byte|"
        "Is another process using the image");

    return locked.match(error).hasMatch();
}

}
