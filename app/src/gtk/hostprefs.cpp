// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/hostprefs.h"

#include <QTimer>

#include "core/hostgpus.h"
#include "gtk/notices.h"
#include "gtk/richtext.h"
#include "gtk/rows.h"
#include "gtk/widgets.h"
#include "ui/hosttuningtext.h"

using Gtk4::utf8;

/* The floor's choices, in the combo row's order */
enum Floor { Automatic, Off, AtLeast };

HostPrefs::HostPrefs(Gtk4::Dialog::Presenter present)
    : m_present(std::move(present)), m_retune(new QTimer(&m_qt)),
      m_wasEnabled(HostSettings::enabled()), m_wasFloor(HostSettings::gpuFloor()),
      m_wasCpuLatency(HostSettings::cpuLatency()),
      m_page(ADW_PREFERENCES_PAGE(adw_preferences_page_new()))
{
    const QString floor = HostSettings::gpuFloor();

    adw_preferences_page_set_name(m_page, "host");
    adw_preferences_page_set_title(m_page, "Host");
    adw_preferences_page_set_icon_name(m_page, "computer-symbolic");

    /* on or off, and whether it is active */
    AdwPreferencesGroup *tuning = Gtk4::group("Host Tuning", HostTuningText::summary());
    /* the user's choice, whatever the state: the row under it says that */
    m_tune = Gtk4::switchRow("Tune the Host While VMs Run");
    gtk_widget_set_name(m_tune, "tune-host");
    adw_switch_row_set_active(ADW_SWITCH_ROW(m_tune), HostSettings::enabled());
    m_state = adw_action_row_new();
    gtk_widget_set_name(m_state, "tune-host-state");
    adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(m_state), 0);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(m_state), false);
    m_setUp = Gtk4::rowTextButton(
        "_Set Up…", "Adds you to the vitrine group: asks for an administrator's password");
    gtk_widget_set_name(m_setUp, "tune-host-set-up");
    gtk_widget_add_css_class(m_setUp, "suggested-action");
    adw_action_row_add_suffix(ADW_ACTION_ROW(m_state), m_setUp);
    adw_preferences_group_add(tuning, m_tune);
    adw_preferences_group_add(tuning, m_state);
    adw_preferences_page_add(m_page, tuning);

    /* the GPU clock floor; gpu-input-awake for the dedicated GPUs */
    AdwPreferencesGroup *gpu = Gtk4::group("GPU");
    m_floor = Gtk4::comboRow("GPU Clock Floor", {"Automatic", "Off", "At Least"},
                             HostTuningPrefsText::floorHint());
    gtk_widget_set_name(m_floor, "gpu-floor");
    Gtk4::setSelected(m_floor, floor == "auto" ? Automatic : floor == "off" ? Off : AtLeast);
    m_mhz = Gtk4::spinRow("Lowest Clock", 200, 4000, 100);
    gtk_widget_set_name(m_mhz, "gpu-floor-mhz");
    Gtk4::setUnit(m_mhz, "MHz");
    Gtk4::setValue(m_mhz, Gtk4::selected(m_floor) == AtLeast ? floor.toInt() : 1800);
    adw_preferences_group_add(gpu, m_floor);
    adw_preferences_group_add(gpu, m_mhz);
    for (const HostGpu &each : GpuInputAwake::served(HostGpus::list())) {
        m_dedicated << each.name();
    }
    if (!m_dedicated.isEmpty()) {
        m_awake = Gtk4::actionRow("gpu-input-awake");
        gtk_widget_set_name(m_awake, "gpu-input-awake");
        adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(m_awake), 0);
        m_awakeInstall = Gtk4::rowTextButton("_Install…");
        gtk_widget_set_name(m_awakeInstall, "gpu-input-awake-install");
        m_awakeRemove = Gtk4::rowTextButton("Re_move…");
        gtk_widget_set_name(m_awakeRemove, "gpu-input-awake-remove");
        adw_action_row_add_suffix(ADW_ACTION_ROW(m_awake), m_awakeInstall);
        adw_action_row_add_suffix(ADW_ACTION_ROW(m_awake), m_awakeRemove);
        adw_preferences_group_add(gpu, m_awake);
        on<void()>(m_awakeInstall, "clicked", [this]() { runAwake(true); });
        on<void()>(m_awakeRemove, "clicked", [this]() { runAwake(false); });
        refreshAwake();
    }
    adw_preferences_page_add(m_page, gpu);

    AdwPreferencesGroup *cpu = Gtk4::group("CPU");
    m_cpuLatency = Gtk4::switchRow("Keep the CPU Ready to Wake Up While VMs Run",
                                   HostTuningPrefsText::cpuHint());
    gtk_widget_set_name(m_cpuLatency, "cpu-latency");
    adw_switch_row_set_active(ADW_SWITCH_ROW(m_cpuLatency), HostSettings::cpuLatency());
    adw_preferences_group_add(cpu, m_cpuLatency);
    adw_preferences_page_add(m_page, cpu);

    on<void(GParamSpec *)>(m_tune, "notify::active", [this](GParamSpec *) {
        HostSettings::setEnabled(adw_switch_row_get_active(ADW_SWITCH_ROW(m_tune)));
        update();
        changed();
    });
    on<void(GParamSpec *)>(m_floor, "notify::selected", [this](GParamSpec *) {
        update();
        saveFloor();
    });
    on<void(GParamSpec *)>(m_mhz, "notify::value", [this](GParamSpec *) {
        if (Gtk4::selected(m_floor) == AtLeast) {
            saveFloor();
        }
    });
    on<void(GParamSpec *)>(m_cpuLatency, "notify::active", [this](GParamSpec *) {
        HostSettings::setCpuLatency(adw_switch_row_get_active(ADW_SWITCH_ROW(m_cpuLatency)));
        changed();
    });
    on<void()>(m_setUp, "clicked", [this]() {
        gtk_widget_set_sensitive(m_setUp, false);
        Notices::setUpGroup(m_present, guarded<void()>([this]() {
            gtk_widget_set_sensitive(m_setUp, true);
            refresh();
        }));
    });

    m_retune->setSingleShot(true);
    m_retune->setInterval(500);
    QObject::connect(m_retune, &QTimer::timeout, &m_qt, [this]() { flush(); });
    update();
}

