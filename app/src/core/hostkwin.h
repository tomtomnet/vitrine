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
 * The host's KWin (host/kwin, README.md there): this computer's own
 * compositor with the research's host set, an optional component of the
 * Builds window ("Host desktop").  host/kwin/build.sh builds it in a
 * container with the research's recipe; host/kwin/install.sh installs it
 * as root (pkexec), version-locked, or puts the distribution's KWin back.
 *
 * Here: what the system has (System: rpm, dnf), what the recipe makes of it
 * (Recipe: the Fedora it builds for, the set for this Plasma), what Vitrine
 * built (Build) and installed (Record), and what all that makes of it
 * (Status).  A KWin is Vitrine's when its header digest (rpm's
 * SHA256HEADER) is that of one of Vitrine's builds or of its record: a
 * patched KWin from elsewhere (the research's own, say) never passes for
 * it, whatever its version says.
 */
namespace HostKwin {

/* host/kwin: $VITRINE_HOST_KWIN_DIR, else the kwin/ of StackBuilder::hostDir(); empty if none */
QString sourceDir();
/* Where build.sh puts its builds, and the record: <data>/vitrine/host-kwin */
QString outDir();
/* build.sh's work folder (kde-rpms.sh's VGD_BUILD) and dnf's cache: <cache>/vitrine/host-kwin-build */
QString workDir();

/* The packages of KWin's source RPM (Fedora's spec) */
QStringList kwinPackages();
/* Those whose versions must be those a set was built against: KWin uses
   Qt's and kdecoration's private API, and libplasma is Plasma's */
QStringList abiPackages();
/* "6.7.91" of "6.7.91.1": the Plasma release a version belongs to */
QString plasmaRelease(const QString &version);
/* What a user calls a package of plasmaRelease()'s and abiPackages(): Plasma, Qt... */
QString packageTitle(const QString &name);
/* A shell pattern of versions.sh as a user reads it: 6.8.x for 6.8.* */
QString versionText(const QString &pattern);

/* A host set of the recipe, as research/setup/versions.sh's kde_set_for maps
   KWin's versions to them: for the window (build.sh decides with bash) */
struct Set {
    QString series;             // hl39
    QString tag;                // what the RPMs' release gets
    QStringList versions;       // the versions it is for, as shell patterns: 6.7.91, 6.8.*
};
/* The host sets of host/kwin @sourceDir, in versions.sh's order */
QList<Set> sets(const QString &sourceDir);

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
};

/* What the system has */
struct System {
    bool rpm = false;           // rpm and dnf5
    bool versionlock = false;   // dnf's versionlock command
    bool pkexec = false;
    QString fedora;             // os-release's VERSION_ID on Fedora; empty elsewhere
    QList<Package> kwin;        // KWin's packages installed
    QList<Package> others;      // plasma-workspace and abiPackages()
    QHash<QString, QString> locks;  // versionlock's entries of KWin's packages: name -> EVR
    QDateTime session;          // when this user's kwin_wayland started; invalid if none

    Package package(const QString &name) const;
    /* plasma-workspace's version: the Plasma a KWin must match */
    QString plasma() const;
    /* rpm, dnf5, KWin and Plasma: where the component is offered at all */
    bool isPlasmaHost() const;

    /* rpm -q's lines (Tools::rpmFormat()), dnf repoquery's (NAME FROM_REPO)
       and dnf versionlock list's output */
    static QList<Package> parsePackages(const QByteArray &rpm, const QByteArray &repos);
    static QHash<QString, QString> parseLocks(const QByteArray &list);
    /* With rpm, dnf and pkexec as PATH has them, @osRelease and /proc */
    static System read(const QString &osRelease = QStringLiteral("/etc/os-release"),
                       const QString &proc = QStringLiteral("/proc"));
};

/* rpm -q's --qf for parsePackages() */
QString rpmFormat();
/* When this user's newest kwin_wayland started, from @proc; invalid if none */
QDateTime sessionStart(const QString &proc);

/* What the recipe makes of this host: build.sh --print-recipe PLASMA FEDORA */
struct Recipe {
    QString fedora;             // the Fedora its container builds for
    QString copr;               // dnf's id of the COPR its PLASMA=beta enables
    QString series;             // the set for this Plasma; empty if none
    QString tag;
    QString key;                // of its inputs: a build of other inputs is out of date

    bool isValid() const { return !fedora.isEmpty() && !key.isEmpty(); }
    static Recipe parse(const QByteArray &output);
    static Recipe read(const QString &sourceDir, const QString &plasma, const QString &fedora);
};

/* A build of build.sh: <out>/<EVR>/build.conf and the RPMs in kwin/ */
struct Build {
    struct Rpm {
        QString name;
        QString arch;
        QString digest;         // rpm's SHA256HEADER
        QString file;           // in the build's folder: kwin/kwin-6.7.91-1.hl39.fc44.x86_64.rpm
    };

    QString dir;
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
    QDateTime when;
    QHash<QString, QString> digests;    // name -> SHA256HEADER
    QHash<QString, QString> buildroot;  // abiPackages() as the set was built against them

    bool isValid() const { return !digests.isEmpty(); }
    QByteArray serialize() const;
    static Record parse(const QByteArray &conf);
    static Record read(const QString &path);
    bool write(const QString &path) const;
    static QString path(const QString &out);
    /* Of @build, installed now */
    static Record of(const Build &build, const QDateTime &when);
};

/* What it all makes of the component */
struct Status {
    enum class Offer {
        Hidden,             // no Fedora Plasma with rpm and dnf5 here: not shown
        Unsupported,        // a Fedora or a Plasma the recipe does not build for (why)
        NoSet,              // no host set for this Plasma
        Offered,
    };
    enum class Owner {
        Distribution,       // the repositories' KWin, not locked
        Vitrine,            // one Vitrine installed
        Foreign,            // another: installed from a file, or locked by someone else
    };

    Offer offer = Offer::Hidden;
    QString why;            // Unsupported's reason
    QString plasma;         // plasma-workspace's version
    QString plasmaFrom;     // beta, testing, fedora: where it comes from
    QString series;         // the set for it
    QString tag;
    Build build;            // the newest build for this Plasma; invalid if none
    Build other;            // else the newest of another version, if any
    QStringList differs;    // how the build differs from the system: abiPackages() as "qt6-qtbase 6.11.2 6.11.3"
    bool recipeChanged = false; // the build is from another recipe than Vitrine's now
    Owner owner = Owner::Distribution;
    Package kwin;           // the KWin installed
    bool locked = false;    // KWin is version-locked
    QString lockedAt;       // at what
    QStringList outdated;   // Vitrine's KWin against the system now: "plasma-workspace 6.7.91 6.8.0"
    bool nextLogin = false; // installed or reverted after the session's KWin started
    bool versionlock = false;   // dnf has its versionlock command
    bool pkexec = false;        // pkexec is there to ask for the password

    /* "Built for this Plasma and Qt, not installed": what the row says */
    QString summary() const;
    /* Whether a build may start; why not */
    bool canBuild(QString *why = nullptr) const;
    bool canInstall(QString *why = nullptr) const;
    bool canRevert(QString *why = nullptr) const;
    /* The installed KWin is Vitrine's, and no longer matches Plasma or Qt:
       the next login would run it against other libraries */
    bool isOutdated() const { return owner == Owner::Vitrine && !outdated.isEmpty(); }
    /* Update All builds it only while Vitrine's KWin is installed */
    bool wantsUpdates() const { return owner == Owner::Vitrine && offer == Offer::Offered; }
};

Status status(const System &system, const Recipe &recipe, const QList<Build> &builds,
              const Record &record);

/* pkexec bash <install.sh> install FILE=SHA256...: @build's RPMs */
QStringList installCommand(const QString &sourceDir, const Build &build);
/* pkexec bash <install.sh> revert DIGEST...: the digests of Vitrine's kwin
   packages, of its builds and its record */
QStringList revertCommand(const QString &sourceDir, const QList<Build> &builds,
                          const Record &record);

}

