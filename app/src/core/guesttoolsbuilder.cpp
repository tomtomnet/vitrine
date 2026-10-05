// SPDX-License-Identifier: GPL-2.0-or-later
#include "guesttoolsbuilder.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <csignal>
#include <sys/prctl.h>
#include <sys/types.h>
#include <unistd.h>

#include "core/guesttools.h"
#include "core/hostmemory.h"
#include "core/paths.h"
#include "core/stackbuilder.h"

/* The scripts' lines for the app start with it, as host/build.sh's */
static const char kTag[] = "vitrine-build: ";
/* The log kept of a build: Mesa's alone is some 25 MB */
static const qsizetype kMaxLog = 4 << 20;
/* What a stop may take: the script removes its container first */
static const int kStopMs = 20000;
/* A package's container, unless $MEMORY says otherwise (build-rpms.sh's) */
static const qint64 kDefaultCapMiB = 10 << 10;

static QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

static bool isGuestDir(const QString &dir)
{
    return !dir.isEmpty() && QFileInfo(dir + "/build-rpms.sh").isFile() &&
           QFileInfo(dir + "/build-medium.sh").isFile() &&
           QFileInfo(dir + "/rpm-build-inside.sh").isFile();
}

/* 10.0, for messages */
static QString gib(qint64 mib)
{
    return QString::number(double(mib) / 1024, 'f', 1);
}

static QString release()
{
    return QString::fromLatin1(GuestTools::kFedoraRelease);
}

/* What a package's spec is called, in its folder */
static QString specOf(const QString &package)
{
    return package == "tools" ? QStringLiteral("vitrine-guest-tools.spec") : package + ".spec";
}

GuestToolsBuilder::Line GuestToolsBuilder::Line::parse(const QString &text)
{
    /* ninja's, and dnf's with their padding */
    static const QRegularExpression counted("^\\[\\s*(\\d+)/(\\d+)\\]");
    static const QRegularExpression percent("^\\[\\s*(\\d+)%\\]");
    static const QRegularExpression section("^Executing\\(%(\\w+)\\)");
    static const QList<std::pair<QString, Kind>> kinds = {
        {"building: ", Building}, {"phase: ", Phase},     {"up to date: ", UpToDate},
        {"built: ", Built},       {"missing: ", Missing}, {"install: ", Install},
        {"warning: ", Warning},   {"error: ", Error},
    };
    QString line = text;
    Line result;

    while (line.endsWith('\r') || line.endsWith('\n')) {
        line.chop(1);
    }
    if (line.startsWith(kTag)) {
        line = line.mid(qsizetype(sizeof(kTag)) - 1);
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
    result.text = line;
    if (const QRegularExpressionMatch m = counted.match(line); m.hasMatch()) {
        result.kind = Counted;
        result.done = m.captured(1).toInt();
        result.total = m.captured(2).toInt();
    } else if (const QRegularExpressionMatch m = percent.match(line); m.hasMatch()) {
        result.kind = Percent;
        result.done = qMin(m.captured(1).toInt(), 100);
        result.total = 100;
    } else if (const QRegularExpressionMatch m = section.match(line); m.hasMatch()) {
        result.kind = Section;
        result.text = m.captured(1);
    } else if (line.startsWith("Processing files: ")) {
        result.kind = Section;
        result.text = QStringLiteral("files");
    }
    return result;
}

GuestToolsBuilder::Made GuestToolsBuilder::Made::parse(const QByteArray &manifest)
{
    const QJsonObject o = QJsonDocument::fromJson(manifest).object();
    const QJsonObject inputs = o["inputs"].toObject();
    Made made;

    made.built = QDateTime::fromString(o["built"].toString(), Qt::ISODate);
    for (auto it = inputs.begin(); it != inputs.end(); ++it) {
        made.inputs.insert(it.key(), it.value().toString());
    }
    return made;
}

QString GuestToolsBuilder::guestDir()
{
    const QString chosen = qEnvironmentVariable("VITRINE_GUEST_DIR");
    QStringList candidates;

    /* the tests', or a developer's: that one or none */
    if (!chosen.isEmpty()) {
        return isGuestDir(chosen) ? QFileInfo(chosen).canonicalFilePath() : QString();
    }
    /* installed: bin/vitrine and share/vitrine/guest; first, as the source
       tree it was built from may have moved on since */
    if (QCoreApplication::instance()) {
        candidates << QCoreApplication::applicationDirPath() + "/../share/vitrine/guest";
    }
#ifdef VITRINE_GUEST_SOURCE_DIR
    candidates << QStringLiteral(VITRINE_GUEST_SOURCE_DIR);
#endif
    candidates << QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, "vitrine/guest",
                                            QStandardPaths::LocateDirectory);
    for (const QString &dir : std::as_const(candidates)) {
        if (isGuestDir(dir)) {
            return QFileInfo(dir).canonicalFilePath();
        }
    }
    return {};
}

