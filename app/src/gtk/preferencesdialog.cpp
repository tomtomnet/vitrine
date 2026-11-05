// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/preferencesdialog.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QTimer>

#include "core/buildmanager.h"
#include "core/guesttoolsbuilder.h"
#include "core/paths.h"
#include "core/stackbuilder.h"
#include "core/vmstore.h"
#include "gtk/hostprefs.h"
#include "gtk/richtext.h"
#include "gtk/rows.h"
#include "gtk/widgets.h"
#include "ui/buildtexts.h"
#include "ui/preferencestext.h"
#include "ui/qemudocs.h"

using Gtk4::utf8;

/* A row that only says something: a subtitle of @markup, no title */
static GtkWidget *noteRow(const char *name)
{
    GtkWidget *row = adw_action_row_new();

    gtk_widget_set_name(row, name);
    adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 0);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), false);
    return row;
}

static void setNote(GtkWidget *row, const QString &markup)
{
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), utf8(markup).constData());
}

/*
 * @text in @entry without its apply button showing: what the user typed
 * shows the button, a path chosen from a file dialog is applied at once
 */
static void setTextApplied(GtkWidget *entry, const QString &text)
{
    adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(entry), false);
    Gtk4::setText(entry, text);
    adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(entry), true);
}

static AdwPreferencesPage *page(const char *name, const char *title, const char *icon)
{
    auto *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());

    adw_preferences_page_set_name(page, name);
    adw_preferences_page_set_title(page, title);
    adw_preferences_page_set_icon_name(page, icon);
    return page;
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

PreferencesDialog::PreferencesDialog(VmStore *store, Presenter presenter, ShowBuilds showBuilds)
    : Dialog(adw_preferences_dialog_new(), std::move(presenter)), m_store(store),
      m_showBuilds(std::move(showBuilds)), m_check(new QTimer(qt())),
      m_general(page("general", "General", "preferences-system-symbolic"))
{
    auto *preferences = ADW_PREFERENCES_DIALOG(dialog());

    adw_dialog_set_title(dialog(), "Preferences");
    buildGeneral(m_general);
    adw_preferences_dialog_add(preferences, m_general);
    m_host = std::make_unique<HostPrefs>(Dialog::presenter());
    adw_preferences_dialog_add(preferences, m_host->page());

    /* checked as it is typed, as the Qt dialog checks it: a moment after the last key */
    m_check->setSingleShot(true);
    m_check->setInterval(400);
    QObject::connect(m_check, &QTimer::timeout, qt(), [this]() { checkQemu(); });
    QObject::connect(StackBuilder::instance(), &StackBuilder::started, qt(), [this]() { updateStack(); });
    QObject::connect(StackBuilder::instance(), &StackBuilder::finished, qt(), [this]() { updateStack(); });
    GuestToolsBuilder *tools = GuestToolsBuilder::instance();
    QObject::connect(tools, &GuestToolsBuilder::started, qt(), [this]() { updateTools(); });
    QObject::connect(tools, &GuestToolsBuilder::stepStarted, qt(), [this]() { updateTools(); });
    QObject::connect(tools, &GuestToolsBuilder::waitingChanged, qt(), [this]() { updateTools(); });
    QObject::connect(tools, &GuestToolsBuilder::finished, qt(), [this]() { updateTools(); });
    QObject::connect(BuildManager::instance(), &BuildManager::guestReleaseChanged, qt(),
                     [this]() { updateTools(); });

    updateStack();
    updateTools();
    checkQemu();
    checkVirtiofsd();
    updateVmsDir();
}

PreferencesDialog::~PreferencesDialog()
{
    if (m_version) {
        m_version->disconnect(qt());
        m_version->kill();
        m_version->waitForFinished(1000);
    }
}

