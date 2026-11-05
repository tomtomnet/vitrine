// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef Q_MOC_RUN
#include "gtk/gtkbase.h"
#endif

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include "gtkdisplay.h"

#ifndef Q_MOC_RUN
#include "core/vmcloner.h"
#include "core/vmconfig.h"
#include "core/vmhardware.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "gtk/argspage.h"
#include "gtk/clonedialog.h"
#include "gtk/displaypage.h"
#include "gtk/gtkloop.h"
#include "gtk/machinepage.h"
#include "gtk/newvmdialog.h"
#include "gtk/settingsdialog.h"
#include "gtk/settingspages.h"
#include "gtk/sharespage.h"
#include "gtk/storagepage.h"
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

    /* A new disk: created on Apply, in the VM folder, and in the boot order */
    void storagePage()
    {
        QTemporaryDir dir;
        QVERIFY(addVm(dir.path(), "s", "-name S\n-m 1G\n"));
        VmStore store(dir.path());
        Vm *vm = store.find("s");
        auto *settings = new SettingsDialog(vm, &store, presenter());

        settings->setPage(SettingsDialog::Storage);
        auto *storage = static_cast<StoragePage *>(settings->pageWidget(SettingsDialog::Storage));
        QVERIFY(storage->bootOrder().isEmpty());
        storage->addDisk({}, 1, VmConfig::Disk::Virtio);
        QCOMPARE(storage->bootOrder(), QStringList({"[x] Hard disk: disk.qcow2"}));
        QVERIFY(settings->isModified());
        /* nothing in the folder until applied */
        QVERIFY(!QFileInfo::exists(QDir(vm->dir()).filePath("disk.qcow2")));
        QVERIFY(settings->apply());
        QVERIFY(QFileInfo::exists(QDir(vm->dir()).filePath("disk.qcow2")));
        QCOMPARE(VmConfig::disks(vm->args()).value(0).file, "disk.qcow2");

        /* a drive with a disc, in the firmware's order with the others, then an order: the disc
           first */
        storage->addCdrom("/nonexistent/install.iso");
        QCOMPARE(storage->bootOrder(), QStringList({"[x] Hard disk: disk.qcow2",
                                                    "[x] CD/DVD: install.iso"}));
        storage->moveBoot(1, -1);
        QCOMPARE(storage->bootOrder(), QStringList({"[x] CD/DVD: install.iso",
                                                    "[x] Hard disk: disk.qcow2"}));
        QVERIFY(settings->apply());
        QCOMPARE(VmConfig::disks(vm->args()).size(), 2);
        QVERIFY(VmConfig::hasBootOrder(vm->args()));
        delete settings;
    }

    /* A share, and the shared memory virtiofs needs */
    void sharesPage()
    {
        QTemporaryDir dir, folder;
        QVERIFY(addVm(dir.path(), "f", "-name F\n-m 1G\n"));
        VmStore store(dir.path());
        Vm *vm = store.find("f");
        auto *settings = new SettingsDialog(vm, &store, presenter());

        QVERIFY(!VmConfig::hasSharedMemory(vm->args()));
        settings->setPage(SettingsDialog::SharedFolders);
        auto *shares = static_cast<SharesPage *>(settings->pageWidget(SettingsDialog::SharedFolders));
        VmConfig::Share share;
        share.tag = "docs";
        share.path = folder.path();
        share.readonly = true;
        share.mount = "/mnt/docs";
        shares->setShare(-1, share);
        QVERIFY(settings->isModified());
        QVERIFY(settings->apply());
        const QList<VmConfig::Share> saved = VmConfig::shares(vm->args());
        QCOMPARE(saved.size(), 1);
        QCOMPARE(saved.first().tag, "docs");
        QCOMPARE(saved.first().mount, "/mnt/docs");
        QVERIFY(saved.first().readonly);
        QVERIFY(VmConfig::hasSharedMemory(vm->args()));
        QCOMPARE(VmConfig::memoryMiB(vm->args()), 1024);

        shares->removeShare(0);
        QVERIFY(settings->apply());
        QVERIFY(VmConfig::shares(vm->args()).isEmpty());
        delete settings;
    }

    /* The screen in Vitrine's window, from QEMU's SDL window */
    void displayPage()
    {
        QTemporaryDir dir;
        QVERIFY(addVm(dir.path(), "d", "-name D\n-m 1G\n-vga std\n-display sdl\n"));
        VmStore store(dir.path());
        Vm *vm = store.find("d");
        auto *settings = new SettingsDialog(vm, &store, presenter());

        settings->setPage(SettingsDialog::Display);
        auto *display = static_cast<DisplayPage *>(settings->pageWidget(SettingsDialog::Display));
        QVERIFY(display->displays().contains("dbus"));
        QVERIFY(display->cards().contains("VGA"));
        QVERIFY(!settings->isModified());
        display->chooseDisplay("dbus");
        QVERIFY(settings->isModified());
        QVERIFY(settings->apply());
        QCOMPARE(VmConfig::screen(vm->args()), VmConfig::Screen::Embedded);
        delete settings;
    }

    /* The topology on -smp, the count kept; the model on -cpu, its flags kept */
    void machinePage()
    {
        QTemporaryDir dir;
        QVERIFY(addVm(dir.path(), "m", "-name M\n-m 1G\n-machine q35\n-cpu host,topoext=on\n"
                                       "-smp 4\n"));
        VmStore store(dir.path());
        Vm *vm = store.find("m");
        auto *settings = new SettingsDialog(vm, &store, presenter());

        settings->setPage(SettingsDialog::Machine);
        auto *machine = static_cast<MachinePage *>(settings->pageWidget(SettingsDialog::Machine));
        QVERIFY(!settings->isModified());
        machine->setTopology(true, 1, 2, 2);
        QVERIFY(settings->isModified());
        QVERIFY(settings->apply());
        VmConfig::Cpus cpus = VmConfig::cpus(vm->args());
        QCOMPARE(cpus.count, 4);
        QCOMPARE(cpus.sockets, 1);
        QCOMPARE(cpus.cores, 2);
        QCOMPARE(cpus.threads, 2);

        machine->setModel("max");
        QVERIFY(settings->apply());
        QCOMPARE(VmConfig::cpus(vm->args()).model, "max");
        QVERIFY(vm->args().valueAt(vm->args().indexOf("cpu")).toString().contains("topoext"));
        QCOMPARE(VmConfig::machineType(vm->args()), "q35");
        delete settings;
    }

    /* The text's tags, the Qt highlighter's formats; Insert's line */
    void argumentsPage()
    {
        QTemporaryDir dir;
        QVERIFY(addVm(dir.path(), "a", "-name A\n-m 1G\n"));
        VmStore store(dir.path());
        Vm *vm = store.find("a");
        auto *settings = new SettingsDialog(vm, &store, presenter());

        settings->setPage(SettingsDialog::Arguments);
        auto *args = static_cast<ArgumentsPage *>(settings->pageWidget(SettingsDialog::Arguments));
        args->setText("# a comment\n-name x,debug-threads=on\n#guest linux,desktop=kde\nbogus\n");
        QVERIFY(args->tagsAt(0, 2).contains("comment"));
        QVERIFY(args->tagsAt(1, 1).contains("option") || args->tagsAt(1, 1).contains("unknown"));
        QVERIFY(args->tagsAt(1, 9).contains("key"));
        QVERIFY(!args->tagsAt(1, 6).contains("key"));
        QVERIFY(args->tagsAt(2, 1).contains("directive"));
        QVERIFY(args->tagsAt(2, 15).contains("key"));
        QVERIFY(args->tagsAt(3, 1).contains("invalid"));

        /* after the cursor's line, or on it if blank */
        args->placeCursor(1, 3);
        args->insertLine("-smp 2");
        QCOMPARE(args->text().split('\n').value(2), "-smp 2");
        args->placeCursor(5, 0);
        args->insertLine("-m 2G");
        QCOMPARE(args->text().split('\n').value(5), "-m 2G");
        QVERIFY(settings->isModified());
        args->check();
        QVERIFY(args->problems().join('\n').contains("Line 5"));
        delete settings;
    }

    /* The copy, its end closing the dialog, the clone a VM with its own disk */
    void cloneDialog()
    {
        QTemporaryDir dir;
        QVERIFY(addVm(dir.path(), "src", "-name Source\n-m 1G\n"
                                         "-drive file=disk.qcow2,format=qcow2,if=virtio\n"));
        QVERIFY(createDiskImage(dir.filePath("src/disk.qcow2"), qint64(1) << 20));
        VmStore store(dir.path());
        Vm *vm = store.find("src");
        QObject owner;
        QPointer<VmCloner> started;
        bool closed = false;
        auto *dialog = new CloneDialog(
            &store, vm, nullptr,
            [&](Vm *source, const QString &name) {
                auto *cloner = new VmCloner(&store, &owner);
                started = cloner;
                cloner->start(source, name);
                return cloner;
            },
            presenter());
        Gtk4::connect<void()>(dialog->dialog(), "closed", [&closed]() { closed = true; });

        QCOMPARE(dialog->name(), "Source clone");
        dialog->clone();
        QVERIFY(started);
        QTRY_VERIFY(closed);
        Vm *clone = nullptr;
        for (Vm *each : store.vms()) {
            if (each->name() == "Source clone") {
                clone = each;
            }
        }
        QVERIFY(clone);
        QVERIFY(QFileInfo::exists(QDir(clone->dir()).filePath("disk.qcow2")));
        QVERIFY(clone->runner()->busy().isEmpty());
        QTest::qWait(50);
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