QString GuestToolsBuilder::rpmsDir()
{
    return GuestTools::dataDir() + "/rpms/fc" + release();
}

QString GuestToolsBuilder::cacheDir()
{
    return Paths::cacheDir() + "/guest-build/fc" + release();
}

QString GuestToolsBuilder::mediumImage()
{
    /* as GuestTools::medium() */
    return QString("%1/vitrine-guest-tools-fc%2.img").arg(GuestTools::dataDir(), release());
}

QString GuestToolsBuilder::manifestOf(const QString &image)
{
    /* as build-medium.sh: ${OUT%.img}.json */
    return (image.endsWith(".img") ? image.chopped(4) : image) + ".json";
}

QStringList GuestToolsBuilder::packages(const QString &guestDir)
{
    QStringList list;

    if (guestDir.isEmpty()) {
        return {};
    }
    for (const char *package : {"tools", "mesa", "kwin"}) {
        if (QFileInfo(guestDir + '/' + package + '/' + specOf(package)).isFile()) {
            list << package;
        }
    }
    return list;
}

QString GuestToolsBuilder::version(const QString &guestDir, const QString &package)
{
    static const QRegularExpression line("^Version:\\s*(\\S+)\\s*$",
                                         QRegularExpression::MultilineOption);
    const QRegularExpressionMatch m = line.match(
        QString::fromUtf8(readFile(guestDir + '/' + package + '/' + specOf(package))));

    /* a macro is no version to show */
    return m.hasMatch() && !m.captured(1).contains('%') ? m.captured(1) : QString();
}

/*
 * The regular files under @root/@path, as find -type f lists them: symbolic
 * links neither listed nor followed, hidden ones too
 */
static bool listFiles(const QString &root, const QString &path, QList<QByteArray> &files)
{
    const QFileInfo info(root + '/' + path);

    if (info.isSymLink()) {
        return true;
    }
    if (info.isFile()) {
        files << QFile::encodeName(path);
        return true;
    }
    /* none, or not a file: find lists nothing */
    if (!info.isDir()) {
        return true;
    }
    const QStringList names = QDir(info.filePath())
                                  .entryList(QDir::AllEntries | QDir::Hidden | QDir::System |
                                                 QDir::NoDotAndDotDot,
                                             QDir::NoSort);
    for (const QString &name : names) {
        if (!listFiles(root, path + '/' + name, files)) {
            return false;
        }
    }
    return true;
}

/*
 * As build-rpms.sh: the sha256 of the lines sha256sum prints for the files
 * of the package's folder and rpm-build-inside.sh, in the C locale's order
 * of their paths
 */
QString GuestToolsBuilder::inputsHash(const QString &guestDir, const QString &package)
{
    QList<QByteArray> files;
    QByteArray lines;

    if (guestDir.isEmpty() || package.isEmpty() || !QFileInfo(guestDir + '/' + package).isDir() ||
        !listFiles(guestDir, package, files) || !listFiles(guestDir, "rpm-build-inside.sh", files)) {
        return {};
    }
    std::sort(files.begin(), files.end());
    for (const QByteArray &name : std::as_const(files)) {
        QFile file(guestDir + '/' + QFile::decodeName(name));
        QCryptographicHash hash(QCryptographicHash::Sha256);

        if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) {
            return {};
        }
        lines += hash.result().toHex() + "  " + name + '\n';
    }
    return QString::fromLatin1(QCryptographicHash::hash(lines, QCryptographicHash::Sha256).toHex());
}

QString GuestToolsBuilder::mediumHash(const QString &guestDir)
{
    QFile script(guestDir + "/build-medium.sh");
    QCryptographicHash hash(QCryptographicHash::Sha256);

    if (guestDir.isEmpty() || !script.open(QIODevice::ReadOnly) || !hash.addData(&script)) {
        return {};
    }
    return QString::fromLatin1(hash.result().toHex());
}

QString GuestToolsBuilder::builtInputs(const QString &rpmsDir, const QString &package)
{
    return QString::fromLatin1(readFile(rpmsDir + '/' + package + "/.inputs")).trimmed();
}

GuestToolsBuilder::Made GuestToolsBuilder::made(const QString &image)
{
    return Made::parse(readFile(manifestOf(image)));
}

