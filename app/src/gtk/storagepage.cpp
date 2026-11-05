// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/storagepage.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>

#include "core/vmstore.h"
#include "gtk/dialog.h"
#include "gtk/rows.h"
#include "gtk/widgets.h"
#include "ui/uiconfig.h"

using Gtk4::utf8;
using Disk = VmConfig::Disk;

static const QList<Gtk4::FileFilter> kDiskImages = {
    {"Disk images", {"*.qcow2", "*.img", "*.raw", "*.vmdk", "*.vdi", "*.vhdx", "*.vhd"}},
    {"All files", {"*"}},
};
static const QList<Gtk4::FileFilter> kDiscImages = {
    {"Disc images", {"*.iso"}},
    {"All files", {"*"}},
};

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

/*
 * Add Hard Disk: a new disk of a size, created on Apply, or an image there
 * is, and the bus: the Qt page's dialog
 */
class AddDiskDialog : public Gtk4::Dialog
{
public:
    using Added = std::function<void(const QString &image, int newGiB, Disk::Bus bus)>;

    AddDiskDialog(bool virt, Presenter presenter, Added added)
        : Dialog(adw_dialog_new(), std::move(presenter)), m_added(std::move(added))
    {
        GtkWidget *view = adw_toolbar_view_new();
        GtkWidget *header = adw_header_bar_new();
        GtkWidget *cancel = gtk_button_new_with_mnemonic("_Cancel");
        auto *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
        AdwPreferencesGroup *disk = Gtk4::group();
        AdwPreferencesGroup *bus = Gtk4::group();

        adw_dialog_set_title(dialog(), "Add a Hard Disk");
        adw_dialog_set_content_width(dialog(), 520);
        m_add = gtk_button_new_with_mnemonic("_Add");
        gtk_widget_add_css_class(m_add, "suggested-action");
        adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), false);
        adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), false);
        adw_header_bar_pack_start(ADW_HEADER_BAR(header), cancel);
        adw_header_bar_pack_end(ADW_HEADER_BAR(header), m_add);
        adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
        adw_dialog_set_default_widget(dialog(), m_add);

        GtkWidget *newRow = radioRow("Create a New Disk", nullptr, &m_new);
        m_size = Gtk4::spinRow("Size", 1, 65536, 1);
        Gtk4::setUnit(m_size, "GiB");
        Gtk4::setValue(m_size, 64);
        m_imageRow = radioRow("Use an Existing Disk Image", m_new, &m_existing);
        m_choose = Gtk4::rowTextButton("C_hoose…", "Choose the disk image");
        adw_action_row_add_suffix(ADW_ACTION_ROW(m_imageRow), m_choose);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(m_new), true);
        adw_preferences_group_add(disk, newRow);
        adw_preferences_group_add(disk, m_size);
        adw_preferences_group_add(disk, m_imageRow);
        adw_preferences_page_add(page, disk);

        /* virt has no SATA */
        m_buses = {Disk::Virtio, virt ? Disk::Scsi : Disk::Sata};
        m_bus = Gtk4::comboRow("Bus", {"VirtIO", virt ? QString("SCSI (virtio-scsi)")
                                                      : QString("SATA")});
        adw_preferences_group_add(bus, m_bus);
        on<void(GParamSpec *)>(m_bus, "notify::selected", [this](GParamSpec *) { describeBus(); });
        describeBus();
        adw_preferences_page_add(page, bus);

        adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), GTK_WIDGET(page));
        adw_dialog_set_child(dialog(), view);

        on<void()>(cancel, "clicked", [this]() { close(); });
        on<void()>(m_add, "clicked", [this]() { add(); });
        on<void(GParamSpec *)>(m_new, "notify::active", [this](GParamSpec *) { update(); });
        on<void()>(m_choose, "clicked", [this]() {
            Gtk4::chooseFile(GTK_WIDGET(dialog()), "Disk Image", kDiskImages, m_image,
                             guarded<void(const QString &)>([this](const QString &path) {
                                 m_image = path;
                                 gtk_check_button_set_active(GTK_CHECK_BUTTON(m_existing), true);
                                 update();
                             }));
        });
        update();
    }

