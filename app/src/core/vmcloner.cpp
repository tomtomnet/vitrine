// SPDX-License-Identifier: GPL-2.0-or-later
#include "vmcloner.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRandomGenerator>
#include <QTimer>
#include <QUuid>

#include "core/vmconfig.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"

/* Whether the VM writes @file: a disk or its firmware variables, not a
   CD/DVD, a kernel, a memory file or a script */
static bool writes(const ArgsFile &args, const VmConfig::FileRef &file)
{
    const ArgsFile::Line &line = args.lines[file.line];
    const OptionValue v(line.value);

    if (line.name == "drive") {
        return v.get("if") == "pflash" ||
               (v.get("media") != "cdrom" && !v.flag("readonly", false));
    }
    if (line.name == "blockdev") {
        return !v.flag("read-only", false);
    }
    return QStringList{"hda", "hdb", "hdc", "hdd", "fda", "fdb", "pflash", "mtdblock",
                       "sd"}
        .contains(line.name);
}

static QString newMac()
{
    QRandomGenerator *random = QRandomGenerator::global();

    /* QEMU's own prefix */
    return QString::asprintf("52:54:00:%02x:%02x:%02x", random->bounded(256),
                             random->bounded(256), random->bounded(256));
}

VmCloner::VmCloner(VmStore *store, QObject *parent)
    : QObject(parent), m_store(store), m_timer(new QTimer(this))
{
    m_timer->setInterval(250);
    connect(m_timer, &QTimer::timeout, this, [this]() {
        if (!m_copies.isEmpty()) {
            emit progress(m_done + QFileInfo(m_copies.first().to).size(), m_total);
        }
    });
}

VmCloner::~VmCloner()
{
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(3000);
    }
}

ArgsFile VmCloner::plan(const ArgsFile &args, const QString &name, const QString &from,
                        const QString &to, QList<Copy> *copies)
{
    const QDir source(from), target(to);
    ArgsFile clone = args;
    QStringList taken;

    VmConfig::setName(clone, name);
    for (const VmConfig::FileRef &file : VmConfig::files(args)) {
        const QString path = QDir::cleanPath(source.absoluteFilePath(file.path));
        const QString inside = source.relativeFilePath(path);

        if (!writes(args, file)) {
            /* shared: the relative ones pointed into the folder of the source */
            if (QDir::isRelativePath(file.path)) {
                VmConfig::setFile(clone, file, path);
            }
            continue;
        }
        /* in the source's folder: the same name; outside: a name of its own */
        QString copy = inside.startsWith("..") ? QFileInfo(path).fileName() : inside;
        for (int n = 2; taken.contains(copy); n++) {
            const QFileInfo fi(QFileInfo(path).fileName());
            copy = QString("%1-%2.%3").arg(fi.completeBaseName()).arg(n).arg(fi.suffix());
        }
        taken << copy;
        VmConfig::setFile(clone, file, copy);
        if (copies && QFileInfo::exists(path)) {
            *copies << Copy{path, target.filePath(copy)};
        }
    }

    for (int i = 0; i < clone.lines.size(); i++) {
        const ArgsFile::Line &line = clone.lines[i];
        OptionValue v(line.value);

        if (line.kind != ArgsFile::Line::Option) {
            continue;
        }
        if (line.name == "uuid") {
            clone.setValueAt(i, QUuid::createUuid().toString(QUuid::WithoutBraces));
        } else if ((line.name == "device" && v.has("mac")) ||
                   ((line.name == "net" || line.name == "nic") && v.has("macaddr"))) {
            v.set(v.has("mac") ? "mac" : "macaddr", newMac());
            clone.setValueAt(i, v);
        }
    }
    return clone;
}

QList<std::pair<QString, qint64>> VmCloner::filesToCopy(const Vm *vm)
{
    QList<Copy> copies;
    QList<std::pair<QString, qint64>> out;

    plan(vm->args(), vm->name(), vm->dir(), vm->dir(), &copies);
    for (const Copy &c : std::as_const(copies)) {
        out << std::pair(c.from, QFileInfo(c.from).size());
    }
    return out;
}

void VmCloner::start(Vm *source, const QString &name)
{
    QString error;

    if (isRunning()) {
        return;
    }
    if (source->runner()->isActive()) {
        emit finished(nullptr, tr("Shut %1 down first: its disks would be copied halfway "
                                  "through its writes.").arg(source->name()));
        return;
    }
    m_clone = m_store->create(name, &error);
    if (!m_clone) {
        emit finished(nullptr, error);
        return;
    }
    m_copies.clear();
    m_args = plan(source->args(), name, source->dir(), m_clone->dir(), &m_copies);
    m_done = 0;
    m_total = 0;
    for (const Copy &c : std::as_const(m_copies)) {
        m_total += QFileInfo(c.from).size();
    }
    m_timer->start();
    copyNext();
}

void VmCloner::copyNext()
{
    QString error;

    if (m_copies.isEmpty()) {
        Vm *clone = m_clone;
        m_timer->stop();
        m_clone = nullptr;
        if (!clone->save(m_args, &error)) {
            QDir(clone->dir()).removeRecursively();
            m_store->reload();
            emit finished(nullptr, error);
            return;
        }
        emit progress(m_total, m_total);
        emit finished(clone, {});
        return;
    }

    const Copy copy = m_copies.first();
    QDir().mkpath(QFileInfo(copy.to).absolutePath());
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        const QString output = QString::fromLocal8Bit(m_process->readAll()).trimmed();
        m_process->deleteLater();
        m_process = nullptr;
        if (status != QProcess::NormalExit || code != 0) {
            fail(output.isEmpty() ? tr("Copying %1 failed").arg(m_copies.first().from) : output);
            return;
        }
        m_done += QFileInfo(m_copies.takeFirst().from).size();
        copyNext();
    });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            const QString error = m_process->errorString();
            m_process->deleteLater();
            m_process = nullptr;
            fail(tr("Cannot run cp: %1").arg(error));
        }
    });
    /* instant on btrfs and XFS; sparse files stay sparse */
    m_process->start("cp", {"--reflink=auto", "--sparse=always", copy.from, copy.to});
}

void VmCloner::cancel()
{
    if (!isRunning()) {
        return;
    }
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(3000);
        m_process->deleteLater();
        m_process = nullptr;
    }
    fail(tr("Cancelled"));
}

void VmCloner::fail(const QString &error)
{
    Vm *clone = m_clone;

    m_timer->stop();
    m_copies.clear();
    m_clone = nullptr;
    /* a new folder, half made: nothing to keep */
    QDir(clone->dir()).removeRecursively();
    m_store->reload();
    emit finished(nullptr, error);
}
