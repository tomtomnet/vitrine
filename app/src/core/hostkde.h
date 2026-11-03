// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

class QProcess;

/*
 * The host's KDE packages (host/kde, README.md there): this computer's own
 * KWin, KSvg, Kirigami and Qt base with the research's host sets, optional
 * components of the Builds window ("Host desktop"), each a section of
 * host/kde/components.conf (Target).  host/kde/build.sh builds one in a
 * container with the research's recipe; host/kde/install.sh installs it as
 * root (pkexec), version-locked, or puts the distribution's packages back.
 *
 * Here: what the system has (System: rpm, dnf), what the recipe makes of it
 * (Recipe: the Fedora it builds for, the set for each package's version),
 * what Vitrine built (Build) and installed (Record) of each, and what all
 * that makes of each package (Status).  A package is Vitrine's when its
 * header digest (rpm's SHA256HEADER) is that of one of Vitrine's builds or
 * of its record: a patched one from elsewhere (the research's own, say)
 * never passes for it, whatever its version says.
 */
namespace HostKde {

/* host/kde: $VITRINE_HOST_KDE_DIR, else the kde/ of StackBuilder::hostDir(); empty if none */
QString sourceDir();
/* Where the packages' builds go, a folder each (Target::outDir): <data>/vitrine */
QString dataDir();
/* build.sh's work folder (kde-rpms.sh's VGD_BUILD) and dnf's cache, one for
   all the packages: <cache>/vitrine/host-kde-build */
QString workDir();
/* An older Vitrine's, of its KWin only: <cache>/vitrine/host-kwin-build */
QString oldWorkDir();

/* One of the host's packages Vitrine can replace: a section of host/kde/components.conf */
struct Target {
    QString id;                 // host-kwin
    QString title;              // KWin
    QString package;            // the source package the recipe builds: kwin
    QString follows;            // the installed package whose version says the
                                // distribution's release: plasma-workspace; empty: the package itself
    int parts = 3;              // of a version, those that make a release: 3 (6.7.5), 2 (KF's 6.31)
    QStringList against;        // what a build must match: qt6-qtbase qt6-qtdeclarative...
    QString patches;            // research/patches/kwin
    bool heavy = false;         // its build waits until the container's cap is free

    bool isValid() const { return !id.isEmpty() && !package.isEmpty(); }
    /* Its builds and record: @dataDir/<id> */
    QString outDir(const QString &dataDir) const { return dataDir + '/' + id; }
    /* The package whose version says which release the distribution has: follows, else the package */
    QString reference() const { return follows.isEmpty() ? package : follows; }

    static QList<Target> parse(const QByteArray &conf);
    /* The sections of @sourceDir's components.conf */
    static QList<Target> read(const QString &sourceDir);
};
/* Of @targets: the one called @id, or none */
Target target(const QList<Target> &targets, const QString &id);

/* "6.7.91" of "6.7.91.1" (@parts 3), "6.31" of "6.31.1" (@parts 2): the release a version belongs to */
QString release(const QString &version, int parts);
/* rpm's comparison of two versions or VERSION-RELEASE: 1, 0 or -1 (rpmvercmp, per part) */
int compareEvr(const QString &a, const QString &b);
/* What a user calls a package: Plasma, Qt, the KDE Frameworks... */
QString packageTitle(const QString &name);
/* A shell pattern of versions.sh as a user reads it: 6.8.x for 6.8.* */
QString versionText(const QString &pattern);

/* A host set of the recipe, as research/setup/versions.sh's kde_set_for maps
   a package's versions to them: for the window (build.sh decides with bash) */
struct Set {
    QString series;             // hl39
    QString tag;                // what the RPMs' release gets
    QStringList versions;       // the versions it is for, as shell patterns: 6.7.91, 6.8.*
    QStringList excluded;       // its patches a build leaves out, by the start of their names: 0082
};
/* The host sets of @package in host/kde @sourceDir, in versions.sh's order */
QList<Set> sets(const QString &sourceDir, const QString &package);

/* A package installed */
struct Package {
    QString name;
    QString version;
    QString release;
    QString arch;
    QString digest;             // rpm's SHA256HEADER
    QString vendor;
    QString sourceRpm;
    QString repo;               // dnf's from_repo: "@commandline" for a local file
    QDateTime installed;

