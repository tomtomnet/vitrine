// SPDX-License-Identifier: GPL-2.0-or-later
#include "stackbuilder.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <csignal>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/types.h>
#include <unistd.h>

#include "core/guesttoolsbuilder.h"
#include "core/paths.h"

/* What a complete build has, written last */
static const char kManifest[] = "/share/vitrine/stack.conf";
/* build.sh's lines for the app start with it */
static const char kTag[] = "vitrine-build: ";
/* The log kept of a build: QEMU's is about 1 MB */
static const qsizetype kMaxLog = 4 << 20;

/*
 * NAME=value lines, as build.sh reads versions.conf: no shell, the name
 * from the start of the line, the value to its end less trailing blanks,
 * the last line of a name winning
 */
static QHash<QString, QString> parseConf(const QByteArray &conf)
{
    static const QRegularExpression line("^\\s*([A-Za-z_][A-Za-z0-9_]*)=(.*)$");
    QHash<QString, QString> values;

    for (const QByteArray &raw : conf.split('\n')) {
        const QRegularExpressionMatch m = line.match(QString::fromUtf8(raw));
        if (m.hasMatch()) {
            QString value = m.captured(2);
            while (!value.isEmpty() && value.back().isSpace()) {
                value.chop(1);
            }
            values.insert(m.captured(1), value);
        }
    }
    return values;
}

static QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

static bool isHostDir(const QString &dir)
{
    return QFileInfo(dir + "/build.sh").isFile() && QFileInfo(dir + "/versions.conf").isFile();
}

bool StackBuilder::Versions::isValid() const
{
    return !qemuUrl.isEmpty() && !qemuCommit.isEmpty() && !virglUrl.isEmpty() &&
           !virglCommit.isEmpty();
}

StackBuilder::Versions StackBuilder::Versions::parse(const QByteArray &conf)
{
    const QHash<QString, QString> values = parseConf(conf);

    return {values.value("QEMU_URL"), values.value("QEMU_COMMIT"), values.value("VIRGL_URL"),
            values.value("VIRGL_COMMIT")};
}

QString StackBuilder::Build::qemuBinary() const
{
    return isValid() ? prefix + "/bin/qemu-system-x86_64" : QString();
}

QString StackBuilder::Build::summary() const
{
    const int patches = int(qemuPatches.size());
    const QString qemu = qemuVersion.isEmpty() ? QStringLiteral("QEMU") : "QEMU " + qemuVersion;

    if (patches == 0) {
        return qemu;
    }
    return patches == 1 ? StackBuilder::tr("%1 + 1 patch").arg(qemu)
                        : StackBuilder::tr("%1 + %2 patches").arg(qemu).arg(patches);
}

StackBuilder::Build StackBuilder::Build::parse(const QString &prefix, const QByteArray &manifest)
{
    const QHash<QString, QString> values = parseConf(manifest);
    Build build;

    build.stamp = values.value("STAMP");
    if (build.stamp.isEmpty()) {
        return {};
    }
    build.prefix = prefix;
    build.built = QDateTime::fromString(values.value("BUILT"), Qt::ISODate);
    build.qemuCommit = values.value("QEMU_COMMIT");
    build.qemuVersion = values.value("QEMU_VERSION");
    build.qemuPatches = values.value("QEMU_PATCHES").split(' ', Qt::SkipEmptyParts);
    build.virglCommit = values.value("VIRGL_COMMIT");
    build.virglVersion = values.value("VIRGL_VERSION");
    build.virglPatches = values.value("VIRGL_PATCHES").split(' ', Qt::SkipEmptyParts);
    return build;
}

StackBuilder::Build StackBuilder::Build::read(const QString &prefix)
{
    const QString dir = QFileInfo(prefix).canonicalFilePath();

    if (dir.isEmpty()) {
        return {};
    }
    return parse(dir, readFile(dir + kManifest));
}