void PreferencesDialog::buildGeneral(AdwPreferencesPage *general)
{
    const QString custom = Paths::customQemuBinary();
    const QString virtiofsd = Paths::virtiofsd();

    /* QEMU: Vitrine's, built from here, or another */
    AdwPreferencesGroup *qemu = Gtk4::group("QEMU");
    m_stack = Gtk4::actionRow("Vitrine's QEMU");
    gtk_widget_set_name(m_stack, "qemu-build");
    adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(m_stack), 0);
    m_stackButton = Gtk4::rowTextButton("Build…");
    gtk_widget_set_name(m_stackButton, "qemu-build-button");
    adw_action_row_add_suffix(ADW_ACTION_ROW(m_stack), m_stackButton);
    on<void()>(m_stackButton, "clicked", [this]() {
        if (m_showBuilds) {
            m_showBuilds("qemu", QString());
        }
    });
    adw_preferences_group_add(qemu, m_stack);

    m_custom = adw_expander_row_new();
    gtk_widget_set_name(m_custom, "custom-qemu");
    Gtk4::setTitle(m_custom, "Use Another QEMU");
    Gtk4::setSubtitle(m_custom, PreferencesText::customQemuTip());
    adw_expander_row_set_show_enable_switch(ADW_EXPANDER_ROW(m_custom), true);
    adw_expander_row_set_enable_expansion(ADW_EXPANDER_ROW(m_custom), !custom.isEmpty());
    adw_expander_row_set_expanded(ADW_EXPANDER_ROW(m_custom), !custom.isEmpty());
    m_qemu = Gtk4::entryRow("QEMU Binary", custom);
    gtk_widget_set_name(m_qemu, "custom-qemu-path");
    adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(m_qemu), true);
    GtkWidget *browse = Gtk4::rowButton("document-open-symbolic", "Choose the QEMU Binary");
    adw_entry_row_add_suffix(ADW_ENTRY_ROW(m_qemu), browse);
    m_qemuStatus = noteRow("custom-qemu-status");
    adw_expander_row_add_row(ADW_EXPANDER_ROW(m_custom), m_qemu);
    adw_expander_row_add_row(ADW_EXPANDER_ROW(m_custom), m_qemuStatus);
    on<void(GParamSpec *)>(m_custom, "notify::enable-expansion", [this](GParamSpec *) {
        applyQemu();
        if (adw_expander_row_get_enable_expansion(ADW_EXPANDER_ROW(m_custom))) {
            gtk_widget_grab_focus(m_qemu);
        }
    });
    on<void()>(m_qemu, "apply", [this]() { applyQemu(); });
    on<void()>(m_qemu, "changed", [this]() { m_check->start(); });
    on<void()>(browse, "clicked", [this, browse]() {
        Gtk4::chooseFile(browse, "QEMU Binary", {}, Gtk4::text(m_qemu).trimmed(),
                         guarded<void(const QString &)>([this](const QString &path) {
            setTextApplied(m_qemu, path);
            applyQemu();
        }));
    });
    adw_preferences_group_add(qemu, m_custom);
    adw_preferences_page_add(general, qemu);

    /* the guest tools' medium, built from here as QEMU is */
    AdwPreferencesGroup *tools = Gtk4::group("Guest Tools");
    m_tools = Gtk4::actionRow("Guest Tools");
    gtk_widget_set_name(m_tools, "guest-tools");
    adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(m_tools), 0);
    m_toolsButton = Gtk4::rowTextButton("Build…");
    gtk_widget_set_name(m_toolsButton, "guest-tools-button");
    adw_action_row_add_suffix(ADW_ACTION_ROW(m_tools), m_toolsButton);
    on<void()>(m_toolsButton, "clicked", [this]() {
        if (m_showBuilds) {
            m_showBuilds("medium", BuildManager::instance()->guestRelease());
        }
    });
    adw_preferences_group_add(tools, m_tools);
    adw_preferences_page_add(general, tools);

    /* virtiofsd: empty for the one found, as the shared folders' page says */
    AdwPreferencesGroup *shares = Gtk4::group("Shared Folders");
    m_virtiofsd = Gtk4::entryRow("virtiofsd",
                                 virtiofsd == PreferencesText::autoVirtiofsd() ? QString() : virtiofsd);
    gtk_widget_set_name(m_virtiofsd, "virtiofsd");
    adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(m_virtiofsd), true);
    GtkWidget *browseVirtiofsd = Gtk4::rowButton("document-open-symbolic", "Choose virtiofsd");
    adw_entry_row_add_suffix(ADW_ENTRY_ROW(m_virtiofsd), browseVirtiofsd);
    m_virtiofsdStatus = noteRow("virtiofsd-status");
    on<void()>(m_virtiofsd, "apply", [this]() { applyVirtiofsd(); });
    on<void()>(m_virtiofsd, "changed", [this]() { checkVirtiofsd(); });
    on<void()>(browseVirtiofsd, "clicked", [this, browseVirtiofsd]() {
        const QString shown = Gtk4::text(m_virtiofsd).trimmed();
        Gtk4::chooseFile(browseVirtiofsd, "virtiofsd", {},
                         shown.isEmpty() ? PreferencesText::autoVirtiofsd() : shown,
                         guarded<void(const QString &)>([this](const QString &path) {
            setTextApplied(m_virtiofsd, path);
            applyVirtiofsd();
        }));
    });
    adw_preferences_group_add(shares, m_virtiofsd);
    adw_preferences_group_add(shares, m_virtiofsdStatus);
    adw_preferences_page_add(general, shares);

    /* the folder of the VMs, which the window lists */
    AdwPreferencesGroup *vms = Gtk4::group("Virtual Machines");
    m_vmsDir = Gtk4::actionRow("Folder of the VMs");
    gtk_widget_set_name(m_vmsDir, "vms-dir");
    adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(m_vmsDir), true);
    GtkWidget *open = Gtk4::rowButton("folder-open-symbolic", "Open the Folder");
    GtkWidget *change = Gtk4::rowTextButton("_Change…", PreferencesText::vmsDirTip());
    gtk_widget_set_name(change, "vms-dir-change");
    m_vmsDirDefault = Gtk4::rowTextButton("_Default", "Back to the folder Vitrine uses at first");
    gtk_widget_set_name(m_vmsDirDefault, "vms-dir-default");
    adw_action_row_add_suffix(ADW_ACTION_ROW(m_vmsDir), open);
    adw_action_row_add_suffix(ADW_ACTION_ROW(m_vmsDir), change);
    adw_action_row_add_suffix(ADW_ACTION_ROW(m_vmsDir), m_vmsDirDefault);
    on<void()>(open, "clicked", [open]() { Gtk4::openPath(open, Paths::vmsDir()); });
    on<void()>(change, "clicked", [this, change]() {
        Gtk4::chooseFolder(change, "Folder of the VMs", Paths::vmsDir(),
                           guarded<void(const QString &)>([this](const QString &dir) { setVmsDir(dir); }));
    });
    on<void()>(m_vmsDirDefault, "clicked", [this]() { setVmsDir(Paths::defaultVmsDir()); });
    m_vmsDirState = noteRow("vms-dir-state");
    adw_preferences_group_add(vms, m_vmsDir);
    adw_preferences_group_add(vms, m_vmsDirState);
    adw_preferences_page_add(general, vms);

    /* Vitrine's own updates */
    AdwPreferencesGroup *updates = Gtk4::group("Updates");
    m_updates = Gtk4::switchRow("Check GitHub for Updates Once a Day", PreferencesText::updatesHint());
    gtk_widget_set_name(m_updates, "check-updates");
    adw_switch_row_set_active(ADW_SWITCH_ROW(m_updates),
                              QSettings(Paths::settingsPath(), QSettings::IniFormat)
                                  .value("updates/check", true)
                                  .toBool());
    on<void(GParamSpec *)>(m_updates, "notify::active", [this](GParamSpec *) {
        QSettings(Paths::settingsPath(), QSettings::IniFormat)
            .setValue("updates/check", bool(adw_switch_row_get_active(ADW_SWITCH_ROW(m_updates))));
    });
    adw_preferences_group_add(updates, m_updates);
    adw_preferences_page_add(general, updates);
}

