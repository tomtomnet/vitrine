// SPDX-License-Identifier: GPL-2.0-or-later
#include "buildmanager.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTimer>

#include <algorithm>
#include <fcntl.h>
#include <functional>
#include <sys/file.h>
#include <unistd.h>

#include "core/guesttoolsbuilder.h"
#include "core/paths.h"
#include "core/stackbuilder.h"
#include "core/tasks.h"

using Builds::Component;
using Builds::Kind;

/* The durations kept of each component's builds */
static const int kHistory = 5;
/* A component's log kept in memory, from its end: Mesa's alone is some 25 MB */
static const qsizetype kMaxLog = 2 << 20;
/* A compiler's count is trusted once that part of it is done (1/50), that long after it began */
static const int kCountFrom = 50;
static const qint64 kCountAfterMs = 15000;

static bool isFinal(BuildManager::State state)
{
    switch (state) {
    case BuildManager::State::Idle:
    case BuildManager::State::Queued:
    case BuildManager::State::Waiting:
    case BuildManager::State::Building:
        return false;
    default:
        return true;
    }
}

BuildManager *BuildManager::instance()
{
    static QPointer<BuildManager> manager;

    if (!manager) {
        manager = new BuildManager(QCoreApplication::instance());
    }
    return manager;
}

BuildManager::BuildManager(QObject *parent) : QObject(parent), m_tick(new QTimer(this))
{
    m_tick->setInterval(1000);
    connect(m_tick, &QTimer::timeout, this, &BuildManager::update);
}

BuildManager::~BuildManager() = default;

void BuildManager::setPlaces(const Builds::Places &places)
{
    m_places = places;
    m_placesSet = true;
    m_loaded = false;
}

void BuildManager::setBuilders(StackBuilder *stack, GuestToolsBuilder *tools)
{
    m_stack = stack;
    m_tools = tools;
    m_connected.clear();
}

void BuildManager::setHistoryFile(const QString &path)
{
    m_historyFile = path;
    m_historyLoaded = false;
}

void BuildManager::setLogDir(const QString &path)
{
    m_logDir = path;
}

void BuildManager::setTick(int ms)
{
    m_tick->setInterval(ms);
}

QList<Component> BuildManager::components() const
{
    if (!m_loaded) {
        const Builds::Places places = m_placesSet ? m_places : Builds::Places::defaults();
        m_components = Builds::load(places);
        m_status.clear();
        for (const Component &c : std::as_const(m_components)) {
            m_status.insert(c.id, Builds::status(c, m_components, places));
        }
        m_loaded = true;
    }
    return m_components;
}

Component BuildManager::component(const QString &id) const
{
    return Builds::find(components(), id);
}

Builds::Status BuildManager::status(const QString &id) const
{
    components();
    return m_status.value(id);
}

void BuildManager::refresh()
{
    m_loaded = false;
    components();
    emit statusChanged();
}

QStringList BuildManager::plan(const QStringList &ids) const
{
    const QList<Component> all = components();
    QStringList wanted;
    std::function<void(const QString &)> add = [&](const QString &id) {
        const Component c = Builds::find(all, id);
        if (!c.isValid() || wanted.contains(id)) {
            return;
        }
        for (const QString &dep : c.deps) {
            if (status(dep).state != Builds::Status::State::UpToDate) {
                add(dep);
            }
        }
        wanted << id;
    };

    for (const QString &id : ids) {
        add(id);
    }
    /* a package reaches the VMs on the medium */
    const bool package = std::any_of(wanted.begin(), wanted.end(), [&all](const QString &id) {
        return Builds::find(all, id).kind == Kind::Package;
    });
    for (const Component &c : all) {
        if (package && c.kind == Kind::Medium && !wanted.contains(c.id)) {
            wanted << c.id;
        }
    }
    /* in build order */
    QStringList ordered;
    for (const Component &c : all) {
        if (wanted.contains(c.id)) {
            ordered << c.id;
        }
    }
    return ordered;
}

QStringList BuildManager::outdated() const
{
    QStringList ids;

    for (const Component &c : components()) {
        const Builds::Status::State state = status(c.id).state;
        if (state == Builds::Status::State::NotBuilt || state == Builds::Status::State::Outdated) {
            ids << c.id;
        }
    }
    return ids.isEmpty() ? QStringList() : plan(ids);
}

