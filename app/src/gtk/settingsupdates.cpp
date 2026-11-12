// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/settingsupdates.h"

#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "gtk/rows.h"
#include "gtk/widgets.h"

using Gtk4::utf8;
using SettingsUpdates::Change;
using SettingsUpdates::Record;

/* "its line 12 is no option: ..." as a sentence of its own */
static QString sentence(const QString &text)
{
    QString out = text;

    if (!out.isEmpty()) {
        out[0] = out[0].toUpper();
        if (!out.endsWith('.')) {
            out += '.';
        }
    }
    return out;
}

static QStringList titles(const QList<Change> &changes)
{
    QStringList out;

    for (const Change &c : changes) {
        out << c.title;
    }
    return out;
}

static QStringList titles(const Record &record)
{
    QStringList out;

    for (const Record::Applied &applied : record.changes) {
        out << SettingsUpdates::title(applied.id);
    }
    return out;
}

SettingsUpdatesStatus::SettingsUpdatesStatus()
    : m_home(gtk_list_box_new()), m_homeRow(adw_action_row_new()),
      m_details(Gtk4::group("Newer Settings")), m_review(Gtk4::rowTextButton("_Review…"))
{
    /* they live as long as this, wherever they are shown */
    g_object_ref_sink(m_home);
    g_object_ref_sink(m_details);

    gtk_list_box_set_selection_mode(GTK_LIST_BOX(m_home), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(m_home, "boxed-list");
    gtk_widget_set_name(m_home, "settingsUpdates");
    adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(m_homeRow), 0);
    adw_action_row_add_prefix(ADW_ACTION_ROW(m_homeRow),
                              gtk_image_new_from_icon_name("software-update-available-symbolic"));
    GtkWidget *homeReview = Gtk4::rowTextButton("_Review…");
    GtkWidget *notNow = Gtk4::rowButton("window-close-symbolic", "Not Now");
    adw_action_row_add_suffix(ADW_ACTION_ROW(m_homeRow), homeReview);
    adw_action_row_add_suffix(ADW_ACTION_ROW(m_homeRow), notNow);
    gtk_list_box_append(GTK_LIST_BOX(m_home), m_homeRow);
    on<void()>(homeReview, "clicked", [this]() {
        if (m_vm && review) {
            review(m_vm);
        }
    });
    on<void()>(notNow, "clicked", [this]() {
        if (m_vm) {
            SettingsUpdatesMonitor::of(m_vm)->setNotNow();
        }
    });

    /* kept while out of the group's header (update()) */
    g_object_ref_sink(m_review);
    gtk_widget_add_css_class(m_review, "flat");
    on<void()>(m_review, "clicked", [this]() {
        if (m_vm && review) {
            review(m_vm);
        }
    });

    gtk_widget_set_visible(m_home, false);
    gtk_widget_set_visible(GTK_WIDGET(m_details), false);
}

SettingsUpdatesStatus::~SettingsUpdatesStatus()
{
    stopCallbacks();
    g_object_unref(m_review);
    g_object_unref(m_details);
    g_object_unref(m_home);
}

Vm *SettingsUpdatesStatus::vm() const
{
    return m_vm;
}

void SettingsUpdatesStatus::setVm(Vm *vm)
{
    if (m_vm != vm) {
        if (m_vm) {
            QObject::disconnect(SettingsUpdatesMonitor::of(m_vm), nullptr, &m_qt, nullptr);
        }
        m_vm = vm;
        if (vm) {
            SettingsUpdatesMonitor *monitor = SettingsUpdatesMonitor::of(vm);
            QObject::connect(monitor, &SettingsUpdatesMonitor::changed, &m_qt,
                             [this]() { update(); });
            /* its QEMU, the preferences' one, may be another since */
            monitor->refresh();
        }
    }
    update();
}

