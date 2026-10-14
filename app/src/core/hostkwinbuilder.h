// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class QProcess;
class QTimer;

/*
 * Builds the host's KWin from the app: host/kwin/build.sh, which runs the
 * research's recipe in a Fedora container capped at $MEMORY (10 GiB), as
 * GuestToolsBuilder runs the guest tools' packages.  It waits until that
 * much memory is free (and what the host keeps free), or until told to go
 * on anyway.  One build at a time; a stop removes the container.  What it
 * built is a folder of HostKwin::outDir(), which install.sh installs.
 */
class HostKwinBuilder : public QObject
{
    Q_OBJECT

public:
    /* The app's */
    static HostKwinBuilder *instance();

    explicit HostKwinBuilder(QObject *parent = nullptr);
    ~HostKwinBuilder() override;

    /* Defaults: HostKwin's folders, /proc/meminfo, nproc (at most 8) jobs */
    void setSourceDir(const QString &dir) { m_sourceDir = dir; }
    void setOutDir(const QString &dir) { m_outDir = dir; }
    void setWorkDir(const QString &dir) { m_workDir = dir; }
    void setMeminfo(const QString &path) { m_meminfo = path; }
    void setJobs(int jobs) { m_jobs = jobs; }
    /* How often a build waiting for memory looks again */
    void setMemoryPoll(int ms);

    /* A build runs, or waits for memory */
    bool isRunning() const { return m_running; }
    /* The set for this computer's Plasma, from where it comes (beta,
       testing, fedora: HostKwin::Status::plasmaFrom) */
    void start(const QString &plasma);
    /* Stops build.sh and what it runs (its process group: it removes its
       container first), or the wait */
    void cancel();

    /* Waiting for memory; then what is free, and what the build needs */
    bool isWaiting() const;
    qint64 availableMiB() const { return m_available; }
    qint64 neededMiB() const { return m_needed; }
    /* Goes on without waiting */
    void proceed();

    /* Of the build running, or of the last one */
    QString log() const { return m_log; }
    QString phase() const { return m_phase; }
    bool isCompiling() const { return m_compiling; }
    int progressDone() const { return m_done; }
    int progressTotal() const { return m_total; }
    QStringList missingCommands() const { return m_missing; }
    QString missingInstall() const { return m_install; }
    /* The build's folder, once built */
    QString built() const { return m_built; }
    bool wasStopped() const { return m_stopped; }
    /* Why the last build failed; empty if it ended well or was stopped */
    QString error() const { return m_failure; }

signals:
    void started();
    void phaseChanged(const QString &text);
    void output(const QString &text);
    /* Of the phase: ninja's [done/total], make's percents; total 0 when unknown */
    void progress(int done, int total);
    void waitingChanged();
    /* @error is empty on success */
    void finished(const QString &error);

private:
    void checkMemory();
    void run();
    void readOutput();
    void handleLine(const QString &line);
    void setPhase(const QString &text, bool compiling);
    void signalGroup(int signal);
    void done(int code, bool crashed);
    void finish(const QString &error, bool stopped = false);
    void append(const QString &text);

    QString m_sourceDir;
    QString m_outDir;
    QString m_workDir;
    QString m_meminfo;
    int m_jobs = 0;
    QString m_plasma;

    bool m_running = false;
    QProcess *m_process = nullptr;
    qint64 m_pid = 0;
    bool m_cancelled = false;
    bool m_stopped = false;
    QTimer *m_memoryTimer;
    qint64 m_available = 0;
    qint64 m_needed = 0;
    QString m_partial;
    QString m_log;
    QString m_phase;
    bool m_compiling = false;
    int m_done = 0;
    int m_total = 0;
    QStringList m_missing;
    QString m_install;
    QString m_error;
    QString m_failure;
    QString m_built;
};