GuestToolsBuilder::State GuestToolsBuilder::state(const QString &guestDir, const QString &image)
{
    if (!isGuestDir(guestDir)) {
        return State::NoSources;
    }
    const QByteArray manifest = readFile(manifestOf(image));
    if (!QFileInfo::exists(image) || !GuestTools::parseManifest(manifest, image).isValid()) {
        return State::NotBuilt;
    }
    /* each package of guest/ on it, from its sources; a manifest of an older
       Vitrine, without inputs, is out of date */
    const Made made = Made::parse(manifest);
    for (const QString &package : packages(guestDir)) {
        if (made.inputs.value(package) != inputsHash(guestDir, package)) {
            return State::Outdated;
        }
    }
    return made.inputs.value("medium") == mediumHash(guestDir) ? State::UpToDate
                                                                : State::Outdated;
}

GuestToolsBuilder::State GuestToolsBuilder::state()
{
    return state(guestDir(), mediumImage());
}

QList<GuestToolsBuilder::Step> GuestToolsBuilder::plan(const QString &guestDir,
                                                       const QString &rpmsDir)
{
    QList<Step> steps;

    for (const QString &package : packages(guestDir)) {
        const QString version = GuestToolsBuilder::version(guestDir, package);
        const QString built = builtInputs(rpmsDir, package);
        Step step{package, {}, package != "tools"};

        if (!built.isEmpty() && built == inputsHash(guestDir, package)) {
            continue;
        }
        if (package == "mesa") {
            step.text = version.isEmpty() ? tr("building Mesa") : tr("building Mesa %1").arg(version);
        } else if (package == "kwin") {
            step.text = version.isEmpty() ? tr("building KWin") : tr("building KWin %1").arg(version);
        } else {
            step.text = version.isEmpty() ? tr("building vitrine-guest-tools")
                                          : tr("building vitrine-guest-tools %1").arg(version);
        }
        steps << step;
    }
    steps << Step{"medium", tr("making the medium"), false};
    return steps;
}

QList<GuestToolsBuilder::Requirement> GuestToolsBuilder::missing()
{
    /* build-rpms.sh's and build-medium.sh's, with Fedora's packages */
    static const QList<std::pair<const char *, const char *>> needs = {
        {"podman", "podman"},  {"mkfs.fat", "dosfstools"}, {"mcopy", "mtools"},
        {"rpm", "rpm"},        {"flock", "util-linux-core"},
    };
    QList<Requirement> list;

    for (const auto &[command, package] : needs) {
        if (QStandardPaths::findExecutable(command).isEmpty()) {
            list << Requirement{command, package};
        }
    }
    return list;
}

QString GuestToolsBuilder::installCommand(const QList<Requirement> &missing)
{
    QStringList packages;

    for (const Requirement &r : missing) {
        if (!packages.contains(r.package)) {
            packages << r.package;
        }
    }
    return packages.isEmpty() ? QString() : "sudo dnf install " + packages.join(' ');
}

qint64 GuestToolsBuilder::memoryCapMiB()
{
    /* podman's --memory: a number of bytes, or with b, k, m or g */
    static const QRegularExpression size("^\\s*(\\d+)\\s*([bkmgBKMG]?)\\s*$");
    const QRegularExpressionMatch m = size.match(qEnvironmentVariable("MEMORY"));

    if (!m.hasMatch()) {
        return kDefaultCapMiB;
    }
    const qint64 value = m.captured(1).toLongLong();
    const QChar unit = m.captured(2).isEmpty() ? QChar('b') : m.captured(2).at(0).toLower();
    const qint64 mib = unit == 'g' ? value << 10 : unit == 'm' ? value : unit == 'k' ? value >> 10
                                                                                     : value >> 20;
    return mib > 0 ? mib : kDefaultCapMiB;
}

GuestToolsBuilder *GuestToolsBuilder::instance()
{
    static QPointer<GuestToolsBuilder> builder;

    if (!builder) {
        builder = new GuestToolsBuilder(QCoreApplication::instance());
    }
    return builder;
}

GuestToolsBuilder::GuestToolsBuilder(QObject *parent)
    : QObject(parent), m_meminfo("/proc/meminfo"), m_memoryTimer(new QTimer(this))
{
    m_memoryTimer->setInterval(2000);
    connect(m_memoryTimer, &QTimer::timeout, this, &GuestToolsBuilder::checkMemory);
}

