// SPDX-License-Identifier: GPL-2.0-or-later
#include <QAction>
#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#include "core/guestos.h"
#include "core/guesttools.h"
#include "core/paths.h"
#include "core/vmconfig.h"
#include "core/vmstore.h"
#include "ui/banner.h"
#include "ui/guesttoolsdialog.h"

/*
 * The guest tools are offered for Fedora guests only (GuestTools::offered):
 * the prompt on the console and the Details (the banner), the menu entry
 * and the dialog go by the VM's system.
 */
class TestGuestOffer : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;
    VmStore *m_store = nullptr;

    /* A VM whose last run had no tools, which the banner then offers */
    static bool writeVm(const QString &dir, const QString &guest, bool accelerated = true)
    {
        QFile f(dir + "/vm.args");

        QSettings(Paths::settingsPath(), QSettings::IniFormat)
            .setValue("guesttools/last/" + QFileInfo(dir).fileName(), "not-installed");
        return QDir().mkpath(dir) && f.open(QIODevice::WriteOnly) &&
               f.write(QString("-name %1\n%2-machine q35\n-device %3\n")
                           .arg(QFileInfo(dir).fileName(), guest,
                                accelerated ? "virtio-gpu-gl-pci,blob=on" : "VGA")
                           .toUtf8()) > 0;
    }

    Vm *vm(const QString &id) const { return m_store->find(id); }

    static QWidget *opened(const char *className)
    {
        for (QWidget *top : QApplication::topLevelWidgets()) {
            if (top->inherits(className)) {
                return top;
            }
        }
        return nullptr;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        qputenv("XDG_DATA_HOME", m_tmp.filePath("data").toUtf8());
        qputenv("XDG_CONFIG_HOME", m_tmp.filePath("config").toUtf8());
        qputenv("XDG_CACHE_HOME", m_tmp.filePath("cache").toUtf8());
        /* vitrine's list of systems, the same on every computer */
        GuestOs::Catalogue::reload({});
        const QString vms = m_tmp.filePath("vms");
        QVERIFY(writeVm(vms + "/fedora", "#guest linux,id=fedora44,desktop=kde\n"));
        QVERIFY(writeVm(vms + "/fedora43", "#guest linux,id=fedora43,desktop=kde\n"));
        QVERIFY(writeVm(vms + "/windows", "#guest windows,id=win11\n"));
        QVERIFY(writeVm(vms + "/ubuntu", "#guest linux,id=ubuntu24.04,desktop=gnome\n"));
        QVERIFY(writeVm(vms + "/linux", "#guest linux,desktop=kde\n"));
        QVERIFY(writeVm(vms + "/unknown", ""));
        QVERIFY(writeVm(vms + "/vga", "#guest linux,id=fedora44\n", false));
        m_store = new VmStore(vms, this);
        QCOMPARE(m_store->vms().size(), 7);
    }

    void cleanupTestCase()
    {
        GuestOs::Catalogue::reload();
    }

    void offer_data()
    {
        QTest::addColumn<QString>("vm");
        QTest::addColumn<bool>("offered");
        QTest::addColumn<bool>("prompted");

        QTest::newRow("fedora") << "fedora" << true << true;
        QTest::newRow("fedora 43") << "fedora43" << true << true;
        QTest::newRow("windows") << "windows" << false << false;
        QTest::newRow("ubuntu") << "ubuntu" << false << false;
        QTest::newRow("linux, not known which") << "linux" << false << false;
        QTest::newRow("not known") << "unknown" << false << false;
        /* the tools' driver is for the 3D card */
        QTest::newRow("fedora without 3D") << "vga" << true << false;
    }

    void offer()
    {
        QFETCH(QString, vm);
        QFETCH(bool, offered);
        QFETCH(bool, prompted);
        Vm *v = this->vm(vm);
        QVERIFY(v);

        /* the prompt */
        GuestToolsBanner banner;
        banner.setVm(v);
        QCOMPARE(!banner.isHidden(), prompted);

        /* the menu entry, and why not */
        QAction action;
        GuestToolsDialog::updateAction(&action, v);
        QCOMPARE(action.isEnabled(), offered);
        QCOMPARE(action.toolTip().contains("for Fedora Linux"), !offered);
        QCOMPARE(action.text().contains("(for Fedora)"), !offered);

        /* the dialog: its Install, and why not */
        GuestToolsDialog::run(nullptr, v);
        auto *dialog = qobject_cast<QDialog *>(opened("GuestToolsDialog"));
        QVERIFY(dialog);
        dialog->setAttribute(Qt::WA_DeleteOnClose, false);
        QPushButton *install = nullptr;
        for (QAbstractButton *b : dialog->findChild<QDialogButtonBox *>()->buttons()) {
            if (b->text() == tr("&Install")) {
                install = qobject_cast<QPushButton *>(b);
            }
        }
        QVERIFY(install);
        QStringList banners;
        for (const Banner *b : dialog->findChildren<Banner *>()) {
            for (const QLabel *label : b->findChildren<QLabel *>()) {
                banners << label->text();
            }
        }
        if (offered) {
            /* not built here: that is what is said */
            QVERIFY2(banners.join('\n').contains("not built yet"), qPrintable(banners.join('\n')));
        } else {
            QVERIFY(!install->isEnabled());
            QVERIFY2(banners.join('\n').contains("for Fedora Linux"),
                     qPrintable(banners.join('\n')));
        }
        /* another release than the tools': said, not refused */
        QCOMPARE(banners.join('\n').contains("they install in a Fedora 44 guest only"),
                 vm == "fedora43");
        delete dialog;

        /* no VM: nothing to offer */
        GuestToolsDialog::updateAction(&action, nullptr);
        QVERIFY(!action.isEnabled());
    }

    /* The system set in the settings: the prompt follows */
    void systemChanged()
    {
        Vm *v = vm("linux");
        GuestToolsBanner banner;
        banner.setVm(v);
        QVERIFY(banner.isHidden());

        ArgsFile args = v->args();
        VmConfig::setGuest(args, {"linux", "kde", "fedora44"});
        QVERIFY(v->save(args));
        QTRY_VERIFY(!banner.isHidden());
        VmConfig::setGuest(args, {"linux", "kde"});
        QVERIFY(v->save(args));
        QTRY_VERIFY(banner.isHidden());
    }
};

QTEST_MAIN(TestGuestOffer)
#include "test_guestoffer.moc"
