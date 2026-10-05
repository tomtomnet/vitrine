// SPDX-License-Identifier: GPL-2.0-or-later
#include "guestos.h"

#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QStandardPaths>
#include <QXmlStreamReader>

#include <algorithm>
#include <memory>

namespace GuestOs {

bool Os::isGeneric() const
{
    return version == "unknown" || id.endsWith("-unknown");
}

bool Os::isEol(const QDate &today) const
{
    /* virt-manager's _OsVariant._get_eol() */
    if (eol.isValid()) {
        return today > eol;
    }
    if (status == "rolling") {
        return false;
    }
    return released.isValid() && today > released.addYears(5);
}

QString guestFamily(const Os &os)
{
    if (os.family == "linux") {
        return "linux";
    }
    if (os.family == "winnt" || os.family == "win9x" || os.family == "win16") {
        return "windows";
    }
    return os.isNull() ? QString() : "other";
}

VmTemplate::Os templateOs(const Os &os)
{
    static const QRegularExpression windowsNumber("^win(\\d+)$");

    if (os.family == "linux") {
        return VmTemplate::Os::Linux;
    }
    if (os.family == "winnt") {
        /* Windows 11 and later check for Secure Boot and a TPM; the others,
           the servers of 11's time among them, do without */
        const QRegularExpressionMatch m = windowsNumber.match(os.id);
        return m.hasMatch() && m.captured(1).toInt() >= 11 ? VmTemplate::Os::Windows11
                                                           : VmTemplate::Os::Windows;
    }
    /* Windows 9x and 3.x, DOS, the BSDs...: devices they all have drivers for */
    return VmTemplate::Os::Other;
}

bool isFedora(const QString &id)
{
    static const QRegularExpression fedora("^fedora(\\d+|-unknown|-rawhide)$");
    return fedora.match(id).hasMatch();
}

QStringList databaseDirs()
{
    const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);

    return {qEnvironmentVariable("OSINFO_SYSTEM_DIR", "/usr/share/osinfo"),
            qEnvironmentVariable("OSINFO_LOCAL_DIR", "/etc/osinfo"),
            qEnvironmentVariable("OSINFO_USER_DIR", config + "/osinfo")};
}

/* The database */

