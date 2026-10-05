// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include "core/guestos.h"

using namespace GuestOs;

/* A primary volume descriptor (ECMA-119 8.4) of @blocks blocks of 2 KiB */
static QByteArray descriptor(const QString &volume, const QString &system = {},
                             const QString &publisher = {}, const QString &application = {},
                             quint32 blocks = 20)
{
    QByteArray d(2048, '\0');
    auto put = [&d](int offset, int length, const QString &text) {
        const QByteArray bytes = text.toLatin1().leftJustified(length, ' ', true);
        d.replace(offset, length, bytes);
    };
    auto both32 = [&d](int offset, quint32 value) {
        for (int i = 0; i < 4; i++) {
            d[offset + i] = char(value >> (8 * i));
            d[offset + 7 - i] = char(value >> (8 * i));
        }
    };

    d[0] = 1;
    d.replace(1, 5, "CD001");
    d[6] = 1;
    put(8, 32, system);
    put(40, 32, volume);
    both32(80, blocks);
    d[128] = char(0x00);
    d[129] = char(0x08);
    d[130] = char(0x08);
    d[131] = char(0x00);
    put(190, 128, {});
    put(318, 128, publisher);
    put(446, 128, {});
    put(574, 128, application);
    return d;
}

/*
 * An ISO 9660 image of 20 blocks, as small as its descriptors make it, and
 * bootable: osinfo-detect looks for a PowerPC boot file in an image with
 * no El Torito record, and never ends on a root directory this empty
 */
static bool writeIso(const QString &path, const QByteArray &pvd)
{
    QFile f(path);
    QByteArray boot(2048, '\0');
    QByteArray terminator(2048, '\0');

    boot.replace(1, 5, "CD001");
    boot[6] = 1;
    boot.replace(7, 23, "EL TORITO SPECIFICATION");
    terminator[0] = char(0xff);
    terminator.replace(1, 5, "CD001");
    terminator[6] = 1;
    return f.open(QIODevice::WriteOnly) && f.write(QByteArray(0x8000, '\0')) == 0x8000 &&
           f.write(pvd) == 2048 && f.write(boot) == 2048 && f.write(terminator) == 2048 &&
           f.write(QByteArray(2048, '\0')) == 2048;
}

static bool write(const QString &path, const QString &text)
{
    QFile f(path);

    return QDir().mkpath(QFileInfo(path).path()) && f.open(QIODevice::WriteOnly) &&
           f.write(text.toUtf8()) > 0;
}

/* An osinfo-db file of one system */
static QString osXml(const QString &uri, const QString &body)
{
    return QString("<?xml version=\"1.0\"?>\n<libosinfo version=\"0.0.1\">\n"
                   "  <os id=\"%1\">\n%2\n  </os>\n</libosinfo>\n")
        .arg(uri, body);
}

static const char kFedora43[] = R"x(
    <short-id>fedora43</short-id>
    <name>Fedora Linux 43</name>
    <name xml:lang="fr">Fedora Linux 43 (en français)</name>
    <version>43</version>
    <vendor>Fedora Project</vendor>
    <vendor xml:lang="cs">Projekt Fedora</vendor>
    <family>linux</family>
    <distro>fedora</distro>
    <upgrades id="fedoraproject.org:fedora:42"/>
    <release-date>2025-10-28</release-date>
    <eol-date>2026-12-09</eol-date>
    <variant id="workstation">
      <name>Fedora Workstation 43</name>
      <name xml:lang="fi">Fedora 43 -työasema</name>
    </variant>
    <variant id="server"><name>Fedora Server 43</name></variant>
    <media arch="x86_64" live="true" installer-script="false">
      <variant id="workstation"/>
      <url>Fedora-Workstation-Live-43-1.6.x86_64.iso</url>
      <iso>
        <volume-id>Fedora-WS-Live-43.*</volume-id>
        <publisher-id>FEDORA PROJECT</publisher-id>
        <application-id>FEDORA-WORKSTATION-LIVE-43</application-id>
      </iso>
      <kernel>images/pxeboot/vmlinuz</kernel>
    </media>
    <media arch="x86_64">
      <variant id="server"/>
      <iso><volume-id>Fedora-S-dvd-x86_64-43</volume-id></iso>
    </media>
    <resources arch="all"><minimum><ram>2147483648</ram></minimum></resources>)x";