StackBuilder::Line StackBuilder::Line::parse(const QString &text)
{
    static const QRegularExpression ninja("^\\[(\\d+)/(\\d+)\\] ");
    static const QRegularExpression step("^step (\\d+)/(\\d+): (.*)$");
    static const QList<std::pair<QString, Kind>> kinds = {
        {"missing: ", Missing}, {"install: ", Install},   {"warning: ", Warning},
        {"up to date: ", UpToDate}, {"built: ", Built}, {"error: ", Error},
    };
    QString line = text;
    Line result;

    while (line.endsWith('\r') || line.endsWith('\n')) {
        line.chop(1);
    }
    if (!line.startsWith(kTag)) {
        const QRegularExpressionMatch m = ninja.match(line);
        if (m.hasMatch()) {
            result.kind = Progress;
            result.done = m.captured(1).toInt();
            result.total = m.captured(2).toInt();
        }
        result.text = line;
        return result;
    }
    line = line.mid(qsizetype(sizeof(kTag)) - 1);
    if (const QRegularExpressionMatch m = step.match(line); m.hasMatch()) {
        result.kind = Step;
        result.done = m.captured(1).toInt();
        result.total = m.captured(2).toInt();
        result.text = m.captured(3);
        return result;
    }
    for (const auto &[prefix, kind] : kinds) {
        if (line.startsWith(prefix)) {
            result.kind = kind;
            result.text = line.mid(prefix.size());
            return result;
        }
    }
    result.text = line;
    return result;
}

QString StackBuilder::hostDir()
{
    const QString chosen = qEnvironmentVariable("VITRINE_HOST_DIR");
    QStringList candidates;

    /* the tests', or a developer's: that one or none */
    if (!chosen.isEmpty()) {
        return isHostDir(chosen) ? QFileInfo(chosen).canonicalFilePath() : QString();
    }
    /* installed: bin/vitrine and share/vitrine/host; first, as the source
       tree it was built from may have moved on since */
    if (QCoreApplication::instance()) {
        candidates << QCoreApplication::applicationDirPath() + "/../share/vitrine/host";
    }
#ifdef VITRINE_HOST_SOURCE_DIR
    candidates << QStringLiteral(VITRINE_HOST_SOURCE_DIR);
#endif
    candidates << QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, "vitrine/host",
                                            QStandardPaths::LocateDirectory);
    for (const QString &dir : std::as_const(candidates)) {
        if (isHostDir(dir)) {
            return QFileInfo(dir).canonicalFilePath();
        }
    }
    return {};
}

QString StackBuilder::workDir()
{
    return Paths::cacheDir() + "/stack-build";
}

StackBuilder::Versions StackBuilder::versions(const QString &hostDir)
{
    return Versions::parse(readFile(hostDir + "/versions.conf"));
}

/*
 * As build.sh's patches_of: the *.patch files of patches/<component> and
 * of its folders (not deeper), in the C order of their paths.  Matched with
 * case, as bash's glob is: a .PATCH file is none; and neither matches
 * hidden files or folders.
 */
QStringList StackBuilder::patches(const QString &hostDir, const QString &component)
{
    const QString path = "patches/" + component;
    const QDir dir(hostDir + '/' + path);
    QStringList list;
    const auto add = [&list](const QDir &folder, const QString &prefix) {
        const QStringList names =
            folder.entryList({"*.patch"}, QDir::Files | QDir::CaseSensitive, QDir::NoSort);
        for (const QString &name : names) {
            list << prefix + name;
        }
    };

    if (hostDir.isEmpty()) {
        return {};
    }
    add(dir, path + '/');
    for (const QString &folder : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort)) {
        add(QDir(dir.filePath(folder)), path + '/' + folder + '/');
    }
    std::sort(list.begin(), list.end());
    return list;
}

/*
 * As build.sh: the sha256 of the lines that sha256sum prints for
 * versions.conf, build.sh, then the patches of qemu and of virglrenderer.
 */
QString StackBuilder::inputStamp(const QString &hostDir)
{
    QStringList files = {"versions.conf", "build.sh"};
    QByteArray lines;

    if (hostDir.isEmpty()) {
        return {};
    }
    files << patches(hostDir, "qemu") << patches(hostDir, "virglrenderer");
    for (const QString &name : std::as_const(files)) {
        QFile file(hostDir + '/' + name);
        QCryptographicHash hash(QCryptographicHash::Sha256);

        if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) {
            return {};
        }
        lines += hash.result().toHex() + "  " + name.toUtf8() + '\n';
    }
    return QString::fromLatin1(QCryptographicHash::hash(lines, QCryptographicHash::Sha256).toHex());
}

StackBuilder::Build StackBuilder::current(const QString &stackDir)
{
    const QFileInfo link(stackDir + "/current");

    return link.exists() ? Build::read(link.canonicalFilePath()) : Build();
}

StackBuilder::Build StackBuilder::current()
{
    return current(Paths::stackDir());
}

