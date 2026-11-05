// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/guesttoolsdialog.h"

#include "core/buildmanager.h"
#include "core/guesttools.h"
#include "core/guesttoolsbuilder.h"
#include "core/snapshots.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "gtk/guesttoolsinstall.h"
#include "gtk/markup.h"
#include "gtk/rows.h"
#include "gtk/widgets.h"
#include "ui/guesttoolstext.h"

using Gtk4::utf8;
using Step = GuestToolsInstall::Step;

namespace {

/* The disks of a VM that writes to qcow2 files can go back to before */
bool canSnapshot(Vm *vm)
{
    for (const VmSnapshots::Drive &d : VmSnapshots::drives(vm->args(), vm->dir())) {
        if (d.canSnapshot() && !d.pflash) {
            return true;
        }
    }
    return false;
}

void setMarkup(GtkWidget *label, const QString &html)
{
    gtk_label_set_markup(GTK_LABEL(label), utf8(Markup::pango(html)).constData());
}

}

GuestToolsDialog::GuestToolsDialog(Vm *vm, std::shared_ptr<GuestToolsInstall> install, Start start,
                                   ShowBuilds builds, Presenter presenter)
    : Dialog(adw_dialog_new(), std::move(presenter)), m_vm(vm), m_install(install),
      m_start(std::move(start)), m_builds(std::move(builds)), m_release(GuestTools::newestRelease()),
      m_seenStep(install ? int(install->step()) : int(Step::Idle)),
      m_build(ADW_BANNER(adw_banner_new(""))), m_warning(ADW_BANNER(adw_banner_new("")))
{
    GtkWidget *view = adw_toolbar_view_new();
    GtkWidget *header = adw_header_bar_new();
    auto *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *about = Gtk4::group();
    AdwPreferencesGroup *how = Gtk4::group();
    const bool update = !GuestToolsMonitor::of(vm)->report().tools.isEmpty();
    const bool snapshots = canSnapshot(vm);

    adw_dialog_set_title(dialog(), update ? "Update Guest Tools" : "Install Guest Tools");
    adw_dialog_set_content_width(dialog(), 600);
    adw_header_bar_set_title_widget(
        ADW_HEADER_BAR(header),
        adw_window_title_new(update ? "Update Guest Tools" : "Install Guest Tools",
                             utf8(vm->name()).constData()));
    m_cancel = gtk_button_new_with_mnemonic("_Cancel");
    m_stop = gtk_button_new_with_mnemonic("Cancel _Install");
    gtk_widget_add_css_class(m_stop, "destructive-action");
    gtk_widget_set_tooltip_text(m_stop, "No install after all, nor restart");
    m_ok = gtk_button_new_with_mnemonic("_Install");
    gtk_widget_add_css_class(m_ok, "suggested-action");
    m_close = gtk_button_new_with_mnemonic("C_lose");
    adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), false);
    adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), false);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), m_cancel);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), m_stop);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), m_ok);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), m_close);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    /* the medium: not built yet, being built, out of date, with Build… or Update… */
    adw_banner_set_use_markup(m_build, true);
    adw_banner_set_use_markup(m_warning, true);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), GTK_WIDGET(m_build));
    /* what else keeps the tools from the VM */
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), GTK_WIDGET(m_warning));

    /* whom they are for, what they install, how */
    m_what = Gtk4::label({});
    gtk_widget_set_name(m_what, "what");
    adw_preferences_group_add(about, m_what);
    adw_preferences_page_add(page, about);

    /* the disks of a VM that writes to qcow2 files can go back to before */
    m_snapshot = Gtk4::switchRow("Take a _Snapshot of the Disks First",
                                 snapshots ? QString("The VM can go back to it from its snapshots")
                                           : QString("The VM's disks are not qcow2 files"));
    adw_preferences_row_set_use_underline(ADW_PREFERENCES_ROW(m_snapshot), true);
    gtk_widget_set_name(m_snapshot, "snapshot");
    adw_switch_row_set_active(ADW_SWITCH_ROW(m_snapshot), snapshots);
    gtk_widget_set_sensitive(m_snapshot, snapshots);
    adw_preferences_group_add(how, m_snapshot);
    m_attachRow = Gtk4::actionRow(
        "Attach the Medium Only",
        QString("The VM starts with the tools medium only: install them by hand in the guest with "
                "sudo bash /run/media/$USER/%1/install")
            .arg(GuestTools::kLabel));
    m_attach = Gtk4::rowTextButton("_Attach");
    gtk_widget_set_name(m_attach, "attach");
    adw_action_row_add_suffix(ADW_ACTION_ROW(m_attachRow), m_attach);
    adw_preferences_group_add(how, m_attachRow);
    adw_preferences_page_add(page, how);

    /* the install under way, or how it ended */
    m_progressGroup = Gtk4::group();
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        m_spinner = adw_spinner_new();
        m_progress = Gtk4::label({});
        gtk_widget_set_name(m_progress, "progress");
        gtk_widget_set_hexpand(m_progress, true);
        gtk_box_append(GTK_BOX(row), m_spinner);
        gtk_box_append(GTK_BOX(row), m_progress);
        adw_preferences_group_add(m_progressGroup, row);
        m_background = Gtk4::caption("Closing this window does not call the install off: "
                                     "Cancel Install does.");
        gtk_widget_set_margin_top(m_background, 6);
        adw_preferences_group_add(m_progressGroup, m_background);
    }
    adw_preferences_page_add(page, m_progressGroup);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), GTK_WIDGET(page));
    adw_dialog_set_child(dialog(), view);

    on<void()>(m_ok, "clicked", [this]() { go(false); });
    on<void()>(m_attach, "clicked", [this]() { go(true); });
    on<void()>(m_cancel, "clicked", [this]() { close(); });
    on<void()>(m_close, "clicked", [this]() { forceClose(); });
    on<void()>(m_stop, "clicked", [this]() {
        if (const std::shared_ptr<GuestToolsInstall> install = m_install.lock()) {
            install->cancel();
        }
    });
    on<void()>(m_build, "button-clicked", [this]() {
        if (m_builds) {
            m_builds("medium", m_release);
        }
    });

    /* not modal for the VM: it may start, stop, change or go meanwhile */
    QObject::connect(vm->runner(), &VmRunner::stateChanged, qt(), [this]() { refresh(); });
    QObject::connect(vm, &Vm::changed, qt(), [this]() { refresh(); });
    QObject::connect(vm, &QObject::destroyed, qt(), [this]() { forceClose(); });
    /* the medium of the VM's release, being built, made by a build or by hand */
    GuestToolsBuilder *builder = GuestToolsBuilder::instance();
    for (auto signal : {&GuestToolsBuilder::started, &GuestToolsBuilder::waitingChanged,
                        &GuestToolsBuilder::mediumChanged}) {
        QObject::connect(builder, signal, qt(), [this]() { refresh(); });
    }
    QObject::connect(builder, &GuestToolsBuilder::stepStarted, qt(), [this]() { refreshBuild(); });
    QObject::connect(builder, &GuestToolsBuilder::phaseChanged, qt(), [this]() { refreshBuild(); });
    QObject::connect(builder, &GuestToolsBuilder::finished, qt(), [this]() { refresh(); });
    /* by whole percents: ninja's lines come by the thousand */
    QObject::connect(builder, &GuestToolsBuilder::progress, qt(), [this](int done, int total) {
        const int percent = total > 0 ? done * 100 / total : -1;
        if (percent != m_percent) {
            m_percent = percent;
            refreshBuild();
        }
    });
    /* a build starting or ending: Install waits for it, or goes on once it is done */
    if (BuildManager *manager = BuildManager::instance()) {
        QObject::connect(manager, &BuildManager::runStarted, qt(), [this]() { refresh(); });
        QObject::connect(manager, &BuildManager::runFinished, qt(), [this]() { refresh(); });
    }
    refresh();
}

