// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The window's dialogs: New VM, Import VM, Settings, Clone, Snapshots,
 * the log and the command line, which its actions open.  Kept apart from
 * mainwindow.cpp, the window itself.
 */
#include "gtk/mainwindow.h"

#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "gtk/newvmdialog.h"
#include "gtk/settingsdialog.h"
#include "gtk/textdialog.h"
#include "gtk/widgets.h"
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