StackBuilder::State StackBuilder::state(const QString &hostDir, const QString &stackDir)
{
    const QString stamp = inputStamp(hostDir);
    const Build build = current(stackDir);

    if (stamp.isEmpty()) {
        return State::NoSources;
    }
    if (!build.isValid()) {
        return State::NotBuilt;
    }
    return build.stamp == stamp ? State::UpToDate : State::Outdated;
}

StackBuilder::State StackBuilder::state()
{
    return state(hostDir(), Paths::stackDir());
}

QStringList StackBuilder::prune(const QString &stackDir, const QStringList &keep)
{
    static const QRegularExpression stampFolder("^[0-9a-f]{16}$");
    const QString stack = QFileInfo(stackDir).canonicalFilePath();
    QSet<QString> needed;
    QStringList removed;

    if (stack.isEmpty()) {
        return {};
    }
    /* build.sh's: no build makes a prefix meanwhile, nor switches `current` */
    const int lock = ::open(QFile::encodeName(stack + "/.lock").constData(),
                            O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (lock < 0) {
        return {};
    }
    if (::flock(lock, LOCK_EX | LOCK_NB) != 0) {
        ::close(lock);
        return {};
    }
    /* the folder of the stack @path is in, if any */
    const auto folderOf = [&stack](const QString &path) {
        const QString canonical = QFileInfo(path).canonicalFilePath();
        return canonical.startsWith(stack + '/')
                   ? canonical.mid(stack.size() + 1).section('/', 0, 0) : QString();
    };
    needed << folderOf(stack + "/current");
    for (const QString &binary : keep) {
        needed << folderOf(binary);
    }
    /*
     * What runs, by argv[0]: VmRunner starts QEMU by the path of its build,
     * and /proc/PID/exe of a QEMU with file capabilities is not readable;
     * a QEMU keeps loading modules and firmware from its prefix
     */
    for (const QString &pid : QDir("/proc").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile cmdline("/proc/" + pid + "/cmdline");
        if (pid.front().isDigit() && cmdline.open(QIODevice::ReadOnly)) {
            needed << folderOf(QString::fromLocal8Bit(cmdline.readAll().split('\0').value(0)));
        }
    }
    for (const QString &name : QDir(stack).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (stampFolder.match(name).hasMatch() && !needed.contains(name) &&
            QDir(stack + '/' + name).removeRecursively()) {
            removed << stack + '/' + name;
        }
    }
    ::flock(lock, LOCK_UN);
    ::close(lock);
    return removed;
}

int StackBuilder::defaultJobs()
{
    static const QRegularExpression total("MemTotal:\\s+(\\d+) kB");
    const QRegularExpressionMatch m = total.match(QString::fromLatin1(readFile("/proc/meminfo")));
    const int memoryGiB = m.hasMatch() ? int(m.captured(1).toLongLong() >> 20) : 4;

    return qBound(1, qMin(QThread::idealThreadCount(), memoryGiB - 1), 64);
}

StackBuilder *StackBuilder::instance()
{
    static QPointer<StackBuilder> builder;

    if (!builder) {
        builder = new StackBuilder(QCoreApplication::instance());
    }
    return builder;
}

StackBuilder::StackBuilder(QObject *parent) : QObject(parent)
{
}

StackBuilder::~StackBuilder()
{
    if (!m_process) {
        return;
    }
    m_process->disconnect(this);
    signalGroup(SIGTERM);
    if (!m_process->waitForFinished(5000)) {
        signalGroup(SIGKILL);
        m_process->waitForFinished(1000);
    }
}

void StackBuilder::start()
{
    const QString host = m_hostDir.isEmpty() ? hostDir() : m_hostDir;
    const QString stack = m_stackDir.isEmpty() ? Paths::stackDir() : m_stackDir;
    const QString work = m_workDir.isEmpty() ? workDir() : m_workDir;
    const int jobs = m_jobs > 0 ? m_jobs : defaultJobs();

    if (isRunning()) {
        return;
    }
    m_cancelled = false;
    m_partial.clear();
    m_log.clear();
    m_step = m_steps = 0;
    m_stepText.clear();
    m_missing.clear();
    m_install.clear();
    m_warnings.clear();
    m_error.clear();
    m_prefix.clear();
    m_stack = stack;
    m_before = current(stack).prefix;

    if (!isHostDir(host)) {
        m_error = tr("This installation of Vitrine has no host/build.sh to build its QEMU with");
        emit finished(m_error);
        return;
    }
    /* the guest tools' build takes many GiB too: one at a time */
    if (GuestToolsBuilder::instance()->isRunning()) {
        m_error = tr("The guest tools are being built: build Vitrine's QEMU once they are done");
        emit finished(m_error);
        return;
    }

    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    /* a process group of its own, which Stop ends as a whole: signalled
       alone, bash would wait for make, and make for the compilers */
    m_process->setChildProcessModifier([]() { ::setpgid(0, 0); });
    connect(m_process, &QProcess::readyRead, this, &StackBuilder::readOutput);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_error = tr("Cannot run bash: %1").arg(m_process->errorString());
            done(-1, false);
        }
    });
    connect(m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        done(code, status == QProcess::CrashExit);
    });

    const QStringList args = {host + "/build.sh", "--stack", stack, "--work", work,
                              "-j", QString::number(jobs)};
    m_log = "$ " + args.join(' ') + '\n';
    emit output(m_log);
    m_process->start("bash", args);
    /* gone already if bash could not start */
    if (m_process) {
        m_pid = m_process->processId();
        emit started();
    }
}