private:
    void describeBus()
    {
        Gtk4::setSubtitle(m_bus, m_buses.value(qMax(0, Gtk4::selected(m_bus))) == Disk::Virtio
                                     ? QString("The fastest; Windows needs its drivers.")
                                 : m_buses.value(1) == Disk::Sata
                                     ? QString("Works without drivers, e.g. on Windows.")
                                     : QString());
    }

    void update()
    {
        const bool create = gtk_check_button_get_active(GTK_CHECK_BUTTON(m_new));

        gtk_widget_set_sensitive(m_size, create);
        gtk_widget_set_sensitive(m_choose, !create);
        Gtk4::setSubtitle(m_imageRow, m_image.isEmpty() ? QString("A disk with a system installed")
                                                        : QDir::toNativeSeparators(m_image));
        gtk_widget_set_sensitive(m_add, create || !m_image.isEmpty());
    }

    void add()
    {
        const bool create = gtk_check_button_get_active(GTK_CHECK_BUTTON(m_new));
        const Disk::Bus bus = m_buses.value(qMax(0, Gtk4::selected(m_bus)));

        if (!create && !QFileInfo(m_image).isFile()) {
            present(Gtk4::alert("Add a Hard Disk", "Choose the disk image to use."));
            return;
        }
        const Added added = m_added;
        const QString image = create ? QString() : QFileInfo(m_image).absoluteFilePath();
        const int size = create ? Gtk4::value(m_size) : 0;
        forceClose();
        added(image, size, bus);
    }

    Added m_added;
    QString m_image;
    QList<Disk::Bus> m_buses;
    GtkWidget *m_new;
    GtkWidget *m_size;
    GtkWidget *m_existing;
    GtkWidget *m_imageRow;
    GtkWidget *m_choose;
    GtkWidget *m_bus;
    GtkWidget *m_add;
};

StoragePage::StoragePage(const QString &vmDir)
    : SettingsPage(adw_preferences_page_new()), m_vmDir(vmDir),
      m_disks(Gtk4::group("Disks and Drives",
                          "New disks are created in the VM folder when you apply; removing a disk "
                          "takes it out of the VM, but its file stays.")),
      m_boot(Gtk4::group("Boot Order"))
{
    auto *page = ADW_PREFERENCES_PAGE(widget());

    m_addDisk = Gtk4::buttonRow("Add Hard _Disk…", "list-add-symbolic");
    gtk_widget_set_name(m_addDisk, "addDisk");
    m_addCdrom = Gtk4::buttonRow("Add _CD/DVD Drive", "list-add-symbolic");
    gtk_widget_set_name(m_addCdrom, "addCdrom");
    m_bootHint = Gtk4::caption({});
    gtk_widget_set_margin_top(m_bootHint, 12);
    /* the page's own: they leave their groups and come back as the rows are made again */
    for (GtkWidget *kept : {m_addDisk, m_addCdrom, m_bootHint}) {
        g_object_ref_sink(kept);
    }
    adw_preferences_group_add(m_disks, m_addDisk);
    adw_preferences_group_add(m_disks, m_addCdrom);
    adw_preferences_page_add(page, m_disks);
    adw_preferences_group_add(m_boot, m_bootHint);
    adw_preferences_page_add(page, m_boot);

    on<void()>(m_addDisk, "activated", [this]() { askNewDisk(); });
    on<void()>(m_addCdrom, "activated", [this]() {
        /* the drive, then its disc, as the Qt page does */
        addCdrom({});
        chooseDisc(int(m_entries.size()) - 1);
    });
}

StoragePage::~StoragePage()
{
    for (GtkWidget *kept : {m_addDisk, m_addCdrom, m_bootHint}) {
        g_object_unref(kept);
    }
}