void SettingsUpdatesStatus::update()
{
    for (GtkWidget *row : std::as_const(m_rows)) {
        adw_preferences_group_remove(m_details, row);
    }
    m_rows.clear();
    m_rowTitles.clear();
    m_homeText.clear();
    if (!m_vm) {
        gtk_widget_set_visible(m_home, false);
        gtk_widget_set_visible(GTK_WIDGET(m_details), false);
        return;
    }
    SettingsUpdatesMonitor *monitor = SettingsUpdatesMonitor::of(m_vm);
    const QList<Change> offered = monitor->offered();
    const QList<Change> declined = monitor->declined();
    const QList<Record> records = monitor->records();
    const QString problem = monitor->problem();
    const bool blocked = !monitor->blocked().isEmpty();

    /* the home page: one row, while there is something to take */
    if (!offered.isEmpty() && !monitor->notNow()) {
        m_homeText = offered.size() == 1 ? QString("This VM lacks a setting that new VMs get")
                                         : QString("This VM lacks %1 settings that new VMs get")
                                               .arg(offered.size());
        Gtk4::setTitle(m_homeRow, m_homeText);
        Gtk4::setSubtitle(m_homeRow, SettingsUpdates::listed(titles(offered), true));
    }
    gtk_widget_set_visible(m_home, !m_homeText.isEmpty());

    /* the Details: all of it */
    const auto add = [this](GtkWidget *row, const QString &title) {
        adw_preferences_group_add(m_details, row);
        m_rows << row;
        m_rowTitles << title;
    };
    for (const Change &c : offered) {
        GtkWidget *row = Gtk4::actionRow(c.title, c.reason);
        adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 0);
        add(row, c.title);
    }
    if (!records.isEmpty()) {
        const Record &last = records.last();
        const QString title =
            QString("Updated on %1").arg(last.time.toString("d MMMM yyyy, HH:mm"));
        GtkWidget *row = Gtk4::actionRow(
            title, QString("%1. vm.args as it was before: %2")
                       .arg(SettingsUpdates::listed(titles(last), true), last.backup));
        GtkWidget *button =
            Gtk4::rowTextButton("_Undo…", blocked ? QString("Shut the VM down first") : QString());
        adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 0);
        gtk_widget_set_sensitive(button, !blocked);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), button);
        on<void()>(button, "clicked", [this]() {
            if (m_vm && undo) {
                undo(m_vm);
            }
        });
        add(row, title);
    }
    for (const Change &c : declined) {
        GtkWidget *row = Gtk4::actionRow(c.title, "Not asked again for this VM");
        GtkWidget *button = Gtk4::rowTextButton("_Ask Again");
        const QString id = c.id;
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), button);
        on<void()>(button, "clicked", [this, id]() {
            if (m_vm) {
                SettingsUpdatesMonitor::of(m_vm)->setDeclined(id, false);
            }
        });
        add(row, c.title);
    }
    if (!problem.isEmpty()) {
        const QString title = "Vitrine cannot check this VM's settings";
        GtkWidget *row = Gtk4::actionRow(title, sentence(problem));
        adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 0);
        add(row, title);
    }

    QString description;
    if (!offered.isEmpty()) {
        description = "Settings vitrine gives the VMs it makes now, which this VM was made "
                      "without.";
        if (blocked) {
            description += " Shut the VM down to apply them.";
        }
    }
    adw_preferences_group_set_description(
        m_details, description.isEmpty() ? nullptr : utf8(description.toHtmlEscaped()).constData());
    /* in the header only when it shows: a hidden one left marks in the group's header */
    const bool reviewing = !offered.isEmpty() || !declined.isEmpty();
    if (reviewing != (adw_preferences_group_get_header_suffix(m_details) != nullptr)) {
        adw_preferences_group_set_header_suffix(m_details, reviewing ? m_review : nullptr);
    }
    gtk_widget_set_visible(GTK_WIDGET(m_details), !m_rows.isEmpty());
}

/* What a change does to vm.args: its lines as they are, and as they become */
static GtkWidget *linesOf(const Change &change)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    const auto part = [box](const QString &heading, const QStringList &lines) {
        if (lines.isEmpty()) {
            return;
        }
        GtkWidget *label = Gtk4::label(lines.join('\n'), true);
        /* a long line wraps where it reaches the edge: not at a hyphen, which reads as a break */
        gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_CHAR);
        gtk_widget_add_css_class(label, "monospace");
        gtk_box_append(GTK_BOX(box), Gtk4::caption(heading));
        gtk_box_append(GTK_BOX(box), label);
    };

    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    if (change.before.isEmpty()) {
        part("Added to vm.args", change.after);
    } else if (change.after.isEmpty()) {
        part("Taken out of vm.args", change.before);
    } else {
        part("In vm.args now", change.before);
        part("Then", change.after);
    }
    return box;
}

