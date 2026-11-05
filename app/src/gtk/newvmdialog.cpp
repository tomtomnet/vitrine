// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/newvmdialog.h"

#include <QDir>
#include <QFileInfo>
#include <QThread>

#include <iterator>
#include <optional>

#include "core/firmware.h"
#include "core/hostmemory.h"
#include "core/paths.h"
#include "core/vmconfig.h"
#include "core/vmhardware.h"
#include "core/vmstore.h"
#include "core/vmtemplate.h"
#include "gtk/rows.h"
#include "gtk/widgets.h"
#include "ui/qemudocs.h"

using Gtk4::utf8;
using VmTemplate::Os;
using TemplateFirmware = VmTemplate::Firmware;

/* The systems to choose from, with the desktop of a Linux guest: the Qt dialog's */
static const struct {
    Os os;
    const char *desktop;
    const char *name;
} kSystems[] = {
    {Os::Linux, "kde", "Linux with KDE Plasma"},
    {Os::Linux, "gnome", "Linux with GNOME"},
    {Os::Linux, "other", "Linux, another desktop or none"},
    {Os::Windows11, "", "Windows 11"},
    {Os::Windows, "", "Windows 10 or older"},
    {Os::Other, "", "Another system"},
};

static const QList<Gtk4::FileFilter> kDiskImages = {
    {"Disk images", {"*.qcow2", "*.img", "*.raw", "*.vmdk", "*.vdi", "*.vhdx", "*.vhd"}},
    {"All files", {"*"}},
};
static const QList<Gtk4::FileFilter> kDiscImages = {
    {"Disc images", {"*.iso"}},
    {"All files", {"*"}},
};

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

/* A row with a check button in front, one of @group's: a radio choice in a list */
static GtkWidget *radioRow(const QString &title, GtkWidget *group, GtkWidget **check)
{
    GtkWidget *row = Gtk4::actionRow(title);

    *check = gtk_check_button_new();
    if (group) {
        gtk_check_button_set_group(GTK_CHECK_BUTTON(*check), GTK_CHECK_BUTTON(group));
    }
    gtk_widget_set_valign(*check, GTK_ALIGN_CENTER);
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), *check);
    adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), *check);
    return row;
}