    bool isValid() const { return !name.isEmpty(); }
    /* 6.7.91-1.hl39.fc44 */
    QString evr() const;
    /* kwin-6.7.91-1.hl39.fc44.x86_64 */
    QString nevra() const;
    /* Installed from a file, not from a repository */
    bool isLocal() const;
    /* Its source package: "qt6-qtbase" of qt6-qtbase-6.11.2-3.fc44.src.rpm */
    QString source() const;
};

/* What the system has */
struct System {
    bool rpm = false;           // rpm and dnf5
    bool versionlock = false;   // dnf's versionlock command
    bool pkexec = false;
    QString fedora;             // os-release's VERSION_ID on Fedora; empty elsewhere
    QList<Package> packages;    // those of the targets' source packages (debug information
                                // aside), plasma-workspace, and the targets' follows and against
    QHash<QString, QString> locks;  // versionlock's entries: name -> EVR (empty: another comparator)
    QHash<QString, QString> newest; // the repositories' newest VERSION-RELEASE of each target's
                                    // package, from dnf's cache (nothing downloaded)
    QDateTime session;          // when this user's kwin_wayland started; invalid if none

    Package package(const QString &name) const;
    /* The installed packages of source package @source */
    QList<Package> of(const QString &source) const;
    /* plasma-workspace's version */
    QString plasma() const;
    /* rpm, dnf5 and Plasma: where the components are offered at all */
    bool isPlasmaHost() const;

    /* rpm -q's lines (rpmFormat()), dnf repoquery's (NAME FROM_REPO), dnf
       versionlock list's output, dnf repoquery's NAME ARCH VERSION-RELEASE */
    static QList<Package> parsePackages(const QByteArray &rpm, const QByteArray &repos);
    static QHash<QString, QString> parseLocks(const QByteArray &list);
    static QHash<QString, QString> parseNewest(const QByteArray &list, const QList<Package> &installed);
    /* With rpm, dnf and pkexec as PATH has them, @osRelease and /proc: the
       packages of @targets */
    static System read(const QList<Target> &targets,
                       const QString &osRelease = QStringLiteral("/etc/os-release"),
                       const QString &proc = QStringLiteral("/proc"));
};

/* rpm -q's --qf for parsePackages() */
QString rpmFormat();
/* When this user's newest kwin_wayland started, from @proc; invalid if none */
QDateTime sessionStart(const QString &proc);

/* What the recipe makes of this host: build.sh --print-recipe FEDORA PACKAGE=VERSION... */
struct Recipe {
    QString fedora;             // the Fedora its container builds for
    QString copr;               // dnf's id of the COPR its PLASMA=beta enables
    QHash<QString, QStringList> sets;   // package -> its set for the version asked: SERIES TAG
    QHash<QString, QString> keys;       // package -> the key of its inputs: a build of
                                        // other inputs is out of date

    bool isValid() const { return !fedora.isEmpty() && !keys.isEmpty(); }
    QString series(const QString &package) const { return sets.value(package).value(0); }
    QString tag(const QString &package) const { return sets.value(package).value(1); }
    QString key(const QString &package) const { return keys.value(package); }
    static Recipe parse(const QByteArray &output);
    /* For @versions, package -> version */
    static Recipe read(const QString &sourceDir, const QString &fedora,
                       const QHash<QString, QString> &versions);
};

/* A build of build.sh: <out>/<EVR>/build.conf and the RPMs in <out>/<EVR>/<package>/ */
struct Build {
    struct Rpm {
        QString name;
        QString arch;
        QString digest;         // rpm's SHA256HEADER
        QString file;           // in the build's folder: kwin/kwin-6.7.91-1.hl39.fc44.x86_64.rpm
    };

    QString dir;
    QString package;            // kwin (an older Vitrine's builds say nothing: KWin's)
    QString evr;                // 6.7.91-1.hl39.fc44
    QString version;            // 6.7.91
    QString series;
    QString tag;
    QString plasma;             // beta, testing, fedora
    QString fedora;
    QString key;                // of the recipe's inputs it was built from
    QString research;           // the research's commit of the recipe
    QDateTime built;
    QList<Rpm> rpms;
    QHash<QString, QString> buildroot;  // what it was built against: name -> EVR

