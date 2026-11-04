// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/settingspages.h"

#include <QDir>
#include <QThread>

#include <iterator>

#include "core/hostmemory.h"
#include "core/paths.h"
#include "core/vmstore.h"
#include "core/vmtemplate.h"
#include "gtk/rows.h"
#include "gtk/widgets.h"
#include "ui/qemudocs.h"

using Gtk4::utf8;

/* What this computer has: the memory, at least some, and the processors */
static qint64 hostMiB()
{
    const qint64 total = HostMemory::read().totalMiB;
    return total > 0 ? total : 16384;
}

static int hostCpus()
{
    return qMax(QThread::idealThreadCount(), 1);
}

/* General */

/* The systems and desktops of #guest, by the names the page gives them: the Qt page's */
static const char *const kSystems[][2] = {
    {"linux", "Linux"},
    {"windows", "Windows"},
    {"other", "Another system"},
};
static const char *const kDesktops[][2] = {
    {"kde", "KDE Plasma"},
    {"gnome", "GNOME"},
    {"other", "Another desktop, or none"},
};

/* @row with @choices and "Not known", and @value as written if it is none of them */
template <size_t N>
static void fillChoices(GtkWidget *row, QStringList &keys, const char *const (&choices)[N][2],
                        const QString &value)
{
    QStringList names;

    keys.clear();
    for (const auto &c : choices) {
        keys << c[0];
        names << c[1];
    }
    keys << QString();
    names << "Not known";
    if (!keys.contains(value)) {
        keys << value;
        names << value;
    }
    Gtk4::setItems(row, names);
    Gtk4::setSelected(row, int(keys.indexOf(value)));
}

GeneralPage::GeneralPage(Vm *vm) : SettingsPage(adw_preferences_page_new())
{
    auto *page = ADW_PREFERENCES_PAGE(widget());
    AdwPreferencesGroup *what = Gtk4::group();
    AdwPreferencesGroup *where = Gtk4::group();
    GtkWidget *folder = Gtk4::actionRow("Folder", QDir::toNativeSeparators(vm->dir()));
    GtkWidget *open = Gtk4::rowButton("folder-open-symbolic", "Open the Folder");
    const QString dir = vm->dir();

    m_name = Gtk4::entryRow("Name");
    gtk_widget_set_name(m_name, "name");
    m_os = Gtk4::comboRow("System", {});
    gtk_widget_set_name(m_os, "os");
    m_desktop = Gtk4::comboRow("Desktop", {}, "The timing of the VM's frames depends on its desktop.");
    gtk_widget_set_name(m_desktop, "desktop");
    adw_preferences_group_add(what, m_name);
    adw_preferences_group_add(what, m_os);
    adw_preferences_group_add(what, m_desktop);
    adw_preferences_page_add(page, what);

    /* the value over its name, as the VM's details show it */
    gtk_widget_add_css_class(folder, "property");
    adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(folder), true);
    adw_action_row_add_suffix(ADW_ACTION_ROW(folder), open);
    adw_preferences_group_add(where, folder);
    GtkWidget *hint = Gtk4::caption("The folder holds the arguments (vm.args), the disks the VM "
                                    "creates and its log.");
    gtk_widget_set_margin_top(hint, 12);
    adw_preferences_group_add(where, hint);
    adw_preferences_page_add(page, where);

    on<void()>(m_name, "changed", [this]() { changed(); });
    on<void(GParamSpec *)>(m_os, "notify::selected", [this](GParamSpec *) {
        updateDesktop();
        changed();
    });
    on<void(GParamSpec *)>(m_desktop, "notify::selected", [this](GParamSpec *) { changed(); });
    on<void()>(open, "clicked", [open, dir]() { Gtk4::openPath(open, dir); });
}

void GeneralPage::updateDesktop()
{
    const QString os = m_osKeys.value(Gtk4::selected(m_os));
    gtk_widget_set_sensitive(m_desktop, os == "linux" || os.isEmpty());
}

VmConfig::Guest GeneralPage::shown() const
{
    const QString os = m_osKeys.value(Gtk4::selected(m_os));
    /* a desktop for Linux, or a system not known */
    return {os, gtk_widget_get_sensitive(m_desktop) ? m_desktopKeys.value(Gtk4::selected(m_desktop))
                                                    : QString()};
}

