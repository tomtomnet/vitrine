// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MainWindow's parts that the Qt app has in its status bar and its menus:
 * Preferences, the notices of the running VMs (host tuning, udmabuf,
 * memory), the tasks under way.  The Qt app's MainWindow sets up the same
 * through the same core calls.
 */
#include "gtk/mainwindow.h"

#include <QPointer>

#include <utility>

#include "core/hostsettings.h"
#include "core/tasks.h"
#include "core/vmcloner.h"
#include "core/vmstore.h"
#include "gtk/clonedialog.h"
#include "gtk/gtkloop.h"
#include "gtk/notices.h"
#include "gtk/preferencesdialog.h"
#include "gtk/taskbutton.h"
#include "gtk/vmpage.h"
#include "gtk/widgets.h"

void MainWindow::createParts()
{
    auto *app = G_ACTION_MAP(m_loop->adw());

    /* Preferences, active now: in the place of the inactive one of createActions() */
    addAction(app, "preferences", [this]() { showPreferences(); }, "<Control>comma");

    /*
     * The tasks at the bottom of the list of VMs; the ring alone beside
     * the list's button while the list is hidden.  Then what the running
     * VMs want the user to know, both at the start of the page's header bar.
     */
    m_tasks = new TaskButton(TaskList::instance(), GTK_WINDOW(m_window));
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(adw_overlay_split_view_get_sidebar(m_split)),
                                    m_tasks->widget());
    adw_header_bar_pack_start(m_page->headerBar(), m_tasks->compactWidget());
    m_tasks->setListShown(adw_overlay_split_view_get_show_sidebar(m_split));
    Gtk4::connect<void(GParamSpec *)>(m_split, "notify::show-sidebar", [this](GParamSpec *) {
        if (m_tasks && m_split) {
            m_tasks->setListShown(adw_overlay_split_view_get_show_sidebar(m_split));
        }
    });
    /* a clone's details: the dialog showing the copy, the clone, or why it failed */
    TaskList::instance()->setDetails(Task::Kind::Clone, [this](Task *task) { showCloneTask(task); });
    QObject::connect(TaskList::instance(), &TaskList::removed, m_qt.get(),
                     [this](Task *task) { m_cloneTasks.remove(task); });

    m_notices = new Notices(HostSettings::instance(), m_store, GTK_WINDOW(m_window), presenter());
    adw_header_bar_pack_start(m_page->headerBar(), m_notices->button());
}

void MainWindow::dropParts()
{
    /* nothing of this window shows the tasks' details any more */
    TaskList::instance()->setDetails(Task::Kind::Clone, {});
    m_cloneTasks.clear();
    delete std::exchange(m_tasks, nullptr);
    delete std::exchange(m_notices, nullptr);
}

Notices *MainWindow::notices() const
{
    return m_notices;
}

TaskButton *MainWindow::tasks() const
{
    return m_tasks;
}

PreferencesDialog *MainWindow::showPreferences()
{
    if (!m_window) {
        return nullptr;
    }
    /* one at a time: the one open stays */
    if (m_preferences) {
        return m_preferences;
    }
    auto *dialog = new PreferencesDialog(m_store, presenter(),
                                         [this](const QString &component, const QString &release) {
        showBuilds(component, release);
    });
    m_preferences = dialog;
    Gtk4::connect<void()>(dialog->dialog(), "closed", [this, dialog]() {
        if (m_preferences == dialog) {
            m_preferences = nullptr;
        }
    });
    present(dialog->dialog());
    return dialog;
}

/*
 * The Builds window on @component: gtk-builds' window, which takes this
 * place when its stream is merged; until then, the primary menu's Builds
 */
void MainWindow::showBuilds(const QString &component, const QString &guestRelease)
{
    Q_UNUSED(component);
    Q_UNUSED(guestRelease);
    if (m_loop) {
        g_action_group_activate_action(G_ACTION_GROUP(m_loop->adw()), "builds", nullptr);
    }
}

void MainWindow::followClone(VmCloner *cloner, const QString &sourceId)
{
    const QPointer<Task> task = cloner->task();

    if (!task || !task->isRunning()) {
        /* refused at once: the window said why */
        return;
    }
    m_cloneTasks.insert(task, {sourceId, QString()});
    QObject::connect(cloner, &VmCloner::finished, m_qt.get(),
                     [this, cloner, task](Vm *clone, const QString &) {
        if (!task) {
            return;
        }
        if (m_cloneTasks.contains(task)) {
            m_cloneTasks[task].second = clone ? clone->id() : QString();
        }
        /* its dialog showed how it ended: seen */
        if (m_cloneDialog && m_cloneDialog->cloner() == cloner) {
            task->seen();
        }
    });
}

void MainWindow::showCloneTask(Task *task)
{
    const auto it = m_cloneTasks.constFind(task);

    if (it == m_cloneTasks.cend() || !m_window) {
        return;
    }
    const QString source = it.value().first;
    const QString clone = it.value().second;
    const Vm *vm = m_store->find(source);
    switch (task->state()) {
    case Task::State::Running:
        /* the copy going on, in the dialog again */
        if (vm) {
            select(source);
            cloneVm();
        }
        break;
    case Task::State::Done:
        select(clone);
        break;
    case Task::State::Failed:
        present(Gtk4::alert(vm ? QString("Cannot Clone %1").arg(vm->name()) : task->title(),
                            task->text()));
        break;
    case Task::State::Stopped:
        break;
    }
}