SettingsUpdatesDialog::SettingsUpdatesDialog(Vm *vm, Presenter presenter, Confirm confirm)
    : Dialog(adw_dialog_new(), std::move(presenter)), m_vm(vm), m_confirm(std::move(confirm)),
      m_page(ADW_PREFERENCES_PAGE(adw_preferences_page_new())),
      m_banner(ADW_BANNER(adw_banner_new(""))),
      m_apply(gtk_button_new_with_mnemonic("_Apply"))
{
    GtkWidget *view = adw_toolbar_view_new();
    GtkWidget *header = adw_header_bar_new();
    GtkWidget *notNowButton = gtk_button_new_with_mnemonic("_Not Now");

    adw_dialog_set_title(dialog(),
                         utf8(QString("Newer Settings for %1").arg(vm->name())).constData());
    adw_dialog_set_content_width(dialog(), 620);
    adw_dialog_set_content_height(dialog(), 640);
    gtk_widget_add_css_class(m_apply, "suggested-action");
    adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), false);
    adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), false);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), notNowButton);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), m_apply);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), GTK_WIDGET(m_banner));
    adw_preferences_page_set_description(
        m_page, "Settings vitrine gives the VMs it makes now, which this VM was made without. "
                "Pick those to take: vitrine saves vm.args as a dated copy beside it first, "
                "changes only their lines, and Undo on the Details puts them back.");
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), GTK_WIDGET(m_page));
    adw_dialog_set_child(dialog(), view);
    adw_dialog_set_default_widget(dialog(), m_apply);

    on<void()>(notNowButton, "clicked", [this]() { notNow(); });
    on<void()>(m_apply, "clicked", [this]() { apply(); });
    /* vm.args edited, the VM started or stopped, a choice made in its Details */
    QObject::connect(SettingsUpdatesMonitor::of(vm), &SettingsUpdatesMonitor::changed, qt(),
                     [this]() { rebuild(); });
    rebuild();
}

QStringList SettingsUpdatesDialog::offered() const
{
    QStringList out;

    for (const Change &c : m_changes) {
        out << c.id;
    }
    return out;
}

QStringList SettingsUpdatesDialog::declined() const
{
    QStringList out;

    for (const Change &c : m_declined) {
        out << c.id;
    }
    return out;
}

bool SettingsUpdatesDialog::isChecked(const QString &id) const
{
    return m_checked.value(id, true);
}

void SettingsUpdatesDialog::setChecked(const QString &id, bool checked)
{
    if (GtkWidget *check = m_checks.value(id)) {
        gtk_check_button_set_active(GTK_CHECK_BUTTON(check), checked);
    }
}

bool SettingsUpdatesDialog::canApply() const
{
    if (!m_vm || !SettingsUpdatesMonitor::of(m_vm)->blocked().isEmpty()) {
        return false;
    }
    for (const Change &c : m_changes) {
        if (isChecked(c.id)) {
            return true;
        }
    }
    return false;
}

