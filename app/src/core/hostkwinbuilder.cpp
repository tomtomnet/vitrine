// SPDX-License-Identifier: GPL-2.0-or-later
#include "hostkwinbuilder.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#include <csignal>
#include <sys/prctl.h>
#include <unistd.h>

#include "core/guesttoolsbuilder.h"
#include "core/hostkwin.h"
#include "core/hostmemory.h"

/* The log kept of a build */
static const qsizetype kMaxLog = 4 << 20;
/* What a stop may take: the script removes its container first */
static const int kStopMs = 20000;

/* 10, 6.3: for messages */
static QString gib(qint64 mib)
{
    return QString::number(double(mib) / 1024, 'f', mib % 1024 == 0 ? 0 : 1);
}

HostKwinBuilder *HostKwinBuilder::instance()
{
    static QPointer<HostKwinBuilder> builder;

    if (!builder) {
        builder = new HostKwinBuilder(QCoreApplication::instance());
    }
    return builder;
}

HostKwinBuilder::HostKwinBuilder(QObject *parent)
    : QObject(parent), m_meminfo("/proc/meminfo"), m_memoryTimer(new QTimer(this))
{
    m_memoryTimer->setInterval(2000);
    connect(m_memoryTimer, &QTimer::timeout, this, &HostKwinBuilder::checkMemory);
}

HostKwinBuilder::~HostKwinBuilder()
{
    if (!m_process) {
        return;
    }
    m_process->disconnect(this);
    /* build.sh removes its container, then ends */
    signalGroup(SIGTERM);
    if (!m_process->waitForFinished(kStopMs)) {
        signalGroup(SIGKILL);
        m_process->waitForFinished(1000);
    }
}

void HostKwinBuilder::setMemoryPoll(int ms)
{
    m_memoryTimer->setInterval(ms);
}

void HostKwinBuilder::start(const QString &plasma)
{
    if (m_running) {
        return;
    }
    m_running = true;
    m_cancelled = false;
    m_stopped = false;
    m_plasma = plasma;
    m_log.clear();
    m_partial.clear();
    m_missing.clear();
    m_install.clear();
    m_error.clear();
    m_failure.clear();
    m_built.clear();
    m_phase.clear();
    m_compiling = false;
    m_done = m_total = 0;
    emit started();
    checkMemory();
}

/* Before the container, which takes up to its cap: that much free, and what
   must stay free for the host (HostMemory), else a wait */
void HostKwinBuilder::checkMemory()
{
    const HostMemory::Info host = HostMemory::read(m_meminfo);
    const qint64 needed = GuestToolsBuilder::memoryCapMiB() + HostMemory::reserveMiB(host);
    const bool waiting = m_memoryTimer->isActive();

    if (!m_running || m_process) {
        m_memoryTimer->stop();
        return;
    }
    if (host.totalMiB <= 0 || host.availableMiB >= needed) {
        m_memoryTimer->stop();
        if (waiting) {
            emit waitingChanged();
        }
        run();
        return;
    }
    /* by 64 MiB: what is free changes all the time */
    if (!waiting || qAbs(host.availableMiB - m_available) >= 64 || needed != m_needed) {
        m_available = host.availableMiB;
        m_needed = needed;
        if (!waiting) {
            append(tr("Waiting for memory: KWin's build takes up to %1 GiB, and %2 GiB is free; "
                      "it starts once %3 GiB is.\n")
                       .arg(gib(GuestToolsBuilder::memoryCapMiB()), gib(host.availableMiB),
                            gib(needed)));
            m_memoryTimer->start();
        }
        emit waitingChanged();
    }
}

bool HostKwinBuilder::isWaiting() const
{
    return m_running && !m_process && m_memoryTimer->isActive();
}

void HostKwinBuilder::proceed()
{
    if (!isWaiting()) {
        return;
    }
    m_memoryTimer->stop();
    append(tr("Going on without the memory free.\n"));
    emit waitingChanged();
    run();
}

void HostKwinBuilder::run()
{
    const QString source = m_sourceDir.isEmpty() ? HostKwin::sourceDir() : m_sourceDir;
    const QString out = m_outDir.isEmpty() ? HostKwin::outDir() : m_outDir;
    const QString work = m_workDir.isEmpty() ? HostKwin::workDir() : m_workDir;
    const QString bash = QStandardPaths::findExecutable("bash");
    const int jobs = m_jobs > 0 ? m_jobs : qBound(1, QThread::idealThreadCount(), 8);
    QStringList args;

    if (source.isEmpty() || !QFileInfo(source + "/build.sh").isFile()) {
        finish(tr("This installation of Vitrine has no host/kwin/build.sh"));
        return;
    }
    args << source + "/build.sh" << "--out" << out << "--work" << work << "-j"
         << QString::number(jobs);
    if (!m_plasma.isEmpty()) {
        args << "--plasma" << m_plasma;
    }
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    /*
     * A process group of its own, which Stop signals as a whole: the script
     * removes its container, podman goes with it.  And stopped if Vitrine
     * goes without stopping it: the container would go on for its 15
     * minutes and its GiB.
     */
    const pid_t app = ::getpid();
    m_process->setChildProcessModifier([app]() {
        ::setpgid(0, 0);
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (::getppid() != app) {
            ::_exit(127);
        }
    });
    connect(m_process, &QProcess::readyRead, this, &HostKwinBuilder::readOutput);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_error = tr("Cannot run bash: %1").arg(m_process->errorString());
            done(-1, false);
        }
    });
    connect(m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        done(code, status == QProcess::CrashExit);
    });
    append("$ bash " + args.join(' ') + '\n');
    m_process->start(bash.isEmpty() ? QStringLiteral("/bin/bash") : bash, args);
    if (m_process) {
        m_pid = m_process->processId();
    }
}