void StoragePage::load(const ArgsFile &args)
{
    QStringList cards;

    m_virt = VmConfig::machineType(args).startsWith("virt");
    m_entries.clear();
    for (const VmConfig::Disk &d : VmConfig::disks(args)) {
        m_entries << Entry{d, d.file, m_pending.value(d.file), false, m_nextId++};
    }
    /* the network cards, as VmConfig counts them: devices of a -netdev */
    for (int i : args.indexesOf("device")) {
        if (args.valueAt(i).has("netdev")) {
            cards << args.valueAt(i).implied();
        }
    }
    m_bootSet = VmConfig::hasBootOrder(args);
    m_bootChanged = false;
    m_bootItems.clear();
    for (const VmConfig::BootEntry &b : VmConfig::bootOrder(args)) {
        BootItem item;
        item.kind = b.kind;
        item.on = b.on;
        item.editable = b.editable;
        if (b.kind == VmConfig::BootEntry::Network) {
            item.nic = b.index;
            item.card = cards.value(b.index);
        } else {
            item.entry = m_entries.value(b.index).id;
        }
        m_bootItems << item;
    }
    fill();
}

void StoragePage::save(ArgsFile &args)
{
    QList<VmConfig::Disk> removed;

    /* discs, which move no line */
    for (const Entry &e : std::as_const(m_entries)) {
        if (e.disk.line >= 0 && !e.removed && e.disk.cdrom && e.file != e.disk.file) {
            VmConfig::setDisc(args, e.disk, e.file);
        }
    }
    /* then the removals, from the bottom */
    for (const Entry &e : std::as_const(m_entries)) {
        if (e.removed) {
            removed << e.disk;
        }
    }
    std::sort(removed.begin(), removed.end(), [](const auto &a, const auto &b) {
        return qMax(a.line, a.deviceLine) > qMax(b.line, b.deviceLine);
    });
    for (const VmConfig::Disk &d : std::as_const(removed)) {
        VmConfig::removeDisk(args, d);
    }
    for (const Entry &e : std::as_const(m_entries)) {
        if (e.disk.line >= 0 || e.removed) {
            continue;
        }
        if (e.disk.cdrom) {
            VmConfig::addCdrom(args, e.file);
        } else {
            VmConfig::addDisk(args, e.file, e.disk.bus);
            if (e.newGiB > 0) {
                m_pending[e.file] = e.newGiB;
            }
        }
    }
    if (m_bootChanged) {
        /* the disks now: those kept, then those added, as the page lists them */
        QList<int> ids;
        QList<VmConfig::BootEntry> order;
        for (const Entry &e : std::as_const(m_entries)) {
            if (!e.removed) {
                ids << e.id;
            }
        }
        for (const BootItem &item : std::as_const(m_bootItems)) {
            VmConfig::BootEntry b;
            b.kind = item.kind;
            b.index = item.kind == VmConfig::BootEntry::Network ? item.nic
                                                                 : int(ids.indexOf(item.entry));
            b.on = item.on;
            b.editable = item.editable;
            if (b.index >= 0) {
                order << b;
            }
        }
        VmConfig::setBootOrder(args, order);
    }
    load(args);
}

bool StoragePage::isModified() const
{
    if (m_bootChanged) {
        return true;
    }
    for (const Entry &e : m_entries) {
        if (e.removed || e.disk.line < 0 || e.file != e.disk.file ||
            (e.newGiB > 0 && e.newGiB != m_pending.value(e.file))) {
            return true;
        }
    }
    return false;
}

bool StoragePage::commit(const ArgsFile &args, const QString &vmDir, QString *error)
{
    QStringList used;

    for (const VmConfig::Disk &d : VmConfig::disks(args)) {
        used << d.file;
    }
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        const QString path = QDir(vmDir).filePath(it.key());
        if (used.contains(it.key()) && !QFileInfo::exists(path) &&
            !createDiskImage(path, qint64(it.value()) << 30, error)) {
            return false;
        }
        it = m_pending.erase(it);
    }
    return true;
}

