// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDateTime>
#include <functional>
#include <QObject>
#include <QStringList>

class QProcess;

/*
 * Builds QEMU from a git checkout, by default a clone of the qemu-gui fork
 * the manager keeps in its data folder: fetches the branch, configures the
 * build tree (again only when the options change) and compiles just the
 * emulator and qemu-img.  QEMU then runs from the build tree, which also
 * gives the reference the full documentation.
 *
 * It can first build a virglrenderer of its own, patched and with the DRM
 * native context renderers of your choice, into the data folder as well.
 * QEMU is then built against it, and loads it from there (rpath) rather
 * than the one of the system, which stays as it is.  Rebuilding
 * virglrenderer takes effect at the next start of a VM, without
 * rebuilding QEMU.
 */
class QemuBuilder : public QObject
{
    Q_OBJECT

public:
    struct Virgl {
        bool enabled = false;
        /* Holds src/ (the checkout, reset and cleaned for each build),
           patches/, build/ and install/ */
        QString dir;
        QString url;
        /* A branch or tag; empty for main, else the newest of the last
           releases that the patches apply to, else main without them */
        QString ref;
        /* Files; http(s) and file URLs, downloaded for each build; or
           resources of the manager (:/patches/...), written out for each */
        QStringList patches;
        /* -Ddrm-renderers, e.g. xe-experimental: those the checkout does
           not know are left out */
        QStringList renderers;
        /* If the Vulkan headers are there */
        bool venus = false;
        QStringList mesonArgs;
    };

    struct Options {
        QString sourceDir;          // cloned from url if it doesn't exist
        QString url;
        QString branch;
        /* Fetch the branch first, discarding changes to the checkout */
        bool update = true;
        QStringList configureArgs;
        Virgl virgl;
        /* Parallel compile jobs, 0 for defaultJobs() */
        int jobs = 0;
    };

    static QString defaultSourceDir();
    static QString defaultUrl();
    static QString defaultBranch();
    static QStringList defaultConfigureArgs();
    /* build-qgm in @sourceDir */
    static QString buildDir(const QString &sourceDir);
    static QString binary(const QString &sourceDir);
    /* The commit of @sourceDir the last build that ended well built; empty if none */
    static QString builtCommit(const QString &sourceDir);
    /* Its branch; empty if built before the manager recorded it */
    static QString builtBranch(const QString &sourceDir);
    /*
     * The folder of a QEMU branch whose *.patch files its virglrenderer
     * needs, applied after the virgl patches: contrib/qemu-gui/virglrenderer
     */
    static QString branchVirglPatches(const QString &sourceDir);
    /* The branch names of `git ls-remote --heads` */
    static QStringList parseHeads(const QByteArray &lsRemote);

    static QString defaultVirglDir();
    static QString defaultVirglUrl();
    /* Every DRM native context renderer: those a checkout lacks are left out */
    static QStringList allRenderers();
    /* Native context for every GPU, and Venus: what the Build QEMU window builds */
    static Virgl defaultVirgl();
    /* The Xe native context patch of github.com/cmspam/xe-native-context-enablement */
    static QString xePatchUrl();
    /*
     * Ours, a resource: AMD host blobs mapped write-combined, else the
     * guest's CPU writes into its upload buffers stall and KDE stutters
     */
    static QString amdgpuWcPatch();
    /* Where the virglrenderer built in @virglDir installs its library */
    static QString virglLibDir(const QString &virglDir);
    /* The libvirglrenderer that @binary loads, as ldd resolves it */
    static QString loadedVirgl(const QString &binary);
    /* The cores, but no more than the memory affords, about 1 GiB each */
    static int defaultJobs();

    explicit QemuBuilder(QObject *parent = nullptr);
    ~QemuBuilder() override;

    bool isRunning() const { return m_process != nullptr; }
    void start(const Options &options);
    void cancel();

signals:
    /* e.g. "Configuring" */
    void stepStarted(const QString &description);
    /* The output of the tools, as it comes */
    void output(const QString &text);
    /* From ninja's [done/total] */
    void progress(int done, int total);
    /* @error is empty on success */
    void finished(const QString &error);

private:
    struct Step {
        QString description;
        QString program;
        QStringList args;
        QString dir;
        bool configure = false;
        QStringList env = {};       // NAME=value
        QString shown = {};         // for the log, if not the program and args
        QString product = {};       // a file it makes: unchanged, it had nothing to do
        std::function<bool()> skip = {};    // asked when its turn comes
    };

    void addVirglSteps(const Virgl &virgl, int jobs, const QString &branchPatches);
    QString configureStamp() const;
    void runNext();
    void parseProgress(const QString &text);

    QList<Step> m_steps;
    QDateTime m_productTime;        // of the running step's product, before it
    Options m_options;
    QStringList m_configureArgs;    // with those virglrenderer adds
    QProcess *m_process = nullptr;
    QString m_line;
    bool m_cancelled = false;
};