/*
 * The app's view of it: the system read again in the background (rpm, dnf,
 * build.sh --print-recipe: well under a second) when asked, then the status
 * of that, which the Builds window and the main window's banner show.
 * Install and Revert run install.sh with pkexec (the desktop's password
 * dialog), one at a time, their output as it comes.
 */
class HostKwinMonitor : public QObject
{
    Q_OBJECT

public:
    static HostKwinMonitor *instance();

    explicit HostKwinMonitor(QObject *parent = nullptr);
    ~HostKwinMonitor() override;

    /* Defaults: HostKwin's folders, /etc/os-release, /proc */
    void setSourceDir(const QString &dir) { m_sourceDir = dir; }
    void setOutDir(const QString &dir) { m_outDir = dir; }
    void setOsRelease(const QString &path) { m_osRelease = path; }
    void setProc(const QString &path) { m_proc = path; }
    QString sourceDir() const;
    QString outDir() const;

    /* What a reading brings back */
    struct Reading {
        HostKwin::System system;
        HostKwin::Recipe recipe;
        QList<HostKwin::Build> builds;
        HostKwin::Record record;
    };
    /* The system, the recipe of host/kwin @source for it, the builds and
       the record in @out: the processes it takes, in whatever thread */
    static Reading read(const QString &source, const QString &out, const QString &osRelease,
                        const QString &proc);

    /* Read once at least */
    bool isRead() const { return m_read; }
    HostKwin::System system() const { return m_system; }
    HostKwin::Recipe recipe() const { return m_recipe; }
    /* Of the system as last read, the builds and the record as they are now */
    HostKwin::Status status() const;
    QList<HostKwin::Build> builds() const;
    HostKwin::Record record() const;

    /* The system again, in the background; changed() then */
    void refresh();
    /* Now, before it returns (the tests) */
    void refreshNow();

    bool isBusy() const { return m_process != nullptr; }
    /* install.sh, with pkexec: @build's RPMs, then the record */
    void install(const HostKwin::Build &build);
    /* install.sh revert, with pkexec: the record goes */
    void revert();
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
    void run(const QStringList &command, bool install, const HostKwin::Build &build);

    QString m_sourceDir;
    QString m_outDir;
    QString m_osRelease = QStringLiteral("/etc/os-release");
    QString m_proc = QStringLiteral("/proc");
    bool m_read = false;
    bool m_reading = false;
    bool m_again = false;
    HostKwin::System m_system;
    HostKwin::Recipe m_recipe;
    QProcess *m_process = nullptr;
    QString m_log;
    QString m_partial;
    QString m_error;
};