void GeneralPage::load(const ArgsFile &args)
{
    const Loading loading(m_loading);
    const VmConfig::Guest guest = VmConfig::guest(args);

    m_loaded = VmConfig::name(args);
    Gtk4::setText(m_name, m_loaded);
    fillChoices(m_os, m_osKeys, kSystems, guest.os);
    fillChoices(m_desktop, m_desktopKeys, kDesktops, guest.desktop);
    updateDesktop();
    /* as the page shows it, so that an untouched page writes nothing */
    m_loadedGuest = shown();
}

void GeneralPage::save(ArgsFile &args)
{
    const QString name = Gtk4::text(m_name).trimmed();
    const VmConfig::Guest guest = shown();

    if (!name.isEmpty() && name != m_loaded) {
        VmConfig::setName(args, name);
        m_loaded = name;
    }
    if (guest.os != m_loadedGuest.os || guest.desktop != m_loadedGuest.desktop) {
        VmConfig::setGuest(args, guest);
        m_loadedGuest = guest;
    }
}

bool GeneralPage::isModified() const
{
    const QString name = Gtk4::text(m_name).trimmed();
    const VmConfig::Guest guest = shown();

    return (!name.isEmpty() && name != m_loaded) || guest.os != m_loadedGuest.os ||
           guest.desktop != m_loadedGuest.desktop;
}

/* Hardware */

HardwarePage::HardwarePage() : SettingsPage(adw_preferences_page_new())
{
    auto *page = ADW_PREFERENCES_PAGE(widget());
    AdwPreferencesGroup *group = Gtk4::group();
    const qint64 memory = hostMiB();
    const int cpus = hostCpus();

    m_memory = Gtk4::spinRow("Memory", 64, double(qMax<qint64>(memory, 1024)), 256,
                             QString("This computer has %1 GiB.")
                                 .arg(QString::number(memory / 1024.0, 'f', 1)));
    Gtk4::setUnit(m_memory, "MiB");
    gtk_widget_set_name(m_memory, "memory");
    m_cpus = Gtk4::spinRow("Processors", 1, cpus, 1,
                           cpus == 1 ? QString("This computer has 1.")
                                     : QString("This computer has %1.").arg(cpus));
    gtk_widget_set_name(m_cpus, "cpus");
    m_topology = Gtk4::caption({});
    gtk_widget_set_margin_top(m_topology, 12);
    adw_preferences_group_add(group, m_memory);
    adw_preferences_group_add(group, m_cpus);
    adw_preferences_group_add(group, m_topology);
    adw_preferences_page_add(page, group);

    on<void(GParamSpec *)>(m_memory, "notify::value", [this](GParamSpec *) { changed(); });
    on<void(GParamSpec *)>(m_cpus, "notify::value", [this](GParamSpec *) { changed(); });
}

void HardwarePage::load(const ArgsFile &args)
{
    const Loading loading(m_loading);
    const qint64 memory = VmConfig::memoryMiB(args);
    const VmConfig::Cpus cpus = VmConfig::cpus(args);
    GtkAdjustment *memoryRange = adw_spin_row_get_adjustment(ADW_SPIN_ROW(m_memory));
    GtkAdjustment *cpuRange = adw_spin_row_get_adjustment(ADW_SPIN_ROW(m_cpus));

    Gtk4::setRange(m_memory, 64, qMax(gtk_adjustment_get_upper(memoryRange), double(memory)));
    Gtk4::setValue(m_memory, double(memory > 0 ? memory : 128));
    Gtk4::setRange(m_cpus, 1, qMax(gtk_adjustment_get_upper(cpuRange), double(cpus.count)));
    Gtk4::setValue(m_cpus, cpus.count);
    /* compare with what the page shows, e.g. 128 MiB for no -m, so that
       an untouched page writes nothing */
    m_loadedMemory = Gtk4::value(m_memory);
    m_loadedCpus = Gtk4::value(m_cpus);
    /* maxcpus=, dies=...: a count written without them would not start */
    gtk_widget_set_sensitive(m_cpus, !cpus.custom);
    QString topology;
    if (cpus.custom) {
        topology = QString("The processors of this VM are set up by hand (-smp %1): change them "
                           "on the Arguments page.")
                       .arg(args.lines[args.indexOf("smp")].value.trimmed());
    } else if (cpus.threads > 1) {
        topology = QString("%1 threads per core, as far as the number allows: the Machine page "
                           "sets the topology.")
                       .arg(cpus.threads);
    }
    gtk_label_set_text(GTK_LABEL(m_topology), utf8(topology).constData());
    gtk_widget_set_visible(m_topology, !topology.isEmpty());
}

