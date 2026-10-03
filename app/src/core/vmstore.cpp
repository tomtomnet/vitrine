// SPDX-License-Identifier: GPL-2.0-or-later
#include "vmstore.h"

#include <QDir>
#include <QFile>
#include <QFileSystemWatcher>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>

#include "core/paths.h"
#include "core/vmconfig.h"
#include "core/vmrunner.h"

static const char kArgsFile[] = "vm.args";

Vm::Vm(const QString &dir, QObject *parent)
    : QObject(parent), m_dir(QDir(dir).absolutePath())
{
    m_runner = new VmRunner(id(), m_dir, this);
    reload();
}

QString Vm::id() const
{
    return QFileInfo(m_dir).fileName();
}

QString Vm::dir() const
{
    return m_dir;
}

QString Vm::argsPath() const
{
    return m_dir + '/' + kArgsFile;
}

QString Vm::name() const
{
    const QString name = VmConfig::name(m_args);
    return name.isEmpty() ? id() : name;
}

bool Vm::save(const ArgsFile &args, QString *error)
{
    QSaveFile f(argsPath());

    if (!f.open(QIODevice::WriteOnly) || f.write(args.toText().toUtf8()) < 0 ||
        !f.commit()) {
        if (error) {
            *error = f.errorString();
        }
        return false;
    }
    m_args = args;
    emit changed();
    return true;
}

void Vm::reload()
{
    QFile f(argsPath());
    ArgsFile args;

    if (f.open(QIODevice::ReadOnly)) {
        args = ArgsFile::parse(QString::fromUtf8(f.readAll()));
    }
    if (args.toText() != m_args.toText()) {
        m_args = args;
        emit changed();
    }
}

VmStore::VmStore(const QString &dir, QObject *parent)
    : QObject(parent), m_dir(QDir(dir).absolutePath()),
      m_watcher(new QFileSystemWatcher(this))
{
    QDir().mkpath(m_dir);
    m_watcher->addPath(m_dir);
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, &VmStore::reload);
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString &path) {
        for (Vm *vm : std::as_const(m_vms)) {
            if (vm->argsPath() == path) {
                vm->reload();
                /* saving replaces the file, which ends the watch */
                if (QFileInfo::exists(path)) {
                    m_watcher->addPath(path);
                }
            }
        }
    });
    reload();
}

QList<Vm *> VmStore::vms() const
{
    QList<Vm *> list = m_vms;

    std::sort(list.begin(), list.end(), [](const Vm *a, const Vm *b) {
        return QString::localeAwareCompare(a->name(), b->name()) < 0;
    });
    return list;
}

Vm *VmStore::find(const QString &id) const
{
    for (Vm *vm : m_vms) {
        if (vm->id() == id) {
            return vm;
        }
    }
    return nullptr;
}

Vm *VmStore::create(const QString &name, QString *error)
{
    static const QRegularExpression unsafe("[^\\w.-]+",
                                           QRegularExpression::UseUnicodePropertiesOption);
    const QDir dir(m_dir);
    QString base = name.trimmed();
    QString id;

    base.replace(unsafe, "-");
    while (base.startsWith('-') || base.startsWith('.')) {
        base.remove(0, 1);
    }
    while (base.endsWith('-')) {
        base.chop(1);
    }
    if (base.isEmpty()) {
        base = "vm";
    }
    id = base;
    for (int n = 2; dir.exists(id); n++) {
        id = QString("%1-%2").arg(base).arg(n);
    }
    if (!dir.mkpath(id)) {
        if (error) {
            *error = tr("Cannot create %1").arg(dir.filePath(id));
        }
        return nullptr;
    }

    ArgsFile args;
    Vm *vm = new Vm(dir.filePath(id), this);

    VmConfig::setName(args, name.trimmed());
    if (!vm->save(args, error)) {
        delete vm;
        QDir(dir.filePath(id)).removeRecursively();
        return nullptr;
    }
    m_vms << vm;
    m_watcher->addPath(vm->argsPath());
    emit added(vm);
    return vm;
}

bool VmStore::remove(Vm *vm, QString *error)
{
    if (vm->runner()->isActive()) {
        if (error) {
            *error = tr("%1 is running").arg(vm->name());
        }
        return false;
    }
    if (!QFile::moveToTrash(vm->dir())) {
        if (error) {
            *error = tr("Cannot move %1 to the trash").arg(vm->dir());
        }
        return false;
    }
    m_watcher->removePath(vm->argsPath());
    m_vms.removeOne(vm);
    emit removed(vm->id());
    vm->deleteLater();
    return true;
}

void VmStore::reload()
{
    const QDir dir(m_dir);
    QStringList present;

    for (const QString &id : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (QFileInfo::exists(dir.filePath(id) + '/' + kArgsFile)) {
            present << id;
        }
    }
    for (Vm *vm : QList<Vm *>(m_vms)) {
        if (!present.contains(vm->id()) && !vm->runner()->isActive()) {
            m_vms.removeOne(vm);
            emit removed(vm->id());
            vm->deleteLater();
        }
    }
    for (const QString &id : std::as_const(present)) {
        if (!find(id)) {
            Vm *vm = new Vm(dir.filePath(id), this);
            m_vms << vm;
            m_watcher->addPath(vm->argsPath());
            emit added(vm);
        }
    }
}

bool createDiskImage(const QString &path, qint64 bytes, QString *error)
{
    const QString qemuImg = Paths::qemuImg();
    QProcess p;

    if (qemuImg.isEmpty()) {
        if (error) {
            *error = QObject::tr("qemu-img not found");
        }
        return false;
    }
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(qemuImg, {"create", "-q", "-f", "qcow2", path, QString::number(bytes)});
    if (!p.waitForFinished(60000) || p.exitStatus() != QProcess::NormalExit ||
        p.exitCode() != 0) {
        if (error) {
            const QString output = QString::fromLocal8Bit(p.readAll()).trimmed();
            *error = output.isEmpty() ? p.errorString() : output;
        }
        return false;
    }
    return true;
}