void PreferencesDialog::setPage(Page page)
{
    adw_preferences_dialog_set_visible_page_name(ADW_PREFERENCES_DIALOG(dialog()),
                                                 page == Host ? "host" : "general");
}

GtkWidget *PreferencesDialog::find(const char *name) const
{
    /* the pages: children of the dialog once presented, else its own */
    for (GtkWidget *root : {GTK_WIDGET(m_general), GTK_WIDGET(m_host->page())}) {
        if (GtkWidget *found = named(root, name)) {
            return found;
        }
    }
    return nullptr;
}

void PreferencesDialog::updateStack()
{
    const StackBuilder::State state = StackBuilder::state();
    const StackBuilder::Build build = StackBuilder::current();
    const bool running = StackBuilder::instance()->isRunning();
    const QString now = running ? PreferencesText::stackBuilding() : BuildTexts::explain(state, build);

    Gtk4::setSubtitle(m_stack, BuildTexts::describe(build) + '\n' + now);
    gtk_widget_set_tooltip_text(m_stack, build.isValid() ? utf8(build.qemuBinary()).constData()
                                                         : nullptr);
    gtk_button_set_label(GTK_BUTTON(m_stackButton),
                         utf8(PreferencesText::stackButton(running, state)).constData());
    gtk_widget_set_sensitive(m_stackButton, state != StackBuilder::State::NoSources || running);
}