namespace {

/* The elements of an <os> of one file: a whole system, or what an
   extension file (in a NAME.d folder) adds to it */
struct Parsed {
    QString uri;
    Os os;
    bool hasName = false;
};

QString text(QXmlStreamReader &xml)
{
    return xml.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
}

/* Not in another language: the names and vendors come in all of them */
bool untranslated(const QXmlStreamReader &xml)
{
    return !xml.attributes().hasAttribute(QStringLiteral("http://www.w3.org/XML/1998/namespace"),
                                          QStringLiteral("lang")) &&
           xml.attributes().value(QStringLiteral("xml:lang")).isEmpty();
}

QRegularExpression regex(const QString &pattern)
{
    return pattern.isEmpty() ? QRegularExpression() : QRegularExpression(pattern);
}

void parseMedia(QXmlStreamReader &xml, Os &os)
{
    DiscRule rule;
    bool iso = false;

    rule.fallback = xml.attributes().value(QStringLiteral("arch")) == u"all";
    while (xml.readNextStartElement()) {
        if (xml.name() != u"iso") {
            xml.skipCurrentElement();
            continue;
        }
        iso = true;
        while (xml.readNextStartElement()) {
            const QStringView name = xml.name();
            if (name == u"volume-id") {
                rule.volume = regex(text(xml));
            } else if (name == u"system-id") {
                rule.system = regex(text(xml));
            } else if (name == u"publisher-id") {
                rule.publisher = regex(text(xml));
            } else if (name == u"application-id") {
                rule.application = regex(text(xml));
            } else if (name == u"volume-size") {
                rule.size = text(xml).toLongLong();
            } else {
                xml.skipCurrentElement();
            }
        }
    }
    if (iso) {
        os.discs << rule;
    }
}

void parseVariant(QXmlStreamReader &xml, Os &os)
{
    while (xml.readNextStartElement()) {
        if (xml.name() == u"name" && untranslated(xml)) {
            const QString name = text(xml);
            if (!name.isEmpty() && !os.editions.contains(name)) {
                os.editions << name;
            }
        } else {
            xml.skipCurrentElement();
        }
    }
}

Parsed parseOs(QXmlStreamReader &xml)
{
    Parsed p;
    Os &os = p.os;

    p.uri = xml.attributes().value(QStringLiteral("id")).toString();
    while (xml.readNextStartElement()) {
        const QStringView name = xml.name();
        if (name == u"short-id") {
            const QString id = text(xml);
            if (os.id.isEmpty()) {
                os.id = id;
            } else if (!id.isEmpty() && !os.aliases.contains(id)) {
                os.aliases << id;
            }
        } else if (name == u"name" && untranslated(xml)) {
            os.name = text(xml);
            p.hasName = true;
        } else if (name == u"version") {
            os.version = text(xml);
        } else if (name == u"family") {
            os.family = text(xml).toLower();
        } else if (name == u"distro") {
            /* Manjaro's is "Manjaro" */
            os.distro = text(xml).toLower();
        } else if (name == u"release-date") {
            os.released = QDate::fromString(text(xml), Qt::ISODate);
        } else if (name == u"eol-date") {
            os.eol = QDate::fromString(text(xml), Qt::ISODate);
        } else if (name == u"release-status") {
            os.status = text(xml);
        } else if (name == u"variant") {
            parseVariant(xml, os);
        } else if (name == u"media") {
            parseMedia(xml, os);
        } else {
            xml.skipCurrentElement();
        }
    }
    return p;
}

/* What a later <os> of the same system adds: its fields, its discs */
void merge(Os &into, const Parsed &p)
{
    const Os &o = p.os;

    if (into.id.isEmpty()) {
        into.id = o.id;
    }
    for (const QString &alias : o.aliases + QStringList{o.id}) {
        if (!alias.isEmpty() && alias != into.id && !into.aliases.contains(alias)) {
            into.aliases << alias;
        }
    }
    if (p.hasName) {
        into.name = o.name;
    }
    for (QString Os::*field : {&Os::version, &Os::family, &Os::distro, &Os::status}) {
        if (!(o.*field).isEmpty()) {
            into.*field = o.*field;
        }
    }
    if (o.released.isValid()) {
        into.released = o.released;
    }
    if (o.eol.isValid()) {
        into.eol = o.eol;
    }
    for (const QString &edition : o.editions) {
        if (!into.editions.contains(edition)) {
            into.editions << edition;
        }
    }
    into.discs += o.discs;
}

/*
 * The XML files of the systems in @dirs: those of a later folder replace
 * those of the same path in an earlier one, as libosinfo's loader has it
 */
QStringList databaseFiles(const QStringList &dirs)
{
    QMap<QString, QString> files;

    for (const QString &dir : dirs) {
        if (dir.isEmpty()) {
            continue;
        }
        const QString os = dir + "/os";
        QDirIterator it(os, {"*.xml"}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            files.insert(path.mid(os.size()), path);
        }
    }
    /* in the order of their paths: an extension (NAME.d/) after its file */
    QStringList keys = files.keys();
    std::sort(keys.begin(), keys.end(), [](const QString &a, const QString &b) {
        return QString(a).replace(".d/", "~/") < QString(b).replace(".d/", "~/");
    });
    QStringList list;
    for (const QString &key : std::as_const(keys)) {
        list << files.value(key);
    }
    return list;
}

/*
 * vitrine's list: the main systems, the distributions by themselves (their
 * releases come from the database, or from detect() and find()), and a few
 * releases the template or the guest tools go by.  Ids, names and
 * distributions as the database has them where it has them.
 */
struct Known {
    const char *id;
    const char *name;
    const char *version;
    const char *family;
    const char *distro;
    const char *status;
};
const Known kKnown[] = {
    {"fedora44", "Fedora Linux 44", "44", "linux", "fedora", ""},
    {"fedora-unknown", "Fedora", "unknown", "linux", "fedora", "prerelease"},
    {"silverblue-unknown", "Fedora Silverblue", "unknown", "linux", "fedora", "prerelease"},
    {"ubuntu-unknown", "Ubuntu", "unknown", "linux", "ubuntu", ""},
    {"debian-unknown", "Debian", "unknown", "linux", "debian", ""},
    {"archlinux", "Arch Linux", "", "linux", "archlinux", "rolling"},
    {"opensusetumbleweed", "openSUSE Tumbleweed", "tumbleweed", "linux", "opensuse", ""},
    {"opensuse-unknown", "openSUSE", "unknown", "linux", "opensuse", "prerelease"},
    {"linuxmint-unknown", "Linux Mint", "unknown", "linux", "linuxmint", ""},
    {"manjaro", "Manjaro", "", "linux", "manjaro", "rolling"},
    {"endeavouros", "EndeavourOS", "", "linux", "endeavouros", "rolling"},
    {"cachyos", "CachyOS", "", "linux", "cachyos", "rolling"},
    {"kdelinux", "KDE Linux", "", "linux", "kdelinux", "rolling"},
    {"kdeneon", "KDE neon", "", "linux", "kdeneon", "rolling"},
    {"popos-unknown", "Pop!_OS", "unknown", "linux", "popos", ""},
    {"elementary-unknown", "elementary OS", "unknown", "linux", "elementaryos", ""},
    {"zorin-unknown", "Zorin OS", "unknown", "linux", "zorin", ""},
    {"kalilinux", "Kali Linux", "", "linux", "kalilinux", "rolling"},
    {"rhel-unknown", "Red Hat Enterprise Linux", "unknown", "linux", "rhel", ""},
    {"centos-stream-unknown", "CentOS Stream", "unknown", "linux", "centos", ""},
    {"almalinux-unknown", "AlmaLinux", "unknown", "linux", "almalinux", ""},
    {"rocky-unknown", "Rocky Linux", "unknown", "linux", "rocky", ""},
    {"sle-unknown", "SUSE Linux Enterprise", "unknown", "linux", "sle", ""},
    {"alpinelinux-unknown", "Alpine Linux", "unknown", "linux", "alpinelinux", ""},
    {"nixos-unknown", "NixOS", "unknown", "linux", "nixos", ""},
    {"gentoo", "Gentoo Linux", "", "linux", "gentoo", "rolling"},
    {"voidlinux", "Void Linux", "", "linux", "voidlinux", "rolling"},
    {"win11", "Microsoft Windows 11", "11.0", "winnt", "win", ""},
    {"win10", "Microsoft Windows 10", "10.0", "winnt", "win", ""},
    {"win2k25", "Microsoft Windows Server 2025", "10.0", "winnt", "win", ""},
    {"win2k22", "Microsoft Windows Server 2022", "10.0", "winnt", "win", ""},
    {"win8.1", "Microsoft Windows 8.1", "6.3", "winnt", "win", ""},
    {"win7", "Microsoft Windows 7", "6.1", "winnt", "win", ""},
    {"freebsd-unknown", "FreeBSD", "unknown", "freebsd", "freebsd", ""},
    {"openbsd-unknown", "OpenBSD", "unknown", "openbsd", "openbsd", ""},
    {"netbsd-unknown", "NetBSD", "unknown", "netbsd", "netbsd", ""},
    {"macos-unknown", "macOS", "unknown", "darwin", "osx", ""},
    {"haiku-unknown", "Haiku", "unknown", "haiku", "haiku", ""},
    {"freedos-unknown", "FreeDOS", "unknown", "dos", "freedos", ""},
};

/* The distributions' names where their systems' names do not tell */
const char *const kDistroNames[][2] = {
    {"fedora", "Fedora"},
    {"win", "Microsoft Windows"},
    {"rhel", "Red Hat Enterprise Linux"},
    {"rhl", "Red Hat Linux"},
    {"centos", "CentOS"},
    {"sle", "SUSE Linux Enterprise"},
    {"sled", "SUSE Linux Enterprise Desktop"},
    {"sles", "SUSE Linux Enterprise Server"},
    {"elementaryos", "elementary OS"},
    {"osx", "macOS"},
    {"opensuse", "openSUSE"},
    {"ol", "Oracle Linux"},
    {"oel", "Oracle Enterprise Linux"},
    {"alpinelinux", "Alpine Linux"},
    {"eos", "Endless OS"},
};

int compareVersions(const QString &a, const QString &b)
{
    const QStringList x = a.split('.');
    const QStringList y = b.split('.');

    for (qsizetype i = 0; i < qMax(x.size(), y.size()); i++) {
        const int p = x.value(i).toInt();
        const int q = y.value(i).toInt();
        if (p != q) {
            return p < q ? -1 : 1;
        }
    }
    return 0;
}

std::unique_ptr<Catalogue> &appCatalogue()
{
    static std::unique_ptr<Catalogue> catalogue;
    return catalogue;
}

}