void BuildManager::build(const QStringList &ids, bool clean)
{
    if (m_running) {
        return;
    }
    if (!m_stack) {
        m_stack = StackBuilder::instance();
    }
    if (!m_tools) {
        m_tools = GuestToolsBuilder::instance();
    }
    /* their lines, per component, while a run of ours goes on */
    if (!m_connected.contains("stack")) {
        m_connected << "stack";
        connect(m_stack, &StackBuilder::output, this, [this](const QString &text) {
            if (m_running && !m_part.isEmpty() && component(m_part.first()).kind == Kind::Host) {
                append(m_current.isEmpty() ? m_part.first() : m_current, text);
            }
        });
        connect(m_stack, &StackBuilder::componentStarted, this, [this](const QString &id) {
            if (m_running && m_part.contains(id)) {
                setCurrent(id);
            }
        });
        connect(m_stack, &StackBuilder::componentDone, this,
                [this](const QString &id, bool built) {
            if (m_running && m_part.contains(id)) {
                componentDone(id, built ? State::Built : State::UpToDate);
            }
        });
        connect(m_stack, &StackBuilder::stepStarted, this,
                [this](int, int, const QString &text) {
            if (m_running && !m_current.isEmpty() && m_part.contains(m_current)) {
                Progress &p = m_progress[m_current];
                p.phase = text;
                p.done = p.total = 0;
                m_phaseClocks[m_current].start();
                emit progressChanged(m_current);
                updateTask();
            }
        });
        connect(m_stack, &StackBuilder::progress, this, [this](int done, int total) {
            if (m_running && !m_current.isEmpty() && m_part.contains(m_current)) {
                Progress &p = m_progress[m_current];
                if (p.done != done || p.total != total) {
                    p.done = done;
                    p.total = total;
                    emit progressChanged(m_current);
                }
            }
        });
        connect(m_stack, &StackBuilder::finished, this, [this](const QString &error) {
            if (m_running && !m_part.isEmpty() && component(m_part.first()).kind == Kind::Host) {
                partFinished(error, m_stack->wasStopped());
            }
        });
    }
    if (!m_connected.contains("tools")) {
        m_connected << "tools";
        const auto guestPart = [this]() {
            return m_running && !m_part.isEmpty() && component(m_part.first()).kind != Kind::Host;
        };
        connect(m_tools, &GuestToolsBuilder::output, this, [this, guestPart](const QString &text) {
            if (guestPart()) {
                append(m_current.isEmpty() ? m_part.first() : m_current, text);
            }
        });
        connect(m_tools, &GuestToolsBuilder::stepStarted, this,
                [this, guestPart](int step, int, const QString &) {
            if (guestPart() && step >= 1 && step <= m_part.size()) {
                setCurrent(m_part.at(step - 1));
            }
        });
        connect(m_tools, &GuestToolsBuilder::phaseChanged, this,
                [this, guestPart](const QString &text) {
            if (guestPart() && !m_current.isEmpty()) {
                Progress &p = m_progress[m_current];
                p.phase = text;
                p.done = p.total = 0;
                m_phaseClocks[m_current].start();
                emit progressChanged(m_current);
                updateTask();
            }
        });
        connect(m_tools, &GuestToolsBuilder::progress, this,
                [this, guestPart](int done, int total) {
            if (guestPart() && !m_current.isEmpty()) {
                Progress &p = m_progress[m_current];
                if (p.done != done || p.total != total) {
                    p.done = done;
                    p.total = total;
                    emit progressChanged(m_current);
                }
            }
        });
        connect(m_tools, &GuestToolsBuilder::waitingChanged, this, [this, guestPart]() {
            if (guestPart() && !m_current.isEmpty()) {
                const State state = m_progress.value(m_current).state;
                if (m_tools->isWaiting() && state == State::Building) {
                    setState(m_current, State::Waiting);
                } else if (!m_tools->isWaiting() && state == State::Waiting) {
                    /* its time counts from now */
                    m_clocks[m_current].start();
                    setState(m_current, State::Building);
                }
            }
        });
        connect(m_tools, &GuestToolsBuilder::packageDone, this,
                [this, guestPart](const QString &id, bool restored) {
            if (guestPart() && m_part.contains(id)) {
                componentDone(id, restored ? State::Restored : State::Built);
            }
        });
        connect(m_tools, &GuestToolsBuilder::finished, this, [this, guestPart](const QString &error) {
            if (!guestPart()) {
                return;
            }
            /* the medium, the last step, says nothing else when it is made */
            if (error.isEmpty() && component(m_part.last()).kind == Kind::Medium &&
                !isFinal(m_progress.value(m_part.last()).state)) {
                componentDone(m_part.last(), State::Built);
            }
            partFinished(error, m_tools->wasStopped());
        });
    }

    const QStringList queue = plan(ids);
    if (queue.isEmpty()) {
        return;
    }
    m_queue = queue;
    m_part.clear();
    m_current.clear();
    m_error.clear();
    m_running = true;
    m_cancelled = false;
    m_clean = clean;
    m_progress.clear();
    m_clocks.clear();
    m_phaseClocks.clear();
    for (const QString &id : std::as_const(m_queue)) {
        m_progress[id].state = State::Queued;
        m_logs.remove(id);
    }
    m_runClock.start();
    /* in the app's tasks: this run, the last one's leaving them */
    if (m_task) {
        m_task->dismiss();
    }
    m_task = TaskList::instance()->add(Task::Kind::Build,
                                       tr("Building %1").arg(component(m_queue.first()).title));
    m_tick->start();
    emit runStarted();
    for (const QString &id : std::as_const(m_queue)) {
        emit progressChanged(id);
    }
    updateTask();
    nextPart();
}

