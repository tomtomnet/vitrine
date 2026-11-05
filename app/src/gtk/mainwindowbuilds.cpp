// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/mainwindowbuilds.h"

#include <QTimer>

#include <utility>

#include "core/buildmanager.h"
#include "core/guesttools.h"
#include "core/guesttoolsbuilder.h"
#include "core/hostkde.h"
#include "core/paths.h"
#include "core/stackbuilder.h"
#include "core/tasks.h"
#include "core/vmstore.h"
#include "gtk/appbanners.h"
#include "gtk/buildswindow.h"
#include "gtk/gtkloop.h"
#include "gtk/guesttoolsdialog.h"
#include "gtk/guesttoolsinstall.h"
#include "gtk/guesttoolsstatus.h"
#include "gtk/hostkdealerts.h"
#include "gtk/mainwindow.h"
#include "gtk/updatenotifier.h"
#include "gtk/vmpage.h"
#include "gtk/widgets.h"
#include "ui/hostkdetext.h"
#include "ui/qemudocs.h"

using Gtk4::utf8;

void MainWindow::showBuilds(const QString &component, const QString &guestRelease)
{
    if (m_builds) {
        m_builds->showBuilds(component, guestRelease);
    }
}

MainWindowBuilds::MainWindowBuilds(MainWindow *window)
    : m_window(window), m_qt(std::make_unique<QObject>()), m_banners(std::make_unique<AppBanners>()),
      m_toolsStatus(std::make_unique<GuestToolsStatus>()), m_qemuBefore(Paths::qemuBinary())
{
    auto *win = G_ACTION_MAP(window->m_window);

    window->addAction(win, "install-guest-tools", [this]() { installGuestTools(m_window->current()); });

    /* new commits of Vitrine: the primary menu's buttons say so, Check for Updates… tells */
    m_updates = std::make_unique<UpdateNotifier>();
    m_updates->present = [this](AdwDialog *dialog) { m_window->present(dialog); };
    m_updates->notify = [this](const QString &text) { m_window->notify(text, 15000); };
    window->addAction(G_ACTION_MAP(window->m_loop->adw()), "check-updates",
                      [this]() { m_updates->checkNow(); });
    std::function<void(GtkWidget *)> findMenus = [&](GtkWidget *widget) {
        if (GTK_IS_MENU_BUTTON(widget) &&
            g_strcmp0(gtk_menu_button_get_icon_name(GTK_MENU_BUTTON(widget)), "open-menu-symbolic") == 0) {
            m_updates->addMenuButton(widget);
            return;
        }
        for (GtkWidget *child = gtk_widget_get_first_child(widget); child;
             child = gtk_widget_get_next_sibling(child)) {
            findMenus(child);
        }
    };
    findMenus(window->m_window);

    /* the banners over the views, and over the empty state; not over a VM's screen */
    m_banners->showBuilds = [this](const QString &component) { showBuilds(component, {}); };
    m_banners->revert = [this](const QStringList &ids) {
        HostKdeAlerts::revert(m_window->presenter(), ids);
    };
    m_banners->addTo(ADW_TOOLBAR_VIEW(window->m_page->widget()));
    if (GtkWidget *empty = gtk_stack_get_child_by_name(window->m_main, "empty")) {
        m_banners->addTo(ADW_TOOLBAR_VIEW(empty));
    }
    window->m_page->screenShownChanged = [this](bool shown) {
        m_banners->setScreenShown(ADW_TOOLBAR_VIEW(m_window->m_page->widget()), shown);
    };

    /* the guest tools of the VM shown: its home page, its Details */
    m_toolsStatus->install = [this](Vm *vm) { installGuestTools(vm); };
    m_toolsStatus->cancelPending = [this](Vm *vm) { cancelPending(vm); };
    window->m_page->addHomeWidget(m_toolsStatus->homeWidget());
    window->m_page->addDetailsGroup(m_toolsStatus->detailsGroup());

    /* the tasks of the builds: their details are the Builds window */
    TaskList::instance()->setDetails(Task::Kind::Build, [this](Task *) {
        showBuilds(BuildManager::instance()->focus(), {});
    });
    /* the host packages' install or revert: a task of the app's while it runs */
    HostKdeMonitor *monitor = HostKdeMonitor::instance();
    QObject::connect(monitor, &HostKdeMonitor::started, m_qt.get(),
                     [this](bool install) { hostStarted(install); });
    QObject::connect(monitor, &HostKdeMonitor::finished, m_qt.get(),
                     [this](const QString &error, bool cancelled) { hostFinished(error, cancelled); });

    /* Vitrine's QEMU built anew, here or by hand: the preferred QEMU's documentation with it */
    QObject::connect(StackBuilder::instance(), &StackBuilder::started, m_qt.get(),
                     [this]() { m_qemuBefore = Paths::qemuBinary(); });
    QObject::connect(StackBuilder::instance(), &StackBuilder::built, m_qt.get(),
                     [this]() { qemuBuilt(); });
    /*
     * The Builds window's guest tools: the newest release the VMs' guests
     * run, which the user may change there
     */
    QList<GuestTools::System> systems;
    for (const Vm *vm : window->m_store->vms()) {
        systems << GuestTools::system(vm->id());
    }
    if (const QString release = GuestTools::defaultRelease(systems);
        release != GuestTools::newestRelease() && !BuildManager::instance()->isRunning()) {
        BuildManager::instance()->setGuestRelease(release);
    }
    watchActive(window->m_window);
}

