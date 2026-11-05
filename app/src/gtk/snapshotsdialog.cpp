// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/snapshotsdialog.h"

#include <QDateTime>
#include <QFileInfo>
#include <QLocale>

#include <algorithm>
#include <utility>

#include "core/snapshots.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "gtk/rows.h"
#include "gtk/widgets.h"

using Gtk4::utf8;

static QString clockText(qint64 ms)
{
    const qint64 s = ms / 1000;

    return QString("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QChar('0'))
        .arg(s % 60, 2, 10, QChar('0'));
}

static const char kAbout[] =
    "A snapshot keeps the disks of the VM as they are, inside their qcow2 files, and takes no "
    "space until the VM writes over what it keeps. Taken while the VM runs, it keeps the running "
    "state too, its memory, and the VM can go back to that moment.";

static void setBanner(AdwBanner *banner, const QString &text)
{
    if (!text.isEmpty()) {
        adw_banner_set_title(banner, utf8(text.toHtmlEscaped()).constData());
    }
    adw_banner_set_revealed(banner, !text.isEmpty());
}

SnapshotsDialog::SnapshotsDialog(Vm *vm, Presenter presenter, StartFrom startFrom)
    : Dialog(adw_dialog_new(), std::move(presenter)), m_vm(vm),
      m_snapshots(new VmSnapshots(vm->runner(), qt())), m_startFrom(std::move(startFrom)),
      m_drives(ADW_BANNER(adw_banner_new(""))), m_error(ADW_BANNER(adw_banner_new(""))),
      m_notice(ADW_BANNER(adw_banner_new(""))), m_take(gtk_button_new()),
      m_stack(gtk_stack_new()), m_list(Gtk4::group({}, kAbout)),
      m_progress(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12)), m_progressText(Gtk4::label({}))
{
    GtkWidget *view = adw_toolbar_view_new();
    GtkWidget *header = adw_header_bar_new();
    GtkWidget *empty = adw_status_page_new();
    auto *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());

    adw_dialog_set_title(dialog(), utf8(QString("%1 Snapshots").arg(vm->name())).constData());
    adw_dialog_set_content_width(dialog(), 680);
    adw_dialog_set_content_height(dialog(), 560);
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header),
                                    adw_window_title_new("Snapshots", utf8(vm->name()).constData()));
    {
        GtkWidget *content = adw_button_content_new();
        adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(content), "camera-photo-symbolic");
        adw_button_content_set_label(ADW_BUTTON_CONTENT(content), "_Take…");
        adw_button_content_set_use_underline(ADW_BUTTON_CONTENT(content), true);
        gtk_button_set_child(GTK_BUTTON(m_take), content);
    }
    gtk_widget_set_name(m_take, "take");
    gtk_widget_set_tooltip_text(m_take, "Take a Snapshot");
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), m_take);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    for (AdwBanner *banner : {m_drives, m_error, m_notice}) {
        adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), GTK_WIDGET(banner));
    }

    adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty), "camera-photo-symbolic");
    adw_status_page_set_title(ADW_STATUS_PAGE(empty), "No Snapshots");
    adw_status_page_set_description(ADW_STATUS_PAGE(empty), kAbout);
    adw_preferences_page_add(page, m_list);
    gtk_stack_add_named(GTK_STACK(m_stack), empty, "empty");
    gtk_stack_add_named(GTK_STACK(m_stack), GTK_WIDGET(page), "list");
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), m_stack);

    /* what QEMU or qemu-img is at */
    for (const auto side : {&gtk_widget_set_margin_start, &gtk_widget_set_margin_end,
                            &gtk_widget_set_margin_top, &gtk_widget_set_margin_bottom}) {
        side(m_progress, 12);
    }
    gtk_box_append(GTK_BOX(m_progress), adw_spinner_new());
    gtk_widget_set_hexpand(m_progressText, true);
    gtk_box_append(GTK_BOX(m_progress), m_progressText);
    gtk_widget_set_visible(m_progress, false);
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(view), m_progress);
    adw_dialog_set_child(dialog(), view);

    on<void()>(m_take, "clicked", [this]() { askName(); });
    QObject::connect(m_snapshots, &VmSnapshots::listed, qt(), [this](const QString &error) {
        if (!error.isEmpty()) {
            setBanner(m_error, error);
        }
        fill();
    });
    QObject::connect(m_snapshots, &VmSnapshots::finished, qt(), [this](const QString &error) {
        gtk_widget_set_visible(m_progress, false);
        setBanner(m_error, error);
        updateButtons();
        if (std::exchange(m_startAfterRestore, false) && error.isEmpty() && m_startFrom) {
            m_startFrom(QString());
            forceClose();
        }
    });
    QObject::connect(m_snapshots, &VmSnapshots::busyChanged, qt(), [this]() { updateButtons(); });
    QObject::connect(m_snapshots, &VmSnapshots::notice, qt(),
                     [this](const QString &text) { setBanner(m_notice, text); });
    QObject::connect(vm, &Vm::changed, qt(), [this]() {
        m_snapshots->setVm(m_vm->args(), m_vm->dir());
        updateDrives();
    });
    /* QEMU or qemu-img: a new list */
    QObject::connect(vm->runner(), &VmRunner::stateChanged, qt(), [this]() {
        updateDrives();
        updateButtons();
        if (!m_snapshots->isBusy()) {
            m_snapshots->refresh();
        }
    });
    m_snapshots->setVm(vm->args(), vm->dir());
    m_snapshots->refresh();
    updateDrives();
    fill();
}