    bool isValid() const { return !evr.isEmpty() && !rpms.isEmpty(); }
    Rpm rpm(const QString &name) const;
    /* The source package's VERSION-RELEASE it was built from: 6.7.91-1.fc44 of 6.7.91-1.hl39.fc44 */
    QString sourceEvr() const;
    static Build parse(const QString &dir, const QByteArray &conf, const QByteArray &buildroot);
    static Build read(const QString &dir);
};

/* The builds in @out, the newest first */
QList<Build> builds(const QString &out);

/* What Vitrine installed: <out>/installed.conf, written once install.sh
   succeeded, gone once it reverted */
struct Record {
    QString evr;
    QString series;
    QString tag;                // the RPMs' release has it (an older record: none)
    QDateTime when;
    QHash<QString, QString> digests;    // name -> SHA256HEADER
    QHash<QString, QString> buildroot;  // the target's against as the set was built against them

    bool isValid() const { return !digests.isEmpty(); }
    QByteArray serialize() const;
    static Record parse(const QByteArray &conf);
    static Record read(const QString &path);
    bool write(const QString &path) const;
    static QString path(const QString &out);
    /* Of @build, installed now; the versions of @against it was built with */
    static Record of(const Build &build, const QDateTime &when, const QStringList &against);
};

/* What it all makes of one package */
struct Status {
    enum class Offer {
        Hidden,             // no Fedora Plasma with rpm and dnf5 here, or not installed: not shown
        Unsupported,        // a Fedora or a Plasma the recipe does not build for (why)
        NoSet,              // no host set for the version
        Offered,
    };
    enum class Owner {
        Distribution,       // the repositories' package, not locked
        Vitrine,            // one Vitrine installed
        Foreign,            // another: installed from a file, or locked by someone else
    };

    Target target;
    Offer offer = Offer::Hidden;
    QString why;            // Unsupported's reason
    QString version;        // the version a set is looked up for, and a build is for
    QString basis;          // what it comes from, in words: "Plasma 6.8.0", "KDE Frameworks
                            // 6.32.0", "Qt 6.11.3", or the repositories' "6.11.3"
    bool basisInRepositories = false;   // basis is the repositories' newest
    QString plasma;         // plasma-workspace's version
    QString plasmaFrom;     // beta, testing, fedora: where Plasma comes from
    QString series;         // the set for the version
    QString tag;
    Build build;            // the newest build for this version; invalid if none
    Build other;            // else the newest of another version, if any
    QStringList differs;    // how the build differs from the system: "qt6-qtbase 6.11.2 6.11.3"
                            // (the build's, the system's), its follows' too
    bool recipeChanged = false; // the build is from another recipe than Vitrine's now
    Owner owner = Owner::Distribution;
    Package installed;      // its package installed (kwin, qt6-qtbase...)
    QStringList names;      // the names of its source package's packages installed
    bool locked = false;    // the package is version-locked
    QString lockedAt;       // at what
    QStringList outdated;   // Vitrine's against the system now: "plasma-workspace 6.7.91 6.8.0"
    QString heldBack;       // Vitrine's locked, and the repositories have this newer VERSION-RELEASE
    bool nextLogin = false; // installed or reverted after the session's KWin started
    bool versionlock = false;   // dnf has its versionlock command
    bool pkexec = false;        // pkexec is there to ask for the password

    /* "Built, not installed": what the row says */
    QString summary() const;
    /* Whether a build may start; why not */
    bool canBuild(QString *why = nullptr) const;
    bool canInstall(QString *why = nullptr) const;
    bool canRevert(QString *why = nullptr) const;
    /* The installed one is Vitrine's, and no longer matches the system: the
       next login or program would run it against other libraries */
    bool isOutdated() const { return owner == Owner::Vitrine && !outdated.isEmpty(); }
    /* Update All builds it only while Vitrine's is installed */
    bool wantsUpdates() const { return owner == Owner::Vitrine && offer == Offer::Offered; }
};

/* The digests that make @target's installed package Vitrine's: its record's and builds' */
QStringList digests(const Target &target, const QList<Build> &builds, const Record &record);
/* The version a set of @target is looked up for: the installed package's
   own while it is the distribution's, else its follows' (the release the
   distribution has moved to); the repositories' newest when it is newer
   (what a build of it takes now) */
QString versionOf(const Target &target, const System &system, const QStringList &mine);

Status status(const Target &target, const System &system, const Recipe &recipe,
              const QList<Build> &builds, const Record &record);

/* pkexec bash <install.sh> install FILE=SHA256...: @build's RPMs */
QStringList installCommand(const QString &sourceDir, const Build &build);
/* The same for several builds at once: one password, one transaction (install.sh groups
   their files by source package) */
QStringList installCommand(const QString &sourceDir, const QList<Build> &builds);
/* pkexec bash <install.sh> revert PACKAGE=DIGEST...: for each package, the
   digests of Vitrine's (its builds' and its record's) */
QStringList revertCommand(const QString &sourceDir, const QHash<QString, QStringList> &digests);

}