static const char kFedoraUnknown[] = R"x(
    <short-id>fedora-unknown</short-id>
    <name>Fedora</name>
    <version>unknown</version>
    <family>linux</family>
    <distro>fedora</distro>
    <release-status>prerelease</release-status>
    <media arch="x86_64" live="true">
      <iso><volume-id>Fedora-.*-Live-(4[4-9]|[5-9][0-9]).*</volume-id></iso>
    </media>)x";

/* Windows 10 and 11 share the label of their discs.  (No URL in these raw
   strings: moc 6.11 takes the "// of one for a comment, and misses the class.) */
static const char kWindowsMedia[] = R"x(
    <media arch="x86_64" installer-reboots="2">
      <iso>
        <volume-id>^(J_)?(CCSN?A|C?CCOMA)_X64FREE?_</volume-id>
        <publisher-id>MICROSOFT CORPORATION</publisher-id>
        <l10n-language regex="true" l10n-language-map="microsoft.com:win:8:l10n">[A-Z0-9_]*_([A-Z]*-[A-Z]*)</l10n-language>
      </iso>
    </media>)x";

class TestGuestOs : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;

    /* A database in three folders, as the system's, /etc's and the user's */
    QStringList makeDatabase()
    {
        const QString system = m_tmp.filePath("db/system");
        const QString local = m_tmp.filePath("db/local");
        const QString user = m_tmp.filePath("db/user");
        const QString fedora = "/os/fedoraproject.org/";
        const QString microsoft = "/os/microsoft.com/";
        bool ok = true;

        ok &= write(system + fedora + "fedora-43.xml",
                    osXml("http://fedoraproject.org/fedora/43", kFedora43));
        ok &= write(system + fedora + "fedora-unknown.xml",
                    osXml("http://fedoraproject.org/fedora/unknown", kFedoraUnknown));
        ok &= write(system + microsoft + "win-10.xml",
                    osXml("http://microsoft.com/win/10",
                          "<short-id>win10</short-id><name>Microsoft Windows 10</name>"
                          "<version>10.0</version><family>winnt</family><distro>win</distro>"
                          "<release-date>2018-11-13</release-date>" +
                              QString(kWindowsMedia)));
        /* an extension: drivers for it, which repeat its ids */
        ok &= write(system + microsoft + "win-10.d/drivers.xml",
                    osXml("http://microsoft.com/win/10",
                          "<short-id>win10</short-id><name>Microsoft Windows 10</name>"
                          "<driver arch=\"x86_64\" location=\"https://example\"/>"));
        ok &= write(system + microsoft + "win-11.xml",
                    osXml("http://microsoft.com/win/11",
                          "<short-id>win11</short-id><name>Microsoft Windows 11</name>"
                          "<version>11.0</version><family>winnt</family><distro>win</distro>"
                          "<release-date>2021-10-05</release-date>" +
                              QString(kWindowsMedia)));
        ok &= write(system + "/os/debian.org/debian-12.xml",
                    osXml("http://debian.org/debian/12",
                          "<short-id>debian12</short-id><short-id>debianbookworm</short-id>"
                          "<name>Debian 12</name><version>12</version><family>linux</family>"
                          "<distro>debian</distro><release-date>2023-06-10</release-date>"
                          "<media arch=\"x86_64\"><iso><volume-id>Debian 12</volume-id>"
                          "<system-id>LINUX</system-id><volume-size>40960</volume-size>"
                          "</iso></media>"));
        /* rolling: its discs only when no other system's match */
        ok &= write(system + "/os/manjaro.org/manjaro-rolling.xml",
                    osXml("http://manjaro.org/manjaro/rolling",
                          "<short-id>manjaro</short-id><name>Manjaro</name>"
                          "<family>linux</family><distro>Manjaro</distro>"
                          "<release-status>rolling</release-status>"
                          "<media arch=\"x86_64\"><iso><volume-id>^MANJARO</volume-id></iso>"
                          "</media>"));
        ok &= write(system + "/os/example.org/notmanjaro.xml",
                    osXml("http://example.org/notmanjaro/1",
                          "<short-id>notmanjaro1</short-id><name>Not Manjaro 1</name>"
                          "<version>1</version><family>linux</family><distro>notmanjaro</distro>"
                          "<media arch=\"x86_64\"><iso><volume-id>^MANJARO_NOT</volume-id></iso>"
                          "</media>"));
        /* /etc's newer file replaces the system's */
        ok &= write(local + fedora + "fedora-43.xml",
                    osXml("http://fedoraproject.org/fedora/43",
                          QString(kFedora43).replace("2026-12-09", "2026-12-24")));
        /* the user's has a newer release */
        ok &= write(user + fedora + "fedora-44.xml",
                    osXml("http://fedoraproject.org/fedora/44",
                          "<short-id>fedora44</short-id><name>Fedora Linux 44</name>"
                          "<version>44</version><family>linux</family><distro>fedora</distro>"
                          "<release-date>2026-04-28</release-date>"));
        /* a file that is not XML is passed over */
        ok &= write(system + "/os/broken.org/broken.xml", "<libosinfo><os id=\"x\"><short-id>");
        return ok ? QStringList{system, local, user} : QStringList();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
    }

    /* Without the database: vitrine's list */
    void builtInList()
    {
        const Catalogue c(QStringList{m_tmp.filePath("none")});
        const Os fedora = c.find("fedora44");

        QCOMPARE(c.fromDatabase(), 0);
        QVERIFY(c.systems().size() > 30);
        QCOMPARE(fedora.name, "Fedora Linux 44");
        QCOMPARE(fedora.family, "linux");
        QCOMPARE(fedora.distro, "fedora");
        QVERIFY(fedora.builtIn);
        QVERIFY(!fedora.isGeneric());
        QVERIFY(c.find("fedora-unknown").isGeneric());
        QCOMPARE(c.find("archlinux").status, "rolling");
        QCOMPARE(c.find("win11").family, "winnt");
        QCOMPARE(c.find("kdelinux").name, "KDE Linux");
        QVERIFY(c.find("nothing").isNull());
        QVERIFY(c.find("").isNull());

        /* releases the list has not: as their distribution's newest, or its own name */
        QCOMPARE(c.find("fedora45").name, "Fedora Linux 45");
        QCOMPARE(c.find("fedora45").version, "45");
        QCOMPARE(c.find("fedora45").distro, "fedora");
        QCOMPARE(c.find("ubuntu24.04").name, "Ubuntu 24.04");
        QCOMPARE(c.find("ubuntu24.04").family, "linux");
        QCOMPARE(c.find("linuxmint22.2").name, "Linux Mint 22.2");
        QCOMPARE(c.find("nixos-25.05").name, "NixOS 25.05");
        QCOMPARE(c.find("rocky9").name, "Rocky Linux 9");
        QVERIFY(c.find("nothing12").isNull());

        QCOMPARE(c.distroName("fedora"), "Fedora");
        QCOMPARE(c.distroName("win"), "Microsoft Windows");
        QCOMPARE(c.distroName("ubuntu"), "Ubuntu");
        QCOMPARE(c.distroName("linuxmint"), "Linux Mint");
    }

    /* The database's folders, merged as libosinfo merges them */
    void database()
    {
        const QStringList dirs = makeDatabase();
        QVERIFY(!dirs.isEmpty());
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("broken.xml: Premature end"));
        const Catalogue c(dirs);
        const Os fedora = c.find("fedora43");

        QCOMPARE(c.fromDatabase(), 8);
        QCOMPARE(fedora.name, "Fedora Linux 43");
        QCOMPARE(fedora.version, "43");
        QCOMPARE(fedora.family, "linux");
        QCOMPARE(fedora.distro, "fedora");
        QCOMPARE(fedora.released, QDate(2025, 10, 28));
        /* from /etc's file */
        QCOMPARE(fedora.eol, QDate(2026, 12, 24));
        QCOMPARE(fedora.editions, QStringList({"Fedora Workstation 43", "Fedora Server 43"}));
        QCOMPARE(fedora.discs.size(), 2);
        QVERIFY(!fedora.builtIn);
        /* the user's, rather than vitrine's */
        QCOMPARE(c.find("fedora44").released, QDate(2026, 4, 28));
        QVERIFY(!c.find("fedora44").builtIn);
        /* the extension keeps what the file has */
        QCOMPARE(c.find("win10").name, "Microsoft Windows 10");
        QCOMPARE(c.find("win10").discs.size(), 1);
        QCOMPARE(c.find("win10").version, "10.0");
        /* another short id */
        QCOMPARE(c.find("debianbookworm").id, "debian12");
        QCOMPARE(c.find("manjaro").distro, "manjaro");
        /* vitrine's, for those it lacks, once */
        QVERIFY(c.find("kdelinux").builtIn);
        int fedora44 = 0;
        for (const Os &os : c.systems()) {
            fedora44 += os.id == "fedora44";
        }
        QCOMPARE(fedora44, 1);
        /* newest first */
        QCOMPARE(c.systems()[c.byRelease().first()].id, "fedora44");
    }

    void families()
    {
        const Catalogue c(QStringList{m_tmp.filePath("none")});

        QCOMPARE(guestFamily(c.find("fedora44")), "linux");
        QCOMPARE(guestFamily(c.find("win11")), "windows");
        QCOMPARE(guestFamily(c.find("freebsd-unknown")), "other");
        QCOMPARE(guestFamily(Os{}), "");
        QCOMPARE(templateOs(c.find("fedora44")), VmTemplate::Os::Linux);
        QCOMPARE(templateOs(c.find("win11")), VmTemplate::Os::Windows11);
        QCOMPARE(templateOs(c.find("win10")), VmTemplate::Os::Windows);
        QCOMPARE(templateOs(c.find("win2k22")), VmTemplate::Os::Windows);
        QCOMPARE(templateOs(c.find("macos-unknown")), VmTemplate::Os::Other);

        QVERIFY(isFedora("fedora44"));
        QVERIFY(isFedora("fedora-unknown"));
        QVERIFY(isFedora("fedora-rawhide"));
        QVERIFY(!isFedora("silverblue43"));
        QVERIFY(!isFedora("fedora-coreos-stable"));
        QVERIFY(!isFedora("fedora-eln"));
        QVERIFY(!isFedora("win11"));
        QVERIFY(!isFedora(""));
    }

    /* As virt-manager hides them */
    void endOfLife()
    {
        const QDate today(2026, 10, 5);
        Os os;

        QVERIFY(!os.isEol(today));
        os.eol = QDate(2026, 5, 13);
        QVERIFY(os.isEol(today));
        os.eol = QDate(2026, 12, 9);
        QVERIFY(!os.isEol(today));
        os.eol = {};
        os.released = QDate(2020, 1, 1);
        QVERIFY(os.isEol(today));
        os.status = "rolling";
        QVERIFY(!os.isEol(today));
        os.status.clear();
        os.released = QDate(2023, 1, 1);
        QVERIFY(!os.isEol(today));
    }

    void discDescriptor()
    {
        Disc d = parseDescriptor(descriptor("Fedora-WS-Live-43-1-6", "LINUX", "FEDORA PROJECT",
                                            "FEDORA-WORKSTATION-LIVE-43", 1000));

        QCOMPARE(d.volume, "Fedora-WS-Live-43-1-6");
        QCOMPARE(d.system, "LINUX");
        QCOMPARE(d.publisher, "FEDORA PROJECT");
        QCOMPARE(d.application, "FEDORA-WORKSTATION-LIVE-43");
        QCOMPARE(d.size, 1000 * 2048);
        QVERIFY(d.isValid());

        /* not one: another descriptor, too short, no label */
        QByteArray other = descriptor("X");
        other[0] = 2;
        QVERIFY(!parseDescriptor(other).isValid());
        QVERIFY(!parseDescriptor(descriptor("X").left(100)).isValid());
        QVERIFY(!parseDescriptor(descriptor("   ")).isValid());

        const QString iso = m_tmp.filePath("disc.iso");
        QVERIFY(writeIso(iso, descriptor("ARCH_202510", {}, "ARCH LINUX")));
        d = readDisc(iso);
        QCOMPARE(d.volume, "ARCH_202510");
        QCOMPARE(d.publisher, "ARCH LINUX");
        QVERIFY(!readDisc(m_tmp.filePath("missing.iso")).isValid());
        QFile small(m_tmp.filePath("small.img"));
        QVERIFY(small.open(QIODevice::WriteOnly) && small.write("not a disc") > 0);
        small.close();
        QVERIFY(!readDisc(small.fileName()).isValid());
    }

    /* By the database's rules */
    void detectWithDatabase()
    {
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("broken.xml: Premature end"));
        const Catalogue c(makeDatabase());
        auto detectLabel = [&c](const QString &volume, const QString &publisher = {},
                                const QString &application = {}, quint32 blocks = 20) {
            return detect(c, parseDescriptor(descriptor(volume, {}, publisher, application,
                                                        blocks)));
        };

        Detection d = detectLabel("Fedora-WS-Live-43-1-6", "FEDORA PROJECT",
                                  "FEDORA-WORKSTATION-LIVE-43");
        QCOMPARE(d.id, "fedora43");
        QCOMPARE(d.by, "osinfo");
        QCOMPARE(d.desktop, "gnome");
        /* the application id is part of the rule */
        QCOMPARE(detectLabel("Fedora-WS-Live-43-1-6", "FEDORA PROJECT").by, "vitrine");
        QCOMPARE(detectLabel("Fedora-S-dvd-x86_64-43").id, "fedora43");
        /* the database knows Fedora, vitrine the release */
        d = detectLabel("Fedora-KDE-Live-45-1-1");
        QCOMPARE(d.id, "fedora45");
        QCOMPARE(d.by, "vitrine");
        QCOMPARE(d.desktop, "kde");
        QCOMPARE(c.find(d.id).name, "Fedora Linux 45");
        /* both Windows match: the newer */
        d = detectLabel("CCCOMA_X64FRE_EN-US_DV9", "MICROSOFT CORPORATION");
        QCOMPARE(d.id, "win11");
        QCOMPARE(d.by, "osinfo");
        /* the size of the volume, when the rule has one */
        QCOMPARE(detectLabel("Debian 12.7.0 amd64 n").id, "debian12");
        QCOMPARE(detectLabel("Debian 12.7.0 amd64 n").by, "vitrine");
        Disc debian = parseDescriptor(descriptor("Debian 12.7.0 amd64 n", "LINUX", {}, {}, 20));
        QCOMPARE(detect(c, debian).by, "osinfo");
        /* a rolling system's rules last */
        QCOMPARE(detectLabel("MANJARO_NOT_KDE").id, "notmanjaro1");
        QCOMPARE(detectLabel("MANJARO_KDE_2510").id, "manjaro");
        QCOMPARE(detectLabel("MANJARO_KDE_2510").by, "osinfo");
        /* neither knows it */
        d = detect(c, parseDescriptor(descriptor("ISOIMAGE")), "/somewhere/shell.iso");
        QCOMPARE(d.id, "");
        QCOMPARE(d.by, "");
        QCOMPARE(d.desktop, "");
    }

    /* By vitrine's rules, labels and file names of the main systems */
    void detectWithoutDatabase_data()
    {
        QTest::addColumn<QString>("label");
        QTest::addColumn<QString>("file");
        QTest::addColumn<QString>("id");
        QTest::addColumn<QString>("desktop");

        QTest::newRow("fedora kde") << "Fedora-KDE-Live-44-1-1" << "" << "fedora44" << "kde";
        QTest::newRow("fedora ws") << "Fedora-WS-Live-44-1-1" << "" << "fedora44" << "gnome";
        QTest::newRow("fedora server") << "Fedora-S-dvd-x86_64-44" << "" << "fedora44" << "";
        QTest::newRow("fedora file") << "ISOIMAGE" << "/isos/Fedora-KDE-Desktop-Live-44-1.6.x86_64.iso"
                                     << "fedora44" << "kde";
        QTest::newRow("fedora file arch first") << "" << "Fedora-Workstation-Live-x86_64-44-1.6.iso"
                                                << "fedora44" << "gnome";
        QTest::newRow("silverblue") << "Fedora-SB-ostree-x86_64-43" << "" << "silverblue43"
                                    << "gnome";
        QTest::newRow("fedora no release") << "Fedora-Live" << "" << "fedora-unknown" << "";
        QTest::newRow("ubuntu") << "Ubuntu 24.04.3 LTS amd64" << "" << "ubuntu24.04" << "gnome";
        QTest::newRow("kubuntu") << "Kubuntu 24.04.3 LTS amd64" << "" << "ubuntu24.04" << "kde";
        QTest::newRow("ubuntu server") << "Ubuntu-Server 24.04.3 LTS amd64" << "" << "ubuntu24.04"
                                       << "";
        QTest::newRow("ubuntu file") << "" << "ubuntu-25.10-desktop-amd64.iso" << "ubuntu25.10"
                                     << "gnome";
        QTest::newRow("debian") << "Debian 13.1.0 amd64 n" << "" << "debian13" << "";
        QTest::newRow("debian live kde") << "d-live 13.1.0 kd amd64" << "" << "debian13" << "kde";
        QTest::newRow("arch") << "ARCH_202510" << "" << "archlinux" << "";
        QTest::newRow("arch file") << "" << "archlinux-2025.10.01-x86_64.iso" << "archlinux" << "";
        QTest::newRow("tumbleweed") << "openSUSE-Tumbleweed-DVD-x86_64" << "" << "opensusetumbleweed"
                                    << "";
        QTest::newRow("leap") << "openSUSE-Leap-15.6-DVD-x86_64" << "" << "opensuse15.6" << "";
        QTest::newRow("mint") << "Linux Mint 22.2 Cinnamon 64-bit" << "" << "linuxmint22.2"
                              << "other";
        QTest::newRow("mint file") << "" << "linuxmint-22.2-xfce-64bit.iso" << "linuxmint22.2"
                                   << "other";
        QTest::newRow("manjaro") << "MANJARO_KDE_2510" << "" << "manjaro" << "kde";
        QTest::newRow("kde linux") << "KDE LINUX 202610050254" << "" << "kdelinux" << "kde";
        QTest::newRow("kde linux file") << "" << "kde-linux_202610050254.iso" << "kdelinux" << "kde";
        QTest::newRow("rocky") << "Rocky-9-6-x86_64-dvd" << "" << "rocky9" << "";
        QTest::newRow("alma") << "AlmaLinux-9-6-x86_64-dvd" << "" << "almalinux9" << "";
        QTest::newRow("rhel") << "RHEL-9-6-0-BaseOS-x86_64" << "" << "rhel9.6" << "";
        QTest::newRow("centos stream") << "CentOS-Stream-9-BaseOS-x86_64" << "" << "centos-stream9"
                                       << "";
        QTest::newRow("alpine") << "alpine-std 3.22.1 x86_64" << "" << "alpinelinux3.22" << "";
        QTest::newRow("nixos") << "nixos-minimal-25.05-x86_64" << "" << "nixos-25.05" << "";
        QTest::newRow("windows") << "CCCOMA_X64FRE_EN-US_DV9" << "" << "win11" << "";
        QTest::newRow("windows 32-bit") << "CCSA_X86FRE_EN-US_DV5" << "" << "win10" << "";
        QTest::newRow("windows file") << "" << "Win11_25H2_English_x64.iso" << "win11" << "";
        QTest::newRow("windows 8.1 file") << "" << "Win8.1_English_x64.iso" << "win8.1" << "";
        QTest::newRow("freebsd") << "14_3_RELEASE_AMD64_CD" << "" << "freebsd14.3" << "";
        QTest::newRow("unknown") << "ISOIMAGE" << "shell.iso" << "" << "";
        QTest::newRow("nothing") << "" << "" << "" << "";
    }

    void detectWithoutDatabase()
    {
        QFETCH(QString, label);
        QFETCH(QString, file);
        QFETCH(QString, id);
        QFETCH(QString, desktop);
        const Catalogue c(QStringList{m_tmp.filePath("none")});
        const Disc disc = label.isEmpty() ? Disc{} : parseDescriptor(descriptor(label));
        const Detection d = detect(c, disc, file);

        QCOMPARE(d.id, id);
        QCOMPARE(d.desktop, desktop);
        QCOMPARE(d.by, id.isEmpty() ? QString() : QString("vitrine"));
        /* a system the list can name */
        if (!id.isEmpty()) {
            QVERIFY2(!c.find(id).isNull(), qPrintable(id));
            QVERIFY(!c.find(id).name.isEmpty());
        }
    }

    /*
     * This computer's database, if installed: the discs osinfo-detect tells
     * are told alike (its name of the system, or of the edition matched)
     */
    void systemDatabase()
    {
        const QString system = "/usr/share/osinfo";
        if (!QDir(system + "/os").exists()) {
            QSKIP("osinfo-db is not installed");
        }
        QElapsedTimer clock;
        clock.start();
        const Catalogue c(QStringList{system});
        qInfo("read %d systems of the database in %lld ms", c.fromDatabase(), clock.elapsed());
        QVERIFY(c.fromDatabase() > 500);
        QCOMPARE(c.find("fedora43").name, "Fedora Linux 43");
        QCOMPARE(c.find("win11").family, "winnt");
        QCOMPARE(c.find("manjaro").distro, "manjaro");
        QCOMPARE(c.distroName("ubuntu"), "Ubuntu");

        const QString tool = QStandardPaths::findExecutable("osinfo-detect");
        const struct {
            const char *volume, *publisher, *application, *id;
        } discs[] = {
            {"Fedora-WS-Live-43-1-6", "FEDORA PROJECT", "FEDORA-WORKSTATION-LIVE-43", "fedora43"},
            {"Fedora-S-dvd-x86_64-43", "", "", "fedora43"},
            {"Fedora-KDE-Live-44-1-1", "", "", "fedora44"},
            {"Ubuntu 24.04.3 LTS amd64", "", "", "ubuntu24.04"},
            {"Debian 13.1.0 amd64 n", "", "", "debian13"},
            {"CCCOMA_X64FRE_EN-US_DV9", "MICROSOFT CORPORATION", "", "win11"},
            {"Rocky-9-6-x86_64-dvd", "", "", "rocky9"},
            {"CentOS-Stream-9-BaseOS-x86_64", "", "", "centos-stream9"},
            {"alpine-std 3.22.1 x86_64", "", "", "alpinelinux3.22"},
            {"nixos-minimal-25.05-x86_64", "", "", "nixos-25.05"},
            {"KDE LINUX 202610050254", "KDE", "SYSTEMD-REPART", "kdelinux"},
        };
        QTemporaryDir empty;
        for (const auto &disc : discs) {
            const QString iso = m_tmp.filePath(QString("%1.iso").arg(disc.id));
            QVERIFY(writeIso(iso, descriptor(disc.volume, {}, disc.publisher, disc.application)));
            const Detection d = detect(c, readDisc(iso), iso);
            QCOMPARE(d.id, QString(disc.id));
            if (tool.isEmpty()) {
                continue;
            }
            /* "Media is an installer for OS 'Fedora Workstation 43 (x86_64)'" */
            QProcess p;
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert("LC_ALL", "C");
            env.insert("OSINFO_SYSTEM_DIR", system);
            env.insert("OSINFO_LOCAL_DIR", empty.path());
            env.insert("OSINFO_USER_DIR", empty.path());
            p.setProcessEnvironment(env);
            p.start(tool, {iso});
            QVERIFY(p.waitForFinished(20000));
            const QString out = QString::fromUtf8(p.readAllStandardOutput());
            static const QRegularExpression osName("for OS '(.*) \\([^)]*\\)'");
            const QRegularExpressionMatch m = osName.match(out);
            /* the system osinfo-detect names, by its name or an edition's */
            Os named;
            for (const Os &os : c.systems()) {
                if (m.hasMatch() && named.isNull() &&
                    (os.name == m.captured(1) || os.editions.contains(m.captured(1)))) {
                    named = os;
                }
            }
            if (d.by == "vitrine") {
                /* osinfo-detect knows none, or only the distribution */
                QVERIFY2(!m.hasMatch() || (named.isGeneric() &&
                                           named.distro == c.find(d.id).distro),
                         qPrintable(d.id + ": " + out));
                continue;
            }
            QVERIFY2(m.hasMatch(), qPrintable(out));
            QVERIFY2(named.id == d.id, qPrintable(d.id + ": " + out));
        }
    }

    /* Real discs, read only: VITRINE_TEST_ISO="a.iso b.iso", each "PATH=ID" to check */
    void realDiscs()
    {
        const QStringList discs =
            qEnvironmentVariable("VITRINE_TEST_ISO").split(' ', Qt::SkipEmptyParts);
        if (discs.isEmpty()) {
            QSKIP("no VITRINE_TEST_ISO");
        }
        const Catalogue c(databaseDirs());
        for (const QString &item : discs) {
            const QString path = item.section('=', 0, 0);
            const QString expected = item.section('=', 1);
            const Disc disc = readDisc(path);
            const Detection d = detect(c, disc, path);
            qInfo("%s: label \"%s\", publisher \"%s\", application \"%s\", %lld bytes: "
                  "%s (%s) by %s, desktop %s",
                  qPrintable(path), qPrintable(disc.volume), qPrintable(disc.publisher),
                  qPrintable(disc.application), disc.size, qPrintable(d.id),
                  qPrintable(c.find(d.id).name), qPrintable(d.by), qPrintable(d.desktop));
            if (!expected.isEmpty()) {
                QCOMPARE(d.id, expected);
            }
        }
    }
};

QTEST_GUILESS_MAIN(TestGuestOs)
#include "test_guestos.moc"