Catalogue::Catalogue(const QStringList &dirs)
{
    QHash<QString, qsizetype> byUri;

    for (const QString &path : databaseFiles(dirs)) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        QXmlStreamReader xml(&file);
        if (!xml.readNextStartElement() || xml.name() != u"libosinfo") {
            continue;
        }
        while (xml.readNextStartElement()) {
            if (xml.name() != u"os") {
                xml.skipCurrentElement();
                continue;
            }
            const Parsed p = parseOs(xml);
            if (p.uri.isEmpty()) {
                continue;
            }
            if (const auto it = byUri.constFind(p.uri); it != byUri.cend()) {
                merge(m_systems[*it], p);
            } else if (!p.os.id.isEmpty()) {
                byUri.insert(p.uri, m_systems.size());
                m_systems << p.os;
            }
        }
        if (xml.hasError()) {
            qWarning("osinfo database: %s: %s", qPrintable(path), qPrintable(xml.errorString()));
        }
    }
    m_fromDatabase = int(m_systems.size());

    for (qsizetype i = 0; i < m_systems.size(); i++) {
        m_index.insert(m_systems[i].id, i);
    }
    for (qsizetype i = 0; i < m_systems.size(); i++) {
        for (const QString &alias : std::as_const(m_systems[i].aliases)) {
            if (!m_index.contains(alias)) {
                m_index.insert(alias, i);
            }
        }
    }
    for (const Known &k : kKnown) {
        if (m_index.contains(k.id)) {
            continue;
        }
        Os os;
        os.id = k.id;
        os.name = k.name;
        os.version = k.version;
        os.family = k.family;
        os.distro = k.distro;
        os.status = k.status;
        os.builtIn = true;
        m_index.insert(os.id, m_systems.size());
        m_systems << os;
    }

    /* a system without a family, as some of the database's are (elementary
       5.0): its distribution's */
    for (Os &os : m_systems) {
        for (qsizetype i = 0; os.family.isEmpty() && !os.distro.isEmpty() && i < m_systems.size();
             i++) {
            if (m_systems[i].distro == os.distro) {
                os.family = m_systems[i].family;
            }
        }
    }
    for (qsizetype i = 0; i < m_systems.size(); i++) {
        m_byRelease << i;
    }
    std::stable_sort(m_byRelease.begin(), m_byRelease.end(), [this](qsizetype a, qsizetype b) {
        /* the dated ones first, the newest first */
        const QDate x = m_systems[a].released;
        const QDate y = m_systems[b].released;
        return x.isValid() && (!y.isValid() || x > y);
    });
}

