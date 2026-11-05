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
    return cloner;
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