Vm *GuestToolsDialog::vm() const
{
    return m_vm;
}

void GuestToolsDialog::setInstall(std::shared_ptr<GuestToolsInstall> install)
{
    m_install = install;
    refresh();
}

void GuestToolsDialog::refreshBuild()
{
    QString button;
    const QString text = GuestToolsText::mediumNote(m_release, &button);

    if (!text.isEmpty()) {
        adw_banner_set_title(m_build, utf8(Markup::pango(text)).constData());
    }
    adw_banner_set_button_label(m_build,
                                button.isEmpty() ? nullptr : utf8('_' + button).constData());
    adw_banner_set_revealed(m_build, !text.isEmpty());
}

void GuestToolsDialog::refresh()
{
    if (!m_vm || isClosed()) {
        return;
    }
    const std::shared_ptr<GuestToolsInstall> install = m_install.lock();
    const bool busy = install && install->isBusy();
    const bool over = install && install->step() == Step::Done;

    /* over and well, or called off: nothing more to show */
    if (over && (install->succeeded() || install->cancelled())) {
        forceClose();
        return;
    }
    /*
     * Out of the way of the VM's screen, where the guest may ask what to
     * do (Plasma answers the power button with its logout screen), for as
     * long as it takes: the window's notice says how the guest was asked,
     * the task what the install waits for, and Install Guest Tools… brings
     * this back.  Not when brought back during that wait.
     */
    const int step = install ? int(install->step()) : int(Step::Idle);
    if (step == int(Step::ShuttingDown) && m_seenStep != step) {
        m_seenStep = step;
        forceClose();
        return;
    }
    m_seenStep = step;
    const bool running = m_vm->runner()->isActive();
    const QString release = GuestToolsText::toolsRelease(m_vm);
    const GuestTools::Medium medium = GuestToolsText::mediumOf(m_vm);
    const QString problem = GuestToolsText::obstacle(m_vm, medium);
    const bool building = GuestToolsText::mediumBeingBuilt(release);
    const bool idle = !busy && !over;

    /* what the guest said it runs may have changed it */
    m_release = release.isEmpty() ? GuestTools::newestRelease() : release;
    refreshBuild();
    setMarkup(m_what, GuestToolsText::what(m_vm, medium));

    /* the medium missing: the build banner tells it */
    if (!problem.isEmpty()) {
        adw_banner_set_title(m_warning, utf8(problem.toHtmlEscaped()).constData());
    }
    adw_banner_set_revealed(m_warning, idle && !problem.isEmpty() &&
                                           problem != GuestToolsText::notBuilt());
    gtk_button_set_label(GTK_BUTTON(m_ok), building ? (running ? "_Restart and Install When Built"
                                                               : "_Install When Built")
                                                    : (running ? "_Restart and Install"
                                                               : "_Install"));
    /* no medium yet, but one on its way: Install waits for it too */
    gtk_widget_set_sensitive(m_ok, problem.isEmpty() ||
                                       (building && problem == GuestToolsText::notBuilt()));
    gtk_widget_set_sensitive(m_attach, idle && problem.isEmpty() && !building);
    gtk_widget_set_sensitive(m_snapshot, idle && canSnapshot(m_vm));

    gtk_widget_set_visible(m_cancel, idle);
    gtk_widget_set_visible(m_ok, idle);
    gtk_widget_set_visible(m_stop, busy && install->step() != Step::Starting);
    gtk_widget_set_visible(m_close, !idle);
    adw_dialog_set_default_widget(dialog(), idle ? m_ok : m_close);

    gtk_widget_set_visible(GTK_WIDGET(m_progressGroup), !idle);
    gtk_widget_set_visible(m_spinner, busy);
    gtk_widget_set_visible(m_background, busy);
    if (install) {
        setMarkup(m_progress, install->progress());
    }
    if (over && install->failed()) {
        gtk_widget_add_css_class(m_progress, "error");
    } else {
        gtk_widget_remove_css_class(m_progress, "error");
    }
}