void PreferencesDialog::updateTools()
{
    const GuestToolsBuilder *builder = GuestToolsBuilder::instance();
    /* the release the Builds window shows: the newest, unless another was chosen */
    const QString release = BuildManager::instance()->guestRelease();
    const GuestToolsBuilder::State state = GuestToolsBuilder::releaseState(release);
    const bool running = builder->isRunning();
    const QString now = running ? PreferencesText::toolsBuilding(builder->progressText(false))
                                : BuildTexts::explain(state);

    Gtk4::setTitle(m_tools, QString("Guest Tools for Fedora %1").arg(release));
    Gtk4::setSubtitle(m_tools, BuildTexts::describeGuestTools(release) + '\n' + now);
    gtk_widget_set_tooltip_text(m_tools, utf8(GuestToolsBuilder::mediumImage(release)).constData());
    gtk_button_set_label(GTK_BUTTON(m_toolsButton),
                         utf8(PreferencesText::toolsButton(running, state)).constData());
    gtk_widget_set_sensitive(m_toolsButton, state != GuestToolsBuilder::State::NoSources || running);
}

void PreferencesDialog::applyQemu()
{
    const bool on = adw_expander_row_get_enable_expansion(ADW_EXPANDER_ROW(m_custom));
    /* VMs with a #qemu line of their own keep it, whatever is chosen here */
    const QString binary = on ? Gtk4::text(m_qemu).trimmed() : QString();

    if (binary != Paths::customQemuBinary()) {
        Paths::setQemuBinary(binary);
        QemuDocs::reloadPreferred();
    }
    checkQemu();
}

void PreferencesDialog::checkQemu()
{
    const QString binary = Gtk4::text(m_qemu).trimmed();

    m_check->stop();
    if (m_version) {
        m_version->disconnect(qt());
        m_version->kill();
        m_version->deleteLater();
        m_version = nullptr;
    }
    if (!adw_expander_row_get_enable_expansion(ADW_EXPANDER_ROW(m_custom))) {
        return;
    }
    if (binary.isEmpty()) {
        setNote(m_qemuStatus, Gtk4::markup(PreferencesText::qemuNone()));
        return;
    }
    if (!QFileInfo(binary).isExecutable()) {
        setNote(m_qemuStatus, Gtk4::markup(PreferencesText::notExecutable()));
        return;
    }
    setNote(m_qemuStatus, Gtk4::markup(PreferencesText::checking()));
    auto *version = new QProcess(qt());
    m_version = version;
    version->setProcessChannelMode(QProcess::MergedChannels);
    QObject::connect(version, &QProcess::finished, qt(), [this, version]() {
        setNote(m_qemuStatus,
                Gtk4::markup(PreferencesText::qemuVersion(QString::fromLocal8Bit(version->readAll()))));
        version->deleteLater();
        m_version = nullptr;
    });
    QObject::connect(version, &QProcess::errorOccurred, qt(),
                     [this, version](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            setNote(m_qemuStatus,
                    Gtk4::escaped(PreferencesText::cannotRun(version->errorString())));
            version->deleteLater();
            m_version = nullptr;
        }
    });
    version->start(binary, {"-version"});
}

void PreferencesDialog::applyVirtiofsd()
{
    Paths::setVirtiofsd(Gtk4::text(m_virtiofsd).trimmed());
    checkVirtiofsd();
}

void PreferencesDialog::checkVirtiofsd()
{
    const QString typed = Gtk4::text(m_virtiofsd).trimmed();

    setNote(m_virtiofsdStatus,
            Gtk4::markup(PreferencesText::virtiofsd(typed.isEmpty() ? PreferencesText::autoVirtiofsd()
                                                                    : typed)));
}

void PreferencesDialog::setVmsDir(const QString &dir)
{
    const QString path = QDir(dir).absolutePath();

    if (dir.isEmpty() || path == QDir(Paths::vmsDir()).absolutePath()) {
        return;
    }
    Paths::setVmsDir(path);
    /* the VMs found in the folder chosen, no import needed; the running ones
       of the other stay listed until they stop */
    if (m_store && m_store->dir() != Paths::vmsDir()) {
        m_store->setDir(Paths::vmsDir());
    }
    m_dirChanged = true;
    updateVmsDir();
}

void PreferencesDialog::updateVmsDir()
{
    const QString dir = Paths::vmsDir();

    Gtk4::setSubtitle(m_vmsDir, QDir::toNativeSeparators(dir));
    gtk_widget_set_sensitive(m_vmsDirDefault,
                             QDir(Paths::defaultVmsDir()).absolutePath() != QDir(dir).absolutePath());
    /* once changed here: what the folder chosen holds, and what changed */
    gtk_widget_set_visible(m_vmsDirState, m_dirChanged);
    if (m_dirChanged) {
        setNote(m_vmsDirState, Gtk4::markup(PreferencesText::vmsDirState(dir)));
    }
}

void PreferencesDialog::closed()
{
    /* what the running VMs are tuned with, now rather than in a moment */
    m_host->flush();
    m_check->stop();
}
