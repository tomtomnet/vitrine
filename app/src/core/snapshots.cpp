// SPDX-License-Identifier: GPL-2.0-or-later
#include "snapshots.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>

#include <algorithm>
#include <functional>

#include "core/paths.h"
#include "core/qmpclient.h"
#include "core/vmconfig.h"
#include "core/vmhardware.h"
#include "core/vmrunner.h"

namespace {

/* qemu-img's exit code and what it printed; @started is false if it could not run */
using Done = std::function<void(bool started, int exitCode, const QByteArray &out,
                                const QString &err)>;

}

struct VmSnapshots::Private
{
    VmSnapshots *q;
    QPointer<VmRunner> runner;
    ArgsFile args;
    QString dir;
    QList<Snapshot> snapshots;
    bool busy = false;
    /* of the listing: the answer to an older one is dropped */
    int generation = 0;

    /* QEMU answers: running or paused, QMP ready */
    bool live() const
    {
        return runner && runner->qmp() && (runner->state() == VmRunner::State::Running ||
                                           runner->state() == VmRunner::State::Paused);
    }

    /* The qemu-img of the VM's own QEMU, else the one of the preferences */
    QString qemuImg() const
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

    /* The files qemu-img takes snapshots of, which exist */
    QList<Drive> snapshotDrives() const
    {
        QList<Drive> list;
        for (const Drive &drive : VmSnapshots::drives(args, dir)) {
            if (drive.canSnapshot() && QFileInfo::exists(drive.file)) {
                list << drive;
            }
        }
        return list;
    }

    const Snapshot *find(const QString &name) const
    {
        for (const Snapshot &s : snapshots) {
            if (s.name == name) {
                return &s;
            }
        }
        return nullptr;
    }

    void run(const QStringList &arguments, const Done &done)
    {
        auto *p = new QProcess(q);
        auto called = std::make_shared<bool>(false);

        p->setWorkingDirectory(dir);
        QObject::connect(p, &QProcess::finished, q, [p, done, called](int code, QProcess::ExitStatus status) {
            if (!std::exchange(*called, true)) {
                done(true, status == QProcess::NormalExit ? code : -1, p->readAllStandardOutput(),
                     QString::fromLocal8Bit(p->readAllStandardError()).trimmed());
            }
            p->deleteLater();
        });
        QObject::connect(p, &QProcess::errorOccurred, q, [p, done, called](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart && !std::exchange(*called, true)) {
                done(false, -1, {}, p->errorString());
                p->deleteLater();
            }
        });
        p->start(qemuImg(), arguments);
    }

    /* @commands of qemu-img in turn, up to an error unless @all; @done gets the first */
    void runAll(QList<QStringList> commands, const std::function<void(QString)> &done,
                bool all = false, const QString &firstError = {})
    {
        if (commands.isEmpty()) {
            done(firstError);
            return;
        }
        const QStringList command = commands.takeFirst();
        run(command, [this, commands, done, all, firstError](bool started, int code,
                                                             const QByteArray &,
                                                             const QString &err) {
            QString error = firstError;
            if ((!started || code != 0) && error.isEmpty()) {
                error = err.isEmpty() ? VmSnapshots::tr("qemu-img ended with %1").arg(code) : err;
            }
            if (!error.isEmpty() && !all) {
                done(error);
                return;
            }
            runAll(commands, done, all, error);
        });
    }

    /* The listing of the stopped VM, from @drives[@i] on */
    void info(QList<Drive> drives, QList<std::pair<QString, QJsonArray>> images, int generation)
    {
        if (generation != this->generation) {
            return;
        }
        if (drives.isEmpty()) {
            snapshots = VmSnapshots::merge(images);
            emit q->listed({});
            return;
        }
        const Drive drive = drives.takeFirst();
        run({"info", "--output=json", "-U", drive.file},
            [this, drive, drives, images, generation](bool started, int code,
                                                       const QByteArray &out,
                                                       const QString &err) mutable {
            if (generation != this->generation) {
                return;
            }
            if (!started || code != 0) {
                snapshots.clear();
                emit q->listed(QString("%1: %2").arg(QFileInfo(drive.file).fileName(), err));
                return;
            }
            images.append({drive.file, QJsonDocument::fromJson(out)["snapshots"].toArray()});
            info(drives, images, generation);
        });
    }

    /* HMP @command, through QMP; @done gets its error */
    void hmp(const QString &command, const std::function<void(QString)> &done)
    {
        const QPointer<VmSnapshots> self(q);

        runner->qmp()->execute("human-monitor-command", {{"command-line", command}},
                               [self, done](const QJsonValue &result, const QString &error) {
            if (self) {
                done(error.isEmpty() ? VmSnapshots::hmpError(result.toString()) : error);
            }
        });
    }

    void setBusy(bool on)
    {
        if (busy != on) {
            busy = on;
            emit q->busyChanged(on);
        }
    }

    void end(const QString &error)
    {
        setBusy(false);
        q->refresh();
        emit q->finished(VmSnapshots::explain(error));
    }

    /*
     * The qcow2 disks of the running VM, not its state, all at the same point:
     * what the menu of qemu-gui does when QEMU cannot save the state
     */
    void takeDisks(const QString &name, const QString &savevmError)
    {
        const QPointer<VmSnapshots> self(q);

        runner->qmp()->execute("query-block", {}, [=, this](const QJsonValue &result,
                                                           const QString &error) {
            if (!self) {
                return;
            }
            const QJsonArray actions = VmSnapshots::diskActions(result.toArray(), name);
            if (!error.isEmpty() || actions.isEmpty() || !live()) {
                end(error.isEmpty() ? savevmError : error);
                return;
            }
            /* one of that name goes, as savevm would replace it */
            hmp("delvm " + VmSnapshots::hmpQuote(name), [=, this](const QString &) {
                if (!live()) {
                    end(savevmError);
                    return;
                }
                runner->qmp()->execute("transaction", {{"actions", actions}},
                                       [=, this](const QJsonValue &, const QString &error) {
                    if (!self) {
                        return;
                    }
                    if (error.isEmpty()) {
                        emit q->notice(VmSnapshots::disksOnlyReason(savevmError));
                    }
                    end(error);
                });
            });
        });
    }

    /* Nothing to do it with, or on */
    bool cannot(const QString &name, bool needsName = true)
    {
        QString error;

        if (busy) {
            error = VmSnapshots::tr("Another snapshot action is under way");
        } else if (needsName && !VmSnapshots::isValidName(name)) {
            error = VmSnapshots::tr("A snapshot needs a name that is not only digits");
        } else if (!live() && runner && runner->isActive()) {
            error = VmSnapshots::tr("The VM is starting or stopping: wait for it");
        } else if (!live() && qemuImg().isEmpty()) {
            error = VmSnapshots::tr("qemu-img was not found: install it with QEMU "
                                    "(the qemu-img package)");
        } else if (!live() && snapshotDrives().isEmpty()) {
            error = VmSnapshots::tr("The VM has no qcow2 disk to keep snapshots in");
        }
        if (!error.isEmpty()) {
            emit q->finished(error);
            return true;
        }
        return false;
    }
};

