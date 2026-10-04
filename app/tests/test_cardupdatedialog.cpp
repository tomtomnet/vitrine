// SPDX-License-Identifier: GPL-2.0-or-later
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>
#include <QTimer>

#include "core/cardupdate.h"
#include "core/paths.h"
#include "core/vmstore.h"
#include "ui/cardupdatedialog.h"
#include "ui/vmconsole.h"
#include "ui/vmdetails.h"
#include "ui/vmpane.h"

/*
 * The user's desktop VM: made while Fedora's QEMU stood in for vitrine's
 * (no native context, no vblank, -accel kvm, venus=off), then given
 * drm_native_context=on by hand, with lines of the user's own
 */
static const char kUserVm[] =
    "# The QEMU command line of this VM, one option per line.\n"
    "-name Fedora KDE,debug-threads=on\n"
    "#guest linux,desktop=kde\n"
    "\n"
    "# System\n"
    "-machine q35,memory-backend=mem,dump-guest-core=off\n"
    "-accel kvm\n"
    "-cpu host\n"
    "-smp 4\n"
    "-object memory-backend-memfd,id=mem,size=4G,share=on\n"
    "\n"
    "# Display\n"
    "-vga none\n"
    "-device virtio-gpu-gl-pci,hostmem=4G,blob=on,venus=off,drm_native_context=on\n"
    "-display dbus,p2p=yes,gl=on\n"
    "# mine: a serial console\n"
    "-serial file:serial.log\n";

static const char kOldCard[] =
    "-device virtio-gpu-gl-pci,hostmem=4G,blob=on,venus=off,drm_native_context=on";
static const char kNewCard[] =
    "-device virtio-gpu-gl-pci,hostmem=4G,blob=on,drm_native_context=on,x-host-vblank=on,"
    "x-vblank-lead=3000,x-vblank-lead-auto=on";

/* kUserVm updated: the card, and -accel */
static QString updated(const QString &text)
{
    return QString(text)
        .replace(QString(kOldCard) + "\n", QString(kNewCard) + "\n")
        .replace("-accel kvm\n", "-accel kvm,honor-guest-pat=on\n");
}

/*
 * The banner on the Details tab and the Console's home page, the dialog
 * and what it saves, Don't Ask Again and its undo, and the settings'
 * unapplied changes, offscreen
 */