NewVmDialog::NewVmDialog(VmStore *store, Presenter presenter, Created created)
    : Dialog(adw_dialog_new(), std::move(presenter)), m_store(store),
      m_created(std::move(created))
{
    GtkWidget *view = adw_toolbar_view_new();
    GtkWidget *header = adw_header_bar_new();
    GtkWidget *cancel = gtk_button_new_with_mnemonic("_Cancel");
    auto *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    QStringList systems;
    const qint64 memory = hostMiB();

    adw_dialog_set_title(dialog(), "New Virtual Machine");
    adw_dialog_set_content_width(dialog(), 560);
    m_create = gtk_button_new_with_mnemonic("C_reate");
    gtk_widget_add_css_class(m_create, "suggested-action");
    adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), false);
    adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), false);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), cancel);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), m_create);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    adw_dialog_set_default_widget(dialog(), m_create);

    /* What it is */
    for (const auto &system : kSystems) {
        systems << system.name;
    }
    AdwPreferencesGroup *what = Gtk4::group();
    m_name = Gtk4::entryRow("Name");
    gtk_widget_set_name(m_name, "name");
    adw_entry_row_set_activates_default(ADW_ENTRY_ROW(m_name), true);
    m_system = Gtk4::comboRow("System", systems);
    gtk_widget_set_name(m_system, "system");
    /* what the template gives the system chosen, under the list */
    m_note = Gtk4::caption({});
    gtk_widget_set_margin_top(m_note, 12);
    adw_preferences_group_add(what, m_name);
    adw_preferences_group_add(what, m_system);
    adw_preferences_group_add(what, m_note);
    adw_preferences_page_add(page, what);

    /* Its hardware, at most this computer's */
    AdwPreferencesGroup *hardware = Gtk4::group("Hardware");
    m_memory = Gtk4::spinRow("Memory", 128, double(qMax<qint64>(memory, 1024)), 256,
                             QString("This computer has %1 GiB.")
                                 .arg(QString::number(memory / 1024.0, 'f', 1)));
    Gtk4::setUnit(m_memory, "MiB");
    gtk_widget_set_name(m_memory, "memory");
    m_cpus = Gtk4::spinRow("Processors", 1, hostCpus(), 1,
                           hostCpus() == 1 ? QString("This computer has 1.")
                                           : QString("This computer has %1.").arg(hostCpus()));
    gtk_widget_set_name(m_cpus, "cpus");
    adw_preferences_group_add(hardware, m_memory);
    adw_preferences_group_add(hardware, m_cpus);
    adw_preferences_page_add(page, hardware);

    /* Its disk: a new one, or an image with a system on it */
    AdwPreferencesGroup *disk = Gtk4::group("Disk");
    GtkWidget *newRow = radioRow("Create a New Disk", nullptr, &m_newDisk);
    m_diskSize = Gtk4::spinRow("Size", 1, 65536, 1);
    Gtk4::setUnit(m_diskSize, "GiB");
    gtk_widget_set_name(m_diskSize, "diskSize");
    m_imageRow = radioRow("Use an Existing Disk Image", m_newDisk, &m_existingDisk);
    gtk_widget_set_name(m_newDisk, "newDisk");
    gtk_widget_set_name(m_existingDisk, "existingDisk");
    m_chooseImage = Gtk4::rowTextButton("C_hoose…", "Choose the disk image");
    adw_action_row_add_suffix(ADW_ACTION_ROW(m_imageRow), m_chooseImage);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(m_newDisk), true);
    adw_preferences_group_add(disk, newRow);
    adw_preferences_group_add(disk, m_diskSize);
    adw_preferences_group_add(disk, m_imageRow);
    adw_preferences_page_add(page, disk);

    /* What to install from, if anything */
    AdwPreferencesGroup *install = Gtk4::group("Installation");
    m_discRow = Gtk4::actionRow("Install From");
    GtkWidget *chooseDisc = Gtk4::rowTextButton("Ch_oose…", "Choose a disc image to install from");
    m_clearDisc = Gtk4::rowButton("edit-clear-symbolic", "Install from nothing");
    adw_action_row_add_suffix(ADW_ACTION_ROW(m_discRow), m_clearDisc);
    adw_action_row_add_suffix(ADW_ACTION_ROW(m_discRow), chooseDisc);
    m_settings = Gtk4::switchRow("Open the Settings After Creating It");
    gtk_widget_set_name(m_settings, "openSettings");
    adw_preferences_row_set_use_underline(ADW_PREFERENCES_ROW(m_settings), false);
    adw_preferences_group_add(install, m_discRow);
    adw_preferences_page_add(page, install);
    AdwPreferencesGroup *after = Gtk4::group();
    adw_preferences_group_add(after, m_settings);
    adw_preferences_page_add(page, after);

    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), GTK_WIDGET(page));
    adw_dialog_set_child(dialog(), view);

    on<void()>(cancel, "clicked", [this]() { close(); });
    on<void()>(m_create, "clicked", [this]() { create(); });
    on<void(GParamSpec *)>(m_system, "notify::selected", [this](GParamSpec *) { applyDefaults(); });
    on<void()>(m_name, "changed", [this]() { update(); });
    on<void(GParamSpec *)>(m_newDisk, "notify::active", [this](GParamSpec *) { update(); });
    on<void()>(m_chooseImage, "clicked", [this]() {
        Gtk4::chooseFile(GTK_WIDGET(dialog()), "Disk Image", kDiskImages, m_diskImage,
                         guarded<void(const QString &)>([this](const QString &path) {
                             setDiskImage(path);
                         }));
    });
    on<void()>(chooseDisc, "clicked", [this]() {
        Gtk4::chooseFile(GTK_WIDGET(dialog()), "Installation Disc Image", kDiscImages,
                         m_installDisc.isEmpty() ? QDir::homePath() : m_installDisc,
                         guarded<void(const QString &)>([this](const QString &path) {
                             setInstallDisc(path);
                         }));
    });
    on<void()>(m_clearDisc, "clicked", [this]() { setInstallDisc({}); });

    applyDefaults();
    update();
    gtk_widget_grab_focus(m_name);
}