VmSnapshots::VmSnapshots(VmRunner *runner, QObject *parent)
    : QObject(parent), d(new Private{this, runner, {}, {}, {}, false, 0})
{
}

VmSnapshots::~VmSnapshots()
{
    delete d;
}

void VmSnapshots::setVm(const ArgsFile &args, const QString &dir)
{
    d->args = args;
    d->dir = dir;
}

QList<VmSnapshots::Snapshot> VmSnapshots::snapshots() const
{
    return d->snapshots;
}

bool VmSnapshots::isBusy() const
{
    return d->busy;
}

void VmSnapshots::refresh()
{
    const int generation = ++d->generation;

    if (d->live()) {
        const QPointer<VmSnapshots> self(this);
        d->runner->qmp()->execute("query-block", {},
                                  [this, self, generation](const QJsonValue &result,
                                                           const QString &error) {
            /* gone with the VM shown, or asked again since */
            if (!self || generation != d->generation) {
                return;
            }
            d->snapshots = error.isEmpty() ? merge(images(result.toArray())) : QList<Snapshot>();
            emit listed(error);
        });
        return;
    }
    if (d->runner && d->runner->isActive()) {
        /* starting or stopping: QEMU has the files */
        d->snapshots.clear();
        emit listed({});
        return;
    }
    const QList<Drive> drives = d->snapshotDrives();
    if (drives.isEmpty()) {
        d->snapshots.clear();
        emit listed({});
        return;
    }
    if (d->qemuImg().isEmpty()) {
        d->snapshots.clear();
        emit listed(tr("qemu-img was not found: install it with QEMU (the qemu-img package)"));
        return;
    }
    d->info(drives, {}, generation);
}

