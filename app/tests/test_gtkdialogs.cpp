// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef Q_MOC_RUN
#include "gtk/gtkbase.h"
#endif

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include "gtkdisplay.h"

#ifndef Q_MOC_RUN
#include "core/vmconfig.h"
#include "core/vmhardware.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "gtk/argspage.h"
#include "gtk/gtkloop.h"
#include "gtk/newvmdialog.h"
#include "gtk/settingsdialog.h"
#include "gtk/settingspages.h"
#include "gtk/textdialog.h"
#include "ui/uiconfig.h"
#endif

/*
 * The GTK app's dialogs, on a display no one sees: New VM makes what the
 * template makes, with the memory, processors and disk asked for; the
 * settings edit one copy of the arguments, page after page, which Apply
 * saves and Discard drops, and untouched pages write nothing; the command
 * line is the runner's.
 */
class TestGtkDialogs : public QObject
{
    Q_OBJECT

    QProcess m_broadway;
    GTestDBus *m_bus = nullptr;
    GtkLoop *m_loop = nullptr;
    /* the headings of the alerts the dialogs presented */
    QStringList m_alerts;

    Gtk4::Dialog::Presenter presenter()
    {
        return [this](AdwDialog *dialog) {
            g_object_ref_sink(dialog);
            if (ADW_IS_ALERT_DIALOG(dialog)) {
                m_alerts << QString::fromUtf8(adw_alert_dialog_get_heading(ADW_ALERT_DIALOG(dialog)));
            }
            g_object_unref(dialog);
        };
    }

    /* A VM folder @id holding @args */
    static bool addVm(const QString &dir, const QString &id, const QString &args)
    {
        QFile file(dir + "/" + id + "/vm.args");
        return QDir().mkpath(dir + "/" + id) && file.open(QIODevice::WriteOnly) &&
               file.write(args.toUtf8()) > 0;
    }

    /* The widget named @name under @widget, depth first */
    static GtkWidget *named(GtkWidget *widget, const char *name)
    {
        if (g_strcmp0(gtk_widget_get_name(widget), name) == 0) {
            return widget;
        }
        for (GtkWidget *child = gtk_widget_get_first_child(widget); child;
             child = gtk_widget_get_next_sibling(child)) {
            if (GtkWidget *found = named(child, name)) {
                return found;
            }
        }
        return nullptr;
    }

private slots:
    void initTestCase()
    {
        QString error;

        QStandardPaths::setTestModeEnabled(true);
        m_bus = GtkDisplay::up(m_broadway);
        if (!m_bus) {
            QSKIP("gtk4-broadwayd is not installed: no display to test on but the desktop's");
        }
        m_loop = new GtkLoop("org.vitrine.Vitrine");
        QCOMPARE(m_loop->start(&error), GtkLoop::Start::Primary);
    }

    void cleanupTestCase()
    {
        delete m_loop;
        GtkDisplay::down(m_broadway, m_bus);
    }

    void init()
    {
        m_alerts.clear();
    }

    /* What the user chose, on the template's VM */
    void newVm()
    {
        QTemporaryDir dir;
        VmStore store(dir.path());
        Vm *made = nullptr;
        bool settings = true;
        auto *dialog = new NewVmDialog(&store, presenter(),
                                       [&](Vm *vm, const QStringList &, bool open) {
                                           made = vm;
                                           settings = open;
                                       });

        /* a name first, then a disk image if it uses one */
        QVERIFY(!dialog->canCreate());
        dialog->setName("  Fedora  ");
        QVERIFY(dialog->canCreate());
        dialog->setDiskImage({});
        dialog->setNewDisk(2);
        dialog->setMemoryMiB(1536);
        dialog->setCpus(2);
        dialog->setOpenSettings(false);
        QCOMPARE(dialog->memoryMiB(), 1536);
        QCOMPARE(dialog->diskGiB(), 2);

        Vm *vm = dialog->create();
        QVERIFY2(vm, qPrintable(m_alerts.join(", ")));
        QCOMPARE(made, vm);
        QVERIFY(!settings);
        QCOMPARE(vm->name(), "Fedora");
        QCOMPARE(store.vms().size(), 1);
        QCOMPARE(VmConfig::memoryMiB(vm->args()), 1536);
        QCOMPARE(VmConfig::cpus(vm->args()).count, 2);
        QCOMPARE(VmConfig::guest(vm->args()).desktop, "kde");
        const QList<VmConfig::Disk> disks = VmConfig::disks(vm->args());
        QCOMPARE(disks.size(), 1);
        QCOMPARE(disks.first().file, "disk.qcow2");
        QVERIFY(QFileInfo::exists(QDir(vm->dir()).filePath("disk.qcow2")));
        /* the guest's SSH on a port of this computer */
        QVERIFY(VmConfig::network(vm->args()).sshPort > 0);
        /* closed, it goes once GLib's loop runs */
        QVERIFY(dialog->isClosed());
        QTest::qWait(50);
    }

    /* An existing image instead of a new disk, and another system */
    void newVmFromImage()
    {
        QTemporaryDir dir, images;
        const QString image = images.filePath("cloud.qcow2");
        QVERIFY(createDiskImage(image, qint64(1) << 30));
        VmStore store(dir.path());
        auto *dialog = new NewVmDialog(&store, presenter(), {});

        dialog->setName("Windows");
        dialog->setSystem(3);
        dialog->setDiskImage(image);
        QVERIFY(dialog->canCreate());
        Vm *vm = dialog->create();
        QVERIFY2(vm, qPrintable(m_alerts.join(", ")));
        QCOMPARE(VmConfig::guest(vm->args()).os, "windows");
        QCOMPARE(VmConfig::disks(vm->args()).value(0).file, image);
        QVERIFY(!QFileInfo::exists(QDir(vm->dir()).filePath("disk.qcow2")));
        QTest::qWait(50);
    }