class TestCardUpdateDialog : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;

    static bool write(const QString &path, const QByteArray &text, bool executable = false)
    {
        QFile f(path);

        if (!QDir().mkpath(QFileInfo(path).path()) || !f.open(QIODevice::WriteOnly) ||
            f.write(text) < 0) {
            return false;
        }
        f.close();
        return !executable ||
               f.setPermissions(QFileDevice::ReadOwner | QFileDevice::ExeOwner);
    }

    static QString read(const QString &path)
    {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    }

    /* A VM folder named @id holding @text */
    QString vmDir(const QString &id, const QString &text)
    {
        const QString dir = m_tmp.filePath("vms/" + id);
        return write(dir + "/vm.args", text.toUtf8()) ? dir : QString();
    }

    static CardUpdateBanner *bannerIn(QWidget *w)
    {
        return w->findChild<CardUpdateBanner *>();
    }

    static QPushButton *button(QWidget *w, const QString &name)
    {
        return w->findChild<QPushButton *>(name);
    }

    /* The text of @banner's message */
    static QString bannerText(CardUpdateBanner *banner)
    {
        QString text;
        for (QLabel *label : banner->findChildren<QLabel *>()) {
            text += label->text();
        }
        return text;
    }

    /* The dialog open over @top, if any */
    static CardUpdateDialog *dialogOver(QWidget *top)
    {
        for (CardUpdateDialog *d : top->findChildren<CardUpdateDialog *>()) {
            if (d->isVisible()) {
                return d;
            }
        }
        return nullptr;
    }

    /* Clicks the button of the next message box whose text starts with @text */
    static void answerNextBox(const QString &text)
    {
        QTimer::singleShot(0, [text]() {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(box);
            for (QAbstractButton *b : box->buttons()) {
                if (b->text().remove('&').startsWith(text)) {
                    b->click();
                    return;
                }
            }
            QFAIL("no such button");
        });
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        if (Paths::hostArch() != "x86_64") {
            QSKIP("the 3D card of Linux on a PC");
        }
        /* settings and stack in here: vitrine's QEMU not built, none chosen */
        qputenv("XDG_DATA_HOME", m_tmp.filePath("data").toUtf8());
        qputenv("XDG_CONFIG_HOME", m_tmp.filePath("config").toUtf8());
        qputenv("XDG_CACHE_HOME", m_tmp.filePath("cache").toUtf8());
        /* the system's QEMU meanwhile: Fedora's, without native context */
        QVERIFY(write(m_tmp.filePath("bin/" + Paths::qemuSystemName()),
                      "#!/bin/sh\n"
                      "case \"$1 $2\" in\n"
                      "'-device virtio-gpu-gl-pci,help')\n"
                      "  echo 'virtio-gpu-gl-pci options:'\n"
                      "  echo '  blob=<bool>'\n"
                      "  echo '  hostmem=<size>'\n"
                      "  echo '  venus=<bool>' ;;\n"
                      "'-object kvm-accel,help')\n"
                      "  echo 'kvm-accel options:'\n"
                      "  echo '  kernel-irqchip=<on|off|split>' ;;\n"
                      "esac\n",
                      true));
        qputenv("PATH", m_tmp.filePath("bin").toUtf8());
        Paths::setQemuBinary({});
        QVERIFY(Paths::stackQemu().isEmpty());
    }

    void cleanup()
    {
        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (qobject_cast<CardUpdateDialog *>(w)) {
                w->close();
            }
        }
    }

    /* Details: the banner, the dialog, and vm.args after Apply */
    void detailsApplies()
    {
        const QString dir = vmDir("desktop", kUserVm);
        Vm vm(dir);
        VmDetails details;

        details.setVm(&vm);
        CardUpdateBanner *banner = bannerIn(&details);
        QVERIFY(banner);
        QVERIFY(!banner->isHidden());
        QVERIFY(!banner->isDeclined());
        QVERIFY2(bannerText(banner).contains("This VM's 3D card lacks Vitrine's settings "
                                             "for native context and smooth frames."),
                 qPrintable(bannerText(banner)));
        QCOMPARE(button(banner, "updateCard")->text(), "Update…");
        QCOMPARE(button(banner, "dontAskAgain")->text(), "Don't Ask Again");

        button(banner, "updateCard")->click();
        CardUpdateDialog *dialog = dialogOver(&details);
        QVERIFY(dialog);
        QCOMPARE(dialog->windowTitle(), "Update the 3D Card");
        /* in plain words, then the lines as they are and as they will be */
        const QString list = dialog->findChild<QLabel *>("changes")->text();
        for (const CardUpdate::Change &c : CardUpdate::changes(vm.args())) {
            QVERIFY2(list.contains(CardUpdate::describe(c).toHtmlEscaped()), qPrintable(list));
        }
        QVERIFY(list.contains("venus=off"));
        QVERIFY(list.contains("honor-guest-pat=on"));
        QCOMPARE(dialog->findChild<QPlainTextEdit *>("before")->toPlainText(),
                 QString("-accel kvm\n") + kOldCard);
        QCOMPARE(dialog->findChild<QPlainTextEdit *>("after")->toPlainText(),
                 QString("-accel kvm,honor-guest-pat=on\n") + kNewCard);
        const QString notes = dialog->findChild<QLabel *>("notes")->text();
        QVERIFY(!notes.contains("running"));
        /* it has native context already, which the system's QEMU lacks */
        QVERIFY(!notes.contains("Build QEMU"));
        QCOMPARE(read(dir + "/vm.args"), kUserVm);

        button(dialog, "apply")->click();
        QVERIFY(!dialog->isVisible());
        QCOMPARE(read(dir + "/vm.args"), updated(kUserVm));
        QCOMPARE(vm.args().toText(), updated(kUserVm));
        QVERIFY(banner->isHidden());
        QVERIFY(!CardUpdate::isDeclined(vm.id()));
    }

    /* Cancel changes nothing; an edit by hand meanwhile is shown, not applied unseen */
    void cancelAndOutsideEdits()
    {
        const QString dir = vmDir("edited-meanwhile", kUserVm);
        Vm vm(dir);
        VmDetails details;

        details.setVm(&vm);
        button(bannerIn(&details), "updateCard")->click();
        CardUpdateDialog *dialog = dialogOver(&details);
        QVERIFY(dialog);
        dialog->reject();
        QCOMPARE(read(dir + "/vm.args"), kUserVm);
        QVERIFY(!bannerIn(&details)->isHidden());

        button(bannerIn(&details), "updateCard")->click();
        dialog = dialogOver(&details);
        QVERIFY(dialog);
        /* the user gives it the vblank by hand while the dialog is open */
        const QString byHand = QString(kUserVm).replace(
            kOldCard, QString(kOldCard) + ",x-host-vblank=off");
        QVERIFY(vm.save(ArgsFile::parse(byHand)));
        button(dialog, "apply")->click();
        QVERIFY(dialog->isVisible());
        QCOMPARE(read(dir + "/vm.args"), byHand);
        QVERIFY(dialog->findChild<QLabel *>("notes")->text().contains("changed meanwhile"));
        QCOMPARE(dialog->findChild<QPlainTextEdit *>("after")->toPlainText(),
                 "-accel kvm,honor-guest-pat=on\n"
                 "-device virtio-gpu-gl-pci,hostmem=4G,blob=on,drm_native_context=on,"
                 "x-host-vblank=off");
        button(dialog, "apply")->click();
        QVERIFY(!dialog->isVisible());
        QCOMPARE(read(dir + "/vm.args"),
                 QString(byHand)
                     .replace("venus=off,", "")
                     .replace("-accel kvm\n", "-accel kvm,honor-guest-pat=on\n"));
    }

    /* Don't Ask Again: for this VM, everywhere, until Details offers it again */
    void dontAskAgain()
    {
        const QString dir = vmDir("declined", kUserVm);
        Vm vm(dir);
        Vm other(vmDir("other", kUserVm));
        VmDetails details;
        VmDetails otherDetails;
        VmConsole console(&vm);
        auto *text = details.findChild<QTextBrowser *>("details");

        details.setVm(&vm);
        otherDetails.setVm(&other);
        QVERIFY(text);
        QVERIFY(!bannerIn(&console)->isHidden());
        QVERIFY(!text->toHtml().contains("Offer again"));

        button(bannerIn(&details), "dontAskAgain")->click();
        QVERIFY(bannerIn(&details)->isHidden());
        QVERIFY(bannerIn(&console)->isHidden());
        QVERIFY(!bannerIn(&otherDetails)->isHidden());
        QVERIFY(CardUpdate::isDeclined("declined"));
        QVERIFY(!CardUpdate::isDeclined("other"));
        /* vm.args stays as it is */
        QCOMPARE(read(dir + "/vm.args"), kUserVm);
        QVERIFY(text->toHtml().contains("Offer again"));

        /* it stays so for new pages, and after a change of the arguments */
        {
            VmDetails again;
            again.setVm(&vm);
            QVERIFY(bannerIn(&again)->isHidden());
            QVERIFY(bannerIn(&again)->isDeclined());
        }
        QVERIFY(vm.save(ArgsFile::parse(QString(kUserVm).replace("-smp 4", "-smp 2"))));
        QVERIFY(bannerIn(&details)->isHidden());

        /* the link on the Details tab undoes it */
        emit text->anchorClicked(QUrl("vitrine:offer-card-update"));
        QVERIFY(!CardUpdate::isDeclined("declined"));
        QVERIFY(!bannerIn(&details)->isHidden());
        QVERIFY(!bannerIn(&console)->isHidden());
        QVERIFY(!text->toHtml().contains("Offer again"));
    }

    /* The settings' unapplied changes: applied or kept first, never lost or overwritten */
    void unappliedSettings()
    {
        const QString dir = vmDir("settings", kUserVm);
        Vm vm(dir);
        VmPane pane;

        pane.setVm(&vm);
        pane.setTab(VmPane::Settings);
        pane.setPage(VmPane::General);
        auto *name = pane.findChild<QLineEdit *>("name");
        QVERIFY(name);
        name->setText("Renamed");
        QVERIFY(pane.isModified());
        QPushButton *update = button(bannerIn(pane.details()), "updateCard");

        /* Cancel: nothing opens, the edits wait */
        answerNextBox("Cancel");
        update->click();
        QVERIFY(!dialogOver(&pane));
        QVERIFY(pane.isModified());
        QCOMPARE(read(dir + "/vm.args"), kUserVm);

        /* Apply: the edits saved, then the dialog for the arguments as saved */
        answerNextBox("Apply");
        update->click();
        QVERIFY(!pane.isModified());
        const QString renamed = QString(kUserVm).replace("-name Fedora KDE,", "-name Renamed,");
        QCOMPARE(read(dir + "/vm.args"), renamed);
        CardUpdateDialog *dialog = dialogOver(&pane);
        QVERIFY(dialog);
        button(dialog, "apply")->click();
        QCOMPARE(read(dir + "/vm.args"), updated(renamed));
        /* the pages show what was saved, with nothing to apply */
        QVERIFY(!pane.isModified());
        QCOMPARE(name->text(), "Renamed");
    }

    /* A VM without native context: once updated, it waits for vitrine's QEMU */
    void needsVitrinesQemu()
    {
        const QString fedora = QString(kUserVm).replace(",drm_native_context=on", "");
        Vm vm(vmDir("fedora", fedora));
        VmDetails details;

        details.setVm(&vm);
        button(bannerIn(&details), "updateCard")->click();
        CardUpdateDialog *dialog = dialogOver(&details);
        QVERIFY(dialog);
        QVERIFY2(dialog->findChild<QLabel *>("notes")->text().contains("File &gt; Build QEMU"),
                 qPrintable(dialog->findChild<QLabel *>("notes")->text()));
        QVERIFY(dialog->findChild<QLabel *>("changes")->text().contains("drm_native_context=on"));
    }

    /* Nothing for a VM that has it all, or that is no Linux one */
    void noOffer()
    {
        Vm current(vmDir("current", updated(kUserVm)));
        Vm windows(vmDir("windows", QString(kUserVm).replace("#guest linux,desktop=kde",
                                                             "#guest windows")));
        VmDetails details;

        details.setVm(&current);
        QVERIFY(bannerIn(&details)->isHidden());
        QVERIFY(!bannerIn(&details)->isDeclined());
        details.setVm(&windows);
        QVERIFY(bannerIn(&details)->isHidden());
        details.setVm(nullptr);
        QVERIFY(bannerIn(&details)->isHidden());
    }
};

QTEST_MAIN(TestCardUpdateDialog)
#include "test_cardupdatedialog.moc"