void SnapshotsDialog::closed()
{
    /* QEMU or qemu-img at it: it goes once they are done, unwatched */
    if (m_snapshots->isBusy()) {
        m_snapshots->setParent(nullptr);
        QObject::disconnect(m_snapshots, nullptr, qt(), nullptr);
        QObject::connect(m_snapshots, &VmSnapshots::finished, m_snapshots, &QObject::deleteLater);
    }
}

QStringList SnapshotsDialog::names() const
{
    QStringList list;

    for (const Actions &a : m_actions) {
        list << a.name;
    }
    return list;
}

bool SnapshotsDialog::isBusy() const
{
    return m_snapshots->isBusy();
}

void SnapshotsDialog::fill()
{
    QList<VmSnapshots::Snapshot> list = m_snapshots->snapshots();
    const QLocale locale;

    for (GtkWidget *row : std::as_const(m_rows)) {
        adw_preferences_group_remove(m_list, row);
    }
    m_rows.clear();
    m_actions.clear();
    /* the latest first */
    std::stable_sort(list.begin(), list.end(),
                     [](const auto &a, const auto &b) { return a.date > b.date; });
    for (const VmSnapshots::Snapshot &s : std::as_const(list)) {
        const bool state = s.stateBytes > 0;
        QStringList files;
        for (const QString &file : s.files) {
            files << QFileInfo(file).fileName();
        }
        const QString subtitle =
            state ? QString("%1 · disks and running state, %2, VM clock %3")
                        .arg(locale.toString(s.date, QLocale::ShortFormat),
                             locale.formattedDataSize(s.stateBytes, 1, QLocale::DataSizeIecFormat),
                             clockText(s.vmClockMs))
                  : QString("%1 · disks only").arg(locale.toString(s.date, QLocale::ShortFormat));
        GtkWidget *row = Gtk4::actionRow(s.name, subtitle);
        GtkWidget *restore = Gtk4::rowButton("edit-undo-symbolic", "Go Back to It");
        GtkWidget *start = Gtk4::rowButton("media-playback-start-symbolic", "Start From It");
        GtkWidget *remove = Gtk4::rowButton("user-trash-symbolic", "Delete");
        const QString name = s.name;

        adw_action_row_add_prefix(ADW_ACTION_ROW(row),
                                  gtk_image_new_from_icon_name("camera-photo-symbolic"));
        gtk_widget_set_tooltip_text(row, utf8(QString("In %1").arg(files.join(", "))).constData());
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), restore);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), start);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove);
        on<void()>(restore, "clicked", [this, name]() { this->restore(name); });
        on<void()>(start, "clicked", [this, name]() { startFrom(name); });
        on<void()>(remove, "clicked", [this, name]() { this->remove(name); });
        adw_preferences_group_add(m_list, row);
        m_rows << row;
        m_actions << Actions{name, restore, start, remove};
    }
    gtk_stack_set_visible_child_name(GTK_STACK(m_stack), m_rows.isEmpty() ? "empty" : "list");
    updateButtons();
}