void BuildManager::buildOutdated()
{
    build(outdated());
}

void BuildManager::nextPart()
{
    QStringList rest;

    for (const QString &id : std::as_const(m_queue)) {
        if (m_progress.value(id).state == State::Queued) {
            rest << id;
        }
    }
    if (rest.isEmpty()) {
        finishRun({}, false);
        return;
    }
    /* the components of the same script, one after the other */
    const bool host = component(rest.first()).kind == Kind::Host;
    m_part.clear();
    for (const QString &id : std::as_const(rest)) {
        if ((component(id).kind == Kind::Host) != host) {
            break;
        }
        m_part << id;
    }
    if (host) {
        startHost(m_part);
    } else {
        startGuest(m_part);
    }
}

void BuildManager::startHost(const QStringList &ids)
{
    /* build.sh builds its target and what it needs */
    m_stack->start(ids.last(), m_clean);
}

void BuildManager::startGuest(const QStringList &ids)
{
    m_tools->start(ids);
}

void BuildManager::partFinished(const QString &error, bool stopped)
{
    if (error.isEmpty() && !m_cancelled) {
        /* what said nothing of itself was there already */
        for (const QString &id : std::as_const(m_part)) {
            if (!isFinal(m_progress.value(id).state)) {
                componentDone(id, State::UpToDate);
            }
        }
        m_current.clear();
        m_part.clear();
        /* what was built changes what the rest needs */
        m_loaded = false;
        nextPart();
        return;
    }
    finishRun(error.isEmpty() ? tr("Stopped") : error, stopped || m_cancelled);
}

