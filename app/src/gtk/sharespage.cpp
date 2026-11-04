// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/sharespage.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include "core/paths.h"
#include "gtk/dialog.h"
#include "gtk/rows.h"
#include "gtk/widgets.h"

using Gtk4::utf8;
using Share = VmConfig::Share;

static const char *const kCacheModes[][2] = {
    {"auto", "Automatic"},
    {"always", "Always"},
    {"never", "Never"},
};

static QString cacheName(const QString &mode)
{
    for (const auto &m : kCacheModes) {
        if (mode == m[0]) {
            return m[1];
        }
    }
    return mode;
}

static bool sameShares(const QList<Share> &a, const QList<Share> &b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (qsizetype i = 0; i < a.size(); i++) {
        if (a[i].tag != b[i].tag || a[i].path != b[i].path || a[i].cache != b[i].cache ||
            a[i].readonly != b[i].readonly || a[i].mount != b[i].mount) {
            return false;
        }
    }
    return true;
}

/* Adds or edits a shared folder: the Qt app's ShareDialog, with its checks */
class ShareDialog : public Gtk4::Dialog
{
public:
    using Done = std::function<void(const Share &share)>;

    ShareDialog(const Share &share, const QStringList &otherTags, Presenter presenter, Done done)
        : Dialog(adw_dialog_new(), std::move(presenter)), m_done(std::move(done)),
          m_otherTags(otherTags)
    {
        GtkWidget *view = adw_toolbar_view_new();
        GtkWidget *header = adw_header_bar_new();
        GtkWidget *cancel = gtk_button_new_with_mnemonic("_Cancel");
        GtkWidget *choose = Gtk4::rowButton("folder-open-symbolic", "Choose the Folder");
        auto *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
        AdwPreferencesGroup *what = Gtk4::group();
        AdwPreferencesGroup *how = Gtk4::group();
        AdwPreferencesGroup *mount = Gtk4::group();
        QStringList caches;
        const bool adding = share.tag.isEmpty();

        adw_dialog_set_title(dialog(), adding ? "Add a Shared Folder" : "Edit Shared Folder");
        adw_dialog_set_content_width(dialog(), 540);
        m_ok = gtk_button_new_with_mnemonic(adding ? "_Add" : "_Save");
        gtk_widget_add_css_class(m_ok, "suggested-action");
        adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), false);
        adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), false);
        adw_header_bar_pack_start(ADW_HEADER_BAR(header), cancel);
        adw_header_bar_pack_end(ADW_HEADER_BAR(header), m_ok);
        adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
        adw_dialog_set_default_widget(dialog(), m_ok);

        m_path = Gtk4::entryRow("Folder", share.path);
        gtk_widget_set_name(m_path, "path");
        adw_entry_row_add_suffix(ADW_ENTRY_ROW(m_path), choose);
        m_tag = Gtk4::entryRow("Name in the Guest", share.tag);
        gtk_widget_set_name(m_tag, "tag");
        adw_preferences_group_add(what, m_path);
        adw_preferences_group_add(what, m_tag);
        GtkWidget *tagHint = Gtk4::caption("The guest mounts the folder by this name.");
        gtk_widget_set_margin_top(tagHint, 6);
        adw_preferences_group_add(what, tagHint);
        adw_preferences_page_add(page, what);

        for (const auto &mode : kCacheModes) {
            caches << mode[1];
        }
        m_cache = Gtk4::comboRow("Cache", caches);
        int cache = 0;
        for (int i = 0; i < int(std::size(kCacheModes)); i++) {
            if (share.cache == kCacheModes[i][0]) {
                cache = i;
            }
        }
        Gtk4::setSelected(m_cache, cache);
        m_readonly = Gtk4::switchRow("Read Only", "The VM cannot change the files.");
        adw_switch_row_set_active(ADW_SWITCH_ROW(m_readonly), share.readonly);
        adw_preferences_group_add(how, m_cache);
        adw_preferences_group_add(how, m_readonly);
        adw_preferences_page_add(page, how);

        /* new folders are mounted at /mnt/NAME, following the name */
        m_mount = Gtk4::switchRow("Mount It in the Guest at Start");
        adw_switch_row_set_active(ADW_SWITCH_ROW(m_mount), adding || !share.mount.isEmpty());
        m_mountDir = Gtk4::entryRow("Mount Point",
                                    share.mount.isEmpty() ? "/mnt/" + share.tag : share.mount);
        gtk_widget_set_name(m_mountDir, "mount");
        m_tagEdited = !share.tag.isEmpty();
        m_mountEdited = !share.mount.isEmpty();
        adw_preferences_group_add(mount, m_mount);
        adw_preferences_group_add(mount, m_mountDir);
        GtkWidget *mountHint = Gtk4::caption("systemd 254 and later mount it at boot; older guests "
                                             "need the QEMU guest agent (qemu-guest-agent).");
        gtk_widget_set_margin_top(mountHint, 6);
        adw_preferences_group_add(mount, mountHint);
        m_error = Gtk4::label({});
        gtk_widget_add_css_class(m_error, "error");
        gtk_widget_set_margin_top(m_error, 12);
        adw_preferences_group_add(mount, m_error);
        adw_preferences_page_add(page, mount);

        adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), GTK_WIDGET(page));
        adw_dialog_set_child(dialog(), view);

        on<void()>(cancel, "clicked", [this]() { close(); });
        on<void()>(m_ok, "clicked", [this]() {
            const Done done = m_done;
            const Share result = this->share();
            forceClose();
            done(result);
        });
        on<void()>(choose, "clicked", [this]() {
            Gtk4::chooseFolder(GTK_WIDGET(dialog()), "Folder to Share", Gtk4::text(m_path),
                               guarded<void(const QString &)>([this](const QString &path) {
                                   Gtk4::setText(m_path, path);
                               }));
        });
        on<void()>(m_path, "changed", [this]() {
            if (!m_tagEdited) {
                static const QRegularExpression unsafe("[^A-Za-z0-9_.-]+");
                QString tag = QFileInfo(QDir::cleanPath(Gtk4::text(m_path).trimmed())).fileName();
                tag.replace(unsafe, "-");
                m_settingTag = true;
                Gtk4::setText(m_tag, tag.left(36));
                m_settingTag = false;
                followTag();
            }
            validate();
        });
        on<void()>(m_tag, "changed", [this]() {
            if (!m_settingTag) {
                m_tagEdited = true;
            }
            followTag();
            validate();
        });
        on<void()>(m_mountDir, "changed", [this]() {
            if (!m_settingMount) {
                m_mountEdited = true;
            }
            validate();
        });
        on<void(GParamSpec *)>(m_mount, "notify::active", [this](GParamSpec *) {
            gtk_widget_set_sensitive(m_mountDir, adw_switch_row_get_active(ADW_SWITCH_ROW(m_mount)));
            validate();
        });
        on<void(GParamSpec *)>(m_cache, "notify::selected", [this](GParamSpec *) { describeCache(); });
        gtk_widget_set_sensitive(m_mountDir, adw_switch_row_get_active(ADW_SWITCH_ROW(m_mount)));
        describeCache();
        validate();
    }

    Share share() const
    {
        Share s;

        s.tag = Gtk4::text(m_tag).trimmed();
        s.path = QDir::cleanPath(Gtk4::text(m_path).trimmed());
        s.cache = kCacheModes[qMax(0, Gtk4::selected(m_cache))][0];
        s.readonly = adw_switch_row_get_active(ADW_SWITCH_ROW(m_readonly));
        s.mount = adw_switch_row_get_active(ADW_SWITCH_ROW(m_mount))
                      ? QDir::cleanPath(Gtk4::text(m_mountDir).trimmed())
                      : QString();
        return s;
    }