void HardwarePage::save(ArgsFile &args)
{
    if (Gtk4::value(m_memory) != m_loadedMemory) {
        VmConfig::setMemoryMiB(args, Gtk4::value(m_memory));
        m_loadedMemory = Gtk4::value(m_memory);
    }
    if (Gtk4::value(m_cpus) != m_loadedCpus) {
        VmConfig::setCpuCount(args, Gtk4::value(m_cpus));
        m_loadedCpus = Gtk4::value(m_cpus);
    }
}

bool HardwarePage::isModified() const
{
    return Gtk4::value(m_memory) != m_loadedMemory || Gtk4::value(m_cpus) != m_loadedCpus;
}

/* Network */

NetworkPage::NetworkPage(Vm *vm)
    : SettingsPage(adw_preferences_page_new()), m_vm(vm), m_custom(ADW_BANNER(adw_banner_new("")))
{
    auto *page = ADW_PREFERENCES_PAGE(widget());
    AdwPreferencesGroup *nat = Gtk4::group();
    AdwPreferencesGroup *ssh = Gtk4::group();

    adw_preferences_page_set_banner(page, m_custom);
    m_nat = Gtk4::switchRow("Connect the VM to the Network",
                            "Through this computer's connection (NAT): the VM reaches the network "
                            "and the internet; other computers reach the VM only through the "
                            "ports forwarded to it.");
    gtk_widget_set_name(m_nat, "nat");
    adw_preferences_group_add(nat, m_nat);
    adw_preferences_page_add(page, nat);

    m_ssh = Gtk4::switchRow("Forward a Port to the Guest's SSH Server");
    gtk_widget_set_name(m_ssh, "ssh");
    m_port = Gtk4::spinRow("Port of This Computer", 1024, 65535, 1);
    gtk_widget_set_name(m_port, "sshPort");
    /* it shows an address as written in vm.args */
    m_sshInfo = Gtk4::caption({});
    gtk_label_set_selectable(GTK_LABEL(m_sshInfo), true);
    gtk_widget_set_margin_top(m_sshInfo, 12);
    adw_preferences_group_add(ssh, m_ssh);
    adw_preferences_group_add(ssh, m_port);
    adw_preferences_group_add(ssh, m_sshInfo);
    adw_preferences_page_add(page, ssh);

    on<void(GParamSpec *)>(m_nat, "notify::active", [this](GParamSpec *) {
        if (!m_loading) {
            update();
            changed();
        }
    });
    on<void(GParamSpec *)>(m_ssh, "notify::active", [this](GParamSpec *) {
        if (m_loading) {
            return;
        }
        GtkAdjustment *range = adw_spin_row_get_adjustment(ADW_SPIN_ROW(m_port));
        const int minimum = int(gtk_adjustment_get_lower(range));
        if (adw_switch_row_get_active(ADW_SWITCH_ROW(m_ssh)) && m_loaded.sshPort == 0 &&
            Gtk4::value(m_port) == minimum) {
            /* a port no other VM forwards, whatever for, nor this one's
               other forwards, which stay; and free now */
            QList<int> taken = m_otherForwards;
            if (auto *store = m_vm ? qobject_cast<VmStore *>(m_vm->parent()) : nullptr) {
                for (const Vm *other : store->vms()) {
                    if (other != m_vm) {
                        taken << VmConfig::forwardedPorts(other->args());
                    }
                }
            }
            const Loading loading(m_loading);
            Gtk4::setValue(m_port, qMax(VmConfig::freePort(taken), minimum));
        }
        update();
        changed();
    });
    on<void(GParamSpec *)>(m_port, "notify::value", [this](GParamSpec *) {
        if (!m_loading) {
            update();
            changed();
        }
    });
}