void SettingsUpdatesDialog::rebuild()
{
    if (!m_vm) {
        forceClose();
        return;
    }
    SettingsUpdatesMonitor *monitor = SettingsUpdatesMonitor::of(m_vm);
    const QList<Change> offered = monitor->offered();
    const QList<Change> declined = monitor->declined();

    /* the same list: what changed is whether the VM runs */
    if (m_group && offered == m_changes && declined == m_declined) {
        update();
        return;
    }
    /* changed by something else than a choice made here: vm.args edited */
    if (m_group && !m_ownChange && offered != m_changes) {
        m_stale = true;
    }
    m_ownChange = false;
    if (m_group) {
        adw_preferences_page_remove(m_page, m_group);
    }
    if (m_declinedGroup) {
        adw_preferences_page_remove(m_page, m_declinedGroup);
    }
    m_group = nullptr;
    m_declinedGroup = nullptr;
    m_checks.clear();
    m_changes = offered;
    m_declined = declined;

    m_group = Gtk4::group({}, offered.isEmpty() ? QString("Nothing left to take.") : QString());
    for (const Change &c : offered) {
        GtkWidget *row = adw_expander_row_new();
        GtkWidget *check = gtk_check_button_new();
        const QString id = c.id;

        Gtk4::setTitle(row, c.title);
        adw_expander_row_set_subtitle(ADW_EXPANDER_ROW(row),
                                      utf8(c.reason.toHtmlEscaped()).constData());
        adw_expander_row_set_subtitle_lines(ADW_EXPANDER_ROW(row), 0);
        gtk_widget_set_valign(check, GTK_ALIGN_CENTER);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(check), isChecked(id));
        gtk_widget_set_tooltip_text(check, "Take it");
        adw_expander_row_add_prefix(ADW_EXPANDER_ROW(row), check);
        on<void()>(check, "toggled", [this, id, check]() {
            m_checked.insert(id, gtk_check_button_get_active(GTK_CHECK_BUTTON(check)));
            update();
        });
        m_checks.insert(id, check);

        adw_expander_row_add_row(ADW_EXPANDER_ROW(row), linesOf(c));
        GtkWidget *since = Gtk4::actionRow(
            QString("New VMs have it since %1").arg(c.since.toString("d MMMM yyyy")));
        GtkWidget *decline = Gtk4::rowTextButton(
            "_Don't Ask Again", "Not suggested again for this VM: Ask Again in its Details brings "
                                "it back");
        adw_action_row_add_suffix(ADW_ACTION_ROW(since), decline);
        on<void()>(decline, "clicked", [this, id]() { this->decline(id); });
        adw_expander_row_add_row(ADW_EXPANDER_ROW(row), since);
        adw_preferences_group_add(m_group, row);
    }
    adw_preferences_page_add(m_page, m_group);

    if (!declined.isEmpty()) {
        m_declinedGroup = Gtk4::group("Not Asked Again");
        for (const Change &c : declined) {
            GtkWidget *row = Gtk4::actionRow(c.title, c.reason);
            GtkWidget *again = Gtk4::rowTextButton("_Ask Again");
            const QString id = c.id;
            adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 0);
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), again);
            on<void()>(again, "clicked", [this, id]() { askAgain(id); });
            adw_preferences_group_add(m_declinedGroup, row);
        }
        adw_preferences_page_add(m_page, m_declinedGroup);
    }
    update();
}

void SettingsUpdatesDialog::update()
{
    m_note.clear();
    if (m_vm) {
        const QString blocked = SettingsUpdatesMonitor::of(m_vm)->blocked();
        if (m_vm->runner()->isActive()) {
            m_note = QString("Shut %1 down to apply them.").arg(m_vm->name());
        } else if (!blocked.isEmpty()) {
            m_note = sentence(blocked);
        } else if (m_stale) {
            m_note = "vm.args changed meanwhile: the list is up to date again.";
        }
    }
    adw_banner_set_title(m_banner, utf8(m_note.toHtmlEscaped()).constData());
    adw_banner_set_revealed(m_banner, !m_note.isEmpty());
    gtk_widget_set_sensitive(m_apply, canApply());
}

void SettingsUpdatesDialog::apply()
{
    if (!canApply()) {
        return;
    }
    if (m_confirm) {
        m_confirm(m_vm, guarded<void(bool)>([this](bool go) {
                      if (go) {
                          applyNow();
                      }
                  }));
        return;
    }
    applyNow();
}

void SettingsUpdatesDialog::applyNow()
{
    QList<Change> chosen;
    QString error;
    bool stale = false;

    if (!m_vm) {
        return;
    }
    for (const Change &c : std::as_const(m_changes)) {
        if (isChecked(c.id)) {
            chosen << c;
        }
    }
    if (SettingsUpdatesMonitor::of(m_vm)->apply(chosen, &error, &stale)) {
        forceClose();
        return;
    }
    if (stale) {
        /* the monitor checked again: the list is new, and says so */
        m_stale = true;
        update();
        return;
    }
    present(Gtk4::alert("Cannot Apply the Settings", sentence(error)));
}

void SettingsUpdatesDialog::decline(const QString &id)
{
    if (m_vm) {
        m_checked.remove(id);
        m_ownChange = true;
        SettingsUpdatesMonitor::of(m_vm)->setDeclined(id, true);
    }
}

void SettingsUpdatesDialog::askAgain(const QString &id)
{
    if (m_vm) {
        m_ownChange = true;
        SettingsUpdatesMonitor::of(m_vm)->setDeclined(id, false);
    }
}

void SettingsUpdatesDialog::notNow()
{
    if (m_vm) {
        SettingsUpdatesMonitor::of(m_vm)->setNotNow();
    }
    forceClose();
}