private:
    /* the mount point follows the name, until edited */
    void followTag()
    {
        if (!m_mountEdited) {
            m_settingMount = true;
            Gtk4::setText(m_mountDir, "/mnt/" + Gtk4::text(m_tag).trimmed());
            m_settingMount = false;
        }
    }

    void describeCache()
    {
        const QString mode = kCacheModes[qMax(0, Gtk4::selected(m_cache))][0];
        Gtk4::setSubtitle(
            m_cache,
            mode == "auto" ? QString("The guest keeps files in its cache for a moment: changes made "
                                     "on this computer show up within a second.")
            : mode == "always" ? QString("The fastest. The guest keeps files in its cache, so changes "
                                         "made on this computer may not show up in the guest. Best "
                                         "when only the VM uses the folder.")
                               : QString("Always up to date with this computer, but slower."));
    }

    void validate()
    {
        static const QRegularExpression valid("^[A-Za-z0-9_.-]{1,36}$");
        const QString path = Gtk4::text(m_path).trimmed();
        const QString tag = Gtk4::text(m_tag).trimmed();
        const bool mount = adw_switch_row_get_active(ADW_SWITCH_ROW(m_mount));
        QString error;

        if (path.isEmpty()) {
            error = " ";
        } else if (!QFileInfo(path).isDir()) {
            error = "This folder does not exist.";
        } else if (tag.isEmpty()) {
            error = "Give the folder a name for the guest.";
        } else if (!valid.match(tag).hasMatch()) {
            error = "The name can have up to 36 letters, digits, dots, dashes and underscores.";
        } else if (m_otherTags.contains(tag)) {
            error = "Another shared folder has this name.";
        } else if (mount && !Gtk4::text(m_mountDir).trimmed().startsWith('/')) {
            error = QString("The guest mounts it at a full path, like /mnt/%1.").arg(tag);
        }
        gtk_label_set_text(GTK_LABEL(m_error), utf8(error.trimmed()).constData());
        gtk_widget_set_visible(m_error, !error.trimmed().isEmpty());
        gtk_widget_set_sensitive(m_ok, error.isEmpty());
    }

    Done m_done;
    QStringList m_otherTags;
    bool m_tagEdited = false;
    bool m_mountEdited = false;
    /* the dialog itself writes them, not the user */
    bool m_settingTag = false;
    bool m_settingMount = false;
    GtkWidget *m_path;
    GtkWidget *m_tag;
    GtkWidget *m_cache;
    GtkWidget *m_readonly;
    GtkWidget *m_mount;
    GtkWidget *m_mountDir;
    GtkWidget *m_error;
    GtkWidget *m_ok;
};