void VmSnapshots::take(const QString &name)
{
    if (d->cannot(name)) {
        return;
    }
    d->setBusy(true);
    if (d->live()) {
        /* replaces the snapshot of that name, as qemu-img does not */
        d->hmp("savevm " + hmpQuote(name), [this, name](const QString &error) {
            if (needsDisksOnly(error)) {
                d->takeDisks(name, error);
            } else {
                d->end(error);
            }
        });
        return;
    }
    QList<QStringList> removals, creations;
    const Snapshot *old = d->find(name);
    for (const Drive &drive : d->snapshotDrives()) {
        if (old && old->files.contains(drive.file)) {
            removals << QStringList{"snapshot", "-d", name, drive.file};
        }
        creations << QStringList{"snapshot", "-c", name, drive.file};
    }
    d->runAll(removals, [this, name, creations](const QString &error) {
        if (!error.isEmpty()) {
            d->end(error);
            return;
        }
        d->runAll(creations, [this, name, creations](const QString &error) {
            if (error.isEmpty()) {
                d->end({});
                return;
            }
            /* none, rather than some of the disks */
            QList<QStringList> undo;
            for (const QStringList &c : creations) {
                undo << QStringList{"snapshot", "-d", name, c.last()};
            }
            d->runAll(undo, [this, error](const QString &) { d->end(error); }, true);
        });
    });
}

void VmSnapshots::restore(const QString &name)
{
    const Snapshot *snapshot = d->find(name);

    if (d->cannot(name, false)) {
        return;
    }
    if (!snapshot) {
        emit finished(tr("There is no snapshot %1").arg(name));
        return;
    }
    if (d->live() && snapshot->stateBytes == 0) {
        emit finished(explain("This is a disk-only snapshot"));
        return;
    }
    d->setBusy(true);
    if (d->live()) {
        d->hmp("loadvm " + hmpQuote(name), [this](const QString &error) { d->end(error); });
        return;
    }
    QList<QStringList> commands;
    for (const QString &file : snapshot->files) {
        commands << QStringList{"snapshot", "-a", name, file};
    }
    d->runAll(commands, [this](const QString &error) { d->end(error); });
}

void VmSnapshots::remove(const QString &name)
{
    const Snapshot *snapshot = d->find(name);

    if (d->cannot(name, false)) {
        return;
    }
    if (!snapshot) {
        emit finished(tr("There is no snapshot %1").arg(name));
        return;
    }
    d->setBusy(true);
    if (d->live()) {
        d->hmp("delvm " + hmpQuote(name), [this](const QString &error) { d->end(error); });
        return;
    }
    QList<QStringList> commands;
    for (const QString &file : snapshot->files) {
        commands << QStringList{"snapshot", "-d", name, file};
    }
    d->runAll(commands, [this](const QString &error) { d->end(error); });
}

QList<VmSnapshots::Drive> VmSnapshots::drives(const ArgsFile &args, const QString &dir)
{
    QList<Drive> list;
    const QDir base(dir);

    for (const VmConfig::Disk &disk : VmConfig::disks(args)) {
        const ArgsFile::Line &line = args.lines[disk.line];
        if (disk.cdrom || disk.file.isEmpty()) {
            continue;
        }
        if (line.name == "drive" || line.name == "blockdev") {
            const OptionValue v = args.valueAt(disk.line);
            /* not written to, or written to a temporary file */
            if (v.flag("readonly") || v.flag("read-only") || v.flag("snapshot")) {
                continue;
            }
        }
        list << Drive{base.absoluteFilePath(disk.file),
                      disk.format.isEmpty() ? VmConfig::diskFormat(disk.file) : disk.format,
                      false};
    }
    for (int i : args.indexesOf("drive")) {
        const OptionValue v = args.valueAt(i);
        const QString file = v.get("file");
        if (v.get("if") != "pflash" || file.isEmpty() || v.flag("readonly")) {
            continue;
        }
        list << Drive{base.absoluteFilePath(file), v.get("format", VmConfig::diskFormat(file)),
                      true};
    }
    for (int i : args.indexesOf("pflash")) {
        list << Drive{base.absoluteFilePath(args.lines[i].value), "raw", true};
    }
    return list;
}

QList<VmSnapshots::Snapshot> VmSnapshots::merge(
    const QList<std::pair<QString, QJsonArray>> &images)
{
    QList<Snapshot> list;
    QHash<QString, qsizetype> byName;

    for (const auto &[file, snapshots] : images) {
        for (const QJsonValue &value : snapshots) {
            const QJsonObject o = value.toObject();
            const QString name = o["name"].toString();
            const QDateTime date = QDateTime::fromMSecsSinceEpoch(
                o["date-sec"].toInteger() * 1000 + o["date-nsec"].toInteger() / 1000000);
            const qint64 clock =
                o["vm-clock-sec"].toInteger() * 1000 + o["vm-clock-nsec"].toInteger() / 1000000;
            const qint64 state = o["vm-state-size"].toInteger();
            const auto it = byName.constFind(name);

            if (it == byName.constEnd()) {
                byName.insert(name, list.size());
                list << Snapshot{name, date, clock, state, {file}};
                continue;
            }
            /* the running state is in one of the files */
            Snapshot &s = list[*it];
            if (!s.files.contains(file)) {
                s.files << file;
            }
            s.date = qMax(s.date, date);
            s.vmClockMs = qMax(s.vmClockMs, clock);
            s.stateBytes = qMax(s.stateBytes, state);
        }
    }
    std::stable_sort(list.begin(), list.end(),
                     [](const Snapshot &a, const Snapshot &b) { return a.date < b.date; });
    return list;
}