void StackBuilder::signalGroup(int signal)
{
    /* the group is bash's, which leads it */
    if (m_pid > 0) {
        ::kill(pid_t(-m_pid), signal);
    }
}

void StackBuilder::cancel()
{
    if (!m_process) {
        return;
    }
    m_cancelled = true;
    signalGroup(SIGTERM);
    /* what ignores it */
    QTimer::singleShot(10000, m_process, [this]() { signalGroup(SIGKILL); });
}

void StackBuilder::readOutput()
{
    const QString text = QString::fromLocal8Bit(m_process->readAll());
    qsizetype end;

    m_log += text;
    if (m_log.size() > kMaxLog) {
        /* the start goes, from a line on */
        const qsizetype cut = m_log.indexOf('\n', m_log.size() - kMaxLog * 3 / 4);
        m_log.remove(0, cut < 0 ? m_log.size() - kMaxLog * 3 / 4 : cut + 1);
    }
    emit output(text);
    m_partial += text;
    while ((end = m_partial.indexOf('\n')) >= 0) {
        handleLine(m_partial.left(end));
        m_partial.remove(0, end + 1);
    }
}

void StackBuilder::handleLine(const QString &text)
{
    const Line line = Line::parse(text);

    switch (line.kind) {
    case Line::Step:
        m_step = line.done;
        m_steps = line.total;
        m_stepText = line.text;
        emit stepStarted(line.done, line.total, line.text);
        break;
    case Line::Progress:
        emit progress(line.done, line.total);
        break;
    case Line::Missing:
        m_missing << line.text;
        break;
    case Line::Install:
        m_install = line.text;
        break;
    case Line::Warning:
        m_warnings << line.text;
        break;
    case Line::UpToDate:
    case Line::Built:
        m_prefix = line.text;
        break;
    case Line::Error:
        m_error = line.text;
        break;
    case Line::Other:
        break;
    }
}

void StackBuilder::done(int code, bool crashed)
{
    if (!m_partial.isEmpty()) {
        handleLine(m_partial);
        m_partial.clear();
    }
    m_process->deleteLater();
    m_process = nullptr;
    m_pid = 0;

    /*
     * By what was done: a build that switched `current` is the VMs' QEMU
     * now, even if a Stop came too late to keep it from it (build.sh
     * holds the stack's lock, so no other build switched it); one that
     * ended well is, even if Stop came after it did
     */
    const Build now = current(m_stack);
    if (now.isValid() && now.prefix != m_before) {
        m_cancelled = false;
        emit built(now.qemuBinary());
        emit finished({});
        return;
    }
    if (code == 0 && !crashed && Build::read(m_prefix).isValid()) {
        m_cancelled = false;
    }
    if (m_cancelled) {
        emit finished(tr("Stopped"));
        return;
    }
    if (code != 0 || crashed) {
        QString error = !m_missing.isEmpty() ? tr("Build dependencies are missing")
                        : !m_error.isEmpty() ? m_error
                                             : tr("The build failed (status %1)").arg(code);
        error[0] = error[0].toUpper();
        emit finished(error);
        return;
    }
    const Build build = Build::read(m_prefix);
    if (!build.isValid()) {
        emit finished(tr("build.sh ended without a complete build"));
        return;
    }
    /* first: when the windows hear of it, the rest of the app knows already */
    emit built(build.qemuBinary());
    emit finished({});
}