void NewVmDialog::applyDefaults()
{
    const int index = qMax(0, Gtk4::selected(m_system));
    const Os os = kSystems[index].os;
    const VmTemplate::Defaults d = VmTemplate::defaults(os);
    GtkAdjustment *memory = adw_spin_row_get_adjustment(ADW_SPIN_ROW(m_memory));
    GtkAdjustment *cpus = adw_spin_row_get_adjustment(ADW_SPIN_ROW(m_cpus));
    QString note;

    /* at most half this computer */
    const auto maxMemory = qint64(gtk_adjustment_get_upper(memory));
    const int maxCpus = int(gtk_adjustment_get_upper(cpus));
    Gtk4::setValue(m_memory, double(qMin<qint64>(d.memoryMiB, qMax<qint64>(maxMemory / 2, 1024))));
    Gtk4::setValue(m_cpus, qMin(d.cpus, qMax(maxCpus / 2, 1)));
    Gtk4::setValue(m_diskSize, d.diskGiB);

    switch (os) {
    case Os::Linux:
        note = VmTemplate::hasVga() ? QString("Fast virtio devices, 3D graphics through this "
                                              "computer's GPU driver, and UEFI without Secure "
                                              "Boot.")
                                    : QString("Fast virtio devices and 3D graphics, with UEFI.");
        break;
    case Os::Windows11:
    case Os::Windows:
        if (!VmTemplate::hasVga()) {
            note = "Windows on ARM uses virtio devices: keep the virtio-win drivers at hand.";
        } else if (os == Os::Windows11) {
            note = "Windows uses a SATA disk and an Intel network card, which need no extra "
                   "drivers, and UEFI with Secure Boot. Windows 11 also checks for a TPM, which "
                   "Vitrine does not provide yet.";
        } else {
            note = "Windows uses a SATA disk and an Intel network card, which need no extra "
                   "drivers.";
        }
        break;
    case Os::Other:
        note = "Devices most systems have drivers for.";
        break;
    }
    gtk_label_set_text(GTK_LABEL(m_note), utf8(note).constData());
}

void NewVmDialog::update()
{
    const bool existing = !gtk_check_button_get_active(GTK_CHECK_BUTTON(m_newDisk));

    gtk_widget_set_sensitive(m_diskSize, !existing);
    gtk_widget_set_sensitive(m_chooseImage, existing);
    Gtk4::setSubtitle(m_imageRow, m_diskImage.isEmpty()
                                      ? QString("A disk with a system installed, e.g. a cloud image")
                                      : QDir::toNativeSeparators(m_diskImage));
    Gtk4::setSubtitle(m_discRow, m_installDisc.isEmpty()
                                     ? QString("Optional: a disc image to install from")
                                     : QDir::toNativeSeparators(m_installDisc));
    gtk_widget_set_visible(m_clearDisc, !m_installDisc.isEmpty());
    gtk_widget_set_sensitive(m_create, canCreate());
}

bool NewVmDialog::canCreate() const
{
    const bool existing = !gtk_check_button_get_active(GTK_CHECK_BUTTON(m_newDisk));

    return !Gtk4::text(m_name).trimmed().isEmpty() && (!existing || !m_diskImage.isEmpty());
}

Vm *NewVmDialog::create()
{
    const QString name = Gtk4::text(m_name).trimmed();
    const bool existing = !gtk_check_button_get_active(GTK_CHECK_BUTTON(m_newDisk));
    QString error;

    if (name.isEmpty()) {
        present(Gtk4::alert("New Virtual Machine", "Give the VM a name."));
        return nullptr;
    }
    if (existing && !QFileInfo(m_diskImage).isFile()) {
        present(Gtk4::alert("New Virtual Machine", "Choose the disk image to use."));
        return nullptr;
    }
    if (!m_installDisc.isEmpty() && !QFileInfo(m_installDisc).isFile()) {
        present(Gtk4::alert("New Virtual Machine", "The disc image does not exist."));
        return nullptr;
    }

    Vm *vm = m_store->create(name, &error);
    if (!vm) {
        present(Gtk4::alert("Cannot Create the VM", error));
        return nullptr;
    }
    m_warnings.clear();
    if (!make(vm, &error)) {
        /* nothing worth keeping yet */
        QDir(vm->dir()).removeRecursively();
        m_store->reload();
        present(Gtk4::alert("Cannot Create the VM", error));
        return nullptr;
    }
    const bool open = adw_switch_row_get_active(ADW_SWITCH_ROW(m_settings));
    const QStringList warnings = m_warnings;
    Created created = m_created;
    forceClose();
    if (created) {
        created(vm, warnings, open);
    }
    return vm;
}

