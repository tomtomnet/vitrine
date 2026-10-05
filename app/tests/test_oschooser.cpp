// SPDX-License-Identifier: GPL-2.0-or-later
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QLineEdit>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeView>

#include "core/guestos.h"
#include "core/vmstore.h"
#include "ui/oschooser.h"
#include "ui/settingspages.h"

/*
 * The system of a VM, chosen from a list to search, as virt-manager has it:
 * by families, distributions and releases, the releases no longer
 * supported left out unless asked for; and the General page, which writes
 * it in the #guest directive.
 */
class TestOsChooser : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;

    static bool write(const QString &path, const QString &text)
    {
        QFile f(path);

        return QDir().mkpath(QFileInfo(path).path()) && f.open(QIODevice::WriteOnly) &&
               f.write(text.toUtf8()) > 0;
    }

    static QString os(const QString &uri, const QString &body)
    {
        return QString("<libosinfo version=\"0.0.1\"><os id=\"%1\">%2</os></libosinfo>")
            .arg(uri, body);
    }

    /* The ids of the systems the list shows, in its order */
    static QStringList shown(const QTreeView *tree, const QModelIndex &parent = {})
    {
        QStringList ids;
        const QAbstractItemModel *model = tree->model();

        for (int r = 0; r < model->rowCount(parent); r++) {
            const QModelIndex i = model->index(r, 0, parent);
            if (model->hasChildren(i)) {
                ids += shown(tree, i);
            } else {
                const QString id = i.data(OsChooser::IdRole).toString();
                ids << (id.isEmpty() ? "family:" + i.data(OsChooser::FamilyRole).toString() : id);
            }
        }
        return ids;
    }

    /* The rows of the list's first level, its families */
    static QStringList families(const QTreeView *tree)
    {
        QStringList names;
        for (int r = 0; r < tree->model()->rowCount(); r++) {
            names << tree->model()->index(r, 0).data().toString();
        }
        return names;
    }

