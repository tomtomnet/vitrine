// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/settingsdialog.h"

#include <QSettings>

#include <cstring>
#include <iterator>

#include "core/paths.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "gtk/argspage.h"
#include "gtk/displaypage.h"
#include "gtk/machinepage.h"
#include "gtk/settingspages.h"
#include "gtk/sharespage.h"
#include "gtk/storagepage.h"
#include "gtk/widgets.h"

using Gtk4::utf8;

/* By name, as the Qt app's settings file keeps them (VmPane::pageKey()) */
static const char *const kPageKeys[] = {"general",  "hardware", "display", "storage",
                                        "shares",   "usb",      "network", "machine",
                                        "pci",      "arguments"};
static_assert(std::size(kPageKeys) == size_t(SettingsDialog::Arguments) + 1);

/* The settings file's key of the page shown last */
static constexpr char kPageSetting[] = "gtk/settingsPage";

const char *SettingsDialog::pageKey(Page page)
{
    return kPageKeys[qBound(0, int(page), int(Arguments))];
}

SettingsDialog::Page SettingsDialog::pageOf(const QString &key)
{
    for (int i = 0; i <= Arguments; i++) {
        if (key == kPageKeys[i]) {
            return Page(i);
        }
    }
    return General;
}

SettingsDialog::SettingsDialog(Vm *vm, VmStore *store, Presenter presenter,
                               ResetFirmware resetFirmware)
    : Dialog(adw_dialog_new(), std::move(presenter)), m_vm(vm),
      m_resetFirmware(std::move(resetFirmware)),
      m_split(ADW_NAVIGATION_SPLIT_VIEW(adw_navigation_split_view_new())),
      m_stack(ADW_VIEW_STACK(adw_view_stack_new())),
      m_running(ADW_BANNER(adw_banner_new(
          "The VM is running: the changes apply the next time it starts."))),
      m_apply(gtk_button_new_with_mnemonic("_Apply"))
{
    GtkWidget *sidebarView = adw_toolbar_view_new();
    GtkWidget *sidebarHeader = adw_header_bar_new();
    GtkWidget *switcher = adw_view_switcher_sidebar_new();
    GtkWidget *contentView = adw_toolbar_view_new();
    GtkWidget *contentHeader = adw_header_bar_new();
    const QString name = vm->name();

    adw_dialog_set_title(dialog(), utf8(QString("%1 Settings").arg(name)).constData());
    adw_dialog_set_content_width(dialog(), 920);
    adw_dialog_set_content_height(dialog(), 660);
    /* the least a dialog with breakpoints takes: GNOME's narrowest window */
    gtk_widget_set_size_request(GTK_WIDGET(dialog()), 360, 294);

    /* The pages, down the side: the VM they are of over them */
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(sidebarHeader),
                                    adw_window_title_new("Settings", utf8(name).constData()));
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(sidebarView), sidebarHeader);
    adw_view_switcher_sidebar_set_stack(ADW_VIEW_SWITCHER_SIDEBAR(switcher), m_stack);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(sidebarView), switcher);
    AdwNavigationPage *sidebar = adw_navigation_page_new(sidebarView, "Settings");

    /* The page shown, Apply over it */
    gtk_widget_add_css_class(m_apply, "suggested-action");
    gtk_widget_set_tooltip_text(m_apply, "Save the changes (Ctrl+S)");
    gtk_widget_set_name(m_apply, "apply");
    adw_header_bar_pack_end(ADW_HEADER_BAR(contentHeader), m_apply);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(contentView), contentHeader);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(contentView), GTK_WIDGET(m_running));
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(contentView), GTK_WIDGET(m_stack));
    m_content = adw_navigation_page_new(contentView, "General");

    adw_navigation_split_view_set_sidebar(m_split, sidebar);
    adw_navigation_split_view_set_content(m_split, m_content);
    adw_navigation_split_view_set_min_sidebar_width(m_split, 200);
    adw_navigation_split_view_set_max_sidebar_width(m_split, 260);
    adw_dialog_set_child(dialog(), GTK_WIDGET(m_split));

    /* narrow: the list of pages, a page over it */
    AdwBreakpoint *narrow = adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 600sp"));
    adw_breakpoint_add_setters(narrow, G_OBJECT(m_split), "collapsed", true, nullptr);
    adw_breakpoint_add_setters(narrow, G_OBJECT(switcher), "mode", ADW_SIDEBAR_MODE_PAGE, nullptr);
    adw_dialog_add_breakpoint(dialog(), narrow);

    /* in the order of the list; the Qt app's Page order names them */
    addPage(General, std::make_unique<GeneralPage>(vm));
    addPage(Hardware, std::make_unique<HardwarePage>());
    addPage(Display, std::make_unique<DisplayPage>());
    addPage(Storage, std::make_unique<StoragePage>(vm->dir()));
    addPage(Network, std::make_unique<NetworkPage>(vm));
    addPage(SharedFolders, std::make_unique<SharesPage>());
    addPage(UsbDevices, std::make_unique<UsbPage>());
    addPage(Machine, std::make_unique<MachinePage>(vm, m_resetFirmware));
    addPage(PciDevices, std::make_unique<PciPage>());
    addPage(Arguments, std::make_unique<ArgumentsPage>(vm->dir()));

    on<void()>(switcher, "activated",
               [this]() { adw_navigation_split_view_set_show_content(m_split, true); });
    on<void(GParamSpec *)>(m_stack, "notify::visible-child-name", [this](GParamSpec *) {
        const char *shown = adw_view_stack_get_visible_child_name(m_stack);
        if (shown && (m_current < 0 || strcmp(shown, kPageKeys[m_current]) != 0)) {
            switchTo(pageOf(QString::fromUtf8(shown)));
        }
    });
    on<void()>(m_apply, "clicked", [this]() { apply(); });
    /* closing with changes not applied: asked first */
    on<void()>(dialog(), "close-attempt", [this]() {
        confirmChanges("Apply them before closing?", [this](bool go) {
            if (go) {
                forceClose();
            }
        });
    });
    /* Ctrl+S applies, wherever the keys are in the dialog */
    {
        GtkEventController *keys = gtk_shortcut_controller_new();
        GtkShortcutAction *action = gtk_callback_action_new(
            [](GtkWidget *, GVariant *, gpointer self) -> gboolean {
                auto *settings = static_cast<SettingsDialog *>(self);
                if (settings->isModified()) {
                    settings->apply();
                }
                return true;
            },
            this, nullptr);
        gtk_shortcut_controller_add_shortcut(
            GTK_SHORTCUT_CONTROLLER(keys),
            gtk_shortcut_new(gtk_shortcut_trigger_parse_string("<Control>s"), action));
        gtk_shortcut_controller_set_scope(GTK_SHORTCUT_CONTROLLER(keys), GTK_SHORTCUT_SCOPE_MANAGED);
        gtk_widget_add_controller(GTK_WIDGET(dialog()), keys);
    }

    QObject::connect(vm, &Vm::changed, qt(), [this]() { vmChanged(); });
    QObject::connect(vm->runner(), &VmRunner::stateChanged, qt(), [this]() { update(); });
    /* the VM gone: nothing to set */
    QObject::connect(store, &VmStore::removed, qt(), [this](const QString &id) {
        if (!m_vm || m_vm->id() == id) {
            forceClose();
        }
    });

    m_args = vm->args();
    m_loaded = m_args.toText();
    const QSettings settings(Paths::settingsPath(), QSettings::IniFormat);
    const Page shown = pageOf(settings.value(kPageSetting).toString());
    switchTo(m_pages[shown] ? shown : General);
}