void GuestToolsDialog::go(bool mediumOnly)
{
    if (!m_vm || !m_start) {
        return;
    }
    if (const std::shared_ptr<GuestToolsInstall> install = m_install.lock();
        install && install->isBusy()) {
        return;
    }
    const bool snapshot = gtk_widget_get_sensitive(m_snapshot) &&
                          adw_switch_row_get_active(ADW_SWITCH_ROW(m_snapshot));
    m_install = m_start(m_vm, mediumOnly, snapshot);
    refresh();
}

QString GuestToolsDialog::installLabel() const
{
    return QString::fromUtf8(gtk_button_get_label(GTK_BUTTON(m_ok))).remove('_');
}

bool GuestToolsDialog::canInstall() const
{
    return gtk_widget_get_visible(m_ok) && gtk_widget_get_sensitive(m_ok);
}

bool GuestToolsDialog::canAttach() const
{
    return gtk_widget_get_sensitive(m_attach);
}

bool GuestToolsDialog::canCancel() const
{
    return gtk_widget_get_visible(m_stop);
}

QString GuestToolsDialog::text() const
{
    return QString::fromUtf8(gtk_label_get_text(GTK_LABEL(m_what)));
}

QString GuestToolsDialog::progressText() const
{
    return gtk_widget_get_visible(GTK_WIDGET(m_progressGroup))
               ? QString::fromUtf8(gtk_label_get_text(GTK_LABEL(m_progress)))
               : QString();
}

QString GuestToolsDialog::buildBannerText() const
{
    return adw_banner_get_revealed(m_build)
               ? Markup::plain(QString::fromUtf8(adw_banner_get_title(m_build)))
               : QString();
}

QString GuestToolsDialog::warningText() const
{
    return adw_banner_get_revealed(m_warning)
               ? Markup::plain(QString::fromUtf8(adw_banner_get_title(m_warning)))
               : QString();
}

void GuestToolsDialog::setSnapshot(bool snapshot)
{
    adw_switch_row_set_active(ADW_SWITCH_ROW(m_snapshot), snapshot);
}
