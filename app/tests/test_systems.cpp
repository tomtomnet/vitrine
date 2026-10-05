// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QFile>
#include <QPainter>
#include <QTemporaryDir>

#include "core/guestos.h"
#include "scales.h"
#include "ui/icons.h"
#include "ui/systems.h"

/*
 * The systems as the windows show them: the logo of a system, the theme's
 * when it has one, else drawn for the size and pixel ratio asked, crisp at
 * every scale (scales.h), with the list's badges on top as on the
 * computer; and their names.
 */
class TestSystems : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;
    QStringList m_themePaths;
    QString m_theme;

    static QImage image(const QPixmap &pixmap)
    {
        return pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
    }

    static GuestOs::Os os(const QString &id)
    {
        return GuestOs::Catalogue::instance().find(id);
    }

    /* An icon theme with one logo, and nothing to fall back on */
    bool makeTheme()
    {
        const QString dir = m_tmp.filePath("icons/vitrinetest");
        QFile index(dir + "/index.theme");
        QFile logo(dir + "/scalable/apps/distributor-logo-archlinux.svg");

        return QDir().mkpath(dir + "/scalable/apps") && index.open(QIODevice::WriteOnly) &&
               index.write("[Icon Theme]\nName=vitrinetest\nDirectories=scalable/apps\n\n"
                           "[scalable/apps]\nSize=48\nMinSize=8\nMaxSize=512\n"
                           "Type=Scalable\n") > 0 &&
               logo.open(QIODevice::WriteOnly) &&
               logo.write("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 4 4\">"
                          "<rect width=\"4\" height=\"4\" fill=\"#00ff00\"/></svg>") > 0;
    }

