// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/usbdialog.h"

#include <QTimer>

#include "core/usbhotplug.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "gtk/rows.h"
#include "gtk/widgets.h"
#include "ui/usbaccess.h"

using Gtk4::utf8;
using Entry = UsbChoices::Entry;

static int s_refreshMs = 1000;

void UsbDialog::setRefreshInterval(int ms)
{
    s_refreshMs = ms;
}

/* Qt's mnemonic as GTK's: "&Keep" as "_Keep", "&&" as "&" */
static QString mnemonic(QString text)
{
    text.replace("&&", QChar(0x1));
    text.replace('&', '_');
    text.replace(QChar(0x1), '&');
    return text;
}

/* Below a device's name: what the entry says beside it, then what to know of it but its bus */
static QString subtitleOf(const Entry &e)
{
    QStringList lines;

    if (!e.right.isEmpty()) {
        lines << e.right;
    }
    lines << e.tips.mid(1);
    return lines.join('\n');
}

/* Notes and devices: the rows of the group, in their order */
static bool listed(const Entry &e)
{
    return e.kind == Entry::Note || e.kind == Entry::Device;
}

UsbDialog::UsbDialog(VmStore *store, Vm *vm, Presenter presenter, SaveGuard guard,
                     std::function<void(Vm *)> openSettings)
    : Dialog(adw_dialog_new(), std::move(presenter)), m_store(store), m_vm(vm),
      m_guard(std::move(guard)), m_openSettings(std::move(openSettings)),
      m_refresh(new QTimer(qt())),
      m_toasts(ADW_TOAST_OVERLAY(adw_toast_overlay_new())),
      m_title(ADW_WINDOW_TITLE(adw_window_title_new("USB Devices", ""))),
      m_devices(Gtk4::group(QString(), UsbChoices::tip())),
      /* their titles: the entries' (rebuild()) */
      m_keep(Gtk4::buttonRow(QString())),
      m_settings(Gtk4::buttonRow(QString(), "emblem-system-symbolic"))
{
    GtkWidget *view = adw_toolbar_view_new();
    GtkWidget *header = adw_header_bar_new();
    auto *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *commands = Gtk4::group();

    adw_dialog_set_title(dialog(), "USB Devices");
    adw_dialog_set_content_width(dialog(), 520);
    adw_dialog_set_content_height(dialog(), 560);
    adw_window_title_set_subtitle(m_title, vm ? utf8(vm->name()).constData() : "");
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), GTK_WIDGET(m_title));
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    gtk_widget_set_name(m_keep, "usb-keep");
    gtk_widget_set_name(m_settings, "usb-settings");
    adw_preferences_group_add(commands, m_keep);
    adw_preferences_group_add(commands, m_settings);
    adw_preferences_page_add(page, m_devices);
    adw_preferences_page_add(page, commands);
    adw_toast_overlay_set_child(m_toasts, GTK_WIDGET(page));
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), GTK_WIDGET(m_toasts));
    adw_dialog_set_child(dialog(), view);

    on<void()>(m_keep, "activated", [this]() { keep(); });
    on<void()>(m_settings, "activated", [this]() {
        const QPointer<Vm> vm = m_vm;
        forceClose();
        if (vm && m_openSettings) {
            m_openSettings(vm);
        }
    });

    /* devices plugged in or out while it shows */
    QObject::connect(m_refresh, &QTimer::timeout, qt(), [this]() {
        readHosts();
        rebuild();
    });
    /* what each VM has, known as it runs: another VM may have the device asked for */
    const auto follow = [this](Vm *each) {
        QObject::connect(UsbHotplug::of(each->runner()), &UsbHotplug::changed, qt(),
                         [this]() { rebuild(); });
        QObject::connect(each->runner(), &VmRunner::stateChanged, qt(), [this]() { rebuild(); });
    };
    for (Vm *each : store->vms()) {
        follow(each);
        /* what the VMs have now: QEMU's own monitor may have changed it */
        if (UsbChoices::runs(each)) {
            UsbHotplug::of(each->runner())->refresh();
        }
    }
    QObject::connect(store, &VmStore::added, qt(), follow);

    readHosts();
    rebuild();
    m_refresh->start(s_refreshMs);
}

