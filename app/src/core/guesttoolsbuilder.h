// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QStringList>

class QProcess;
class QTimer;

/*
 * Builds the guest tools from the app, as StackBuilder builds Vitrine's
 * QEMU: the packages of guest/ in a Fedora container (guest/build-rpms.sh,
 * rootless podman), one after the other, then the medium the VMs install
 * them from (guest/build-medium.sh), where GuestTools::medium() finds it.
 *
 * Only the packages whose sources changed since their last build are built
 * again, by the scripts' notion: the hash of their inputs (inputsHash()),
 * which a package's build records in its folder and the medium's manifest
 * records for each package on it.  Mesa and KWin take up to the container's
 * memory cap: before each, the build waits until that much memory is free,
 * or until told to go on anyway.  A failed or stopped build keeps the last
 * good packages and medium.  One build at a time, and none while Vitrine's
 * QEMU builds: both take a lot of memory.
 */
class GuestToolsBuilder : public QObject
{
    Q_OBJECT

public:
    enum class State {
        NoSources,      // no guest/ to build from
        NotBuilt,       // no medium
        Outdated,       // a medium made from other sources than guest/'s
        UpToDate,
    };

    /* A step of a build: a package of build-rpms.sh, or the medium */
    struct Step {
        QString name;           // tools, mesa, kwin, medium
        QString text;           // "building Mesa 26.2.3"
        bool heavy = false;     // takes up to the container's memory cap
    };

    /* A line of the scripts' output */
    struct Line {
        enum Kind {
            Other,
            Building,       // vitrine-build: building: PACKAGE (log: FILE)
            Phase,          // vitrine-build: phase: WHAT
            Section,        // rpmbuild's: Executing(%build), Processing files
            Counted,        // [done/total]: ninja's, dnf's
            Percent,        // [ 45%]: make's (CMake)
            UpToDate,
            Built,
            Missing,
            Install,
            Warning,
            Error,
        };
        Kind kind = Other;
        int done = 0;           // Counted, Percent (total 100)
        int total = 0;
        QString text;           // after the tag; Section: build, install, files...

        static Line parse(const QString &line);
    };

    /* What a medium's manifest says of how it was made */
    struct Made {
        QDateTime built;
        /* tools, mesa, kwin (those on it), medium: the hashes of their inputs */
        QHash<QString, QString> inputs;

        static Made parse(const QByteArray &manifest);
    };

    /* What the build needs that this host lacks */
    struct Requirement {
        QString command;        // podman
        QString package;        // Fedora's: podman
    };

    /*
     * The guest/ folder: $VITRINE_GUEST_DIR, else installed with the app
     * (<prefix>/share/vitrine/guest), else the source tree's for a build
     * tree's app, else in the data folders; empty if none
     */
    static QString guestDir();
    /* build-rpms.sh's output: <data>/guest-tools/rpms/fcRELEASE */
    static QString rpmsDir();
    /* Its downloads, kept for the next build: <cache>/guest-build/fcRELEASE */
    static QString cacheDir();
    /* The medium build-medium.sh makes, where GuestTools::medium() reads it */
    static QString mediumImage();
    /* The manifest beside @image */
    static QString manifestOf(const QString &image);

    /* The packages @guestDir builds: tools, mesa, kwin, those it has */
    static QStringList packages(const QString &guestDir);
    /* The Version of @package's spec, "" if none */
    static QString version(const QString &guestDir, const QString &package);
    /* What build-rpms.sh's inputs_hash() prints: @package's folder and the
       build script; empty if a file cannot be read */
    static QString inputsHash(const QString &guestDir, const QString &package);
    /* What build-medium.sh records of itself */
    static QString mediumHash(const QString &guestDir);
    /* The inputs @package's last build in @rpmsDir was made from; empty if none */
    static QString builtInputs(const QString &rpmsDir, const QString &package);
    static Made made(const QString &image);
    static State state(const QString &guestDir, const QString &image);
    static State state();
    /* The steps a build takes now: the packages not built from @guestDir's
       sources in @rpmsDir, then the medium */
    static QList<Step> plan(const QString &guestDir, const QString &rpmsDir);