HostPrefs::~HostPrefs()
{
    flush();
    stopCallbacks();
}

void HostPrefs::update()
{
    const bool on = adw_switch_row_get_active(ADW_SWITCH_ROW(m_tune));

    /* off: no warning, whatever the state */
    gtk_widget_set_visible(m_state, on);
    if (on && !m_checked) {
        refresh();
    }
    gtk_widget_set_visible(m_setUp, on && m_needsGroup);
    gtk_widget_set_sensitive(m_floor, on);
    gtk_widget_set_sensitive(m_mhz, on);
    gtk_widget_set_sensitive(m_cpuLatency, on);
    gtk_widget_set_visible(m_mhz, Gtk4::selected(m_floor) == AtLeast);
}

void HostPrefs::refresh()
{
    m_checked = true;
    adw_action_row_set_subtitle(ADW_ACTION_ROW(m_state), "Checking…");
    m_needsGroup = false;
    gtk_widget_set_visible(m_setUp, false);
    /*
     * Without interaction: pkcheck and the user database; through the
     * window's HostSettings, so that its notice says the same (and VMs
     * that ran untuned get tuned if it is active now)
     */
    const auto show = guarded<void(const HostSettings::Status &)>(
        [this](const HostSettings::Status &status) { showState(status); });
    if (HostSettings *host = HostSettings::instance()) {
        host->recheck(&m_qt, show);
    } else {
        HostSettings::check(&m_qt, show);
    }
}

void HostPrefs::showState(const HostSettings::Status &status)
{
    QString text;

    if (status.active()) {
        /* what the helper said in this run, else what the kernel shows: no
           real-time threads without the fair server */
        const HostSettings *host = HostSettings::instance();
        text = HostTuningPrefsText::active(host ? host->realtimeOff() : HostSettings::realtimeLimit());
    } else {
        text = HostTuningPrefsText::notActive(status);
    }
    adw_action_row_set_subtitle(ADW_ACTION_ROW(m_state), utf8(Gtk4::markup(text)).constData());
    m_needsGroup = status.needsGroup();
    gtk_widget_set_visible(m_setUp, m_needsGroup && adw_switch_row_get_active(ADW_SWITCH_ROW(m_tune)));
}