SettingsDialog::~SettingsDialog()
{
    /* the pages go before the dialog's widgets: nothing of theirs runs meanwhile */
    stopCallbacks();
}

void SettingsDialog::addPage(Page page, std::unique_ptr<SettingsPage> settings)
{
    SettingsPage *p = settings.get();
    AdwViewStackPage *stackPage = adw_view_stack_add_titled_with_icon(
        m_stack, p->widget(), kPageKeys[page], utf8(p->title()).constData(), p->icon());

    /* the pages most VMs never need, apart: from the first of them in the list */
    const bool advanced = page >= Machine;
    bool started = false;
    for (int other = Machine; other <= Arguments; other++) {
        started |= bool(m_pages[other]);
    }
    if (advanced && !started) {
        adw_view_stack_page_set_starts_section(stackPage, true);
        adw_view_stack_page_set_section_title(stackPage, "Advanced");
    }
    p->setChanged([this]() { update(); });
    p->setPresenter(presenter());
    m_pages[page] = std::move(settings);
}

Vm *SettingsDialog::vm() const
{
    return m_vm;
}

SettingsDialog::Page SettingsDialog::page() const
{
    return m_current < 0 ? General : Page(m_current);
}

void SettingsDialog::setPage(Page page)
{
    if (m_pages[page]) {
        switchTo(page);
        /* narrow: the page, over the list */
        adw_navigation_split_view_set_show_content(m_split, true);
    }
}