QList<std::pair<QString, QJsonArray>> VmSnapshots::images(const QJsonArray &queryBlock)
{
    QList<std::pair<QString, QJsonArray>> list;

    for (const QJsonValue &block : queryBlock) {
        const QJsonObject inserted = block["inserted"].toObject();
        /* no medium, or a CD/DVD: not written to */
        if (inserted.isEmpty() || inserted["ro"].toBool()) {
            continue;
        }
        list.append({inserted["file"].toString(),
                     inserted["image"].toObject()["snapshots"].toArray()});
    }
    return list;
}

QString VmSnapshots::hmpError(const QString &output)
{
    const QString text = output.trimmed();

    return text.startsWith("Error:") ? text.mid(6).trimmed() : QString();
}

QString VmSnapshots::hmpQuote(const QString &text)
{
    return '"' + QString(text).replace('\\', "\\\\").replace('"', "\\\"") + '"';
}

bool VmSnapshots::isValidName(const QString &name)
{
    static const QRegularExpression digits("^[0-9]+$");

    return !name.trimmed().isEmpty() && name == name.trimmed() && !digits.match(name).hasMatch();
}

bool VmSnapshots::needsDisksOnly(const QString &error)
{
    /* devices QEMU cannot migrate (virgl, VFIO...), or raw files it writes to */
    return error.contains("migrat", Qt::CaseInsensitive) ||
           error.contains("does not support snapshots");
}

QString VmSnapshots::disksOnlyReason(const QString &error)
{
    static const QRegularExpression writable("Device '([^']+)' is writable but does not "
                                             "support snapshots");
    const QRegularExpressionMatch m = writable.match(error);

    if (error.contains("virgl")) {
        return tr("This snapshot holds the disks only: QEMU cannot save the running state of "
                  "a VM with 3D graphics (virgl).");
    }
    if (m.hasMatch()) {
        return tr("This snapshot holds the qcow2 disks only: %1 is not in qcow2, so QEMU "
                  "cannot save the running state.")
            .arg(m.captured(1).startsWith("pflash") ? tr("the UEFI variables")
                                                    : m.captured(1));
    }
    return tr("This snapshot holds the disks only: QEMU cannot save the running state of "
              "this VM (%1).").arg(error.trimmed());
}

QJsonArray VmSnapshots::diskActions(const QJsonArray &queryBlock, const QString &name)
{
    QJsonArray actions;

    for (const QJsonValue &block : queryBlock) {
        const QJsonObject inserted = block["inserted"].toObject();
        if (inserted.isEmpty() || inserted["ro"].toBool() ||
            inserted["drv"].toString() != "qcow2") {
            continue;
        }
        /* the drive's name, else the node's of a -blockdev */
        const QString device = block["device"].toString().isEmpty()
                                   ? inserted["node-name"].toString()
                                   : block["device"].toString();
        actions.append(QJsonObject{{"type", "blockdev-snapshot-internal-sync"},
                                   {"data", QJsonObject{{"device", device}, {"name", name}}}});
    }
    return actions;
}

QString VmSnapshots::explain(const QString &error)
{
    static const QRegularExpression writable("Device '([^']+)' is writable but does not "
                                             "support snapshots");
    const QRegularExpressionMatch m = writable.match(error);

    if (m.hasMatch() && m.captured(1).startsWith("pflash")) {
        return tr("The UEFI variables of the VM are in a raw file, and a snapshot of a "
                  "running VM needs every file it writes to in qcow2. Take snapshots with "
                  "the VM stopped, of the disks only, or give it UEFI variables in qcow2, "
                  "as new VMs have.");
    }
    if (m.hasMatch()) {
        return tr("The disk %1 is not in qcow2, and a snapshot of a running VM needs every "
                  "disk it writes to in qcow2.").arg(m.captured(1));
    }
    if (error.contains("disk-only snapshot")) {
        return tr("This snapshot holds the disks only, not the running state: stop the VM "
                  "to go back to it.");
    }
    if (error.contains("does not exist in one or more devices")) {
        return tr("Not all the disks of the VM have this snapshot: %1").arg(error);
    }
    return error;
}