void HostPrefs::saveFloor()
{
    switch (Gtk4::selected(m_floor)) {
    case Automatic:
        HostSettings::setGpuFloor("auto");
        break;
    case Off:
        HostSettings::setGpuFloor("off");
        break;
    default:
        HostSettings::setGpuFloor(QString::number(Gtk4::value(m_mhz)));
        break;
    }
    changed();
}

void HostPrefs::changed()
{
    m_retune->start();
}

void HostPrefs::flush()
{
    m_retune->stop();
    /* for the VMs running now, not only the next ones */
    HostSettings *running = HostSettings::instance();
    if (running && (HostSettings::enabled() != m_wasEnabled ||
                    HostSettings::gpuFloor() != m_wasFloor ||
                    HostSettings::cpuLatency() != m_wasCpuLatency)) {
        running->preferencesChanged();
    }
    m_wasEnabled = HostSettings::enabled();
    m_wasFloor = HostSettings::gpuFloor();
    m_wasCpuLatency = HostSettings::cpuLatency();
}

void HostPrefs::refreshAwake()
{
    if (!m_awake) {
        return;
    }
    m_installed = GpuInputAwake::installed(GpuInputAwake::sourceDir());
    gtk_widget_set_sensitive(m_awakeInstall, false);
    gtk_widget_set_sensitive(m_awakeRemove, false);
    if (m_installed == GpuInputAwake::Installed::No) {
        showAwake(QString());
        return;
    }
    GpuInputAwake::serviceState(&m_qt, guarded<void(const QString &, const QString &)>(
                                           [this](const QString &active, const QString &) {
        showAwake(active);
    }));
}

void HostPrefs::showAwake(const QString &active)
{
    using enum GpuInputAwake::Installed;
    const bool source = !GpuInputAwake::sourceDir().isEmpty();
    const bool busy = GpuInputAwake::running();
    /* both run install.sh of Vitrine's copy */
    const QByteArray tip = source ? "Asks for an administrator's password"
                                  : "This installation of Vitrine has no host/gpu-input-awake";

    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(m_awake),
        utf8(Gtk4::markup(HostTuningPrefsText::awake(m_dedicated, m_installed, active))).constData());
    gtk_widget_set_visible(m_awakeInstall, m_installed != Vitrine);
    gtk_widget_set_sensitive(m_awakeInstall, source && !busy);
    gtk_widget_set_tooltip_text(m_awakeInstall, tip.constData());
    gtk_widget_set_visible(m_awakeRemove, m_installed != No);
    gtk_widget_set_sensitive(m_awakeRemove, source && !busy);
    gtk_widget_set_tooltip_text(m_awakeRemove, tip.constData());
}

void HostPrefs::runAwake(bool install)
{
    const HostTuningText::Box confirm = HostTuningPrefsText::awakeConfirm(install, m_installed);
    AdwDialog *alert = Gtk4::alert(confirm.title, confirm.text,
                                   {{"cancel", "_Cancel"}, {"go", install ? "_Install" : "_Remove"}},
                                   "go", !install);

    Gtk4::onResponse(alert, guarded<void(const QString &)>([this, install, confirm](const QString &response) {
        if (response != "go") {
            return;
        }
        gtk_widget_set_sensitive(m_awakeInstall, false);
        gtk_widget_set_sensitive(m_awakeRemove, false);
        /* before: the answer may come at once, and its state goes over this */
        adw_action_row_set_subtitle(ADW_ACTION_ROW(m_awake),
                                    utf8(Gtk4::escaped(HostTuningPrefsText::awakeBusy(install))).constData());
        const bool started = GpuInputAwake::run(
            &m_qt, install,
            guarded<void(GpuInputAwake::Result, const QString &)>(
                [this, confirm](GpuInputAwake::Result result, const QString &error) {
            if (result == GpuInputAwake::Result::Failed) {
                m_present(Gtk4::alert(confirm.title, HostTuningPrefsText::awakeFailed(error)));
            }
            refreshAwake();
        }));
        if (!started) {
            /* one runs already (another window's) */
            refreshAwake();
        }
    }));
    m_present(alert);
}