/* The Qt dialog's create(), with the same core calls */
bool NewVmDialog::make(Vm *vm, QString *error)
{
    const int system = qMax(0, Gtk4::selected(m_system));
    const VmTemplate::Defaults defaults = VmTemplate::defaults(kSystems[system].os);
    const TemplateFirmware choice = defaults.firmware;
    /*
     * The QEMU it runs with: vitrine's, built or not, unless the
     * preferences choose another (the VM gets no #qemu line), which may
     * lack some of what vitrine's has: asked now if its documentation is
     * not loaded (yet)
     */
    const QString chosen = Paths::customQemuBinary();
    const QemuDocs *docs = QemuDocs::preferred();
    const QemuInfo *info = !chosen.isEmpty() && docs->binary() == chosen ? docs->info() : nullptr;
    std::optional<Firmware> firmware;
    VmTemplate::Options o;
    QString firmwareError;

    o.name = Gtk4::text(m_name).trimmed();
    o.os = kSystems[system].os;
    o.desktop = kSystems[system].desktop;
    o.memoryMiB = memoryMiB();
    o.cpus = cpus();
    o.graphics = defaults.graphics;
    o.passt = VmTemplate::hasPasst(info, chosen);
    o.gpuProperties = VmTemplate::gpuProperties(info, chosen);
    if (o.os == Os::Linux) {
        /* the guest's SSH, on a port no other VM forwards, whatever for */
        QList<int> taken;
        for (const Vm *other : m_store->vms()) {
            if (other != vm) {
                taken << VmConfig::forwardedPorts(other->args());
            }
        }
        o.sshPort = VmConfig::freePort(taken);
    }
    if (!m_installDisc.trimmed().isEmpty()) {
        o.iso = QDir::cleanPath(m_installDisc.trimmed());
    }
    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(m_newDisk))) {
        if (!createDiskImage(QDir(vm->dir()).filePath("disk.qcow2"), qint64(diskGiB()) << 30,
                             error)) {
            return false;
        }
        o.disk = "disk.qcow2";
    } else {
        o.disk = QFileInfo(m_diskImage.trimmed()).absoluteFilePath();
    }

    if (choice != TemplateFirmware::Bios) {
        firmware = FirmwareDb::find(choice == TemplateFirmware::UefiSecureBoot);
        if (!firmware && choice == TemplateFirmware::UefiSecureBoot) {
            firmware = FirmwareDb::find(false);
            if (firmware) {
                m_warnings << "No UEFI firmware with Secure Boot was found, so the VM uses UEFI "
                              "without it.";
            }
        }
        if (!firmware) {
            m_warnings << "No UEFI firmware was found, so the VM uses BIOS. Install edk2-ovmf "
                          "(sudo dnf install edk2-ovmf) for UEFI.";
        }
    }

    const ArgsFile args = VmTemplate::build(o, [&](ArgsFile &a) {
        if (firmware && !FirmwareDb::apply(a, *firmware, vm->dir(), &firmwareError)) {
            m_warnings << QString("The UEFI firmware could not be set up (%1), so the VM uses "
                                  "BIOS.")
                              .arg(firmwareError);
        }
    });
    return vm->save(args, error);
}

void NewVmDialog::setName(const QString &name)
{
    Gtk4::setText(m_name, name);
}

void NewVmDialog::setSystem(int index)
{
    Gtk4::setSelected(m_system, qBound(0, index, int(std::size(kSystems)) - 1));
}

void NewVmDialog::setMemoryMiB(int mib)
{
    Gtk4::setValue(m_memory, mib);
}

void NewVmDialog::setCpus(int cpus)
{
    Gtk4::setValue(m_cpus, cpus);
}

void NewVmDialog::setNewDisk(int gib)
{
    gtk_check_button_set_active(GTK_CHECK_BUTTON(m_newDisk), true);
    Gtk4::setValue(m_diskSize, gib);
}

void NewVmDialog::setDiskImage(const QString &path)
{
    m_diskImage = path;
    gtk_check_button_set_active(GTK_CHECK_BUTTON(m_existingDisk), !path.isEmpty());
    update();
}

void NewVmDialog::setInstallDisc(const QString &path)
{
    m_installDisc = path;
    update();
}

void NewVmDialog::setOpenSettings(bool open)
{
    adw_switch_row_set_active(ADW_SWITCH_ROW(m_settings), open);
}

int NewVmDialog::memoryMiB() const
{
    return Gtk4::value(m_memory);
}

int NewVmDialog::cpus() const
{
    return Gtk4::value(m_cpus);
}

int NewVmDialog::diskGiB() const
{
    return Gtk4::value(m_diskSize);
}