void BuildManager::finishRun(const QString &error, bool stopped)
{
    QString failed;

    if (!error.isEmpty()) {
        /* the one that went wrong, then those it kept from being built */
        for (const QString &id : std::as_const(m_queue)) {
            Progress &p = m_progress[id];
            if (isFinal(p.state)) {
                continue;
            }
            if (failed.isEmpty() && (p.state != State::Queued || m_part.contains(id))) {
                failed = id;
                p.state = stopped ? State::Stopped : State::Failed;
                p.error = stopped ? QString() : error;
                if (m_clocks.contains(id)) {
                    p.elapsedMs = m_clocks.value(id).elapsed();
                }
                saveLog(id);
            } else {
                p.state = stopped ? State::Stopped : State::Skipped;
            }
            p.remainingMs = -1;
        }
    }
    m_running = false;
    m_tick->stop();
    m_current.clear();
    m_part.clear();
    m_error = stopped ? QString() : error;
    if (!error.isEmpty() && !failed.isEmpty() && !stopped) {
        m_error = component(failed).title + ": " + error;
    }

    if (m_task) {
        QStringList built;
        for (const QString &id : std::as_const(m_queue)) {
            const State state = m_progress.value(id).state;
            if (state == State::Built || state == State::Restored) {
                built << component(id).title;
            }
        }
        if (error.isEmpty()) {
            m_task->setTitle(built.isEmpty()        ? tr("Builds up to date")
                             : built.size() == 1    ? tr("%1 built").arg(built.first())
                             : built.size() == 2    ? tr("%1 and %2 built").arg(built[0], built[1])
                                                    : tr("%n components built", nullptr,
                                                         int(built.size())));
            m_task->finish(Task::State::Done, built.isEmpty() ? QString()
                                                              : tr("Built: %1").arg(built.join(", ")));
        } else {
            const QString title = component(failed.isEmpty() ? m_queue.first() : failed).title;
            m_task->setTitle(tr("Building %1").arg(title));
            m_task->finish(stopped ? Task::State::Stopped : Task::State::Failed,
                           stopped ? tr("Stopped") : error);
        }
    }
    refresh();
    for (const QString &id : std::as_const(m_queue)) {
        emit progressChanged(id);
    }
    emit runFinished(m_error.isEmpty() && stopped ? tr("Stopped") : m_error);
}

void BuildManager::cancel()
{
    if (!m_running) {
        return;
    }
    m_cancelled = true;
    if (m_stack && m_stack->isRunning()) {
        m_stack->cancel();
    } else if (m_tools && m_tools->isRunning()) {
        m_tools->cancel();
    } else {
        finishRun(tr("Stopped"), true);
    }
}

void BuildManager::proceed()
{
    if (isWaiting()) {
        m_tools->proceed();
    }
}

bool BuildManager::isWaiting() const
{
    return m_running && m_tools && m_tools->isWaiting();
}

BuildManager::Progress BuildManager::progress(const QString &id) const
{
    Progress p = m_progress.value(id);

    if (m_clocks.contains(id) && !isFinal(p.state)) {
        p.elapsedMs = m_clocks.value(id).elapsed();
    }
    if (!isFinal(p.state) && p.state != State::Idle) {
        p.remainingMs = remainingOf(id);
    }
    return p;
}

QString BuildManager::focus() const
{
    if (!m_current.isEmpty()) {
        return m_current;
    }
    for (const QString &id : m_queue) {
        if (m_progress.value(id).state == State::Failed) {
            return id;
        }
    }
    return m_queue.isEmpty() ? QString() : m_queue.last();
}