SettingsPage *SettingsDialog::pageWidget(Page page) const
{
    return m_pages[page].get();
}

void SettingsDialog::switchTo(Page page)
{
    if (!m_pages[page] || !m_vm) {
        return;
    }
    if (page == m_current) {
        return;
    }
    /* what the page left changed goes to the arguments, for the next page */
    if (m_current >= 0 && m_pages[m_current]->isModified()) {
        m_pages[m_current]->save(m_args);
    }
    m_current = page;
    m_pages[page]->load(m_args);
    adw_view_stack_set_visible_child_name(m_stack, kPageKeys[page]);
    adw_navigation_page_set_title(m_content, utf8(m_pages[page]->title()).constData());
    update();
}

bool SettingsDialog::isModified() const
{
    return m_vm && (m_args.toText() != m_loaded ||
                    (m_current >= 0 && m_pages[m_current]->isModified()));
}

bool SettingsDialog::apply()
{
    QString error;

    if (!m_vm) {
        return false;
    }
    if (m_current >= 0 && m_pages[m_current]->isModified()) {
        m_pages[m_current]->save(m_args);
    }
    /* new disks, firmware copies: now that the arguments are final */
    for (const auto &page : m_pages) {
        if (page && !page->commit(m_args, m_vm->dir(), &error)) {
            present(Gtk4::alert("Cannot Save the Settings", error));
            return false;
        }
    }
    if (m_args.toText() != m_vm->args().toText() && !m_vm->save(m_args, &error)) {
        present(Gtk4::alert("Cannot Save the Settings", error));
        return false;
    }
    m_args = m_vm->args();
    m_loaded = m_args.toText();
    if (m_current >= 0) {
        m_pages[m_current]->load(m_args);
    }
    update();
    return true;
}

void SettingsDialog::discard()
{
    if (!m_vm) {
        return;
    }
    m_args = m_vm->args();
    m_loaded = m_args.toText();
    if (m_current >= 0) {
        m_pages[m_current]->load(m_args);
    }
    update();
}

void SettingsDialog::confirmChanges(const QString &question, std::function<void(bool go)> done)
{
    if (!isModified()) {
        done(true);
        return;
    }
    if (m_asking) {
        return;
    }
    m_asking = true;
    AdwDialog *alert = Gtk4::alert(
        "Unsaved Changes",
        QString("The settings of %1 have changes that are not applied.").arg(m_vm->name()) +
            "\n\n" + question,
        {{"cancel", "_Cancel"}, {"discard", "_Discard"}, {"apply", "_Apply"}}, "apply");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(alert), "discard",
                                             ADW_RESPONSE_DESTRUCTIVE);
    Gtk4::onResponse(alert, guarded<void(const QString &)>([this, done](const QString &response) {
        m_asking = false;
        if (response == "apply") {
            done(apply());
        } else if (response == "discard") {
            discard();
            done(true);
        } else {
            done(false);
        }
    }));
    present(alert);
}

/* vm.args saved, here or by hand: the pages show it, unless they have changes */
void SettingsDialog::vmChanged()
{
    if (!m_vm) {
        return;
    }
    if (!isModified()) {
        m_args = m_vm->args();
        m_loaded = m_args.toText();
        if (m_current >= 0) {
            m_pages[m_current]->load(m_args);
        }
    }
    update();
}

void SettingsDialog::update()
{
    const bool modified = isModified();

    gtk_widget_set_sensitive(m_apply, modified);
    adw_dialog_set_can_close(dialog(), !modified);
    adw_banner_set_revealed(m_running, m_vm && m_vm->runner()->isActive());
}

void SettingsDialog::closed()
{
    /* the page to show next time */
    QSettings(Paths::settingsPath(), QSettings::IniFormat).setValue(kPageSetting,
                                                                    pageKey(page()));
}