/* How the guest mounts @share, with the commands to copy */
static GtkWidget *mountHelp(const Share &share)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    const QString tag = share.tag;
    QStringList commands;
    QStringList more;

    for (const auto side : {&gtk_widget_set_margin_start, &gtk_widget_set_margin_end,
                            &gtk_widget_set_margin_top, &gtk_widget_set_margin_bottom}) {
        side(box, 12);
    }
    auto text = [box](const QString &line) { gtk_box_append(GTK_BOX(box), Gtk4::label(line, true)); };
    auto code = [box](const QStringList &lines) {
        GtkWidget *label = Gtk4::label(lines.join('\n'), true);
        gtk_widget_add_css_class(label, "monospace");
        gtk_box_append(GTK_BOX(box), label);
    };
    if (!share.mount.isEmpty()) {
        text(QString("The guest mounts it at %1 at each start. systemd 254 and later do so at boot "
                     "(Fedora 39, Debian 13, Ubuntu 24.04 and later); older guests need the QEMU "
                     "guest agent, the qemu-guest-agent package, which starts by itself once "
                     "installed.")
                 .arg(share.mount));
        text("By hand instead:");
        code({QString("sudo mount -t virtiofs %1 %2").arg(tag, share.mount)});
        return box;
    }
    const QString mount = "/mnt/" + tag;
    text("Mount it:");
    code({QString("sudo mkdir -p %1").arg(mount), QString("sudo mount -t virtiofs %1 %2").arg(tag, mount)});
    text("Or mount it at boot, with this line in /etc/fstab:");
    code({QString("%1 %2 virtiofs defaults,nofail 0 0").arg(tag, mount)});
    return box;
}

SharesPage::SharesPage()
    : SettingsPage(adw_preferences_page_new()),
      m_memory(ADW_BANNER(adw_banner_new(""))),
      m_missing(Gtk4::group()),
      m_list(Gtk4::group({}, "Folders of this computer the VM can use. They are shared with "
                             "virtiofs, which is fast and works with Linux 5.4 or later in the "
                             "guest."))
{
    auto *page = ADW_PREFERENCES_PAGE(widget());
    GtkWidget *missing = Gtk4::actionRow("virtiofsd Is Not Installed",
                                         "Shared folders do not work without it: install it with "
                                         "sudo dnf install virtiofsd, or set its path in the "
                                         "preferences.");
    GtkWidget *warning = gtk_image_new_from_icon_name("dialog-warning-symbolic");
    AdwPreferencesGroup *windows = Gtk4::group();

    adw_preferences_page_set_banner(page, m_memory);
    adw_banner_set_button_label(m_memory, "_Use Shared Memory");
    gtk_widget_add_css_class(warning, "warning");
    adw_action_row_add_prefix(ADW_ACTION_ROW(missing), warning);
    adw_preferences_group_add(m_missing, missing);
    adw_preferences_page_add(page, m_missing);

    m_add = Gtk4::buttonRow("_Add Shared Folder…", "list-add-symbolic");
    g_object_ref_sink(m_add);
    adw_preferences_group_add(m_list, m_add);
    adw_preferences_page_add(page, m_list);
    adw_preferences_group_add(windows, Gtk4::caption("Windows guests need the virtio-win drivers "
                                                     "and WinFsp."));
    adw_preferences_page_add(page, windows);

    on<void()>(m_add, "activated", [this]() { edit(-1); });
    on<void()>(m_memory, "button-clicked", [this]() {
        m_fixMemory = true;
        updateNotes();
        changed();
    });
}