const Catalogue &Catalogue::instance()
{
    std::unique_ptr<Catalogue> &catalogue = appCatalogue();

    if (!catalogue) {
        catalogue = std::make_unique<Catalogue>(databaseDirs());
    }
    return *catalogue;
}

void Catalogue::reload(const QStringList &dirs)
{
    appCatalogue() = std::make_unique<Catalogue>(dirs);
}

Os Catalogue::find(const QString &id) const
{
    /* a distribution's name, then its release: fedora45, ubuntu26.04, nixos-26.05 */
    static const QRegularExpression release("^(.*?[a-z])-?(\\d+(?:\\.\\d+)*)$");

    if (id.isEmpty()) {
        return {};
    }
    if (const auto it = m_index.constFind(id); it != m_index.cend()) {
        return m_systems[*it];
    }
    const QRegularExpressionMatch m = release.match(id);
    if (!m.hasMatch()) {
        return {};
    }
    const QString prefix = m.captured(1);
    const QString version = m.captured(2);
    const Os *newest = nullptr;
    const Os *generic = nullptr;

    for (const Os &os : m_systems) {
        static const QRegularExpression number("^\\d+(\\.\\d+)*$");
        if (os.id == prefix + "-unknown") {
            generic = &os;
        } else if (number.match(os.version).hasMatch() &&
                   (os.id == prefix + os.version || os.id == prefix + '-' + os.version) &&
                   (!newest || compareVersions(os.version, newest->version) > 0)) {
            newest = &os;
        }
    }
    Os made;
    if (newest) {
        made = *newest;
        made.name = newest->name.contains(newest->version)
                        ? QString(newest->name).replace(newest->version, version)
                        : newest->name + ' ' + version;
    } else if (generic) {
        /* "Ubuntu" 24.04; "Red Hat Enterprise Linux Unknown", the database's, without it */
        made = *generic;
        made.name = QString(generic->name).remove(QRegularExpression(" [Uu]nknown$")) + ' ' +
                    version;
    } else {
        return {};
    }
    made.id = id;
    made.version = version;
    made.aliases.clear();
    made.released = {};
    made.eol = {};
    made.status.clear();
    made.editions.clear();
    made.discs.clear();
    return made;
}