/*
 * The app's view of them: the system read again in the background (rpm,
 * dnf, build.sh --print-recipe: about a second) when asked, then the status
 * of each package, which the Builds window and the main window's banner
 * show.  Install and Revert run install.sh with pkexec (the desktop's
 * password dialog), one at a time, their output as it comes.
 */
class HostKdeMonitor : public QObject
{
    Q_OBJECT

public:
    static HostKdeMonitor *instance();

    explicit HostKdeMonitor(QObject *parent = nullptr);
    ~HostKdeMonitor() override;

    /* Defaults: HostKde's folders, /etc/os-release, /proc */
    void setSourceDir(const QString &dir) { m_sourceDir = dir; }
    void setDataDir(const QString &dir) { m_dataDir = dir; }
    void setOsRelease(const QString &path) { m_osRelease = path; }
    void setProc(const QString &path) { m_proc = path; }
    QString sourceDir() const;
    QString dataDir() const;
    /* The packages of the source folder's components.conf, as last read */
    QList<HostKde::Target> targets() const;
    HostKde::Target target(const QString &id) const;
    QString outDir(const QString &id) const;

    /* What a reading brings back */
    struct Reading {
        QList<HostKde::Target> targets;
        HostKde::System system;
        HostKde::Recipe recipe;
    };
    /* The packages of host/kde @source, the system, and the recipe for it
       (the builds and records in @dataDir's folders say whose packages are
       installed): the processes it takes, in whatever thread */
    static Reading read(const QString &source, const QString &dataDir, const QString &osRelease,
                        const QString &proc);

    /* Read once at least */
    bool isRead() const { return m_read; }
    HostKde::System system() const { return m_system; }
    HostKde::Recipe recipe() const { return m_recipe; }
    /* Of the system as last read, the builds and the record as they are now */
    HostKde::Status status(const QString &id) const;
    QList<HostKde::Status> statuses() const;
    QList<HostKde::Build> builds(const QString &id) const;
    HostKde::Record record(const QString &id) const;

    /* The system again, in the background; changed() then */
    void refresh();
    /* The same, unless a reading is under way, which tells as much, or an
       install or a revert, whose end reads again: for vitrine back in
       front, where several windows ask at once */
    void lookAgain();
    /* Now, before it returns (the tests) */
    void refreshNow();

    bool isBusy() const { return m_process != nullptr; }
    /* The packages the install or revert running is about */
    QStringList busyWith() const { return m_busy; }
    /* install.sh, with pkexec: @build's RPMs, then the record of @id */
    void install(const QString &id, const HostKde::Build &build);
    /* The same for several packages at once (the group's Install All…): each id with its
       build, one password, one transaction, then the record of each */
    void install(const QList<std::pair<QString, HostKde::Build>> &builds);
    /* install.sh revert, with pkexec, of @ids at once: their records go */
    void revert(const QStringList &ids);
    /* Of the last install or revert */
    QString log() const { return m_log; }

signals:
    void changed();
    void started(bool install);
    void output(const QString &text);
    /* @error is empty on success; @cancelled when the password dialog was dismissed */
    void finished(const QString &error, bool cancelled);

private:
    void apply(const Reading &reading);
    void run(const QStringList &command, const QStringList &ids, bool install,
             const QList<std::pair<QString, HostKde::Build>> &installs);

    QString m_sourceDir;
    QString m_dataDir;
    QString m_osRelease = QStringLiteral("/etc/os-release");
    QString m_proc = QStringLiteral("/proc");
    bool m_read = false;
    bool m_reading = false;
    bool m_again = false;
    QList<HostKde::Target> m_targets;
    HostKde::System m_system;
    HostKde::Recipe m_recipe;
    QProcess *m_process = nullptr;
    QStringList m_busy;
    QString m_log;
    QString m_partial;
    QString m_error;
};