/* The disks and drives, a row each, with what can be done to it */
void StoragePage::fill()
{
    for (GtkWidget *row : std::as_const(m_diskRows)) {
        adw_preferences_group_remove(m_disks, row);
    }
    m_diskRows.clear();
    adw_preferences_group_remove(m_disks, m_addDisk);
    adw_preferences_group_remove(m_disks, m_addCdrom);

    for (int i = 0; i < m_entries.size(); i++) {
        const Entry &e = m_entries[i];
        if (e.removed) {
            continue;
        }
        const QString path = QDir(m_vmDir).absoluteFilePath(e.file);
        QStringList subtitle = {e.disk.cdrom ? QString("CD/DVD") : QString("Hard disk"),
                                UiConfig::busName(e.disk.bus)};

        if (e.newGiB > 0) {
            subtitle << QString("New, %1 GiB: created when applied").arg(e.newGiB);
        } else if (!e.disk.editable) {
            subtitle << "Set by hand: see the Arguments page";
        } else if (!e.file.isEmpty() && !QFileInfo::exists(path)) {
            subtitle << "Not found";
        } else if (e.disk.cdrom && e.file.isEmpty()) {
            subtitle << "Empty";
        }
        GtkWidget *row = Gtk4::actionRow(e.file.isEmpty() ? QString("No disc")
                                                          : QFileInfo(e.file).fileName(),
                                         subtitle.join(" · "));
        GtkWidget *icon = gtk_image_new_from_icon_name(e.disk.cdrom ? "drive-optical-symbolic"
                                                                    : "drive-harddisk-symbolic");
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), icon);
        if (!e.file.isEmpty()) {
            gtk_widget_set_tooltip_text(row, utf8(QDir::toNativeSeparators(path)).constData());
        }
        if (e.disk.editable && e.disk.cdrom) {
            GtkWidget *disc = Gtk4::rowButton("document-open-symbolic", "Choose a Disc");
            GtkWidget *eject = Gtk4::rowButton("media-eject-symbolic", "Eject");
            gtk_widget_set_sensitive(eject, !e.file.isEmpty());
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), disc);
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), eject);
            on<void()>(disc, "clicked", [this, i]() { chooseDisc(i); });
            on<void()>(eject, "clicked", [this, i]() {
                later([this, i]() {
                    m_entries[i].file.clear();
                    fill();
                    changed();
                });
            });
        }
        if (e.disk.editable && e.newGiB > 0) {
            GtkWidget *size = Gtk4::rowTextButton("_Size…", "The size of the new disk");
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), size);
            on<void()>(size, "clicked", [this, i]() { askSize(i); });
        }
        if (e.disk.editable) {
            GtkWidget *remove = Gtk4::rowButton("list-remove-symbolic", "Remove From the VM");
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove);
            on<void()>(remove, "clicked", [this, i]() { later([this, i]() { this->remove(i); }); });
        } else {
            gtk_widget_set_sensitive(row, false);
        }
        adw_preferences_group_add(m_disks, row);
        m_diskRows << row;
    }
    adw_preferences_group_add(m_disks, m_addDisk);
    adw_preferences_group_add(m_disks, m_addCdrom);
    fillBoot();
}

/*
 * A disk added on the page: in the firmware's own order (none set), with
 * the others of its kind, on; in an order set, off at the end, which is
 * what the VM does with it until the order changes
 */
void StoragePage::addBootItem(const Entry &entry)
{
    BootItem item;

    item.kind = entry.disk.cdrom ? VmConfig::BootEntry::Cdrom : VmConfig::BootEntry::HardDisk;
    item.entry = entry.id;
    if (m_bootSet || m_bootChanged) {
        m_bootItems << item;
        return;
    }
    item.on = true;
    /* after the last of its kind, or before the first of a kind that comes after */
    qsizetype at = m_bootItems.size();
    for (qsizetype i = 0; i < m_bootItems.size(); i++) {
        if (m_bootItems[i].kind > item.kind || !m_bootItems[i].on) {
            at = i;
            break;
        }
    }
    m_bootItems.insert(at, item);
}

QString StoragePage::bootText(const BootItem &b) const
{
    if (b.kind == VmConfig::BootEntry::Network) {
        return QString("Network (PXE): %1").arg(b.card);
    }
    const auto e = std::find_if(m_entries.cbegin(), m_entries.cend(),
                                [&b](const Entry &e) { return e.id == b.entry; });
    if (e == m_entries.cend() || e->removed) {
        return {};
    }
    const QString file = QFileInfo(e->file).fileName();
    if (b.kind == VmConfig::BootEntry::Cdrom) {
        return file.isEmpty() ? QString("CD/DVD: empty") : QString("CD/DVD: %1").arg(file);
    }
    return QString("Hard disk: %1").arg(file);
}