GuestToolsBuilder::~GuestToolsBuilder()
{
    if (!m_process) {
        return;
    }
    m_process->disconnect(this);
    /* the script removes its container, then ends */
    signalGroup(SIGTERM);
    if (!m_process->waitForFinished(kStopMs)) {
        signalGroup(SIGKILL);
        m_process->waitForFinished(1000);
    }
}

void GuestToolsBuilder::setMemoryPoll(int ms)
{
    m_memoryTimer->setInterval(ms);
}

void GuestToolsBuilder::start()
{
    if (m_running) {
        return;
    }
    const QString guest = m_guestDir.isEmpty() ? guestDir() : m_guestDir;
    const QString rpms = m_rpmsDir.isEmpty() ? rpmsDir() : m_rpmsDir;
    const QList<Requirement> needed = missing();

    m_plan.clear();
    m_index = -1;
    m_guest = guest;
    m_cancelled = m_stopped = false;
    m_partial.clear();
    m_log.clear();
    m_phase.clear();
    m_counting = Counting::None;
    m_done = m_total = 0;
    m_missing.clear();
    m_install.clear();
    m_warnings.clear();
    m_error.clear();
    m_failure.clear();
    m_available = m_needed = 0;

    if (!isGuestDir(guest)) {
        finish(tr("This installation of Vitrine has no guest/ folder to build the guest tools "
                  "from"));
        return;
    }
    /* each takes many GiB: one at a time */
    if (StackBuilder::instance()->isRunning()) {
        finish(tr("Vitrine's QEMU is being built: build the guest tools once it is done"));
        return;
    }
    if (!needed.isEmpty()) {
        for (const Requirement &r : needed) {
            m_missing << r.command;
        }
        m_install = installCommand(needed);
        finish(tr("Build dependencies are missing"));
        return;
    }
    m_plan = plan(guest, rpms);
    m_running = true;
    emit started();
    next();
}

void GuestToolsBuilder::next()
{
    if (++m_index >= m_plan.size()) {
        finish({});
        return;
    }
    const Step &step = m_plan.at(m_index);

    setPhase({}, Counting::None);
    emit stepStarted(m_index + 1, int(m_plan.size()), step.text);
    if (step.heavy) {
        checkMemory();
        return;
    }
    run();
}

/*
 * Before Mesa and KWin, which take up to the container's cap: that much
 * free, and what must stay free for the host (HostMemory), else a wait
 */
void GuestToolsBuilder::checkMemory()
{
    const HostMemory::Info host = HostMemory::read(m_meminfo);
    const qint64 needed = memoryCapMiB() + HostMemory::reserveMiB(host);
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
            append(tr("Waiting for memory: this package's build takes up to %1 GiB, and %2 GiB "
                      "is free; it starts once %3 GiB is.\n")
                       .arg(gib(memoryCapMiB()), gib(host.availableMiB), gib(needed)));
            m_memoryTimer->start();
        }
        emit waitingChanged();
    }
}

bool GuestToolsBuilder::isWaiting() const
{
    return m_running && !m_process && m_memoryTimer->isActive();
}

void GuestToolsBuilder::proceed()
{
    if (!isWaiting()) {
        return;
    }
    m_memoryTimer->stop();
    append(tr("Going on without the memory free.\n"));
    emit waitingChanged();
    run();
}

void GuestToolsBuilder::run()
{
    const Step &step = m_plan.at(m_index);
    const QString rpms = m_rpmsDir.isEmpty() ? rpmsDir() : m_rpmsDir;
    const QString cache = m_cacheDir.isEmpty() ? cacheDir() : m_cacheDir;
    const QString image = m_image.isEmpty() ? mediumImage() : m_image;
    const QString bash = QStandardPaths::findExecutable("bash");
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QStringList args;

    if (step.name == "medium") {
        args << m_guest + "/build-medium.sh";
    } else {
        args << m_guest + "/build-rpms.sh" << "--verbose" << step.name;
    }
    /* where the app looks for them, whatever the scripts' defaults */
    env.insert("RPMS_DIR", rpms);
    env.insert("CACHE_DIR", cache);
    env.insert("OUT", image);
    env.insert("FEDORA_RELEASE", release());

    m_process = new QProcess(this);
    m_process->setProcessEnvironment(env);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    /*
     * A process group of its own, which Stop signals as a whole: the script
     * removes its container, podman and tail go with it.  And stopped if
     * vitrine goes without stopping it (killed, crashed): the container
     * would go on for its 20 minutes and its GiB.
     */
    const pid_t app = ::getpid();
    m_process->setChildProcessModifier([app]() {
        ::setpgid(0, 0);
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
        /* gone already, as it forked */
        if (::getppid() != app) {
            ::_exit(127);
        }
    });
    connect(m_process, &QProcess::readyRead, this, &GuestToolsBuilder::readOutput);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_error = tr("Cannot run bash: %1").arg(m_process->errorString());
            stepDone(-1, false);
        }
    });
    connect(m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        stepDone(code, status == QProcess::CrashExit);
    });

    append(QString("$ RPMS_DIR=%1 CACHE_DIR=%2 OUT=%3 bash %4\n")
               .arg(rpms, cache, image, args.join(' ')));
    m_process->start(bash.isEmpty() ? QStringLiteral("/bin/bash") : bash, args);
    /* gone already if bash could not start */
    if (m_process) {
        m_pid = m_process->processId();
    }
}