UsbDialog::~UsbDialog() = default;

Vm *UsbDialog::vm() const
{
    return m_vm;
}

GtkWidget *UsbDialog::row(int index) const
{
    if (index < 0 || index >= m_shown.size()) {
        return nullptr;
    }
    switch (m_shown[index].kind) {
    case Entry::Keep:
        return m_keep;
    case Entry::Settings:
        return m_settings;
    default:
        return index < m_rows.size() ? m_rows[index] : nullptr;
    }
}

void UsbDialog::readHosts()
{
    m_hosts = UsbHotplug::hostDevices();
}

void UsbDialog::rebuild()
{
    const QList<Entry> entries = UsbChoices::entries(m_store, m_vm, m_hosts, m_pending);
    QList<Entry> rows, shownRows;

    for (const Entry &e : entries) {
        if (listed(e)) {
            rows << e;
        }
    }
    for (const Entry &e : std::as_const(m_shown)) {
        if (listed(e)) {
            shownRows << e;
        }
    }
    /* made again when the notes or devices are not those shown, else updated in place:
       the switch under the pointer stays */
    bool same = rows.size() == shownRows.size();
    for (qsizetype i = 0; same && i < rows.size(); i++) {
        same = rows[i].kind == shownRows[i].kind && rows[i].text == shownRows[i].text &&
               rows[i].device == shownRows[i].device;
    }
    m_syncing = true;
    if (!same) {
        for (GtkWidget *row : std::as_const(m_rows)) {
            adw_preferences_group_remove(m_devices, row);
        }
        m_rows.clear();
        for (const Entry &e : std::as_const(rows)) {
            GtkWidget *row = e.kind == Entry::Device ? Gtk4::switchRow(e.text) : Gtk4::actionRow(e.text);
            if (e.kind == Entry::Device) {
                const int device = e.device;
                gtk_widget_set_name(row, "usb-device");
                on<void(GParamSpec *)>(row, "notify::active", [this, row, device](GParamSpec *) {
                    if (!m_syncing) {
                        toggle(device, adw_switch_row_get_active(ADW_SWITCH_ROW(row)));
                    }
                });
            } else {
                gtk_widget_set_name(row, "usb-note");
                gtk_widget_add_css_class(row, "dim-label");
            }
            adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 0);
            adw_preferences_group_add(m_devices, row);
            m_rows << row;
        }
    }
    for (qsizetype i = 0; i < rows.size(); i++) {
        const Entry &e = rows[i];
        GtkWidget *row = m_rows[i];
        if (e.kind == Entry::Device) {
            Gtk4::setSubtitle(row, subtitleOf(e));
            gtk_widget_set_tooltip_text(row, utf8(e.tips.value(0)).constData());
            if (bool(adw_switch_row_get_active(ADW_SWITCH_ROW(row))) != e.checked) {
                adw_switch_row_set_active(ADW_SWITCH_ROW(row), e.checked);
            }
        } else {
            Gtk4::setSubtitle(row, e.tips.join('\n'));
        }
        gtk_widget_set_sensitive(row, e.kind == Entry::Note || e.enabled);
    }
    m_syncing = false;

    bool hasKeep = false;
    for (const Entry &e : entries) {
        if (e.kind == Entry::Keep) {
            hasKeep = true;
            Gtk4::setTitle(m_keep, mnemonic(e.text));
            gtk_widget_set_sensitive(m_keep, e.enabled);
            gtk_widget_set_tooltip_text(m_keep, utf8(e.tips.join('\n')).constData());
        } else if (e.kind == Entry::Settings) {
            Gtk4::setTitle(m_settings, mnemonic(e.text));
            gtk_widget_set_sensitive(m_settings, e.enabled);
            gtk_widget_set_tooltip_text(m_settings, utf8(e.tips.join('\n')).constData());
        }
    }
    gtk_widget_set_visible(m_keep, hasKeep);
    m_shown = entries;
}

void UsbDialog::toggle(int device, bool on)
{
    if (device < 0 || device >= m_hosts.size() || !m_vm) {
        return;
    }
    const UsbDevice dev = m_hosts[device];
    if (on) {
        attach(dev);
    } else {
        detach(dev);
    }
}