QString Catalogue::distroName(const QString &distro) const
{
    QStringList common;
    bool first = true;

    for (const auto &[key, name] : kDistroNames) {
        if (distro == QLatin1String(key)) {
            return QString::fromUtf8(name);
        }
    }
    /* the words its systems' names start with: "Ubuntu" of "Ubuntu 24.04 LTS"... */
    for (const Os &os : m_systems) {
        if (os.distro != distro || os.name.isEmpty()) {
            continue;
        }
        const QStringList words = os.name.split(' ', Qt::SkipEmptyParts);
        if (first) {
            common = words;
            first = false;
            continue;
        }
        qsizetype n = 0;
        while (n < common.size() && n < words.size() && common[n] == words[n]) {
            n++;
        }
        common.resize(n);
    }
    /* not a release: "Fedora Linux 44" alone is "Fedora Linux" */
    while (!common.isEmpty() && common.last().front().isDigit()) {
        common.removeLast();
    }
    return common.isEmpty() ? distro : common.join(' ');
}

/* Discs */

Disc parseDescriptor(const QByteArray &d)
{
    /* ECMA-119 8.4; libosinfo's PrimaryVolumeDescriptor */
    auto field = [&d](int offset, int length) {
        QByteArray bytes = d.mid(offset, length);
        const qsizetype nul = bytes.indexOf('\0');
        if (nul >= 0) {
            bytes.truncate(nul);
        }
        QString s = QString::fromLatin1(bytes);
        while (!s.isEmpty() && s.back().isSpace()) {
            s.chop(1);
        }
        return s;
    };
    auto le = [&d](int offset, int bytes) {
        quint64 v = 0;
        for (int i = bytes - 1; i >= 0; i--) {
            v = (v << 8) | quint8(d[offset + i]);
        }
        return v;
    };
    Disc disc;

    if (d.size() < 2048 || d[0] != 1 || d.mid(1, 5) != "CD001") {
        return disc;
    }
    disc.system = field(8, 32);
    disc.volume = field(40, 32);
    disc.publisher = field(318, 128);
    disc.application = field(574, 128);
    disc.size = qint64(le(80, 4) * le(128, 2));
    return disc;
}

Disc readDisc(const QString &path)
{
    QFile file(path);

    if (!file.open(QIODevice::ReadOnly) || !file.seek(0x8000)) {
        return {};
    }
    return parseDescriptor(file.read(2048));
}