MainWindowBuilds::~MainWindowBuilds()
{
    closed();
}

void MainWindowBuilds::closed()
{
    if (!m_qt) {
        return;
    }
    /* nothing more to show, nor any VM to start again for its tools */
    m_qt.reset();
    if (m_activeLater) {
        g_source_remove(std::exchange(m_activeLater, 0));
    }
    TaskList::instance()->setDetails(Task::Kind::Build, nullptr);
    m_toolsDialog = nullptr;
    m_installs.clear();
    m_buildsWindow.reset();
    m_updates.reset();
}

std::shared_ptr<GuestToolsInstall> MainWindowBuilds::toolsInstall(const QString &vmId) const
{
    return m_installs.value(vmId);
}

void MainWindowBuilds::setVm(Vm *vm)
{
    m_toolsStatus->setVm(vm);
    m_window->setEnabled("install-guest-tools", vm != nullptr);
}

void MainWindowBuilds::showBuilds(const QString &component, const QString &guestRelease)
{
    if (!m_qt || !m_window->m_window) {
        return;
    }
    /* a VM's: its release's medium (a run keeps its own) */
    if (!guestRelease.isEmpty()) {
        BuildManager::instance()->setGuestRelease(guestRelease);
    }
    if (!m_buildsWindow) {
        m_buildsWindow = std::make_unique<BuildsWindow>(GTK_WINDOW(m_window->m_window));
        m_buildsWindow->closed = [this]() {
            /* not from within its own signal: once GTK is done with it */
            QTimer::singleShot(0, m_qt.get(), [this]() { m_buildsWindow.reset(); });
        };
        watchActive(GTK_WIDGET(m_buildsWindow->window()));
    }
    m_buildsWindow->present(component);
    /* closed meanwhile, not yet let go: a new one */
    if (!m_buildsWindow->window()) {
        m_buildsWindow.reset();
        showBuilds(component, {});
    }
}

void MainWindowBuilds::installGuestTools(Vm *vm)
{
    if (!vm || !m_qt || !m_window->m_window) {
        return;
    }
    /* one at a time: the window's dialogs cover it */
    if (m_toolsDialog) {
        if (m_toolsDialog->vm() == vm) {
            return;
        }
        m_toolsDialog->forceClose();
    }
    const QString id = vm->id();
    auto *dialog = new GuestToolsDialog(
        vm, m_installs.value(id),
        [this](Vm *vm, bool mediumOnly, bool snapshot) {
            return startGuestTools(vm, mediumOnly, snapshot);
        },
        [this](const QString &component, const QString &release) {
            showBuilds(component, release);
        },
        m_window->presenter());
    m_toolsDialog = dialog;
    Gtk4::connect<void()>(dialog->dialog(), "closed", [this, dialog, id]() {
        if (m_toolsDialog == dialog) {
            m_toolsDialog = nullptr;
        }
        /* how it ended was shown: the install goes */
        dropInstall(id);
    });
    m_window->present(dialog->dialog());
}

std::shared_ptr<GuestToolsInstall> MainWindowBuilds::startGuestTools(Vm *vm, bool mediumOnly,
                                                                     bool snapshot)
{
    std::shared_ptr<GuestToolsInstall> &install = m_installs[vm->id()];
    const QString id = vm->id();

    if (!install || !install->isBusy()) {
        /* as the window starts a VM: its checks, USB access... */
        install = std::make_shared<GuestToolsInstall>(vm, [this](Vm *vm) {
            m_window->select(vm->id());
            m_window->start();
        });
        install->changed = [this, id]() { installChanged(id); };
        install->setDetails([this, id]() {
            if (Vm *vm = m_window->m_store->find(id)) {
                installGuestTools(vm);
            }
        });
    }
    /* the install keeps the window's pointer: one copy here, the one returned */
    const std::shared_ptr<GuestToolsInstall> started = install;
    started->start(mediumOnly, snapshot);
    return started;
}

void MainWindowBuilds::installChanged(const QString &id)
{
    const std::shared_ptr<GuestToolsInstall> install = m_installs.value(id);
    const bool shown = m_toolsDialog && m_toolsDialog->install() == install;

    if (!install || !m_qt) {
        return;
    }
    if (shown) {
        m_toolsDialog->refresh();
    }
    if (install->step() != GuestToolsInstall::Step::Done) {
        return;
    }
    /* failed out of sight, the VM off: the dialog comes back to say why */
    if (install->failed() && !shown) {
        if (Vm *vm = m_window->m_store->find(id)) {
            installGuestTools(vm);
            return;
        }
    }
    if (!(m_toolsDialog && m_toolsDialog->install() == install)) {
        dropInstall(id);
    }
}