QString BuildManager::log(const QString &id) const
{
    if (m_logs.contains(id)) {
        return m_logs.value(id);
    }
    QFile file((m_logDir.isEmpty() ? Paths::cacheDir() + "/build-logs" : m_logDir) + '/' + id +
               ".log");
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

qint64 BuildManager::remainingOf(const QString &id) const
{
    const Progress p = m_progress.value(id);
    const qint64 estimate = estimateMs(id);

    switch (p.state) {
    case State::Queued:
    case State::Waiting:
        return estimate;
    case State::Building: {
        const qint64 elapsed = m_clocks.contains(id) ? m_clocks.value(id).elapsed() : 0;
        const qint64 phase = m_phaseClocks.contains(id) ? m_phaseClocks.value(id).elapsed() : 0;
        const bool compiling = component(id).kind == Kind::Host
                                   ? p.phase.startsWith("compiling ") ||
                                         p.phase.startsWith("building ")
                                   : m_tools && m_tools->isCompiling();
        /* from the compiler's count: the rest of it at its pace, and what
           comes after compiling (installing, packaging) */
        if (compiling && p.total > 0 && p.done > 0 && qint64(p.done) * kCountFrom >= p.total &&
            phase >= kCountAfterMs) {
            return phase * (p.total - p.done) / p.done + estimate / 12;
        }
        return std::max(estimate - elapsed, estimate / 10);
    }
    default:
        return 0;
    }
}

qint64 BuildManager::remainingMs() const
{
    qint64 sum = 0;

    if (!m_running) {
        return -1;
    }
    for (const QString &id : m_queue) {
        if (!isFinal(m_progress.value(id).state)) {
            sum += remainingOf(id);
        }
    }
    return sum;
}

qint64 BuildManager::estimateMs(const QString &id) const
{
    QList<qint64> list = durations(id);

    if (list.isEmpty()) {
        const Component c = component(id);
        return qint64(c.isValid() ? c.minutes : 10) * 60000;
    }
    std::sort(list.begin(), list.end());
    return list.size() % 2 ? list.at(list.size() / 2)
                           : (list.at(list.size() / 2 - 1) + list.at(list.size() / 2)) / 2;
}

QList<qint64> BuildManager::durations(const QString &id) const
{
    loadHistory();
    return m_history.value(id);
}

void BuildManager::setCurrent(const QString &id)
{
    m_current = id;
    m_clocks[id].start();
    m_phaseClocks[id].start();
    m_progress[id].phase.clear();
    m_progress[id].done = m_progress[id].total = 0;
    setState(id, State::Building);
}

void BuildManager::setState(const QString &id, State state)
{
    m_progress[id].state = state;
    emit progressChanged(id);
    updateTask();
}

void BuildManager::componentDone(const QString &id, State state)
{
    Progress &p = m_progress[id];

    p.state = state;
    p.remainingMs = -1;
    p.elapsedMs = m_clocks.contains(id) ? m_clocks.value(id).elapsed() : 0;
    /* only a build tells how long one takes */
    if (state == State::Built && m_clocks.contains(id)) {
        record(id, p.elapsedMs);
    }
    saveLog(id);
    if (m_current == id) {
        m_current.clear();
    }
    emit progressChanged(id);
    updateTask();
}

void BuildManager::append(const QString &id, const QString &text)
{
    QString &log = m_logs[id];

    log += text;
    if (log.size() > kMaxLog) {
        /* the start goes, from a line on */
        const qsizetype cut = log.indexOf('\n', log.size() - kMaxLog * 3 / 4);
        log.remove(0, cut < 0 ? log.size() - kMaxLog * 3 / 4 : cut + 1);
    }
    emit output(id, text);
}

void BuildManager::saveLog(const QString &id)
{
    const QString dir = m_logDir.isEmpty() ? Paths::cacheDir() + "/build-logs" : m_logDir;
    QSaveFile file(dir + '/' + id + ".log");

    if (!m_logs.contains(id) || !QDir().mkpath(dir) || !file.open(QIODevice::WriteOnly)) {
        return;
    }
    file.write(m_logs.value(id).toUtf8());
    file.commit();
}

QStringList BuildManager::cacheDirs() const
{
    /* every Fedora release's of the guest tools */
    return m_cacheDirs.isEmpty() ? QStringList{StackBuilder::workDir(),
                                               Paths::cacheDir() + "/guest-build"}
                                 : m_cacheDirs;
}

/* @dir and all in it, its folders made writable first: some downloads
   come read-only */
static bool removeTree(const QString &dir)
{
    QDirIterator it(dir, QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);

    if (!QFileInfo::exists(dir)) {
        return true;
    }
    while (it.hasNext()) {
        const QString path = it.next();
        if (!it.fileInfo().isSymLink()) {
            QFile::setPermissions(path, QFile::permissions(path) | QFileDevice::ReadOwner |
                                            QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        }
    }
    QFile::setPermissions(dir, QFile::permissions(dir) | QFileDevice::WriteOwner);
    return QDir(dir).removeRecursively() || !QFileInfo::exists(dir);
}

bool BuildManager::clearCache(QString *error)
{
    const Builds::Places places = m_placesSet ? m_places : Builds::Places::defaults();
    QStringList lockFiles;
    QList<int> locks;
    QStringList failed;
    const auto release = [&locks]() {
        for (const int fd : std::as_const(locks)) {
            ::flock(fd, LOCK_UN);
            ::close(fd);
        }
    };

    if (m_running || (m_stack && m_stack->isRunning()) || (m_tools && m_tools->isRunning())) {
        *error = tr("A build is running: clear the cache once it is done.");
        return false;
    }
    /* a build from a terminal: build.sh's lock in its work folder,
       build-rpms.sh's in its output */
    for (const QString &dir : cacheDirs()) {
        lockFiles << dir + "/.lock";
    }
    lockFiles << places.rpmsDir + "/.lock";
    for (const QString &path : std::as_const(lockFiles)) {
        const int fd = ::open(QFile::encodeName(path).constData(), O_WRONLY | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            ::close(fd);
            release();
            *error = tr("A build is running from a terminal: clear the cache once it is done.");
            return false;
        }
        locks << fd;
    }
    for (const QString &dir : cacheDirs()) {
        if (!removeTree(dir)) {
            failed << dir;
        }
    }
    release();
    if (!failed.isEmpty()) {
        *error = tr("Some files could not be removed from %1. Those the build container made "
                    "may need: podman unshare rm -rf FOLDER")
                     .arg(failed.join(", "));
        return false;
    }
    return true;
}

void BuildManager::loadHistory() const
{
    QFile file(m_historyFile.isEmpty() ? Paths::dataDir() + "/build-history.json" : m_historyFile);

    if (m_historyLoaded) {
        return;
    }
    m_historyLoaded = true;
    m_history.clear();
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonObject all = QJsonDocument::fromJson(file.readAll()).object();
    for (auto it = all.begin(); it != all.end(); ++it) {
        QList<qint64> list;
        for (const QJsonValue &v : it.value().toObject().value("durations").toArray()) {
            if (v.toDouble() > 0) {
                list << qint64(v.toDouble());
            }
        }
        m_history.insert(it.key(), list);
    }
}

void BuildManager::saveHistory()
{
    const QString path =
        m_historyFile.isEmpty() ? Paths::dataDir() + "/build-history.json" : m_historyFile;
    QSaveFile file(path);
    QJsonObject all;

    for (auto it = m_history.begin(); it != m_history.end(); ++it) {
        QJsonArray list;
        for (const qint64 ms : it.value()) {
            list << double(ms);
        }
        all.insert(it.key(), QJsonObject{{"durations", list}});
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath()) || !file.open(QIODevice::WriteOnly)) {
        return;
    }
    file.write(QJsonDocument(all).toJson());
    file.commit();
}

void BuildManager::record(const QString &id, qint64 ms)
{
    loadHistory();
    QList<qint64> &list = m_history[id];
    list << ms;
    while (list.size() > kHistory) {
        list.removeFirst();
    }
    saveHistory();
}

void BuildManager::update()
{
    if (!m_running) {
        return;
    }
    if (!m_current.isEmpty()) {
        emit progressChanged(m_current);
    }
    updateTask();
}

void BuildManager::updateTask()
{
    if (!m_task || !m_running) {
        return;
    }
    QString id = m_current;
    int index = 0;
    for (int i = 0; i < m_queue.size(); i++) {
        if (id.isEmpty() && !isFinal(m_progress.value(m_queue[i]).state)) {
            id = m_queue[i];
        }
        if (m_queue[i] == id) {
            index = i;
        }
    }
    if (id.isEmpty()) {
        return;
    }
    const Progress p = progress(id);
    QString what = p.state == State::Waiting ? tr("waiting for memory")
                   : p.phase.isEmpty()       ? tr("starting")
                                             : p.phase;
    const qint64 remaining = remainingMs();
    const qint64 elapsed = m_runClock.elapsed();

    m_task->setTitle(tr("Building %1").arg(component(id).title));
    m_task->setText(m_queue.size() > 1
                        ? tr("%1 of %2: %3").arg(index + 1).arg(m_queue.size()).arg(what)
                        : what.left(1).toUpper() + what.mid(1));
    /* the whole run's: what it took so far, of that and what is left */
    if (remaining >= 0 && p.state != State::Waiting) {
        m_task->setProgress(elapsed * 1000 / std::max<qint64>(1, elapsed + remaining), 1000);
    } else {
        m_task->setProgress(0, 0);
    }
    m_task->setRemaining(remaining < 0 ? -1 : (remaining + 999) / 1000);
}
