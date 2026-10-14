// SPDX-License-Identifier: GPL-2.0-or-later
#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#include "core/guesttools.h"
#include "core/paths.h"
#include "core/vmconfig.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "ui/banner.h"
#include "ui/guesttoolsdialog.h"

using GuestTools::System;

/*
 * Whom the guest tools are offered to: guests known to run a system they
 * are for, Fedora at the medium's release, as the guest said through an
 * agent when it last ran.  The prompt on its pages (the banner) for such a
 * guest with the 3D card their driver is for; none for a guest that never
 * said, or runs another system.  Install Guest Tools… for any VM, its
 * dialog saying whom they are for and what the guest runs; its Install
 * off for a guest known to run another system, or a VM set up for another
 * system than Linux whose guest never said.
 */
class TestGuestOffer : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;
    VmStore *m_store = nullptr;

    /*
     * A VM whose last run showed @last ("not-installed", or "installed" and
     * the tools' version), whose guest said it runs @system
     */
    static bool writeVm(const QString &dir, const QString &lines, const System &system,
                        bool accelerated = true, const QString &last = "not-installed")
    {
        const QString id = QFileInfo(dir).fileName();
        QFile f(dir + "/vm.args");

        QSettings(Paths::settingsPath(), QSettings::IniFormat)
            .setValue("guesttools/last/" + id, last);
        GuestTools::setSystem(id, system);
        return QDir().mkpath(dir) && f.open(QIODevice::WriteOnly) &&
               f.write(QString("-name %1\n%2-machine q35\n-device %3\n")
                           .arg(id, lines, accelerated ? "virtio-vga-gl,blob=on" : "VGA")
                           .toUtf8()) > 0;
    }

    Vm *vm(const QString &id) const { return m_store->find(id); }

    static GuestToolsDialog *opened()
    {
        for (QWidget *top : QApplication::topLevelWidgets()) {
            if (auto *dialog = qobject_cast<GuestToolsDialog *>(top)) {
                return dialog;
            }
        }
        return nullptr;
    }

    /* The texts of @widget's labels shown, its banners' among them */
    static QString shownText(QWidget *widget)
    {
        QStringList texts;
        for (const QLabel *label : widget->findChildren<QLabel *>()) {
            if (label->isVisibleTo(widget)) {
                texts << label->text();
            }
        }
        return texts.join('\n');
    }

    /* The banner's button shown, if any */
    static QString button(const GuestToolsBanner &banner)
    {
        for (const QPushButton *b : banner.findChildren<QPushButton *>()) {
            if (b->isVisibleTo(&banner)) {
                return b->text();
            }
        }
        return {};
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        qputenv("XDG_DATA_HOME", m_tmp.filePath("data").toUtf8());
        qputenv("XDG_CONFIG_HOME", m_tmp.filePath("config").toUtf8());
        qputenv("XDG_CACHE_HOME", m_tmp.filePath("cache").toUtf8());
        /* a medium for Fedora 44, so that the dialog says what else keeps the tools out */
        QVERIFY(QDir().mkpath(GuestTools::dataDir()));
        QFile image(GuestTools::dataDir() + "/vitrine-guest-tools-fc44.img");
        QFile json(GuestTools::dataDir() + "/vitrine-guest-tools-fc44.json");
        QVERIFY(image.open(QIODevice::WriteOnly) && image.write(QByteArray(4096, '\0')) > 0);
        QVERIFY(json.open(QIODevice::WriteOnly) &&
                json.write(R"({"label": "VITRINETOOL", "mediumId": "32fe6c83b34e87e8",
                               "fedora": "44", "tools": "0.1.0-1.fc44", "packages": []})") > 0);
        image.close();
        json.close();
        QVERIFY(GuestTools::medium().isValid());

        const QString vms = m_tmp.filePath("vms");
        const System fedora44{"fedora", "44", "Fedora Linux 44 (KDE Plasma Desktop Edition)"};
        const System fedora43{"fedora", "43", "Fedora Linux 43 (Workstation Edition)"};
        const System kde{"kde-linux", "", "KDE Linux"};
        QVERIFY(writeVm(vms + "/fedora", "#guest linux,desktop=kde\n", fedora44));
        QVERIFY(writeVm(vms + "/fedora43", "#guest linux,desktop=kde\n", fedora43));
        QVERIFY(writeVm(vms + "/kde", "#guest linux,desktop=kde\n", kde));
        QVERIFY(writeVm(vms + "/linux", "#guest linux,desktop=kde\n", {}));
        QVERIFY(writeVm(vms + "/unknown", "", {}));
        QVERIFY(writeVm(vms + "/windows", "#guest windows\n", {}));
        QVERIFY(writeVm(vms + "/other", "#guest other\n", {}));
        /* the guest's word over the VM's settings */
        QVERIFY(writeVm(vms + "/saidfedora", "#guest windows\n", fedora44));
        QVERIFY(writeVm(vms + "/vga", "#guest linux,desktop=kde\n", fedora44, false));
        /* older tools than the medium's when it last ran, then another release */
        QVERIFY(writeVm(vms + "/older", "#guest linux,desktop=kde\n", fedora44, true,
                        "installed 0.0.9-1.fc44"));
        QVERIFY(writeVm(vms + "/upgraded", "#guest linux,desktop=kde\n",
                        {"fedora", "45", "Fedora Linux 45 (KDE Plasma Desktop Edition)"}, true,
                        "installed 0.0.9-1.fc44"));
        m_store = new VmStore(vms, this);
        QCOMPARE(m_store->vms().size(), 11);
    }

    void offer_data()
    {
        QTest::addColumn<QString>("vm");
        QTest::addColumn<QString>("prompt");        // the banner's button, empty if none
        QTest::addColumn<bool>("installable");
        QTest::addColumn<QString>("said");          // in the dialog

        QTest::newRow("fedora 44")
            << "fedora" << "Install Guest Tools…" << true
            << "This guest runs Fedora Linux 44 (KDE Plasma Desktop Edition).";
        QTest::newRow("fedora 43")
            << "fedora43" << "" << false
            << "This guest runs Fedora Linux 43 (Workstation Edition), which the guest tools "
               "are not for.";
        QTest::newRow("another system")
            << "kde" << "" << false << "This guest runs KDE Linux, which the guest tools are "
                                       "not for.";
        QTest::newRow("never said")
            << "linux" << "" << true << "What this guest runs is not known yet";
        QTest::newRow("never said, no #guest")
            << "unknown" << "" << true << "What this guest runs is not known yet";
        QTest::newRow("set up for windows")
            << "windows" << "" << false << "This VM is set up for Windows.";
        QTest::newRow("set up for another system")
            << "other" << "" << false << "This VM is set up for another system than Linux.";
        QTest::newRow("set up for windows, said fedora")
            << "saidfedora" << "Install Guest Tools…" << true
            << "This guest runs Fedora Linux 44";
        /* the tools' driver is for the 3D card */
        QTest::newRow("fedora without 3D")
            << "vga" << "" << true << "This guest runs Fedora Linux 44";
        QTest::newRow("older tools") << "older" << "Update…" << true << "This guest runs";
        QTest::newRow("older tools, upgraded since")
            << "upgraded" << "" << false << "This guest runs Fedora Linux 45 (KDE Plasma "
                                            "Desktop Edition), which the guest tools are not for.";
    }

    void offer()
    {
        QFETCH(QString, vm);
        QFETCH(QString, prompt);
        QFETCH(bool, installable);
        QFETCH(QString, said);
        Vm *v = this->vm(vm);
        QVERIFY(v);

        /* the prompt */
        GuestToolsBanner banner;
        banner.setVm(v);
        QCOMPARE(!banner.isHidden(), !prompt.isEmpty());
        if (!prompt.isEmpty()) {
            QCOMPARE(button(banner), prompt);
        }

        /* the dialog: its Install, whom the tools are for, what the guest runs */
        GuestToolsDialog::run(nullptr, v);
        GuestToolsDialog *dialog = opened();
        QVERIFY(dialog);
        dialog->setAttribute(Qt::WA_DeleteOnClose, false);
        QPushButton *install = nullptr;
        for (QAbstractButton *b : dialog->findChild<QDialogButtonBox *>()->buttons()) {
            if (b->text() == "&Install") {
                install = qobject_cast<QPushButton *>(b);
            }
        }
        QVERIFY(install);
        QCOMPARE(install->isEnabled(), installable);
        const QString text = shownText(dialog);
        QVERIFY2(text.contains("The guest tools are for Fedora 44 guests"), qPrintable(text));
        QVERIFY2(text.contains(said), qPrintable(text));
        delete dialog;
    }

    /*
     * No medium built yet: any Fedora is offered (the build makes the one
     * for 44), and a guest known to run another system hears so before a
     * build, beside the build's banner
     */
    void noMedium()
    {
        const QString image = GuestTools::dataDir() + "/vitrine-guest-tools-fc44.img";
        QVERIFY(QFile::rename(image, image + ".away"));
        const auto back = qScopeGuard([image]() { QFile::rename(image + ".away", image); });
        QVERIFY(!GuestTools::medium().isValid());
        /* the VMs as the app finds them then */
        VmStore store(m_tmp.filePath("vms"));

        GuestToolsBanner banner;
        banner.setVm(store.find("fedora43"));
        QVERIFY(!banner.isHidden());
        QCOMPARE(button(banner), "Install Guest Tools…");
        banner.setVm(store.find("kde"));
        QVERIFY(banner.isHidden());

        for (const QString &id : {QString("kde"), QString("fedora43")}) {
            GuestToolsDialog::run(nullptr, store.find(id));
            GuestToolsDialog *dialog = opened();
            QVERIFY(dialog);
            dialog->setAttribute(Qt::WA_DeleteOnClose, false);
            const QString text = shownText(dialog);
            if (id == "kde") {
                QVERIFY2(text.contains("This guest runs KDE Linux, which the guest tools are not "
                                       "for."),
                         qPrintable(text));
            } else {
                QVERIFY2(text.contains("This guest runs Fedora Linux 43 (Workstation Edition)."),
                         qPrintable(text));
                QVERIFY2(!text.contains("not for"), qPrintable(text));
            }
            for (QAbstractButton *b : dialog->findChild<QDialogButtonBox *>()->buttons()) {
                if (b->text() == "&Install") {
                    QVERIFY(!b->isEnabled());
                }
            }
            delete dialog;
        }
    }

    /* What the guest says, as it says it: the prompt follows */
    void guestSays()
    {
        Vm *v = vm("linux");
        GuestToolsBanner banner;
        banner.setVm(v);
        QVERIFY(banner.isHidden());

        emit v->runner()->guestOsInfo(
            {{"id", "fedora"}, {"version-id", "44"}, {"pretty-name", "Fedora Linux 44"}});
        QVERIFY(!banner.isHidden());
        QCOMPARE(GuestTools::system(v->id()).id, "fedora");
        emit v->runner()->guestOsInfo({{"id", "arch"}, {"pretty-name", "Arch Linux"}});
        QVERIFY(banner.isHidden());
        emit v->runner()->guestOsInfo(
            {{"id", "fedora"}, {"version-id", "43"}, {"pretty-name", "Fedora Linux 43"}});
        QVERIFY(banner.isHidden());

        /* the 3D card, in the settings */
        emit v->runner()->guestOsInfo(
            {{"id", "fedora"}, {"version-id", "44"}, {"pretty-name", "Fedora Linux 44"}});
        QVERIFY(!banner.isHidden());
        ArgsFile args = v->args();
        args.setValueAt(args.indexOf("device"), "VGA");
        QVERIFY(v->save(args));
        QTRY_VERIFY(banner.isHidden());
        args.setValueAt(args.indexOf("device"), "virtio-vga-gl,blob=on");
        QVERIFY(v->save(args));
        QTRY_VERIFY(!banner.isHidden());
    }
};

QTEST_MAIN(TestGuestOffer)
#include "test_guestoffer.moc"
