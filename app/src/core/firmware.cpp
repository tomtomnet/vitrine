// SPDX-License-Identifier: GPL-2.0-or-later
#include "firmware.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QStandardPaths>

#include "core/optionvalue.h"
#include "core/paths.h"
#include "core/vmconfig.h"

bool Firmware::isUefi() const
{
    return interfaceTypes.contains("uefi");
}

bool Firmware::hasSecureBoot() const
{
    return features.contains("secure-boot") && features.contains("enrolled-keys");
}

bool Firmware::requiresSmm() const
{
    return features.contains("requires-smm");
}

namespace FirmwareDb {

static QStringList defaultDirs()
{
    return {"/usr/share/qemu/firmware", "/etc/qemu/firmware",
            QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
                "/qemu/firmware"};
}

static QStringList strings(const QJsonValue &array)
{
    QStringList list;

    for (const QJsonValue &v : array.toArray()) {
        list << v.toString();
    }
    return list;
}

/* The flash firmware for @arch a descriptor describes */
static std::optional<Firmware> parse(const QString &path, const QString &arch)
{
    QFile f(path);

    if (!f.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    const QJsonObject mapping = root["mapping"].toObject();
    const QJsonObject executable = mapping["executable"].toObject();
    Firmware fw;

    if (mapping["device"].toString() != "flash") {
        return std::nullopt;
    }
    fw.descriptorPath = path;
    fw.description = root["description"].toString();
    fw.interfaceTypes = strings(root["interface-types"]);
    fw.code = executable["filename"].toString();
    fw.format = executable["format"].toString("raw");
    fw.mode = mapping["mode"].toString("split");
    if (fw.mode == "split") {
        fw.varsTemplate = mapping["nvram-template"].toObject()["filename"].toString();
    }
    for (const QJsonValue &target : root["targets"].toArray()) {
        if (target["architecture"].toString() == arch) {
            fw.machines += strings(target["machines"]);
        }
    }
    fw.features = strings(root["features"]);
    if (fw.code.isEmpty() || fw.machines.isEmpty() ||
        (fw.mode == "split" && fw.varsTemplate.isEmpty())) {
        return std::nullopt;
    }
    return fw;
}

QList<Firmware> list(const QStringList &dirs, const QString &arch)
{
    const QString target = arch.isEmpty() ? Paths::hostArch() : arch;
    QMap<QString, QFileInfo> files;
    QList<Firmware> out;

    /* by file name, later directories first */
    for (const QString &dir : dirs.isEmpty() ? defaultDirs() : dirs) {
        for (const QFileInfo &fi : QDir(dir).entryInfoList({"*.json"}, QDir::Files)) {
            files[fi.fileName()] = fi;
        }
    }
    for (const QFileInfo &fi : std::as_const(files)) {
        /* an empty file masks those of the same name */
        if (fi.size() == 0) {
            continue;
        }
        if (std::optional<Firmware> fw = parse(fi.filePath(), target)) {
            out << *fw;
        }
    }
    return out;
}

QString defaultMachine(const QString &arch)
{
    const QString target = arch.isEmpty() ? Paths::hostArch() : arch;
    return target == "aarch64" || target == "arm" ? "virt"
           : target == "x86_64" || target == "i386" ? "q35" : QString();
}

static bool supports(const Firmware &fw, const QString &machine)
{
    /* the aliases of the latest versioned machines */
    const QString name = machine == "q35" ? "pc-q35-latest"
                         : machine == "pc" ? "pc-i440fx-latest"
                         : machine == "virt" ? "virt-latest" : machine;

    if (name.isEmpty()) {
        return true;
    }
    for (const QString &glob : fw.machines) {
        if (QRegularExpression::fromWildcard(glob, Qt::CaseSensitive).match(name).hasMatch()) {
            return true;
        }
    }
    return false;
}

std::optional<Firmware> find(bool secureBoot, const QString &machine,
                             const QStringList &dirs, const QString &arch)
{
    const QString wanted = machine.isEmpty() ? defaultMachine(arch) : machine;

    for (const Firmware &fw : list(dirs, arch)) {
        if (!fw.isUefi() || !supports(fw, wanted)) {
            continue;
        }
        if (secureBoot ? fw.hasSecureBoot() : !fw.features.contains("secure-boot")) {
            return fw;
        }
    }
    return std::nullopt;
}

static bool isSecureFlash(const OptionValue &global)
{
    return (global.get("driver") == "cfi.pflash01" && global.get("property") == "secure") ||
           global.has("cfi.pflash01.secure");
}

/* Copies @source into @dir, as @name, unless there is one already */
static bool copyFile(const QString &source, const QDir &dir, const QString &name,
                     QString *error)
{
    const QString target = dir.filePath(name);

    if (!QFileInfo::exists(target) && !QFile::copy(source, target)) {
        if (error) {
            *error = QObject::tr("Cannot copy %1 to %2").arg(source, dir.path());
        }
        return false;
    }
    /* the system's are read-only: the copies are the user's */
    QFile::setPermissions(target, QFile::permissions(target) | QFile::ReadOwner |
                                      QFile::WriteOwner);
    return true;
}

bool apply(ArgsFile &args, const Firmware &fw, const QString &vmDir, QString *error)
{
    /* x86 secure boot: ARM has no SMM, nor this flash property */
    const bool secure = fw.requiresSmm();
    const QString format = OptionValue::escape(fw.format);
    const QDir dir(vmDir);
    const QString code = QFileInfo(fw.code).fileName();
    const QString vars = fw.mode == "split" ? QFileInfo(fw.varsTemplate).fileName() : QString();

    /* the combined image holds the variables too: it is written to */
    if (!copyFile(fw.code, dir, code, error) ||
        (!vars.isEmpty() && !copyFile(fw.varsTemplate, dir, vars, error))) {
        return false;
    }

    for (qsizetype i = args.lines.size() - 1; i >= 0; i--) {
        const ArgsFile::Line &line = args.lines[i];
        if (line.kind != ArgsFile::Line::Option) {
            continue;
        }
        const OptionValue v = args.valueAt(int(i));
        if (line.name == "bios" || (line.name == "drive" && v.get("if") == "pflash") ||
            (line.name == "global" && isSecureFlash(v))) {
            args.removeAt(int(i));
        }
    }

    if (fw.mode == "combined") {
        args.add("drive", QString("if=pflash,format=%1,unit=0,file=%2")
                              .arg(format, OptionValue::escape(code)));
    } else {
        args.add("drive", QString("if=pflash,format=%1,unit=0,readonly=on,file=%2")
                              .arg(format, OptionValue::escape(code)));
        if (!vars.isEmpty()) {
            args.add("drive", QString("if=pflash,format=%1,unit=1,file=%2")
                                  .arg(format, OptionValue::escape(vars)));
        }
    }
    if (secure) {
        const QList<int> machines = args.indexesOf("machine");
        if (machines.isEmpty()) {
            args.add("machine", "q35,smm=on");
        } else {
            OptionValue v = args.valueAt(machines.last());
            v.set("smm", "on");
            args.setValueAt(machines.last(), v);
        }
        args.add("global", "driver=cfi.pflash01,property=secure,value=on");
    }
    return true;
}


bool copyIntoVm(ArgsFile &args, const QString &vmDir, QStringList *copied, QString *error)
{
    const QDir dir(vmDir);

    for (const VmConfig::FileRef &file : VmConfig::files(args)) {
        const ArgsFile::Line &line = args.lines[file.line];
        const QFileInfo source(file.path);
        QString name = source.fileName();

        if (!(line.name == "bios" || line.name == "pflash" ||
              (line.name == "drive" && OptionValue(line.value).get("if") == "pflash")) ||
            !source.isAbsolute() || !source.isFile() || source.absolutePath() == dir.absolutePath()) {
            continue;
        }
        /* two different files of the same name */
        for (int n = 2; QFileInfo::exists(dir.filePath(name)); n++) {
            name = QString("%1-%2.%3").arg(source.completeBaseName()).arg(n).arg(source.suffix());
        }
        if (!copyFile(source.filePath(), dir, name, error)) {
            return false;
        }
        VmConfig::setFile(args, file, name);
        if (copied) {
            *copied << source.filePath();
        }
    }
    return true;
}
}