void MainWindowBuilds::dropInstall(const QString &id)
{
    /* not from within its own call: once it is done saying it ended */
    QTimer::singleShot(0, m_qt.get(), [this, id]() {
        const std::shared_ptr<GuestToolsInstall> install = m_installs.value(id);
        if (install && !install->isBusy() &&
            !(m_toolsDialog && m_toolsDialog->install() == install)) {
            m_installs.remove(id);
        }
    });
}

void MainWindowBuilds::cancelPending(Vm *vm)
{
    /* the restart its install waits to make, out of sight */
    if (const std::shared_ptr<GuestToolsInstall> install = m_installs.value(vm->id());
        install && install->isBusy()) {
        install->cancel();
    }
}

void MainWindowBuilds::stayOff(Vm *vm)
{
    if (const std::shared_ptr<GuestToolsInstall> install = m_installs.value(vm->id())) {
        install->stayOff();
    }
}

void MainWindowBuilds::hostStarted(bool install)
{
    HostKdeMonitor *monitor = HostKdeMonitor::instance();
    QStringList titles;

    m_hostIds = monitor->busyWith();
    for (const QString &id : std::as_const(m_hostIds)) {
        titles << monitor->target(id).title;
    }
    if (m_hostTask && m_hostTask->isRunning()) {
        m_hostTask->dismiss();
    }
    m_hostTask = TaskList::instance()->add(
        Task::Kind::Other,
        install ? QString("Installing Vitrine's %1").arg(HostKdeDialog::names(titles))
                : QString("Reverting to the distribution's %1").arg(HostKdeDialog::names(titles)));
    m_hostTask->setText("With the administrator's password");
    const QStringList ids = m_hostIds;
    m_hostTask->setDetails([this, ids]() { showBuilds(ids.value(0), {}); });
}

void MainWindowBuilds::hostFinished(const QString &error, bool cancelled)
{
    if (m_hostTask && m_hostTask->isRunning()) {
        if (!error.isEmpty()) {
            m_hostTask->finish(cancelled ? Task::State::Stopped : Task::State::Failed, error);
        } else {
            m_hostTask->finish(Task::State::Done, "Takes effect at the next login");
        }
    }
    if (!error.isEmpty() && !cancelled) {
        presentOverBuilds(Gtk4::alert("Host Desktop", error));
    }
}

void MainWindowBuilds::presentOverBuilds(AdwDialog *dialog)
{
    /* the Builds window, where the install was asked for, else the main window */
    if (m_buildsWindow) {
        m_buildsWindow->presentDialog(dialog);
    } else {
        m_window->present(dialog);
    }
}

void MainWindowBuilds::qemuBuilt()
{
    /* what runs changed: the answers of the check for updates follow */
    if (m_updates) {
        m_updates->revalidate();
    }
    if (Paths::qemuBinary() != m_qemuBefore) {
        m_qemuBefore = Paths::qemuBinary();
        QemuDocs::reloadPreferred();
        m_window->m_page->refresh();
    }
}

void MainWindowBuilds::watchActive(GtkWidget *window)
{
    Gtk4::connect<void(GParamSpec *)>(window, "notify::is-active", [this, window](GParamSpec *) {
        if (!m_qt) {
            return;
        }
        if (gtk_window_is_active(GTK_WINDOW(window))) {
            if (m_activeLater) {
                g_source_remove(std::exchange(m_activeLater, 0));
            }
            if (!m_active) {
                m_active = true;
                activated();
            }
            return;
        }
        /*
         * From one of the app's windows to another, neither is active for
         * a moment: the app is no longer in front only if none is soon after
         */
        if (!m_activeLater) {
            m_activeLater = g_timeout_add_once(300, [](gpointer self) {
                auto *part = static_cast<MainWindowBuilds *>(self);
                part->m_activeLater = 0;
                GtkWidget *main = part->m_window->m_window;
                GtkWindow *builds = part->m_buildsWindow ? part->m_buildsWindow->window() : nullptr;
                part->m_active = (main && gtk_window_is_active(GTK_WINDOW(main))) ||
                                 (builds && gtk_window_is_active(builds));
            }, this);
        }
    });
}

void MainWindowBuilds::activated()
{
    /*
     * Back in front, from a terminal say: what builds run by hand made
     * meanwhile.  A medium of guest/build-medium.sh: the VMs with older
     * tools offer it.  A QEMU of host/build.sh, the VMs' QEMU now: as after
     * a build here.  The host's packages: Plasma, the KDE Frameworks or Qt
     * updated meanwhile, read again in the background (one reading at a
     * time: the Builds window in sight asks at the same time).
     */
    GuestToolsBuilder::instance()->checkMedium();
    if (!StackBuilder::instance()->isRunning() && Paths::qemuBinary() != m_qemuBefore) {
        qemuBuilt();
    }
    HostKdeMonitor::instance()->lookAgain();
    m_banners->refresh();
    if (m_toolsDialog) {
        m_toolsDialog->refresh();
    }
    if (m_buildsWindow) {
        m_buildsWindow->lookAgain();
    }
}