void SharesPage::load(const ArgsFile &args)
{
    m_shares = VmConfig::shares(args);
    m_loaded = m_shares;
    m_sharedMemory = VmConfig::hasSharedMemory(args);
    m_fixMemory = false;
    fill();
}

void SharesPage::save(ArgsFile &args)
{
    if (!sameShares(m_shares, m_loaded)) {
        VmConfig::setShares(args, m_shares);
        m_loaded = m_shares;
    }
    if (!m_shares.isEmpty() && m_fixMemory && !VmConfig::hasSharedMemory(args)) {
        VmConfig::useSharedMemory(args);
    }
    m_sharedMemory = VmConfig::hasSharedMemory(args);
    m_fixMemory = false;
}

bool SharesPage::isModified() const
{
    return !sameShares(m_shares, m_loaded) || m_fixMemory;
}

void SharesPage::fill()
{
    for (GtkWidget *row : std::as_const(m_rows)) {
        adw_preferences_group_remove(m_list, row);
    }
    m_rows.clear();
    adw_preferences_group_remove(m_list, m_add);
    for (int i = 0; i < m_shares.size(); i++) {
        const Share &s = m_shares[i];
        GtkWidget *row = adw_expander_row_new();
        GtkWidget *edit = Gtk4::rowButton("document-edit-symbolic", "Edit");
        GtkWidget *remove = Gtk4::rowButton("list-remove-symbolic", "Remove");
        const QStringList subtitle = {QDir::toNativeSeparators(s.path),
                                      s.mount.isEmpty() ? QString("mounted by hand")
                                                        : QString("mounted at %1").arg(s.mount),
                                      QString("cache: %1").arg(cacheName(s.cache).toLower()),
                                      s.readonly ? QString("read only") : QString("read and write")};

        Gtk4::setTitle(row, s.tag);
        Gtk4::setSubtitle(row, subtitle.join(" · "));
        adw_expander_row_add_prefix(ADW_EXPANDER_ROW(row),
                                    gtk_image_new_from_icon_name("folder-symbolic"));
        /* Edit, then Remove: suffixes go in front of those before */
        adw_expander_row_add_suffix(ADW_EXPANDER_ROW(row), remove);
        adw_expander_row_add_suffix(ADW_EXPANDER_ROW(row), edit);
        adw_expander_row_add_row(ADW_EXPANDER_ROW(row), mountHelp(s));
        on<void()>(edit, "clicked", [this, i]() { this->edit(i); });
        on<void()>(remove, "clicked", [this, i]() { later([this, i]() { removeShare(i); }); });
        adw_preferences_group_add(m_list, row);
        m_rows << row;
    }
    adw_preferences_group_add(m_list, m_add);
    updateNotes();
}

void SharesPage::updateNotes()
{
    gtk_widget_set_visible(GTK_WIDGET(m_missing),
                           !m_shares.isEmpty() && Paths::virtiofsd().isEmpty());
    if (m_shares.isEmpty() || m_sharedMemory) {
        adw_banner_set_revealed(m_memory, false);
    } else if (m_fixMemory) {
        adw_banner_set_title(m_memory, "The guest memory will come from shared memory (memfd), "
                                       "which virtiofs needs. Its size stays the same.");
        adw_banner_set_button_label(m_memory, nullptr);
        adw_banner_set_revealed(m_memory, true);
    } else {
        adw_banner_set_title(m_memory, "virtiofs needs the guest memory to be shared memory, "
                                       "which it is not in the arguments of this VM.");
        adw_banner_set_button_label(m_memory, "_Use Shared Memory");
        adw_banner_set_revealed(m_memory, true);
    }
}

void SharesPage::edit(int row)
{
    QStringList others;
    const bool adding = row < 0 || row >= m_shares.size();

    for (int i = 0; i < m_shares.size(); i++) {
        if (i != row) {
            others << m_shares[i].tag;
        }
    }
    /* a new one read-only: a guest that can write to a host folder can
       change what the host runs from it, which the user allows by choice */
    Share fresh;
    fresh.readonly = true;
    auto *dialog = new ShareDialog(adding ? fresh : m_shares[row], others, presenter(),
                                   guarded<void(const Share &)>([this, row](const Share &share) {
                                       setShare(row, share);
                                   }));
    present(dialog->dialog());
}

void SharesPage::setShare(int row, const Share &share)
{
    if (row < 0 || row >= m_shares.size()) {
        m_shares << share;
        if (!m_sharedMemory) {
            /* virtiofs needs it: say so rather than ask */
            m_fixMemory = true;
        }
    } else {
        m_shares[row] = share;
    }
    fill();
    changed();
}

void SharesPage::removeShare(int row)
{
    if (row >= 0 && row < m_shares.size()) {
        m_shares.removeAt(row);
        fill();
        changed();
    }
}