void UsbDialog::attach(const UsbDevice &dev)
{
    Vm *vm = m_vm;

    if (!vm || !UsbChoices::runs(vm)) {
        rebuild();
        return;
    }
    /* the host's keyboard, mouse or Bluetooth: not without asking */
    if (dev.isInput() || dev.isWireless()) {
        AdwDialog *alert = Gtk4::alert(UsbChoices::confirmTitle(dev, vm),
                                       UsbChoices::confirmText(dev, vm),
                                       {{"cancel", "_Cancel"}, {"attach", "_Attach"}}, "attach", true);
        /* Enter or Escape gives nothing away */
        adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(alert), "cancel");
        Gtk4::onResponse(alert, guarded<void(const QString &)>([this, dev](const QString &response) {
            if (response == "attach") {
                attachNow(dev);
            } else {
                rebuild();
            }
        }));
        present(alert);
        return;
    }
    attachNow(dev);
}

void UsbDialog::attachNow(const UsbDevice &dev)
{
    const QPointer<Vm> vm = m_vm;

    if (!vm || !UsbChoices::runs(vm)) {
        rebuild();
        return;
    }
    const QString key = UsbChoices::key(vm, dev);
    m_pending.insert(key, UsbChoices::attaching());
    rebuild();
    UsbAccess::grant(
        {dev}, qt(),
        guarded<void(const QStringList &, const QString &)>(
            [this, vm, dev, key](const QStringList &out, const QString &why) {
        if (!vm || !UsbChoices::runs(vm)) {
            m_pending.remove(key);
            rebuild();
            return;
        }
        if (!out.isEmpty()) {
            m_pending.remove(key);
            rebuild();
            warn(UsbChoices::notGiven(dev, vm), UsbChoices::noAccess(dev, why));
            return;
        }
        UsbHotplug::of(vm->runner())->attach(dev, guarded<void(const QString &)>(
                                                      [this, vm, dev, key](const QString &error) {
            m_pending.remove(key);
            rebuild();
            if (!vm) {
                return;
            }
            if (error.isEmpty()) {
                say(UsbChoices::attached(dev, vm));
            } else {
                warn(UsbChoices::notGiven(dev, vm), error);
            }
        }));
    }),
        UsbHotplug::devRoot());
}

void UsbDialog::detach(const UsbDevice &dev)
{
    const QPointer<Vm> vm = m_vm;

    if (!vm) {
        return;
    }
    const QString key = UsbChoices::key(vm, dev);
    m_pending.insert(key, UsbChoices::takingBack());
    rebuild();
    UsbHotplug::of(vm->runner())->detach(dev, guarded<void(const QString &)>(
                                                  [this, vm, dev, key](const QString &error) {
        m_pending.remove(key);
        rebuild();
        if (!vm) {
            return;
        }
        if (error.isEmpty()) {
            say(UsbChoices::back(dev));
        } else {
            warn(UsbChoices::stillIn(dev, vm), error);
        }
    }));
}

void UsbDialog::keep()
{
    Vm *vm = m_vm;

    if (!vm) {
        return;
    }
    /* the settings' own changes first, which may touch the same lines */
    if (m_guard) {
        m_guard(vm, guarded<void(bool)>([this](bool go) {
            if (go) {
                keepNow();
            }
        }));
        return;
    }
    keepNow();
}

void UsbDialog::keepNow()
{
    Vm *vm = m_vm;
    QString message, error;

    if (!vm) {
        return;
    }
    readHosts();
    if (!UsbChoices::keep(vm, m_hosts, &message, &error)) {
        warn(UsbChoices::notKept(vm), error);
    } else if (!message.isEmpty()) {
        say(message);
    }
    rebuild();
}

void UsbDialog::warn(const QString &text, const QString &why)
{
    present(Gtk4::alert(text, why));
}

void UsbDialog::say(const QString &text)
{
    m_lastMessage = text;
    adw_toast_overlay_add_toast(m_toasts, adw_toast_new(utf8(text.toHtmlEscaped()).constData()));
}

void UsbDialog::closed()
{
    m_refresh->stop();
}