namespace {

bool matches(const QRegularExpression &rule, const QString &value)
{
    return rule.pattern().isEmpty() || rule.match(value).hasMatch();
}

bool matches(const DiscRule &r, const Disc &disc)
{
    /* osinfo_media_matches(): a rule with nothing to compare matches nothing */
    if (r.volume.pattern().isEmpty() && r.system.pattern().isEmpty() &&
        r.publisher.pattern().isEmpty() && r.application.pattern().isEmpty() && r.size <= 0) {
        return false;
    }
    return matches(r.volume, disc.volume) && matches(r.application, disc.application) &&
           matches(r.system, disc.system) && matches(r.publisher, disc.publisher) &&
           (r.size <= 0 || r.size == disc.size);
}

/* The database's: rules for every architecture and those of rolling
   systems only when no other matches, as osinfo_db_identify_media() */
QString matchDatabase(const Catalogue &catalogue, const Disc &disc)
{
    for (const bool fallback : {false, true}) {
        for (const qsizetype i : catalogue.byRelease()) {
            const Os &os = catalogue.systems()[i];
            if (os.builtIn) {
                continue;
            }
            for (const DiscRule &r : os.discs) {
                if ((r.fallback || os.status == "rolling") == fallback && matches(r, disc)) {
                    return os.id;
                }
            }
        }
    }
    return {};
}

/*
 * vitrine's rules: labels, and file names as the distributions name their
 * images, for discs the database does not know.  %1 (and %2) are the
 * release, from the first match of @release in the text, the
 * architectures cut out first (x86_64 is no release).
 */
struct LabelRule {
    const char *pattern;
    const char *id;
    const char *release;        // a regular expression with one or two groups
};
const LabelRule kLabelRules[] = {
    {"^fedora[-_ ](?:sb|silverblue)[-_ ]", "silverblue%1", "[-_ ](\\d{2,3})(?=[-_. ]|$)"},
    {"^fedora[-_ ]", "fedora%1", "[-_ ](\\d{2,3})(?=[-_. ]|$)"},
    {"^(?:k|x|l)?ubuntu(?:[-_ ]?(?:server|mate|budgie|studio|unity|kylin))?[-_ ]", "ubuntu%1",
     "[-_ ](\\d{2}\\.\\d{2})"},
    {"^(?:debian|d-live)[-_ ]", "debian%1", "[-_ ](\\d{1,2})(?:\\.\\d+)*(?=[-_ ]|$)"},
    {"^(?:arch_\\d{6}|archlinux-\\d{4})", "archlinux", nullptr},
    {"^opensuse[-_ ]tumbleweed", "opensusetumbleweed", nullptr},
    {"^opensuse[-_ ]leap", "opensuse%1", "[-_ ](\\d{2}\\.\\d)"},
    {"^(?:linux[-_ ]?mint|lmde)", "linuxmint%1", "[-_ ](\\d{2}(?:\\.\\d)?)(?=[-_ ]|$)"},
    {"^manjaro[-_ ]", "manjaro", nullptr},
    {"^(?:eos[-_]|endeavouros)", "endeavouros", nullptr},
    {"^cachyos", "cachyos", nullptr},
    {"^kde[-_ ]linux", "kdelinux", nullptr},
    {"^neon[-_ ]", "kdeneon", nullptr},
    {"^pop[-_ ]?os[-_ ]", "popos%1", "[-_ ](\\d{2}\\.\\d{2})"},
    {"^elementary(?:[-_ ]?os)?[-_ ]", "elementary%1", "[-_ ](\\d+(?:\\.\\d+)?)(?=[-_ ]|$)"},
    {"^zorin[-_ ]?os[-_ ]", "zorin%1", "[-_ ](\\d{2})(?=[-_. ]|$)"},
    {"^kali", "kalilinux", nullptr},
    {"^rhel[-_ ]", "rhel%1.%2", "[-_ ](\\d{1,2})[-_.](\\d{1,2})"},
    {"^centos[-_ ]stream[-_ ]", "centos-stream%1", "[-_ ](\\d{1,2})(?=[-_ ]|$)"},
    {"^almalinux[-_ ]", "almalinux%1", "[-_ ](\\d{1,2})(?=[-_. ]|$)"},
    {"^rocky(?:linux)?[-_ ]", "rocky%1", "[-_ ](\\d{1,2})(?=[-_. ]|$)"},
    {"^alpine[-_ ]", "alpinelinux%1", "[-_ ](\\d\\.\\d{1,2})"},
    {"^nixos[-_ ]", "nixos-%1", "[-_ ](\\d{2}\\.\\d{2})"},
    {"^(?:gentoo|install-amd64-minimal)", "gentoo", nullptr},
    {"^void[-_ ]", "voidlinux", nullptr},
    /* Windows: CCCOMA_X64FRE_EN-US_DV9, the label of 10 and 11 alike */
    {"^(?:j_)?[a-z0-9]+_(?:x64|a64)frev?e?_", "win11", nullptr},
    {"^(?:j_)?[a-z0-9]+_x86frev?e?_", "win10", nullptr},
    {"^win(?:dows)?[-_ ]?\\d", "win%1", "^win(?:dows)?[-_ ]?(\\d+(?:\\.\\d)?)"},
    {"^\\d+_\\d+_release_", "freebsd%1.%2", "^(\\d+)_(\\d+)"},
    {"^freebsd[-_ ]", "freebsd%1", "[-_ ](\\d+\\.\\d+)"},
    {"^openbsd", "openbsd-unknown", nullptr},
    {"^netbsd", "netbsd-unknown", nullptr},
};

/* The architectures in names: not releases */
QString withoutArchitectures(const QString &text)
{
    static const QRegularExpression arch(
        "x86[-_]64|amd64|aarch64|arm64|i[3-6]86|ppc64le|s390x|(?:32|64)[-_ ]?bits?",
        QRegularExpression::CaseInsensitiveOption);
    return QString(text).remove(arch);
}

Detection matchLabel(const QString &text)
{
    Detection d;

    if (text.isEmpty()) {
        return d;
    }
    for (const LabelRule &r : kLabelRules) {
        const QRegularExpression pattern(QLatin1String(r.pattern),
                                         QRegularExpression::CaseInsensitiveOption);
        if (!pattern.match(text).hasMatch()) {
            continue;
        }
        QString id = QLatin1String(r.id);
        if (r.release) {
            const QRegularExpression release(QLatin1String(r.release),
                                             QRegularExpression::CaseInsensitiveOption);
            const QRegularExpressionMatch m = release.match(withoutArchitectures(text));
            if (!m.hasMatch()) {
                /* the distribution, its release not told */
                id = id.section('%', 0, 0);
                id = (id.endsWith('-') ? id : id + '-') + "unknown";
            } else {
                id = id.contains("%2") ? id.arg(m.captured(1), m.captured(2))
                                       : id.arg(m.captured(1));
            }
        }
        d.id = id;
        d.by = "vitrine";
        return d;
    }
    return d;
}

}

