// SPDX-License-Identifier: GPL-2.0-or-later
#include "gpuinputawake.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>

#include <cstring>

#include "core/hostgpus.h"
#include "core/stackbuilder.h"

static const char kScript[] = "gpu-input-awake";
static const char kUnit[] = "gpu-input-awake.service";
/* install.sh's lines for the app */
static const char kPrefix[] = "vitrine-awake: ";
/* pkexec's line when it does not run the program: "...: Not authorized" */
static const char kPkexecError[] = "Error executing command as another user: ";

static bool s_running = false;

static QString tr(const char *text)
{
    return QCoreApplication::translate("GpuInputAwake", text);
}

static QString executable(const char *name, const char *fallback)
{
    const QString found = QStandardPaths::findExecutable(QString::fromLatin1(name));
    return found.isEmpty() ? QString::fromLatin1(fallback) : found;
}

/* Empty when it cannot be read */
static QByteArray sha256(const QString &path)
{
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);

    return file.open(QIODevice::ReadOnly) && hash.addData(&file) ? hash.result() : QByteArray();
}

namespace GpuInputAwake {

Kind kind(const HostGpu &gpu, const QString &sysRoot)
{
    if (gpu.driver == QLatin1String("xe")) {
        return Kind::InputWake;
    }
    if (gpu.driver != QLatin1String("amdgpu")) {
        return Kind::None;
    }
    /* the header: structure_size (16 bits), format_revision, content_revision */
    QFile metrics(sysRoot + "/bus/pci/devices/" + gpu.address + "/gpu_metrics");
    const QByteArray header = metrics.open(QIODevice::ReadOnly) ? metrics.read(4) : QByteArray();
    return header.size() >= 4 && (header[2] == 2 || header[2] == 3) ? Kind::ClockFloor
                                                                     : Kind::InputWake;
}

QList<HostGpu> served(const QList<HostGpu> &gpus, const QString &sysRoot)
{
    QList<HostGpu> out;

    for (const HostGpu &gpu : gpus) {
        if (kind(gpu, sysRoot) == Kind::InputWake) {
            out << gpu;
        }
    }
    return out;
}

QString scriptPath(const QString &root)
{
    return root + "/usr/local/sbin/" + kScript;
}

QString unitPath(const QString &root)
{
    return root + "/etc/systemd/system/" + kUnit;
}

QString unitName()
{
    return QString::fromLatin1(kUnit);
}

QString sourceDir()
{
    const QString chosen = qEnvironmentVariable("VITRINE_AWAKE_DIR");
    const QString host = chosen.isEmpty() ? StackBuilder::hostDir() : QString();
    const QString dir = !chosen.isEmpty() ? chosen : host.isEmpty() ? QString() : host + "/gpu-input-awake";

    if (dir.isEmpty() || !QFileInfo(dir + "/install.sh").isFile() ||
        !QFileInfo(dir + '/' + kScript).isFile() || !QFileInfo(dir + '/' + kUnit).isFile()) {
        return QString();
    }
    return QFileInfo(dir).canonicalFilePath();
}

Installed installed(const QString &source, const QString &root)
{
    const bool script = QFileInfo::exists(scriptPath(root));
    const bool unit = QFileInfo::exists(unitPath(root));

    if (!script && !unit) {
        return Installed::No;
    }
    if (!script || !unit) {
        return Installed::Partial;
    }
    const QByteArray ours = source.isEmpty() ? QByteArray() : sha256(source + '/' + kScript);
    const QByteArray ourUnit = source.isEmpty() ? QByteArray() : sha256(source + '/' + kUnit);
    return !ours.isEmpty() && !ourUnit.isEmpty() && sha256(scriptPath(root)) == ours &&
                   sha256(unitPath(root)) == ourUnit
               ? Installed::Vitrine
               : Installed::Other;
}

QStringList command(const QString &source, bool install)
{
    /* the desktop's polkit agent asks for the password, never a terminal */
    return {executable("pkexec", "/usr/bin/pkexec"), "--disable-internal-agent",
            executable("bash", "/bin/bash"), source + "/install.sh",
            install ? QStringLiteral("install") : QStringLiteral("remove")};
}

Result result(int code, const QString &output, QString *error)
{
    const qsizetype prefix = qsizetype(strlen(kPrefix));
    QString why;
    bool done = false;

    for (const QString &line : output.split('\n', Qt::SkipEmptyParts)) {
        if (line.startsWith(QLatin1String(kPrefix))) {
            const QString what = line.mid(prefix).trimmed();
            done |= what == QLatin1String("done");
            if (why.isEmpty() && what.startsWith(QLatin1String("error: "))) {
                why = what.mid(7);
            }
        } else if (why.isEmpty() && line.startsWith(QLatin1String(kPkexecError))) {
            /* pkexec's: not authorized, no polkit agent... */
            why = line.mid(qsizetype(strlen(kPkexecError))).trimmed();
        }
    }
    if (code == 0 && done) {
        return Result::Done;
    }
    /* pkexec: 126 when the password dialog was dismissed (GNOME Shell's
       agent); KDE's agent gives "Not authorized" (127) for Cancel, as for a
       password that did not do.  install.sh exits with 1 or 2. */
    if (code == 126 || (code == 127 && why == QLatin1String("Not authorized"))) {
        return Result::Cancelled;
    }
    if (why.endsWith('.')) {
        why.chop(1);
    }
    if (why.isEmpty()) {
        why = code < 0 ? tr("the installer stopped unexpectedly")
                       : tr("the installer failed (status %1)").arg(code);
    }
    if (error) {
        *error = why;
    }
    return Result::Failed;
}

bool run(QObject *context, bool install,
         const std::function<void(Result result, const QString &error)> &done,
         const QStringList &commandLine)
{
    const QString source = commandLine.isEmpty() ? sourceDir() : QString();
    QPointer<QObject> guard(context);

    if (s_running) {
        return false;
    }
    if (commandLine.isEmpty() && source.isEmpty()) {
        done(Result::Failed, tr("this installation of Vitrine has no host/gpu-input-awake"));
        return true;
    }
    const QStringList line = commandLine.isEmpty() ? command(source, install) : commandLine;
    /* the app's, not @context's: an install started goes on to its end, its
       window closed or not (pkexec, then root's) */
    auto *process = new QProcess(QCoreApplication::instance());
    process->setProcessChannelMode(QProcess::MergedChannels);
    s_running = true;
    QObject::connect(process, &QProcess::finished, process,
                     [process, guard, done](int code, QProcess::ExitStatus status) {
        QString error;
        const Result r = result(status == QProcess::NormalExit ? code : -1,
                                QString::fromUtf8(process->readAll()), &error);
        s_running = false;
        process->deleteLater();
        if (guard) {
            done(r, error);
        }
    });
    QObject::connect(process, &QProcess::errorOccurred, process,
                     [process, guard, done](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) {
            return;
        }
        s_running = false;
        process->deleteLater();
        if (guard) {
            done(Result::Failed, tr("pkexec is not installed"));
        }
    });
    process->start(line.first(), line.mid(1));
    return true;
}

bool running()
{
    return s_running;
}

void serviceState(QObject *context,
                  const std::function<void(const QString &active, const QString &enabled)> &done)
{
    auto *process = new QProcess(context);

    QObject::connect(process, &QProcess::finished, context, [process, done]() {
        QString active, enabled;
        /* "ActiveState=active", "UnitFileState=enabled" */
        for (const QString &line : QString::fromUtf8(process->readAllStandardOutput()).split('\n')) {
            if (line.startsWith(QLatin1String("ActiveState="))) {
                active = line.section('=', 1).trimmed();
            } else if (line.startsWith(QLatin1String("UnitFileState="))) {
                enabled = line.section('=', 1).trimmed();
            }
        }
        process->deleteLater();
        done(active, enabled);
    });
    QObject::connect(process, &QProcess::errorOccurred, context, [process, done](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            process->deleteLater();
            done(QString(), QString());
        }
    });
    process->start("systemctl", {"show", "--property=ActiveState,UnitFileState", kUnit});
}

}