/* The boot order's rows from m_bootItems: checked, unchecked, or greyed */
void StoragePage::fillBoot()
{
    QList<int> shown;

    for (GtkWidget *row : std::as_const(m_bootRows)) {
        adw_preferences_group_remove(m_boot, row);
    }
    m_bootRows.clear();
    adw_preferences_group_remove(m_boot, m_bootHint);
    for (int i = 0; i < m_bootItems.size(); i++) {
        if (!bootText(m_bootItems[i]).isEmpty()) {
            shown << i;
        }
    }
    for (int n = 0; n < shown.size(); n++) {
        const int i = shown[n];
        const BootItem &b = m_bootItems[i];
        GtkWidget *row = Gtk4::actionRow(bootText(b));
        GtkWidget *check = gtk_check_button_new();
        GtkWidget *icon = gtk_image_new_from_icon_name(
            b.kind == VmConfig::BootEntry::Network ? "network-wired-symbolic"
            : b.kind == VmConfig::BootEntry::Cdrom ? "drive-optical-symbolic"
                                                   : "drive-harddisk-symbolic");

        gtk_widget_set_valign(check, GTK_ALIGN_CENTER);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(check), b.editable && b.on);
        /* the check box first: prefixes go in front of those before */
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), icon);
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), check);
        if (b.editable) {
            /* the devices set up by hand stay where they are: last, off */
            const bool downOk = n + 1 < shown.size() && m_bootItems[shown[n + 1]].editable;
            GtkWidget *up = Gtk4::rowButton("go-up-symbolic", "Move Up");
            GtkWidget *down = Gtk4::rowButton("go-down-symbolic", "Move Down");
            gtk_widget_set_sensitive(up, n > 0);
            gtk_widget_set_sensitive(down, downOk);
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), up);
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), down);
            adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), check);
            on<void()>(up, "clicked", [this, n]() { later([this, n]() { moveBoot(n, -1); }); });
            on<void()>(down, "clicked", [this, n]() { later([this, n]() { moveBoot(n, 1); }); });
            on<void(GParamSpec *)>(check, "notify::active", [this, i, check](GParamSpec *) {
                const bool on = gtk_check_button_get_active(GTK_CHECK_BUTTON(check));
                later([this, i, on]() { setBootOn(i, on); });
            });
        } else {
            /* -drive if=scsi, or a -blockdev without its -device */
            gtk_widget_set_sensitive(row, false);
            gtk_widget_set_tooltip_text(row, "Set up by hand: see the Arguments page");
        }
        adw_preferences_group_add(m_boot, row);
        m_bootRows << row;
    }
    gtk_label_set_text(GTK_LABEL(m_bootHint),
                       m_bootSet || m_bootChanged
                           ? "The VM starts from the first checked device it can start from. The "
                             "others stay in the VM, but it never starts from them."
                           : "No order is set: the firmware starts from the devices in an order of "
                             "its own. Changing the list sets one.");
    adw_preferences_group_add(m_boot, m_bootHint);
}

/* A device checked or unchecked: one stays checked, to start from */
void StoragePage::setBootOn(int i, bool on)
{
    if (i < 0 || i >= m_bootItems.size() || m_bootItems[i].on == on) {
        return;
    }
    if (!on && std::count_if(m_bootItems.cbegin(), m_bootItems.cend(),
                             [](const BootItem &b) { return b.on; }) <= 1) {
        fillBoot();
        return;
    }
    m_bootItems[i].on = on;
    m_bootChanged = true;
    fillBoot();
    changed();
}

void StoragePage::moveBoot(int index, int by)
{
    QList<int> shown;

    for (int i = 0; i < m_bootItems.size(); i++) {
        if (!bootText(m_bootItems[i]).isEmpty()) {
            shown << i;
        }
    }
    if (index < 0 || index + by < 0 || index >= shown.size() || index + by >= shown.size() ||
        !m_bootItems[shown[index]].editable || !m_bootItems[shown[index + by]].editable) {
        return;
    }
    m_bootItems.swapItemsAt(shown[index], shown[index + by]);
    m_bootChanged = true;
    fillBoot();
    changed();
}