void NetworkPage::update()
{
    const bool custom = m_loaded.kind == VmConfig::Network::Custom;
    const bool nat = adw_switch_row_get_active(ADW_SWITCH_ROW(m_nat));
    const bool ssh = adw_switch_row_get_active(ADW_SWITCH_ROW(m_ssh));

    gtk_widget_set_sensitive(m_nat, !custom);
    gtk_widget_set_sensitive(m_ssh, !custom && nat);
    gtk_widget_set_sensitive(m_port, !custom && nat && ssh);
    gtk_widget_set_visible(m_sshInfo, !custom && nat && ssh);

    /* where the forward listens: as written, unless saving writes it anew, on 127.0.0.1 */
    const bool kept = m_loaded.sshPort > 0 && Gtk4::value(m_port) == m_loaded.sshPort;
    const QString address = kept ? m_loaded.sshAddress : QString("127.0.0.1");
    const QString host = address.section('%', 0, 0);
    const QString port = QString::number(Gtk4::value(m_port));
    QString text;

    if (VmConfig::isLoopback(address)) {
        text = QString("From this computer only: ssh -p %1 USER@%2").arg(port, host);
    } else if (host.isEmpty() || host == "0.0.0.0" || host == "::" || host == "[::]") {
        text = address.contains('%')
                   ? QString("From other computers, through %2: ssh -p %1 USER@ADDRESS, with an "
                             "address of this computer there.")
                         .arg(port, address.section('%', 1))
                   : QString("From this computer, ssh -p %1 USER@127.0.0.1, and from other "
                             "computers too: it listens on all the addresses of this computer.")
                         .arg(port);
    } else {
        text = QString("On %2 only, which other computers may reach: ssh -p %1 USER@%2")
                   .arg(port, host);
    }
    gtk_label_set_text(GTK_LABEL(m_sshInfo), utf8(text).constData());
}

void NetworkPage::load(const ArgsFile &args)
{
    const Loading loading(m_loading);

    m_loaded = VmConfig::network(args);
    m_otherForwards = VmConfig::forwardedPorts(args);
    m_otherForwards.removeOne(m_loaded.sshPort);
    /* where the VM had none: passt, if at hand in its QEMU, vitrine's unless chosen */
    const QString own = VmConfig::qemuBinary(args);
    const QString chosen = own.isEmpty() ? Paths::customQemuBinary() : own;
    const QemuDocs *docs = QemuDocs::forArgs(args);
    m_backend = VmTemplate::hasPasst(docs->binary() == chosen ? docs->info() : nullptr, chosen)
                    ? "passt"
                    : "user";
    adw_switch_row_set_active(ADW_SWITCH_ROW(m_nat), m_loaded.kind == VmConfig::Network::Nat);
    adw_switch_row_set_active(ADW_SWITCH_ROW(m_ssh), m_loaded.sshPort > 0);
    const int minimum = qMin(1024, m_loaded.sshPort > 0 ? m_loaded.sshPort : 1024);
    Gtk4::setRange(m_port, minimum, 65535);
    Gtk4::setValue(m_port, m_loaded.sshPort > 0 ? m_loaded.sshPort : minimum);
    adw_banner_set_title(m_custom, utf8(QString("The network of this VM is set up by hand (%1): "
                                                "change it on the Arguments page.")
                                            .arg(m_loaded.custom)
                                            .toHtmlEscaped())
                                       .constData());
    adw_banner_set_revealed(m_custom, m_loaded.kind == VmConfig::Network::Custom);
    update();
}

VmConfig::Network NetworkPage::shown() const
{
    VmConfig::Network n = m_loaded;
    const bool nat = adw_switch_row_get_active(ADW_SWITCH_ROW(m_nat));

    n.kind = nat ? VmConfig::Network::Nat : VmConfig::Network::Off;
    n.sshPort = nat && adw_switch_row_get_active(ADW_SWITCH_ROW(m_ssh)) ? Gtk4::value(m_port) : 0;
    if (n.backend.isEmpty()) {
        n.backend = m_backend;
    }
    return n;
}

void NetworkPage::save(ArgsFile &args)
{
    if (isModified()) {
        VmConfig::setNetwork(args, shown());
        load(args);
    }
}

bool NetworkPage::isModified() const
{
    const VmConfig::Network n = shown();

    if (m_loaded.kind == VmConfig::Network::Custom) {
        return false;
    }
    return n.kind != m_loaded.kind ||
           (n.kind == VmConfig::Network::Nat && n.sshPort != m_loaded.sshPort);
}