    /* The commands the scripts need that are not in PATH */
    static QList<Requirement> missing();
    /* "sudo dnf install podman mtools" */
    static QString installCommand(const QList<Requirement> &missing);
    /* The memory cap of a package's container: $MEMORY as podman reads it, else 10 GiB */
    static qint64 memoryCapMiB();

    /* The app's: builds run one at a time, whichever window shows them */
    static GuestToolsBuilder *instance();

    explicit GuestToolsBuilder(QObject *parent = nullptr);
    ~GuestToolsBuilder() override;

    /* Defaults: guestDir(), rpmsDir(), cacheDir(), mediumImage(), /proc/meminfo */
    void setGuestDir(const QString &dir) { m_guestDir = dir; }
    void setRpmsDir(const QString &dir) { m_rpmsDir = dir; }
    void setCacheDir(const QString &dir) { m_cacheDir = dir; }
    void setMediumImage(const QString &image) { m_image = image; }
    void setMeminfo(const QString &path) { m_meminfo = path; }
    /* How often a build waiting for memory looks again */
    void setMemoryPoll(int ms);

    /* A script runs, or the build waits for memory */
    bool isRunning() const { return m_running; }
    void start();
    /* Stops the script and what it runs (the whole process group; the
       script removes its container), or the wait */
    void cancel();

    /* Waiting for memory before a heavy package */
    bool isWaiting() const;
    /* Then: what is free, and what the package needs (its cap, and what
       must stay free for the host) */
    qint64 availableMiB() const { return m_available; }
    qint64 neededMiB() const { return m_needed; }
    /* Goes on without waiting for it */
    void proceed();

    /* Of the build running, or of the last one */
    QString log() const { return m_log; }
    QList<Step> steps() const { return m_plan; }
    /* 1-based; 0 before the first */
    int step() const { return m_index + 1; }
    QString stepText() const { return m_index >= 0 ? m_plan.value(m_index).text : QString(); }
    /* What the step does now: "installing the build dependencies", "compiling" */
    QString phase() const { return m_phase; }
    /* The phase's progress; total 0 when unknown */
    int progressDone() const { return m_done; }
    int progressTotal() const { return m_total; }
    /* What is missing for the build, and the command installing it */
    QStringList missingCommands() const { return m_missing; }
    QString missingInstall() const { return m_install; }
    QStringList warnings() const { return m_warnings; }
    /* The last build ended with Stop */
    bool wasStopped() const { return m_stopped; }
    /* Why the last build failed; empty if it ended well or was stopped */
    QString error() const { return m_failure; }

signals:
    void started();
    void stepStarted(int step, int total, const QString &text);
    void phaseChanged(const QString &text);
    /* The output, as it comes */
    void output(const QString &text);
    /* Of the phase: ninja's [done/total], make's percents, dnf's counts */
    void progress(int done, int total);
    /* It waits for memory, or no longer does, or what is free changed */
    void waitingChanged();
    /* @error is empty on success */
    void finished(const QString &error);
    /* After a build that ended well: a new medium */
    void built();

private:
    enum class Counting { None, Dnf, Compile };

    void next();
    void checkMemory();
    void run();
    void readOutput();
    void handleLine(const QString &line);
    void setPhase(const QString &text, Counting counting);
    void signalGroup(int signal);
    void stepDone(int code, bool crashed);
    void finish(const QString &error, bool stopped = false);
    void append(const QString &text);

    QString m_guestDir;
    QString m_rpmsDir;
    QString m_cacheDir;
    QString m_image;
    QString m_meminfo;

    bool m_running = false;
    QList<Step> m_plan;
    int m_index = -1;
    QString m_guest;            // of the build running
    QProcess *m_process = nullptr;
    qint64 m_pid = 0;
    bool m_cancelled = false;
    bool m_stopped = false;
    QTimer *m_memoryTimer;
    qint64 m_available = 0;
    qint64 m_needed = 0;
    QString m_partial;          // a line not ended yet
    QString m_log;
    QString m_phase;
    Counting m_counting = Counting::None;
    int m_done = 0;
    int m_total = 0;
    QStringList m_missing;
    QString m_install;
    QStringList m_warnings;
    QString m_error;            // the scripts' last error line
    QString m_failure;
};