void SnapshotsDialog::updateButtons()
{
    const bool busy = m_snapshots->isBusy();
    const VmRunner::State state = m_vm ? m_vm->runner()->state() : VmRunner::State::Stopped;
    const bool live = state == VmRunner::State::Running || state == VmRunner::State::Paused;
    const bool stopped = state == VmRunner::State::Stopped;

    gtk_widget_set_sensitive(m_take, !busy && (live || stopped) && !m_cannotTake);
    for (const Actions &a : std::as_const(m_actions)) {
        const bool withState = hasState(a.name);
        /* a running VM goes back to the running state, not the disks alone */
        gtk_widget_set_sensitive(a.restore, !busy && (stopped || (live && withState)));
        gtk_widget_set_tooltip_text(a.restore, live && !withState
                                                   ? "This snapshot holds the disks only: stop the "
                                                     "VM to go back to it"
                                                   : "Take the VM back to where it was");
        /* of the disks only too: QEMU saves no running state for VMs with some
           devices (vhost-user, 3D blob resources...), whose snapshots all are so */
        gtk_widget_set_sensitive(a.start, !busy && stopped);
        gtk_widget_set_tooltip_text(a.start, !withState
                                                 ? "This snapshot holds the disks only: they go back "
                                                   "to it, and the guest starts from them"
                                                 : "Start the VM where it was when the snapshot was "
                                                   "taken");
        gtk_widget_set_sensitive(a.remove, !busy && (live || stopped));
    }
}

/* The files that cannot keep snapshots, said above the list */
void SnapshotsDialog::updateDrives()
{
    QStringList cannot;
    int can = 0;
    const bool live = m_vm && m_vm->runner()->isActive();
    QString text;

    m_cannotTake = false;
    if (!m_vm) {
        setBanner(m_drives, {});
        return;
    }
    for (const VmSnapshots::Drive &drive : VmSnapshots::drives(m_vm->args(), m_vm->dir())) {
        if (drive.canSnapshot()) {
            can++;
        } else {
            cannot << (drive.pflash ? QString("the UEFI variables (%1)")
                                          .arg(QFileInfo(drive.file).fileName())
                                    : QFileInfo(drive.file).fileName());
        }
    }
    if (can == 0) {
        m_cannotTake = true;
        text = "The VM has no qcow2 disk to keep snapshots in. Only qcow2 files can hold them; "
               "qemu-img convert makes one of a disk.";
    } else if (!cannot.isEmpty() && live) {
        text = QString("Snapshots leave out %1, which is not in qcow2, and hold the disks only: "
                       "QEMU cannot save the running state then.")
                   .arg(cannot.join(", "));
    } else if (!cannot.isEmpty()) {
        text = QString("Snapshots leave out %1, which is not in qcow2.").arg(cannot.join(", "));
    }
    setBanner(m_drives, text);
}

void SnapshotsDialog::busy(const QString &what)
{
    setBanner(m_error, {});
    setBanner(m_notice, {});
    gtk_label_set_text(GTK_LABEL(m_progressText), utf8(what).constData());
    gtk_widget_set_visible(m_progress, true);
}

bool SnapshotsDialog::hasState(const QString &name) const
{
    for (const VmSnapshots::Snapshot &s : m_snapshots->snapshots()) {
        if (s.name == name && s.stateBytes > 0) {
            return true;
        }
    }
    return false;
}