void GuestToolsBuilder::signalGroup(int signal)
{
    /* the group is bash's, which leads it */
    if (m_pid > 0) {
        ::kill(pid_t(-m_pid), signal);
    }
}

void GuestToolsBuilder::cancel()
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
    /* what ignores it, or a script that takes too long to stop */
    QTimer::singleShot(kStopMs, m_process, [this]() { signalGroup(SIGKILL); });
}

void GuestToolsBuilder::append(const QString &text)
{
    m_log += text;
    if (m_log.size() > kMaxLog) {
        /* the start goes, from a line on */
        const qsizetype cut = m_log.indexOf('\n', m_log.size() - kMaxLog * 3 / 4);
        m_log.remove(0, cut < 0 ? m_log.size() - kMaxLog * 3 / 4 : cut + 1);
    }
    emit output(text);
}

void GuestToolsBuilder::readOutput()
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

void GuestToolsBuilder::setPhase(const QString &text, Counting counting)
{
    m_counting = counting;
    m_done = m_total = 0;
    if (text != m_phase) {
        m_phase = text;
        emit phaseChanged(text);
    }
    /* busy until told how far it is */
    emit progress(0, 0);
}

void GuestToolsBuilder::handleLine(const QString &text)
{
    /* rpmbuild's parts of a package's build, as the dialog names them */
    static const QHash<QString, QString> sections = {
        {"prep", tr("unpacking and patching the sources")},
        {"conf", tr("configuring")},
        {"build", tr("compiling")},
        {"install", tr("installing")},
        {"check", tr("checking")},
        {"files", tr("making the packages")},
    };
    const Line line = Line::parse(text);

    switch (line.kind) {
    case Line::Phase:
        /* dnf's phases, "installing ...": it counts what it downloads and installs */
        setPhase(line.text, line.text.startsWith("installing ") ? Counting::Dnf : Counting::None);
        break;
    case Line::Section:
        /* "Processing files" comes for each package made */
        if (const auto it = sections.find(line.text); it != sections.end() && *it != m_phase) {
            setPhase(*it, line.text == "build" ? Counting::Compile : Counting::None);
        }
        break;
    case Line::Counted:
        if (m_counting != Counting::None && line.total > 0 && line.done <= line.total) {
            m_done = line.done;
            m_total = line.total;
            emit progress(m_done, m_total);
        }
        break;
    case Line::Percent:
        /* make's jobs each tell their own: the most */
        if (m_counting == Counting::Compile && (m_total != 100 || line.done > m_done)) {
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
    case Line::Warning:
        m_warnings << line.text;
        break;
    case Line::Error:
        m_error = line.text;
        break;
    case Line::Building:
    case Line::UpToDate:
    case Line::Built:
    case Line::Other:
        break;
    }
}

void GuestToolsBuilder::stepDone(int code, bool crashed)
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

    const bool medium = m_plan.at(m_index).name == "medium";
    /* a stop that came as the medium was made already: made all the same;
       a package built is kept, and the next build skips it */
    if (m_cancelled && !(medium && code == 0 && !crashed)) {
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
    if (medium) {
        const QString image = m_image.isEmpty() ? mediumImage() : m_image;
        if (!QFileInfo::exists(image) ||
            !GuestTools::parseManifest(readFile(manifestOf(image)), image).isValid()) {
            finish(tr("build-medium.sh ended without a medium"));
            return;
        }
    }
    next();
}

void GuestToolsBuilder::finish(const QString &error, bool stopped)
{
    /* a build that ended well made the medium, its last step */
    const bool medium = m_running && error.isEmpty();

    m_running = false;
    m_memoryTimer->stop();
    m_stopped = stopped;
    m_failure = stopped ? QString() : error;
    if (!error.isEmpty()) {
        append(error + '\n');
    }
    /* first: when the windows hear of it, the VMs know already */
    if (medium) {
        emit built();
    }
    emit finished(error);
}
