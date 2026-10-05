// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDate>
#include <QHash>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include "core/vmtemplate.h"

/*
 * What a VM runs, precisely: Fedora Linux 44, Windows 11, Arch Linux.
 *
 * The systems are those of libosinfo's database (osinfo-db), the list
 * virt-manager offers, when it is installed: its XML files, read as
 * libosinfo reads them (its system folder, then /etc/osinfo and
 * ~/.config/osinfo, where osinfo-db-import puts newer ones).  Not through
 * libosinfo itself: a GObject library, whose tools print names for people
 * (osinfo-detect the edition and architecture of the disc it matched, in
 * the user's language), not the ids vitrine keeps.  vitrine's own list adds
 * the main systems the database lacks, and stands in for it when it is not
 * installed.
 *
 * A system is known by the database's short id, e.g. fedora44, which the
 * #guest directive keeps (VmConfig::Guest::id).  vitrine's list uses the
 * same ids, and ids of its own for the systems the database does not have
 * (linuxmint-unknown, kdelinux).
 */
namespace GuestOs {

/*
 * What the primary volume descriptor of an installation disc says when the
 * disc is of a system: regular expressions (PCRE, as GLib's), any of them
 * empty for any value
 */
struct DiscRule {
    QRegularExpression volume;
    QRegularExpression system;
    QRegularExpression publisher;
    QRegularExpression application;
    qint64 size = 0;            // the volume's, in bytes; 0 for any
    /* For every architecture: a match only when no other rule matches */
    bool fallback = false;
};

struct Os {
    QString id;                 // short id: fedora44, win11, archlinux
    QStringList aliases;        // other short ids of it: debianbookworm
    QString name;               // Fedora Linux 44
    QString version;            // 44; "unknown" for a release the list has not
    QString family;             // the kernel's, as libosinfo says: linux, winnt, freebsd...
    QString distro;             // fedora, ubuntu, win, archlinux...
    QDate released;
    QDate eol;
    QString status;             // rolling, prerelease or snapshot; empty for a release
    QStringList editions;       // the names of its variants: Fedora Workstation 43
    QList<DiscRule> discs;
    bool builtIn = false;       // from vitrine's list

    bool isNull() const { return id.isEmpty(); }
    /* The distribution, its release not listed: fedora-unknown */
    bool isGeneric() const;
    /* No longer supported, as virt-manager tells: after its end of life;
       without one, released more than five years ago, unless rolling */
    bool isEol(const QDate &today = QDate::currentDate()) const;
};

/* The #guest directive's family of @os: linux, windows or other */
QString guestFamily(const Os &os);
/* The template a new VM of @os gets */
VmTemplate::Os templateOs(const Os &os);
/* Fedora Linux, which the guest tools are for: its releases, not its
   atomic desktops (Silverblue...), CoreOS or ELN */
bool isFedora(const QString &id);

/* The folders of the database, as libosinfo finds them: $OSINFO_SYSTEM_DIR
   or /usr/share/osinfo, $OSINFO_LOCAL_DIR or /etc/osinfo, $OSINFO_USER_DIR
   or ~/.config/osinfo, the later ones overriding the earlier ones */
QStringList databaseDirs();

class Catalogue
{
public:
    /* The systems of the database in @dirs, then those of vitrine's list
       the database lacks */
    explicit Catalogue(const QStringList &dirs);

    /* The app's, from databaseDirs(), read on first use */
    static const Catalogue &instance();
    /* Read again, e.g. from other folders in a test */
    static void reload(const QStringList &dirs = databaseDirs());

    const QList<Os> &systems() const { return m_systems; }
    /* How many come from the database: none when it is not installed */
    int fromDatabase() const { return m_fromDatabase; }
    /*
     * @id, or a system that has it as an alias.  An id the list has not,
     * of a release newer than those it has (fedora45 after fedora44), as
     * the newest one of its distribution names itself; else null.
     */
    Os find(const QString &id) const;
    /* The name of a distribution: Fedora, Microsoft Windows */
    QString distroName(const QString &distro) const;
    /* The systems, newest first, as the discs' rules are tried */
    const QList<qsizetype> &byRelease() const { return m_byRelease; }

private:
    QList<Os> m_systems;
    QHash<QString, qsizetype> m_index;
    QList<qsizetype> m_byRelease;
    int m_fromDatabase = 0;
};

/* What the primary volume descriptor of an ISO 9660 image says, its fields
   as libosinfo reads them (trailing spaces cut) */
struct Disc {
    QString volume;             // the label
    QString system;
    QString publisher;
    QString application;
    qint64 size = 0;            // the volume's, in bytes
    bool isValid() const { return !volume.isEmpty(); }
};
/* From the 2048 bytes of the descriptor, at 32 KiB into the image */
Disc parseDescriptor(const QByteArray &descriptor);
/* @path's: not valid if it is no ISO 9660 image, or has no label */
Disc readDisc(const QString &path);

struct Detection {
    QString id;                 // the system; empty if not recognized
    QString desktop;            // kde, gnome or other when the disc says; else empty
    /* Whose rule: "osinfo" the database's, "vitrine" vitrine's own */
    QString by;
};
/*
 * The system of an installation disc, by the database's rules for its
 * descriptor, else by vitrine's for its label and file name; vitrine's
 * also tell the release when the database knows only the distribution
 * (Fedora 45 rather than Fedora).  When the rules of several systems
 * match (Windows 10 and 11 share labels), the newest one, as osinfo-detect
 * reports first.
 */
Detection detect(const Catalogue &catalogue, const Disc &disc, const QString &fileName = {});
/* @path's, with the app's catalogue */
Detection detect(const QString &path);
/* The desktop a disc's label or file name tells: kde, gnome, other; empty */
QString desktopOf(const QString &label, const QString &fileName = {});

}
