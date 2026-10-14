// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDateTime>
#include <QObject>
#include <QStringList>

class QProcess;

/*
 * Vitrine's QEMU: upstream QEMU and virglrenderer at the commits of
 * host/versions.conf, with host/patches (for QEMU: the qemu-gui fork's
 * commits, the research series, vitrine's), which host/build.sh builds into
 * folders of their own under Paths::stackDir() (QEMU in <stamp>/, and
 * `current` pointing to the last complete build; virglrenderer in
 * virglrenderer/<key>/).  This runs build.sh and follows its progress; the
 * static functions tell what is built and whether it is what host/ asks
 * for.  The stamp is QEMU's key (Builds::key()), which sums up its inputs
 * and virglrenderer's: a build of other inputs is out of date.
 */
class StackBuilder : public QObject
{
    Q_OBJECT

public:
    /* host/versions.conf */
    struct Versions {
        QString qemuUrl;
        QString qemuCommit;
        QString virglUrl;
        QString virglCommit;

        bool isValid() const;
        /* NAME=value lines; # comments */
        static Versions parse(const QByteArray &conf);
    };

    /* What a complete build has: <prefix>/share/vitrine/stack.conf */
    struct Build {
        QString prefix;
        QString stamp;
        QDateTime built;
        QString qemuCommit;
        QString qemuVersion;        // e.g. 11.1.50
        QStringList qemuPatches;    // their paths in patches/qemu: research/0001-x.patch
        QString virglCommit;
        QString virglVersion;
        QStringList virglPatches;
        /* the virglrenderer it is built against, in a folder of its own;
           none in a build of an older Vitrine (in the prefix's lib64) */
        QString virglKey;
        QString virglPrefix;

        bool isValid() const { return !stamp.isEmpty(); }
        QString qemuBinary() const;
        /* "QEMU 11.1.50 + 7 patches" */
        QString summary() const;
        static Build parse(const QString &prefix, const QByteArray &manifest);
        /* Invalid if @prefix holds no complete build */
        static Build read(const QString &prefix);
    };

    enum class State {
        NoSources,      // no host/ to build from
        NotBuilt,
        Outdated,       // built from other inputs than host/'s
        UpToDate,
    };

    /* A line of build.sh's output */
    struct Line {
        enum Kind {
            Other, Step, Progress, Missing, Install, Warning, UpToDate, Built, Error,
            Component,      // component: NAME, whose lines follow
            Done,           // done: NAME built|up to date FOLDER
        };
        Kind kind = Other;
        int done = 0;               // Step: its number; Progress: ninja's [done/total]
        int total = 0;
        QString text;               // the description, dependency, command, prefix or message
        QString component;          // Component, Done: its name
        bool built = false;         // Done: built, not up to date already
        QString folder;             // Done: where it is

        static Line parse(const QString &line);
    };

    /*
     * The host/ folder: $VITRINE_HOST_DIR, else installed with the app
     * (<prefix>/share/vitrine/host), else the source tree's for a build
     * tree's app, else in the data folders; empty if none
     */
    static QString hostDir();
    /* Sources and build trees: they make updates quick, and can go */
    static QString workDir();
    static Versions versions(const QString &hostDir);
    /*
     * The patches build.sh applies to @component ("qemu"), in its order:
     * those patches/@component/series names, else the *.patch files of
     * @hostDir/patches/@component and of its folders, as paths in @hostDir
     * (patches/qemu/research/0001-x.patch)
     */
    static QStringList patches(const QString &hostDir, const QString &component);
    /* The stamp of @hostDir's inputs: what `build.sh --print-stamp` prints */
    static QString inputStamp(const QString &hostDir);
    /* The build `current` points to in @stackDir; invalid if none */
    static Build current(const QString &stackDir);
    static Build current();
    static State state(const QString &hostDir, const QString &stackDir);
    static State state();
    /*
     * Removes the builds of @stackDir nothing needs any more, some 170 MB
     * each: all but `current`, the build before it (for going back: inputs
     * that come back to it need no build), those a running process was
     * started from (any VM's QEMU, also of another vitrine), and those of
     * the binaries in @keep (the QEMU of the preferences, those of the VMs'
     * #qemu); and the virglrenderer folders none of those kept is built
     * against, but the newest.  Leftovers of builds that never ended go
     * too.  Nothing while a build holds the stack's lock.  Returns the
     * folders removed.
     */
    static QStringList prune(const QString &stackDir, const QStringList &keep);
    /* The cores, but no more than the memory affords, about 1 GiB each */
    static int defaultJobs();

    /* The app's: builds run one at a time, whichever window shows them */
    static StackBuilder *instance();

    explicit StackBuilder(QObject *parent = nullptr);
    ~StackBuilder() override;

    /* Defaults: hostDir(), Paths::stackDir(), workDir(), defaultJobs() */
    void setHostDir(const QString &dir) { m_hostDir = dir; }
    void setStackDir(const QString &dir) { m_stackDir = dir; }
    void setWorkDir(const QString &dir) { m_workDir = dir; }
    void setJobs(int jobs) { m_jobs = jobs; }

    bool isRunning() const { return m_process != nullptr; }
    /*
     * Builds @target ("virglrenderer", or "qemu", the default: the whole
     * stack) and what it needs; @clean: its build trees from nothing
     * (build.sh --clean)
     */
    void start(const QString &target = {}, bool clean = false);
    /* Stops build.sh and what it runs: the whole process group */
    void cancel();

    /* Of the build running, or of the last one */
    QString log() const { return m_log; }
    int step() const { return m_step; }
    int steps() const { return m_steps; }
    QString stepText() const { return m_stepText; }
    /* What build.sh said is missing, and the command installing it */
    QStringList missing() const { return m_missing; }
    QString installCommand() const { return m_install; }
    QStringList warnings() const { return m_warnings; }
    /* The last build ended with Stop */
    bool wasStopped() const { return m_cancelled && !m_process; }
    /* What the last build was for: "qemu", "virglrenderer" */
    QString target() const { return m_target; }

signals:
    void started();
    void stepStarted(int step, int total, const QString &text);
    /* build.sh goes on with @component ("virglrenderer", "qemu") */
    void componentStarted(const QString &component);
    /* It is there: built now, or up to date already */
    void componentDone(const QString &component, bool built, const QString &folder);
    /* The output, line by line as it comes, each once handled */
    void output(const QString &text);
    /* From ninja's [done/total] */
    void progress(int done, int total);
    /* @error is empty on success */
    void finished(const QString &error);
    /* After each build that ended well, with the QEMU now `current` */
    void built(const QString &qemuBinary);

private:
    void readOutput();
    void handleLine(const QString &line);
    void signalGroup(int signal);
    void done(int code, bool crashed);

    QString m_hostDir;
    QString m_stackDir;
    QString m_workDir;
    int m_jobs = 0;

    QProcess *m_process = nullptr;
    qint64 m_pid = 0;
    bool m_cancelled = false;
    QString m_partial;          // a line not ended yet
    QString m_log;
    int m_step = 0;
    int m_steps = 0;
    QString m_stepText;
    QStringList m_missing;
    QString m_install;
    QStringList m_warnings;
    QString m_error;
    QString m_prefix;           // the one built, or up to date
    QString m_stack;            // of the build running
    QString m_before;           // the prefix `current` named when it started
    QString m_target;
};