    /* Changes go from page to page in one copy, which Apply saves */
    void settingsApply()
    {
        QTemporaryDir dir;
        QVERIFY(addVm(dir.path(), "a", "-name Alpha\n-m 1G\n-smp 2\n"));
        VmStore store(dir.path());
        Vm *vm = store.find("a");
        auto *settings = new SettingsDialog(vm, &store, presenter());

        settings->setPage(SettingsDialog::Hardware);
        QCOMPARE(settings->page(), SettingsDialog::Hardware);
        QVERIFY(!settings->isModified());
        GtkWidget *memory = named(settings->pageWidget(SettingsDialog::Hardware)->widget(), "memory");
        QVERIFY(memory);
        QCOMPARE(int(adw_spin_row_get_value(ADW_SPIN_ROW(memory))), 1024);
        adw_spin_row_set_value(ADW_SPIN_ROW(memory), 2048);
        QVERIFY(settings->isModified());
        /* not saved yet, but the Arguments page has it */
        QCOMPARE(VmConfig::memoryMiB(vm->args()), 1024);
        settings->setPage(SettingsDialog::Arguments);
        auto *args = static_cast<ArgumentsPage *>(settings->pageWidget(SettingsDialog::Arguments));
        QCOMPARE(VmConfig::memoryMiB(ArgsFile::parse(args->text())), 2048);
        QVERIFY(settings->isModified());

        QVERIFY(settings->apply());
        QVERIFY(!settings->isModified());
        QCOMPARE(VmConfig::memoryMiB(vm->args()), 2048);
        QCOMPARE(VmConfig::cpus(vm->args()).count, 2);
        delete settings;
    }

    void settingsDiscard()
    {
        QTemporaryDir dir;
        QVERIFY(addVm(dir.path(), "a", "-name Alpha\n-m 1G\n"));
        VmStore store(dir.path());
        Vm *vm = store.find("a");
        const QString before = vm->args().toText();
        auto *settings = new SettingsDialog(vm, &store, presenter());

        settings->setPage(SettingsDialog::Arguments);
        auto *args = static_cast<ArgumentsPage *>(settings->pageWidget(SettingsDialog::Arguments));
        args->setText(args->text() + "-device virtio-rng-pci\n");
        QVERIFY(settings->isModified());
        settings->discard();
        QVERIFY(!settings->isModified());
        QVERIFY(!args->text().contains("virtio-rng-pci"));
        QCOMPARE(vm->args().toText(), before);

        /* an answer to the question on closing: Discard drops them, Apply saves them */
        args->setText(args->text() + "-device virtio-rng-pci\n");
        bool asked = false;
        settings->confirmChanges("Apply them before closing?", [&](bool) { asked = true; });
        QCOMPARE(m_alerts, QStringList({"Unsaved Changes"}));
        QVERIFY(!asked);
        delete settings;
        QCOMPARE(vm->args().toText(), before);
    }

    /* Pages entered and left with no change write nothing, even on VMs written by hand */
    void settingsUntouched()
    {
        QTemporaryDir dir;
        const QString text = "# by hand\n-name Hand\n-machine q35\n-m 3000\n-smp 3\n"
                             "-display sdl\n-vga std\n-nic user\n-drive file=x.img\n";
        QVERIFY(addVm(dir.path(), "h", text));
        VmStore store(dir.path());
        Vm *vm = store.find("h");
        auto *settings = new SettingsDialog(vm, &store, presenter());

        for (int page = SettingsDialog::General; page <= SettingsDialog::Arguments; page++) {
            if (settings->pageWidget(SettingsDialog::Page(page))) {
                settings->setPage(SettingsDialog::Page(page));
                QVERIFY2(!settings->isModified(), SettingsDialog::pageKey(SettingsDialog::Page(page)));
            }
        }
        QVERIFY(settings->apply());
        QCOMPARE(vm->args().toText(), ArgsFile::parse(text).toText());
        delete settings;
    }

    /* The page names of the settings file, the Qt app's */
    void pageKeys()
    {
        QCOMPARE(SettingsDialog::pageKey(SettingsDialog::SharedFolders), "shares");
        QCOMPARE(SettingsDialog::pageOf("arguments"), SettingsDialog::Arguments);
        QCOMPARE(SettingsDialog::pageOf("nonsense"), SettingsDialog::General);
    }

    void commandLine()
    {
        QTemporaryDir dir;
        QVERIFY(addVm(dir.path(), "a", "-name Alpha\n-m 1G\n"));
        VmStore store(dir.path());
        Vm *vm = store.find("a");
        const QString expected = UiConfig::commandText(vm->runner()->commandLine(vm->args()),
                                                       vm->dir(), VmRunner::environment(vm->args()));
        auto *dialog = new TextDialog("Command Line", vm->name(), expected, presenter());

        QCOMPARE(dialog->text(), expected);
        GtkWidget *view = named(adw_dialog_get_child(dialog->dialog()), "text");
        QVERIFY(view);
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
        GtkTextIter start, end;
        gtk_text_buffer_get_bounds(buffer, &start, &end);
        char *shown = gtk_text_buffer_get_text(buffer, &start, &end, false);
        QCOMPARE(QString::fromUtf8(shown), expected);
        g_free(shown);
        QVERIFY(expected.contains("-m 1G") || expected.contains("1G"));
        delete dialog;
    }
};

int main(int argc, char **argv)
{
    QTemporaryDir runtime(QDir::tempPath() + "/vitrine-gtkdlg-XXXXXX");
    if (!GtkDisplay::prepare(runtime)) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    TestGtkDialogs test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_gtkdialogs.moc"