private slots:
    void initTestCase()
    {
        const QString db = m_tmp.filePath("osinfo");
        /* (not "linux": GCC defines it) */
        const QString family = "<family>linux</family>";

        QVERIFY(m_tmp.isValid());
        qputenv("XDG_DATA_HOME", m_tmp.filePath("data").toUtf8());
        qputenv("XDG_CONFIG_HOME", m_tmp.filePath("config").toUtf8());
        QVERIFY(write(db + "/os/fedoraproject.org/fedora-42.xml",
                      os("http://fedoraproject.org/fedora/42",
                         "<short-id>fedora42</short-id><name>Fedora Linux 42</name>"
                         "<version>42</version><distro>fedora</distro>" + family +
                             "<eol-date>2020-01-01</eol-date>")));
        QVERIFY(write(db + "/os/fedoraproject.org/fedora-43.xml",
                      os("http://fedoraproject.org/fedora/43",
                         "<short-id>fedora43</short-id><name>Fedora Linux 43</name>"
                         "<version>43</version><distro>fedora</distro>" + family +
                             "<variant id=\"workstation\"><name>Fedora Workstation 43</name>"
                             "</variant>")));
        QVERIFY(write(db + "/os/fedoraproject.org/fedora-rawhide.xml",
                      os("http://fedoraproject.org/fedora/rawhide",
                         "<short-id>fedora-rawhide</short-id><name>Fedora Rawhide</name>"
                         "<version>Rawhide</version><distro>fedora</distro>" + family +
                             "<release-status>prerelease</release-status>")));
        QVERIFY(write(db + "/os/debian.org/debian-12.xml",
                      os("http://debian.org/debian/12",
                         "<short-id>debian12</short-id><short-id>debianbookworm</short-id>"
                         "<name>Debian 12</name><version>12</version><distro>debian</distro>" +
                             family)));
        QVERIFY(write(db + "/os/microsoft.com/winnt-4.0.xml",
                      os("http://microsoft.com/winnt/4.0",
                         "<short-id>winnt4.0</short-id><name>Microsoft Windows NT Server 4.0</name>"
                         "<version>4.0</version><family>winnt</family><distro>winnt</distro>"
                         "<eol-date>2004-12-31</eol-date>")));
        QVERIFY(write(db + "/os/libosinfo.org/linux-2024.xml",
                      os("http://libosinfo.org/linux/2024",
                         "<short-id>linux2024</short-id><name>Generic Linux 2024</name>"
                         "<version>2024</version>" + family)));
        GuestOs::Catalogue::reload({db});
    }

    void cleanupTestCase()
    {
        GuestOs::Catalogue::reload();
    }

    /* The field: the system with its logo, or its family alone */
    void field()
    {
        OsChooser chooser;

        QCOMPARE(chooser.currentText(), "Not set");
        QCOMPARE(chooser.count(), 1);
        chooser.setSystem("fedora43");
        QCOMPARE(chooser.currentText(), "Fedora Linux 43");
        QCOMPARE(chooser.id(), "fedora43");
        QCOMPARE(chooser.family(), "linux");
        QVERIFY(!chooser.itemIcon(0).isNull());
        chooser.setSystem("fedora-unknown");
        QCOMPARE(chooser.currentText(), "Fedora (another release)");
        /* newer than the list */
        chooser.setSystem("fedora46", "linux");
        QCOMPARE(chooser.currentText(), "Fedora Linux 46");
        chooser.setSystem("", "linux");
        QCOMPARE(chooser.currentText(), "Linux");
        QCOMPARE(chooser.id(), "");
        QCOMPARE(chooser.family(), "linux");
        chooser.setSystem("", "windows");
        QCOMPARE(chooser.currentText(), "Windows");
        /* a system nobody knows, as the file says it */
        chooser.setSystem("someos3", "other");
        QCOMPARE(chooser.currentText(), "someos3");
        QCOMPARE(chooser.family(), "other");
        chooser.setSystem({});
        QCOMPARE(chooser.currentText(), "Not set");
        QCOMPARE(chooser.family(), "");
    }

    /* Families, distributions, releases newest first, the family alone last */
    void list()
    {
        OsChooser chooser;
        chooser.setSystem("fedora43");
        chooser.showPopup();
        QTreeView *tree = chooser.tree();
        QVERIFY(chooser.popup()->isVisible());
        QVERIFY(chooser.searchField()->hasFocus() || !chooser.popup()->isActiveWindow());

        QCOMPARE(families(tree), QStringList({"Linux", "Windows", "BSD", "macOS",
                                              "Other systems"}));
        const QStringList ids = shown(tree);
        /* Fedora: Rawhide, 44 (vitrine's), 43, the others; not 42, no longer supported */
        const qsizetype rawhide = ids.indexOf("fedora-rawhide");
        QVERIFY(rawhide >= 0);
        QCOMPARE(ids.mid(rawhide, 4),
                 QStringList({"fedora-rawhide", "fedora44", "fedora43", "fedora-unknown"}));
        QVERIFY(!ids.contains("fedora42"));
        /* the database's generic ones: the family's row stands for them */
        QVERIFY(!ids.contains("linux2024"));
        QVERIFY(ids.contains("family:linux"));
        QVERIFY(ids.contains("family:windows"));
        QVERIFY(ids.contains("family:other"));
        /* Windows, one distribution (NT 4.0's too): its releases right under it */
        const QModelIndex windows = tree->model()->index(1, 0);
        QCOMPARE(tree->model()->index(0, 0, windows).data(OsChooser::IdRole).toString(), "win11");
        /* a distribution of one row, which stands for all its releases */
        bool zorin = false;
        for (int r = 0; r < tree->model()->rowCount(tree->model()->index(0, 0)); r++) {
            const QModelIndex i = tree->model()->index(r, 0, tree->model()->index(0, 0));
            if (i.data(OsChooser::IdRole).toString() == "zorin-unknown") {
                zorin = i.data().toString() == "Zorin OS";
            }
        }
        QVERIFY(zorin);
        /* the one shown, current, its distribution open */
        QCOMPARE(tree->currentIndex().data(OsChooser::IdRole).toString(), "fedora43");
        QVERIFY(tree->isExpanded(tree->currentIndex().parent()));
        chooser.hidePopup();
        QVERIFY(!chooser.popup()->isVisible());
    }

    void search_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QStringList>("ids");

        QTest::newRow("release") << "fedora 43" << QStringList({"fedora43"});
        QTest::newRow("case") << "FEDORA linux 4" << QStringList({"fedora44", "fedora43"});
        QTest::newRow("edition") << "workstation" << QStringList({"fedora43"});
        QTest::newRow("another name") << "bookworm" << QStringList({"debian12"});
        QTest::newRow("by its id") << "win11" << QStringList({"win11"});
        QTest::newRow("words") << "win 11" << QStringList({"win11"});
        QTest::newRow("family alone") << "another distribution"
                                      << QStringList({"family:linux"});
        QTest::newRow("nothing") << "zzz" << QStringList();
    }

    void search()
    {
        QFETCH(QString, text);
        QFETCH(QStringList, ids);
        OsChooser chooser;
        QSignalSpy chosen(&chooser, &OsChooser::systemChosen);

        chooser.showPopup();
        QTest::keyClicks(chooser.searchField(), text);
        QCOMPARE(shown(chooser.tree()), ids);
        /* Enter: the first found */
        QTest::keyClick(chooser.searchField(), Qt::Key_Return);
        if (ids.isEmpty()) {
            QCOMPARE(chosen.count(), 0);
            QCOMPARE(chooser.id(), "");
            return;
        }
        QCOMPARE(chosen.count(), 1);
        QVERIFY(!chooser.popup()->isVisible());
        QCOMPARE(chooser.id().isEmpty() ? "family:" + chooser.family() : chooser.id(),
                 ids.first());
    }

    /* Releases no longer supported: when asked for, or when shown */
    void oldReleases()
    {
        OsChooser chooser;

        chooser.showPopup();
        QVERIFY(!shown(chooser.tree()).contains("fedora42"));
        chooser.oldReleases()->setChecked(true);
        QVERIFY(shown(chooser.tree()).contains("fedora42"));
        chooser.hidePopup();

        chooser.setSystem("fedora42");
        chooser.showPopup();
        QVERIFY(!chooser.oldReleases()->isChecked() || shown(chooser.tree()).contains("fedora42"));
        chooser.oldReleases()->setChecked(false);
        QVERIFY(shown(chooser.tree()).contains("fedora42"));
        QCOMPARE(chooser.tree()->currentIndex().data(OsChooser::IdRole).toString(), "fedora42");
    }

    /* The keys: down into the list, Enter on a row; a click */
    void keysAndClicks()
    {
        OsChooser chooser;
        QSignalSpy chosen(&chooser, &OsChooser::systemChosen);

        chooser.showPopup();
        QTest::keyClicks(chooser.searchField(), "debian");
        QTest::keyClick(chooser.searchField(), Qt::Key_Down);
        QCOMPARE(chooser.tree()->currentIndex().data(OsChooser::IdRole).toString(), "debian12");
        QTest::keyClick(chooser.tree(), Qt::Key_Down);
        QTest::keyClick(chooser.tree(), Qt::Key_Return);
        QCOMPARE(chosen.count(), 1);
        /* Debian 12, then Debian (another release) */
        QCOMPARE(chooser.id(), "debian-unknown");

        /* a distribution's row opens and closes; a release's chooses it */
        chooser.setSystem({});
        chooser.showPopup();
        QTreeView *tree = chooser.tree();
        QModelIndex fedora;
        for (int r = 0; r < tree->model()->rowCount(tree->model()->index(0, 0)); r++) {
            const QModelIndex i = tree->model()->index(r, 0, tree->model()->index(0, 0));
            if (i.data().toString() == "Fedora") {
                fedora = i;
            }
        }
        QVERIFY(fedora.isValid());
        QVERIFY(!tree->isExpanded(fedora));
        tree->scrollTo(fedora);
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, tree->visualRect(fedora).center());
        QVERIFY(tree->isExpanded(fedora));
        QCOMPARE(chosen.count(), 1);
        const QModelIndex f43 = tree->model()->index(2, 0, fedora);
        QCOMPARE(f43.data(OsChooser::IdRole).toString(), "fedora43");
        tree->scrollTo(f43);
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, tree->visualRect(f43).center());
        QCOMPARE(chosen.count(), 2);
        QCOMPARE(chooser.id(), "fedora43");
        QCOMPARE(chooser.currentText(), "Fedora Linux 43");
        QVERIFY(!chooser.popup()->isVisible());
    }

    /* The General page: the system in #guest, its family with it */
    void generalPage()
    {
        QTemporaryDir dir;
        QVERIFY(write(dir.filePath("vm.args"), "-name Desktop\n#guest linux,desktop=kde\n-m 4G\n"));
        Vm vm(dir.path());
        GeneralPage page(&vm);
        auto *chooser = page.findChild<OsChooser *>("os");
        auto *desktop = page.findChild<QComboBox *>("desktop");
        ArgsFile args = vm.args();

        QVERIFY(chooser && desktop);
        page.load(args);
        QCOMPARE(chooser->currentText(), "Linux");
        QVERIFY(!page.isModified());
        page.save(args);
        QCOMPARE(args.toText(), vm.args().toText());

        /* chosen in the list */
        chooser->showPopup();
        QTest::keyClicks(chooser->searchField(), "fedora 43");
        QTest::keyClick(chooser->searchField(), Qt::Key_Return);
        QVERIFY(page.isModified());
        QVERIFY(desktop->isEnabled());
        page.save(args);
        QCOMPARE(args.toText(), "-name Desktop\n#guest linux,id=fedora43,desktop=kde\n-m 4G\n");
        page.load(args);
        QVERIFY(!page.isModified());
        QCOMPARE(chooser->currentText(), "Fedora Linux 43");

        /* Windows: no desktop of Linux */
        chooser->showPopup();
        QTest::keyClicks(chooser->searchField(), "win 11");
        QTest::keyClick(chooser->searchField(), Qt::Key_Return);
        QVERIFY(!desktop->isEnabled());
        page.save(args);
        QCOMPARE(args.toText(), "-name Desktop\n#guest windows,id=win11\n-m 4G\n");

        /* a system the list has not: the family alone */
        chooser->showPopup();
        QTest::keyClicks(chooser->searchField(), "another distribution");
        QTest::keyClick(chooser->searchField(), Qt::Key_Return);
        page.save(args);
        QCOMPARE(args.toText(), "-name Desktop\n#guest linux,desktop=kde\n-m 4G\n");
    }
};

QTEST_MAIN(TestOsChooser)
#include "test_oschooser.moc"