void HostKwinBuilder::signalGroup(int signal)
{
    if (m_pid > 0) {
        ::kill(pid_t(-m_pid), signal);
    }
}

void HostKwinBuilder::cancel()
{
    if (!m_running) {
        return;
    }
    m_cancelled = true;
    if (!m_process) {
        /* waiting for memory */
        m_memoryTimer->stop();
        emit waitingChanged();
        finish(tr("Stopped"), true);
        return;
    }
    signalGroup(SIGTERM);
    QTimer::singleShot(kStopMs, m_process, [this]() { signalGroup(SIGKILL); });
}

void HostKwinBuilder::append(const QString &text)
{
    m_log += text;
    if (m_log.size() > kMaxLog) {
        const qsizetype cut = m_log.indexOf('\n', m_log.size() - kMaxLog * 3 / 4);
        m_log.remove(0, cut < 0 ? m_log.size() - kMaxLog * 3 / 4 : cut + 1);
    }
    emit output(text);
}

void HostKwinBuilder::readOutput()
{
    const QString text = QString::fromLocal8Bit(m_process->readAll());
    qsizetype end;

    append(text);
    m_partial += text;
    while ((end = m_partial.indexOf('\n')) >= 0) {
        handleLine(m_partial.left(end));
        m_partial.remove(0, end + 1);
    }
}

void HostKwinBuilder::setPhase(const QString &text, bool compiling)
{
    m_compiling = compiling;
    m_done = m_total = 0;
    if (text != m_phase) {
        m_phase = text;
        emit phaseChanged(text);
    }
    emit progress(0, 0);
}

void HostKwinBuilder::handleLine(const QString &text)
{
    /* rpmbuild's parts of the build, as the guest tools' builds name them */
    static const QHash<QString, QString> sections = {
        {"prep", tr("unpacking and patching the sources")},
        {"conf", tr("configuring")},
        {"build", tr("compiling")},
        {"install", tr("installing")},
        {"check", tr("checking")},
        {"files", tr("making the packages")},
    };
    using Line = GuestToolsBuilder::Line;
    const Line line = Line::parse(text);

    switch (line.kind) {
    case Line::Phase:
        setPhase(line.text, false);
        break;
    case Line::Section:
        if (const auto it = sections.find(line.text); it != sections.end() && *it != m_phase) {
            setPhase(*it, line.text == "build");
        }
        break;
    case Line::Counted:
        if (m_compiling && line.total > 0 && line.done <= line.total) {
            m_done = line.done;
            m_total = line.total;
            emit progress(m_done, m_total);
        }
        break;
    case Line::Percent:
        /* make's jobs each tell their own: the most */
        if (m_compiling && (m_total != 100 || line.done > m_done)) {
            m_done = line.done;
            m_total = 100;
            emit progress(m_done, m_total);
        }
        break;
    case Line::Missing:
        if (!m_missing.contains(line.text)) {
            m_missing << line.text;
        }
        break;
    case Line::Install:
        m_install = line.text;
        break;
    case Line::Error:
        m_error = line.text;
        break;
    case Line::Built:
        m_built = line.text;
        break;
    default:
        break;
    }
}

void HostKwinBuilder::done(int code, bool crashed)
{
    if (!m_process) {
        return;
    }
    if (!m_partial.isEmpty()) {
        handleLine(m_partial);
        m_partial.clear();
    }
    m_process->deleteLater();
    m_process = nullptr;
    m_pid = 0;
    if (m_cancelled) {
        finish(tr("Stopped"), true);
        return;
    }
    if (code != 0 || crashed) {
        QString error = !m_missing.isEmpty() ? tr("Build dependencies are missing")
                        : !m_error.isEmpty() ? m_error
                                             : tr("The build failed (status %1)").arg(code);
        error[0] = error[0].toUpper();
        finish(error);
        return;
    }
    if (m_built.isEmpty()) {
        finish(tr("build.sh ended without a build"));
        return;
    }
    finish({});
}

void HostKwinBuilder::finish(const QString &error, bool stopped)
{
    m_running = false;
    m_memoryTimer->stop();
    m_stopped = stopped;
    m_failure = stopped ? QString() : error;
    if (!error.isEmpty()) {
        append(error + '\n');
    }
    emit finished(error);
}