void SnapshotsDialog::askName()
{
    const bool live = m_vm && m_vm->runner()->isActive();
    GtkWidget *entry = gtk_entry_new();
    AdwDialog *alert = Gtk4::alert("Take a Snapshot",
                                   live ? "Its name, for a snapshot with the running state of the VM:"
                                        : "Its name, for a snapshot of the disks:",
                                   {{"cancel", "_Cancel"}, {"take", "_Take"}}, "take");

    Gtk4::setText(entry, QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss"));
    gtk_entry_set_activates_default(GTK_ENTRY(entry), true);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(alert), entry);
    Gtk4::onResponse(alert, guarded<void(const QString &)>([this, entry](const QString &response) {
        const QString name = Gtk4::text(entry);
        if (response != "take") {
            return;
        }
        if (!VmSnapshots::isValidName(name)) {
            setBanner(m_error, "A snapshot needs a name, not only digits, which QEMU would take for "
                               "the number of another, and no spaces around it.");
            return;
        }
        for (const VmSnapshots::Snapshot &s : m_snapshots->snapshots()) {
            if (s.name == name) {
                AdwDialog *replace = Gtk4::alert(
                    QString("Replace %1?").arg(name),
                    "A snapshot of this name exists: the new one replaces it.",
                    {{"cancel", "_Cancel"}, {"replace", "_Replace"}}, "replace", true);
                Gtk4::onResponse(replace, guarded<void(const QString &)>(
                                              [this, name](const QString &answer) {
                                                  if (answer == "replace") {
                                                      take(name);
                                                  }
                                              }));
                present(replace);
                return;
            }
        }
        take(name);
    }));
    present(alert);
    /* the name to type over, rather than the buttons */
    adw_dialog_set_focus(alert, entry);
}

void SnapshotsDialog::take(const QString &name)
{
    const bool live = m_vm && m_vm->runner()->isActive();

    busy(live ? QString("Taking the snapshot: QEMU pauses the VM while it saves the running state…")
              : QString("Taking the snapshot…"));
    m_snapshots->take(name);
}

void SnapshotsDialog::restore(const QString &name)
{
    const bool live = m_vm && m_vm->runner()->isActive();
    AdwDialog *alert = Gtk4::alert(
        QString("Go Back to %1?").arg(name),
        live ? "The VM goes back to where it was when the snapshot was taken: what it did since is "
               "lost."
             : "The disks go back to what they held when the snapshot was taken: what was written "
               "since is lost.",
        {{"cancel", "_Cancel"}, {"restore", "_Go Back"}}, "restore", true);
    Gtk4::onResponse(alert, guarded<void(const QString &)>([this, name](const QString &response) {
        if (response == "restore") {
            busy(QString("Going back to %1…").arg(name));
            m_snapshots->restore(name);
        }
    }));
    present(alert);
}

void SnapshotsDialog::startFrom(const QString &name)
{
    if (hasState(name)) {
        if (m_startFrom) {
            m_startFrom(name);
        }
        forceClose();
        return;
    }
    AdwDialog *alert = Gtk4::alert(
        QString("Start From %1?").arg(name),
        "This snapshot holds the disks only, without the running VM: the disks go back to what "
        "they held when it was taken, what was written since is lost, and the guest starts from "
        "them.",
        {{"cancel", "_Cancel"}, {"start", "_Start"}}, "start", true);
    Gtk4::onResponse(alert, guarded<void(const QString &)>([this, name](const QString &response) {
        if (response == "start") {
            m_startAfterRestore = true;
            busy(QString("Going back to %1…").arg(name));
            m_snapshots->restore(name);
        }
    }));
    present(alert);
}

void SnapshotsDialog::remove(const QString &name)
{
    AdwDialog *alert = Gtk4::alert(
        QString("Delete %1?").arg(name),
        "The VM can no longer go back to it. The space it takes in the qcow2 files is reused.",
        {{"cancel", "_Cancel"}, {"delete", "_Delete"}}, "delete", true);
    Gtk4::onResponse(alert, guarded<void(const QString &)>([this, name](const QString &response) {
        if (response == "delete") {
            busy(QString("Deleting %1…").arg(name));
            m_snapshots->remove(name);
        }
    }));
    present(alert);
}