private slots:
    void initTestCase()
    {
        Scales::quieter();
        QVERIFY(m_tmp.isValid());
        m_themePaths = QIcon::themeSearchPaths();
        m_theme = QIcon::themeName();
        /* vitrine's list of systems, the same on every computer */
        GuestOs::Catalogue::reload({});
        /* a theme without distributor logos, nor Fedora's in hicolor */
        QVERIFY(makeTheme());
        QIcon::setThemeSearchPaths({m_tmp.filePath("icons")});
        QIcon::setThemeName("vitrinetest");
        Systems::reloadIcons();
    }

    void cleanupTestCase()
    {
        QIcon::setThemeSearchPaths(m_themePaths);
        QIcon::setThemeName(m_theme);
        Systems::reloadIcons();
        GuestOs::Catalogue::reload();
    }

    void keys_data()
    {
        QTest::addColumn<QString>("id");
        QTest::addColumn<QString>("family");
        QTest::addColumn<QString>("key");

        QTest::newRow("fedora") << "fedora44" << "linux" << "fedora";
        QTest::newRow("fedora 45") << "fedora45" << "linux" << "fedora";
        QTest::newRow("silverblue") << "silverblue43" << "linux" << "fedora";
        QTest::newRow("ubuntu") << "ubuntu24.04" << "linux" << "ubuntu";
        QTest::newRow("arch") << "archlinux" << "linux" << "archlinux";
        QTest::newRow("kde linux") << "kdelinux" << "linux" << "kde";
        QTest::newRow("mint") << "linuxmint22" << "linux" << "linuxmint";
        QTest::newRow("rhel") << "rhel9.6" << "linux" << "redhat";
        QTest::newRow("windows") << "win11" << "windows" << "windows";
        QTest::newRow("freebsd") << "freebsd14.3" << "other" << "freebsd";
        QTest::newRow("macos") << "macos-unknown" << "other" << "apple";
        /* no logo of its own: its family's, or the computer */
        QTest::newRow("unknown linux") << "mageia9" << "linux" << "linux";
        QTest::newRow("haiku") << "haiku-unknown" << "other" << "";
        QTest::newRow("linux") << "" << "linux" << "linux";
        QTest::newRow("windows family") << "" << "windows" << "windows";
        QTest::newRow("other") << "" << "other" << "";
        QTest::newRow("not known") << "" << "" << "";
    }

    void keys()
    {
        QFETCH(QString, id);
        QFETCH(QString, family);
        QFETCH(QString, key);

        QCOMPARE(Systems::key(os(id), family), key);
    }

    void names()
    {
        QCOMPARE(Systems::name({"linux", "kde", "fedora44"}), "Fedora Linux 44");
        QCOMPARE(Systems::name({"linux", "kde", "fedora45"}), "Fedora Linux 45");
        QCOMPARE(Systems::name({"linux", "", "nothing"}), "nothing");
        QCOMPARE(Systems::name({"linux", "kde", ""}), "Linux");
        QCOMPARE(Systems::name({"windows", "", ""}), "Windows");
        QCOMPARE(Systems::name({"other", "", ""}), "Another system");
        QCOMPARE(Systems::name({}), "");
        QCOMPARE(Systems::desktopName("kde"), "KDE Plasma");
        QCOMPARE(Systems::desktopName("gnome"), "GNOME");
        QCOMPARE(Systems::desktopName(""), "");
    }

    /* The theme's logo first: distributor-logo-NAME, or a distribution's own */
    void themeLogos()
    {
        QCOMPARE(Systems::themeNames("fedora"),
                 QStringList({"distributor-logo-fedora", "fedora-logo-icon"}));
        /* this computer's distributor logo is not a guest's */
        QCOMPARE(Systems::themeNames("linux"), QStringList());
        QCOMPARE(Systems::themeNames(""), QStringList());

        const QIcon arch = Systems::icon(os("archlinux"));
        QCOMPARE(arch.name(), "distributor-logo-archlinux");
        const QImage drawn = image(arch.pixmap(QSize(32, 32), 1.0));
        QCOMPARE(QColor(drawn.pixel(16, 16)), QColor(0x00, 0xff, 0x00));
        /* none in the theme: vitrine's */
        QCOMPARE(Systems::icon(os("ubuntu24.04")).name(), "");
        /* the same icon each time */
        QCOMPARE(Systems::icon(os("ubuntu24.04")).cacheKey(),
                 Systems::icon(os("ubuntu25.10")).cacheKey());
        /* the computer, for the others */
        QCOMPARE(Systems::icon(os("haiku-unknown")).cacheKey(),
                 Systems::icon(VmConfig::Guest{}).cacheKey());
    }

    /*
     * vitrine's logos: the brand's colour on whole device pixels to its
     * edge, the logo or the letter in white (dark on a light colour), at
     * every scale
     */
    void tiles()
    {
        Scales::sweep([this](int n) {
            const qreal ratio = n / 120.0;
            QStringList wrong;
            const struct {
                const char *id;
                const char *family;
                QRgb color;
                bool darkInk;
            } logos[] = {
                {"win11", "windows", 0x0078d4, false},    // vitrine's own drawing
                {"ubuntu24.04", "linux", 0xe95420, false},
                {"mageia9", "linux", 0xfcc624, true},     // Tux, dark on yellow
                {"fedora44", "linux", 0x51a2da, false},   // a letter: the theme has none
            };
            for (const auto &l : logos) {
                const QIcon icon = Systems::icon(os(l.id), l.family);
                for (int size : {16, 22, 32, 48}) {
                    const QPixmap pixmap = icon.pixmap(QSize(size, size), ratio);
                    const QImage img = image(pixmap);
                    const int side = qRound(size * ratio);
                    const QString what = QString("%1 at %2").arg(l.id).arg(size);

                    if (img.size() != QSize(side, side) ||
                        !qFuzzyCompare(pixmap.devicePixelRatio(), ratio)) {
                        wrong << QString("%1: %2x%3 at %4, not %5x%5 at %6")
                                     .arg(what)
                                     .arg(img.width())
                                     .arg(img.height())
                                     .arg(pixmap.devicePixelRatio())
                                     .arg(side)
                                     .arg(ratio);
                        continue;
                    }
                    /* the colour, flat, on the middle row left of the logo */
                    const int y = side / 2;
                    const int x = qRound(side * 4.0 / 32);
                    if ((img.pixel(x, y) & 0xffffff) != l.color || qAlpha(img.pixel(x, y)) != 255) {
                        wrong << QString("%1: %2 at %3,%4").arg(what)
                                     .arg(img.pixel(x, y), 8, 16, QChar('0')).arg(x).arg(y);
                    }
                    /* a sharp edge: one pixel between nothing and the colour */
                    int partial = 0;
                    for (int i = 0; i < x; i++) {
                        partial += qAlpha(img.pixel(i, y)) > 0 && qAlpha(img.pixel(i, y)) < 255;
                    }
                    if (partial > 1) {
                        wrong << QString("%1: an edge %2 pixels wide").arg(what).arg(partial);
                    }
                    /* the logo, in its ink */
                    int ink = 0;
                    const int from = side * 6 / 32;
                    const int to = side * 26 / 32;
                    for (int j = from; j < to; j++) {
                        for (int i = from; i < to; i++) {
                            const QColor c(img.pixel(i, j));
                            ink += l.darkInk ? c.lightness() < 80 : c.lightness() > 230;
                        }
                    }
                    if (ink < (to - from) * (to - from) / 20) {
                        wrong << QString("%1: no logo (%2 pixels)").arg(what).arg(ink);
                    }
                }
            }
            return wrong;
        });
    }

    /* The VM list's: the badge on the logo, the logo as it is above it */
    void badges()
    {
        Scales::sweep([this](int n) {
            const qreal ratio = n / 120.0;
            QStringList wrong;
            const QIcon base = Systems::icon(os("debian13"));
            const QIcon badged = Icons::badged(base, Qt::green, Icons::Badge::Play);

            for (int size : {32, 48}) {
                const QImage plain = image(base.pixmap(QSize(size, size), ratio));
                const QImage with = image(badged.pixmap(QSize(size, size), ratio));
                if (with.size() != plain.size()) {
                    wrong << QString("%1: %2x%3, the icon is %4x%5").arg(size)
                                 .arg(with.width()).arg(with.height())
                                 .arg(plain.width()).arg(plain.height());
                    continue;
                }
                int changed = 0;
                for (int y = 0; y < with.height() * 15 / 32; y++) {
                    for (int x = 0; x < with.width(); x++) {
                        changed += with.pixel(x, y) != plain.pixel(x, y);
                    }
                }
                if (changed > 0) {
                    wrong << QString("%1: %2 pixels above the badge changed").arg(size).arg(changed);
                }
            }
            return wrong;
        });
    }

    /* VITRINE_SYSTEMS_SHEET=FILE.png: every logo at four sizes, on light and dark */
    void sheet()
    {
        const QString path = qEnvironmentVariable("VITRINE_SYSTEMS_SHEET");
        const QStringList ids = {
            "fedora44", "archlinux", "debian13", "ubuntu24.04", "opensusetumbleweed",
            "linuxmint22", "manjaro", "endeavouros", "cachyos", "kdelinux", "popos-unknown",
            "elementary-unknown", "zorin-unknown", "kalilinux", "rhel-unknown",
            "centos-stream-unknown", "almalinux-unknown", "rocky-unknown", "sle-unknown",
            "alpinelinux-unknown", "nixos-unknown", "gentoo", "voidlinux", "slackware",
            "freebsd-unknown", "openbsd-unknown", "netbsd-unknown", "macos-unknown", "win11",
            "haiku-unknown"};
        const qreal ratio = qEnvironmentVariableIsSet("QT_SCALE_FACTOR")
                                ? qEnvironmentVariable("QT_SCALE_FACTOR").toDouble()
                                : 1.0;
        if (path.isEmpty()) {
            QSKIP("VITRINE_SYSTEMS_SHEET is not set");
        }
        /* a row per system: four sizes on white, then on dark */
        const int row = 54;
        const int half = 150;
        QImage sheet(QSize(2 * half, int(ids.size()) * row) * ratio, QImage::Format_ARGB32);
        sheet.setDevicePixelRatio(ratio);
        QPainter p(&sheet);
        p.fillRect(QRectF(0, 0, half, ids.size() * row), Qt::white);
        p.fillRect(QRectF(half, 0, half, ids.size() * row), QColor(0x23, 0x26, 0x29));
        for (qsizetype i = 0; i < ids.size(); i++) {
            const QIcon icon = Systems::icon(os(ids[i]));
            for (int side : {0, 1}) {
                int x = side * half + 4;
                for (int size : {16, 22, 32, 48}) {
                    Icons::paint(&p, icon, size, QPointF(x + size / 2.0, i * row + row / 2.0));
                    x += size + 6;
                }
            }
        }
        p.end();
        QVERIFY(sheet.save(path));
    }
};

QTEST_MAIN(TestSystems)
#include "test_systems.moc"
