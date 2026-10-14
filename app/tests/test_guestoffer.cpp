// SPDX-License-Identifier: GPL-2.0-or-later
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

#include "core/guesttools.h"
#include "core/paths.h"
#include "core/vmconfig.h"
#include "core/vmstore.h"
#include "ui/banner.h"
#include "ui/guesttoolsdialog.h"

/*
 * Whom the guest tools are offered to, as far as the VM's settings tell
 * (#guest): the prompt on its pages (the banner) for a Linux guest with the
 * 3D card their driver is for, or a guest not said that has no Hyper-V
 * enlightenments; the dialog's Install for any guest but another system
 * than Linux.
 */
class TestGuestOffer : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;
    VmStore *m_store = nullptr;

    /* A VM whose last run had no tools, which the banner then offers */
    static bool writeVm(const QString &dir, const QString &lines, bool accelerated = true)
    {
        QFile f(dir + "/vm.args");

        QSettings(Paths::settingsPath(), QSettings::IniFormat)
            .setValue("guesttools/last/" + QFileInfo(dir).fileName(), "not-installed");
        return QDir().mkpath(dir) && f.open(QIODevice::WriteOnly) &&
               f.write(QString("-name %1\n%2-machine q35\n-device %3\n")
                           .arg(QFileInfo(dir).fileName(), lines,
                                accelerated ? "virtio-vga-gl,blob=on" : "VGA")
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

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        qputenv("XDG_DATA_HOME", m_tmp.filePath("data").toUtf8());
        qputenv("XDG_CONFIG_HOME", m_tmp.filePath("config").toUtf8());
        qputenv("XDG_CACHE_HOME", m_tmp.filePath("cache").toUtf8());
        /* a medium, so that the dialog says what else keeps the tools out */
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
        QVERIFY(writeVm(vms + "/linux", "#guest linux,desktop=kde\n"));
        QVERIFY(writeVm(vms + "/unknown", ""));
        QVERIFY(writeVm(vms + "/hyperv", "-cpu host,hv-relaxed,hv-time\n"));
        QVERIFY(writeVm(vms + "/windows", "#guest windows\n"));
        QVERIFY(writeVm(vms + "/other", "#guest other\n"));
        /* the system's id, which vitrine wrote for a while */
        QVERIFY(writeVm(vms + "/withid", "#guest linux,id=ubuntu24.04,desktop=gnome\n"));
        QVERIFY(writeVm(vms + "/vga", "#guest linux,desktop=kde\n", false));
        m_store = new VmStore(vms, this);
        QCOMPARE(m_store->vms().size(), 7);
    }

    void offer_data()
    {
        QTest::addColumn<QString>("vm");
        QTest::addColumn<bool>("prompted");
        QTest::addColumn<bool>("installable");

        QTest::newRow("linux") << "linux" << true << true;
        QTest::newRow("not said") << "unknown" << true << true;
        QTest::newRow("not said, hyper-v") << "hyperv" << false << true;
        QTest::newRow("windows") << "windows" << false << false;
        QTest::newRow("another system") << "other" << false << false;
        QTest::newRow("linux, an id") << "withid" << true << true;
        /* the tools' driver is for the 3D card */
        QTest::newRow("linux without 3D") << "vga" << false << true;
    }

    void offer()
    {
        QFETCH(QString, vm);
        QFETCH(bool, prompted);
        QFETCH(bool, installable);
        Vm *v = this->vm(vm);
        QVERIFY(v);

        /* the prompt */
        GuestToolsBanner banner;
        banner.setVm(v);
        QCOMPARE(!banner.isHidden(), prompted);

        /* the dialog: its Install, and why not */
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
        QStringList warnings;
        for (const Banner *b : dialog->findChildren<Banner *>()) {
            for (const QLabel *label : b->findChildren<QLabel *>()) {
                if (b->isVisibleTo(dialog)) {
                    warnings << label->text();
                }
            }
        }
        QCOMPARE(warnings.join('\n').contains("for Fedora Linux guests"), !installable);
        delete dialog;
    }

    /* The system changed in the settings: the prompt follows */
    void systemChanged()
    {
        Vm *v = vm("linux");
        GuestToolsBanner banner;
        banner.setVm(v);
        QVERIFY(!banner.isHidden());

        ArgsFile args = v->args();
        VmConfig::setGuest(args, {"windows", ""});
        QVERIFY(v->save(args));
        QTRY_VERIFY(banner.isHidden());
        VmConfig::setGuest(args, {"linux", "kde"});
        QVERIFY(v->save(args));
        QTRY_VERIFY(!banner.isHidden());
    }
};

QTEST_MAIN(TestGuestOffer)
#include "test_guestoffer.moc"
