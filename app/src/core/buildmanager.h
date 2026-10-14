// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>

#include "core/buildcomponents.h"
#include "core/tasks.h"

class GuestToolsBuilder;
class StackBuilder;

/*
 * The app's builds: the components (Builds::load()), what is built of them,
 * and the run that builds some of them, one at a time, whichever window
 * shows it.  A run takes those asked for and what they need that is not
 * built from their sources now, in build order, and after a package of the
 * guest tools their medium, which the VMs install from.  StackBuilder runs
 * host/build.sh for the host's, GuestToolsBuilder the guest's scripts
 * (which waits for memory before a heavy package); this follows their
 * lines, per component.
 *
 * Each component has its log, kept beside the cache, and the durations of
 * its last builds on this machine, kept with the data.  The run is one
 * task of the app's (TaskList): its title the component building, its
 * progress the whole run's, from the components done and the compiler's
 * count of the one compiling.
 */
class BuildManager : public QObject
{
    Q_OBJECT

public:
    /* A component in a run */
    enum class State {
        Idle,           // in no run
        Queued,
        Waiting,        // for memory, before a heavy package
        Building,
        Built,
        UpToDate,       // built already: nothing done
        Restored,       // the build before brought back: nothing compiled
        Failed,
        Stopped,
        Skipped,        // not built: one before it failed
    };

    struct Progress {
        State state = State::Idle;
        QString phase;          // "compiling QEMU", "installing the build dependencies"
        int done = 0;           // of its counted phase; total 0 when it cannot tell
        int total = 0;
        qint64 elapsedMs = 0;
        QString error;
    };

    /* The app's */
    static BuildManager *instance();

    explicit BuildManager(QObject *parent = nullptr);
    ~BuildManager() override;

    /* Defaults: Builds::Places::defaults(), StackBuilder's and
       GuestToolsBuilder's instance(), the files in Paths' folders */
    void setPlaces(const Builds::Places &places);
    void setBuilders(StackBuilder *stack, GuestToolsBuilder *tools);
    void setHistoryFile(const QString &path);
    void setLogDir(const QString &path);

    QList<Builds::Component> components() const;
    Builds::Component component(const QString &id) const;
    Builds::Status status(const QString &id) const;
    /* The components and their state again: the sources may have changed */
    void refresh();

    /* What building @ids takes: them and what they need that is not built
       from their sources now, then the medium after a package; in order */
    QStringList plan(const QStringList &ids) const;
    /* What is not built from its sources now: what "Update All" builds */
    QStringList outdated() const;

    bool isRunning() const { return m_running; }
    /* Builds plan(@ids); @clean: the host's build trees from nothing */
    void build(const QStringList &ids, bool clean = false);
    void buildOutdated();
    /* Stops the component building; those after it are not built */
    void cancel();
    /* A heavy package waiting for memory: goes on now */
    void proceed();
    bool isWaiting() const;

    /* Of the run going on, or of the last one */
    QStringList queue() const { return m_queue; }
    QString current() const { return m_current; }
    Progress progress(const QString &id) const;
    /* What to show of the run: the component building, else the one that
       failed, else the last one */
    QString focus() const;
    /* The component's log: of the run, else the one kept of its last build */
    QString log(const QString &id) const;
    /* Its last durations on this machine, ms, the newest last */
    QList<qint64> durations(const QString &id) const;
    /* Why the last run failed; empty if it did not */
    QString error() const { return m_error; }
    /* The run's in the app's tasks, until seen; null if none */
    Task *task() const { return m_task; }

    /* The build cache: the sources and build trees, the downloads, the
       compiler cache; what is built is elsewhere, and stays */
    QStringList cacheDirs() const;
    void setCacheDirs(const QStringList &dirs) { m_cacheDirs = dirs; }
    /* Removes them, unless a build runs, here or from a terminal (the
       scripts' locks); false, and why, if not all of it went */
    bool clearCache(QString *error);

signals:
    void runStarted();
    /* @error is empty when all was built */
    void runFinished(const QString &error);
    /* A component's state, phase or progress */
    void progressChanged(const QString &id);
    /* The statuses were worked out again */
    void statusChanged();
    void output(const QString &id, const QString &text);

private:
    /* The run's next part: the host's components, or the guest's */
    void nextPart();
    void startHost(const QStringList &ids);
    void startGuest(const QStringList &ids);
    void partFinished(const QString &error, bool stopped);
    void finishRun(const QString &error, bool stopped);

    void setCurrent(const QString &id);
    void setState(const QString &id, State state);
    void componentDone(const QString &id, State state);
    void append(const QString &id, const QString &text);
    void saveLog(const QString &id);
    void record(const QString &id, qint64 ms);
    void loadHistory() const;
    void saveHistory();
    void updateTask();

    Builds::Places m_places;
    bool m_placesSet = false;
    QPointer<StackBuilder> m_stack;
    QPointer<GuestToolsBuilder> m_tools;
    QString m_historyFile;
    QString m_logDir;
    QStringList m_cacheDirs;

    mutable bool m_loaded = false;
    mutable QList<Builds::Component> m_components;
    mutable QHash<QString, Builds::Status> m_status;

    bool m_running = false;
    bool m_cancelled = false;
    bool m_clean = false;
    QStringList m_queue;
    QStringList m_part;             // of the queue, the part running
    QString m_current;
    QString m_logTo;                /* the component whose lines come now */
    QString m_error;
    QHash<QString, Progress> m_progress;
    QHash<QString, QElapsedTimer> m_clocks;
    QHash<QString, QString> m_logs;
    mutable QHash<QString, QList<qint64>> m_history;
    mutable bool m_historyLoaded = false;
    qint64 m_shown = 0;             // the run's progress shown, in thousandths
    QPointer<Task> m_task;
    QStringList m_connected;        // the builders followed
};
