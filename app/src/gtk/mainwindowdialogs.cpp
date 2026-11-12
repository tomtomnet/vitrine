// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The window's dialogs: New VM, Import VM, Settings, Clone, Snapshots,
 * the log and the command line, which its actions open.  Kept apart from
 * mainwindow.cpp, the window itself.
 */
#include "gtk/mainwindow.h"

#include "core/tasks.h"
#include "core/vmcloner.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "gtk/clonedialog.h"
#include "gtk/importdialog.h"
#include "gtk/logdialog.h"
#include "gtk/newvmdialog.h"
#include "gtk/settingsdialog.h"
#include "gtk/settingsupdates.h"
#include "gtk/snapshotsdialog.h"
#include "gtk/textdialog.h"
#include "gtk/widgets.h"
#include "ui/qemudocs.h"
#include "ui/uiconfig.h"

std::function<void(AdwDialog *)> MainWindow::presenter()
{
    return [this](AdwDialog *dialog) { present(dialog); };
}

void MainWindow::newVm()
{
    if (!m_window) {
        return;
    }
    auto *dialog = new NewVmDialog(
        m_store, presenter(), [this](Vm *vm, const QStringList &warnings, bool settings) {
            select(vm->id());
            if (!warnings.isEmpty()) {
                present(Gtk4::alert("VM Created", warnings.join("\n\n")));
            }
            if (settings) {
                openSettings(vm, SettingsDialog::General);
            }
        });
    present(dialog->dialog());
}

void MainWindow::importVm()
{
    if (!m_window) {
        return;
    }
    /* the documentation of the preferred QEMU, if loaded, tells which options take a value */
    auto *dialog = new ImportDialog(m_store, QemuDocs::preferred()->info(), presenter(),
                                    [this](Vm *vm) { select(vm->id()); });
    present(dialog->dialog());
}

void MainWindow::showCommandLine()
{
    Vm *vm = current();

    if (!vm || !m_window) {
        return;
    }
    const QString text = UiConfig::commandText(vm->runner()->commandLine(vm->args()), vm->dir(),
                                               VmRunner::environment(vm->args()));
    auto *dialog = new TextDialog("Command Line", vm->name(), text, presenter());
    present(dialog->dialog());
}

void MainWindow::openSettings(Vm *vm, int page)
{
    if (!vm || !m_window) {
        return;
    }
    /* one at a time: the one open shows, on the page asked for */
    if (m_settings) {
        if (page >= 0 && m_settings->vm() == vm) {
            m_settings->setPage(SettingsDialog::Page(page));
        }
        return;
    }
    select(vm->id());
    auto *dialog = new SettingsDialog(
        vm, m_store, presenter(),
        [this](Vm *vm, const QList<FirmwareFiles::File> &files, const QString &accept,
               std::function<void(bool)> done) {
            resetFirmware(vm, files, accept, std::move(done));
        });
    if (page >= 0) {
        dialog->setPage(SettingsDialog::Page(page));
    }
    m_settings = dialog;
    Gtk4::connect<void()>(dialog->dialog(), "closed", [this, dialog]() {
        if (m_settings == dialog) {
            m_settings = nullptr;
        }
    });
    present(dialog->dialog());
}

void MainWindow::settingsShortcut()
{
    if (m_settings) {
        if (m_settings->isModified()) {
            m_settings->apply();
        }
        return;
    }
    openSettings(current());
}

void MainWindow::cloneVm()
{
    Vm *vm = current();

    if (!vm || !m_window || vm->runner()->isActive()) {
        return;
    }
    /* its copy going on, if any: that one shows */
    VmCloner *running = m_clones.value(vm->id());
    auto *dialog = new CloneDialog(
        m_store, vm, running && running->isRunning() ? running : nullptr,
        [this](Vm *source, const QString &name) { return startClone(source, name); }, presenter());
    m_cloneDialog = dialog;
    Gtk4::connect<void()>(dialog->dialog(), "closed", [this, dialog]() {
        if (m_cloneDialog == dialog) {
            m_cloneDialog = nullptr;
        }
    });
    present(dialog->dialog());
}