QStringList StoragePage::bootOrder() const
{
    QStringList list;

    for (const BootItem &b : m_bootItems) {
        const QString text = bootText(b);
        if (!text.isEmpty()) {
            list << (b.on ? "[x] " : "[ ] ") + text;
        }
    }
    return list;
}

QString StoragePage::newDiskName() const
{
    const QDir dir(m_vmDir);

    for (int n = 1;; n++) {
        const QString name = n == 1 ? QString("disk.qcow2") : QString("disk%1.qcow2").arg(n);
        bool used = dir.exists(name) || m_pending.contains(name);
        for (const Entry &e : m_entries) {
            used |= e.file == name;
        }
        if (!used) {
            return name;
        }
    }
}

void StoragePage::addDisk(const QString &file, int newGiB, Disk::Bus bus)
{
    Entry e;

    e.disk.cdrom = false;
    e.disk.bus = bus;
    if (newGiB > 0) {
        e.file = newDiskName();
        e.newGiB = newGiB;
    } else {
        e.file = file;
    }
    e.id = m_nextId++;
    m_entries << e;
    addBootItem(e);
    fill();
    changed();
}

void StoragePage::addCdrom(const QString &iso)
{
    Entry e;

    e.disk.cdrom = true;
    e.disk.bus = m_virt ? Disk::Scsi : Disk::Sata;
    e.file = iso;
    e.id = m_nextId++;
    m_entries << e;
    addBootItem(e);
    fill();
    changed();
}

void StoragePage::askNewDisk()
{
    auto *dialog = new AddDiskDialog(m_virt, presenter(),
                                     guarded<void(const QString &, int, Disk::Bus)>(
                                         [this](const QString &image, int size, Disk::Bus bus) {
                                             addDisk(image, size, bus);
                                         }));
    present(dialog->dialog());
}

void StoragePage::chooseDisc(int i)
{
    if (i < 0 || i >= m_entries.size()) {
        return;
    }
    const QString start = m_entries[i].file.isEmpty()
                              ? QDir::homePath()
                              : QFileInfo(QDir(m_vmDir).absoluteFilePath(m_entries[i].file)).path();
    Gtk4::chooseFile(widget(), "Disc Image", kDiscImages, start,
                     guarded<void(const QString &)>([this, i](const QString &iso) {
                         if (i < m_entries.size()) {
                             m_entries[i].file = iso;
                             fill();
                             changed();
                         }
                     }));
}

void StoragePage::askSize(int i)
{
    if (i < 0 || i >= m_entries.size() || m_entries[i].newGiB <= 0) {
        return;
    }
    AdwDialog *alert = Gtk4::alert("New Disk", "Its size in GiB:",
                                   {{"cancel", "_Cancel"}, {"set", "_Set Size"}}, "set");
    GtkWidget *size = gtk_spin_button_new_with_range(1, 65536, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(size), m_entries[i].newGiB);
    gtk_widget_set_halign(size, GTK_ALIGN_CENTER);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(alert), size);
    Gtk4::onResponse(alert, guarded<void(const QString &)>([this, i, size](const QString &response) {
        if (response != "set" || i >= m_entries.size()) {
            return;
        }
        const int gib = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(size));
        m_entries[i].newGiB = gib;
        if (m_entries[i].disk.line >= 0) {
            /* added already: the size waits with the name */
            m_pending[m_entries[i].file] = gib;
        }
        fill();
        changed();
    }));
    present(alert);
}

void StoragePage::remove(int i)
{
    if (i < 0 || i >= m_entries.size()) {
        return;
    }
    /* out of the boot order too */
    const int id = m_entries[i].id;
    m_bootItems.removeIf([id](const BootItem &item) { return item.entry == id; });
    if (m_entries[i].disk.line < 0) {
        m_entries.removeAt(i);
    } else {
        m_entries[i].removed = true;
    }
    fill();
    changed();
}