QString desktopOf(const QString &label, const QString &fileName)
{
    static const struct {
        const char *pattern;
        const char *desktop;
    } rules[] = {
        {"kde|plasma|kubuntu|kinoite|^neon|^d-live \\S+ kd\\b", "kde"},
        {"gnome|[-_ ](?:ws|sb)[-_ ]|workstation|silverblue|^ubuntu[-_ ]\\d|^d-live \\S+ gn\\b|"
         "^pop[-_ ]?os",
         "gnome"},
        {"xfce|cinnamon|mate\\b|lxqt|lxde|budgie|cosmic|sway|xubuntu|lubuntu|^d-live \\S+ "
         "(?:xf|ci|ma|lx|lq)\\b",
         "other"},
    };
    for (const QString &text : {label, QFileInfo(fileName).fileName()}) {
        if (text.isEmpty()) {
            continue;
        }
        for (const auto &r : rules) {
            const QRegularExpression re(QLatin1String(r.pattern),
                                        QRegularExpression::CaseInsensitiveOption);
            if (re.match(text).hasMatch()) {
                return QLatin1String(r.desktop);
            }
        }
    }
    return {};
}

Detection detect(const Catalogue &catalogue, const Disc &disc, const QString &fileName)
{
    Detection d;
    Detection own = matchLabel(disc.volume);

    if (own.id.isEmpty()) {
        own = matchLabel(QFileInfo(fileName).fileName());
    }
    if (disc.isValid()) {
        d.id = matchDatabase(catalogue, disc);
        d.by = d.id.isEmpty() ? QString() : "osinfo";
    }
    const Os found = catalogue.find(d.id);
    const Os mine = catalogue.find(own.id);
    if (d.id.isEmpty() ||
        (found.isGeneric() && !mine.isNull() && !mine.isGeneric() && mine.distro == found.distro)) {
        /* not in the database, or as Fedora rather than Fedora 45 */
        d.id = mine.isNull() ? QString() : own.id;
        d.by = d.id.isEmpty() ? QString() : own.by;
    }
    d.desktop = desktopOf(disc.volume, fileName);
    return d;
}

Detection detect(const QString &path)
{
    return detect(Catalogue::instance(), readDisc(path), path);
}

}