VmCloner *MainWindow::startClone(Vm *source, const QString &name)
{
    auto *cloner = new VmCloner(m_store, m_qt.get());
    const QString id = source->id();
    const QString sourceName = source->name();

    m_clones.insert(id, cloner);
    QObject::connect(cloner, &VmCloner::finished, m_qt.get(),
                     [this, cloner, id, sourceName](Vm *clone, const QString &error) {
        /* the dialog shows the end of the copy it showed; else the window says it */
        const bool shown = m_cloneDialog && m_cloneDialog->cloner() == cloner;
        const Task *task = cloner->task();

        if (clone) {
            if (shown) {
                select(clone->id());
            } else {
                notify(QString("The clone of %1 is ready: %2").arg(sourceName, clone->name()));
            }
        } else if (!shown && !(task && task->state() == Task::State::Stopped)) {
            present(Gtk4::alert(QString("Cannot Clone %1").arg(sourceName), error));
        }
        if (m_clones.value(id) == cloner) {
            m_clones.remove(id);
        }
        cloner->deleteLater();
        updateActions();
    });
    cloner->start(source, name);
    /* its task's details: this window's (mainwindowparts.cpp) */
    followClone(cloner, id);
    return cloner;
}

void MainWindow::confirmSettings(Vm *vm, std::function<void(bool go)> go, const QString &question)
{
    if (m_settings && m_settings->vm() == vm && m_settings->isModified()) {
        m_settings->confirmChanges(question, std::move(go));
        return;
    }
    go(true);
}

void MainWindow::reviewSettings(Vm *vm)
{
    if (!vm || !m_window) {
        return;
    }
    auto *dialog = new SettingsUpdatesDialog(
        vm, presenter(), [this](Vm *vm, std::function<void(bool go)> go) {
            confirmSettings(vm, std::move(go), "Apply them before the newer settings?");
        });
    present(dialog->dialog());
}

void MainWindow::undoSettingsUpdate(Vm *vm)
{
    if (!vm || !m_window) {
        return;
    }
    const QList<SettingsUpdates::Record> records = SettingsUpdatesMonitor::of(vm)->records();
    if (records.isEmpty()) {
        return;
    }
    const SettingsUpdates::Record last = records.last();
    QStringList titles;
    for (const SettingsUpdates::Record::Applied &applied : last.changes) {
        titles << SettingsUpdates::title(applied.id);
    }
    AdwDialog *alert = Gtk4::alert(
        "Undo the Update?",
        QString("These settings go back as they were before the update of %1: %2. The other "
                "lines of vm.args stay as they are now.")
            .arg(last.time.toString("d MMMM, HH:mm"), titles.join(", ")),
        {{"cancel", "_Cancel"}, {"undo", "_Undo"}}, "undo");
    QPointer<Vm> guard(vm);
    Gtk4::onResponse(alert, [this, guard, last](const QString &response) {
        if (response != "undo" || !guard) {
            return;
        }
        confirmSettings(guard, [this, guard, last](bool go) {
            QStringList failed;
            QString error;

            if (!go || !guard) {
                return;
            }
            if (!SettingsUpdatesMonitor::of(guard)->undo(&failed, &error)) {
                present(Gtk4::alert("Cannot Undo the Update", error));
            } else if (!failed.isEmpty()) {
                present(Gtk4::alert(
                    "Some Settings Stay as They Are",
                    QString("Their lines changed since the update, and were left as they are: "
                            "%1. vm.args as it was before the update is %2, in the VM's folder.")
                        .arg(failed.join(", "), last.backup)));
            } else {
                notify(QString("%1: the update of its settings is undone").arg(guard->name()));
            }
        }, "Apply them before undoing the update?");
    });
    present(alert);
}

void MainWindow::showSnapshots()
{
    Vm *vm = current();

    if (!vm || !m_window) {
        return;
    }
    const QString id = vm->id();
    auto *dialog = new SnapshotsDialog(vm, presenter(), [this, id](const QString &snapshot) {
        select(id);
        start(snapshot);
    });
    present(dialog->dialog());
}

void MainWindow::showLogOf(Vm *vm)
{
    if (!vm || !m_window) {
        return;
    }
    auto *dialog = new LogDialog(vm->name(), vm->runner()->logPath(), presenter());
    present(dialog->dialog());
}
