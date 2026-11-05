// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/notices.h"

#include <QTimer>

#include <algorithm>

#include "core/hostmemory.h"
#include "core/udmabuf.h"
#include "core/vmconfig.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "gtk/richtext.h"
#include "gtk/rows.h"
#include "gtk/widgets.h"
#include "ui/hosttuningtext.h"
#include "ui/memorytext.h"
#include "ui/udmabuftext.h"

using Gtk4::utf8;

/*
 * An alert with @box's title and its rich text, and @responses (the first
 * the close one).  An explanation of several paragraphs, commands among
 * them, reads from the left in a wide alert; a sentence is its body.
 */
static AdwDialog *boxAlert(const HostTuningText::Box &box,
                           const QList<std::pair<QString, QString>> &responses = {{"close", "_Close"}})
{
    AdwDialog *dialog = Gtk4::alert(box.title, QString(), responses);
    auto *alert = ADW_ALERT_DIALOG(dialog);
    const QString markup = Gtk4::markup(box.text);

    if (!markup.contains('\n')) {
        adw_alert_dialog_set_body_use_markup(alert, true);
        adw_alert_dialog_set_body(alert, utf8(markup).constData());
        return dialog;
    }
    GtkWidget *label = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(label), utf8(markup).constData());
    gtk_label_set_wrap(GTK_LABEL(label), true);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_name(label, "explanation");
    /* its link would take the focus first, scrolled to, the heading out of sight; a click opens it */
    gtk_widget_set_focusable(label, false);
    adw_alert_dialog_set_extra_child(alert, label);
    adw_alert_dialog_set_prefer_wide_layout(alert, true);
    return dialog;
}

/* The first sentence of @markup's first paragraph */
static QString firstSentence(const QString &markup)
{
    const QString paragraph = markup.section('\n', 0, 0);
    const qsizetype end = paragraph.indexOf(". ");

    return end < 0 ? paragraph : paragraph.left(end + 1);
}

Notices::Notices(HostSettings *host, VmStore *store, GtkWindow *window,
                 Gtk4::Dialog::Presenter present)
    : m_host(host), m_store(store), m_present(std::move(present)),
      /* native context's guest windows copied for want of udmabuf: host
         tuning raises its limits, and says so first */
      m_udmabuf(new UdmabufWatch(store, host, &m_qt)), m_memoryTimer(new QTimer(&m_qt)),
      m_button(gtk_menu_button_new()), m_popover(gtk_popover_new()),
      m_list(gtk_list_box_new())
{
    GtkWidget *icon = gtk_image_new_from_icon_name("dialog-warning-symbolic");

    /* the header bar's: kept until this goes, in the window or out of it */
    g_object_ref_sink(m_button);
    gtk_widget_set_name(m_button, "notices");
    gtk_widget_add_css_class(icon, "warning");
    gtk_menu_button_set_child(GTK_MENU_BUTTON(m_button), icon);
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(m_list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(m_list, "boxed-list");
    gtk_widget_set_size_request(m_list, 380, -1);
    gtk_widget_set_margin_start(m_list, 6);
    gtk_widget_set_margin_end(m_list, 6);
    gtk_widget_set_margin_top(m_list, 6);
    gtk_widget_set_margin_bottom(m_list, 6);
    /*
     * As wide as a column of text: a wrapping subtitle asks for its whole
     * line, and a popover wider than the monitor is taken down as it shows
     */
    GtkWidget *clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 420);
    adw_clamp_set_child(ADW_CLAMP(clamp), m_list);
    gtk_popover_set_child(GTK_POPOVER(m_popover), clamp);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(m_button), m_popover);
    gtk_widget_set_visible(m_button, false);

    if (host) {
        QObject::connect(host, &HostSettings::untunedChanged, &m_qt, [this]() { updateHost(); });
        QObject::connect(host, &HostSettings::groupSetupSuggested, &m_qt,
                         [this](const HostSettings::Status &status) { offerGroup(status); });
    }
    QObject::connect(m_udmabuf, &UdmabufWatch::changed, &m_qt, [this]() { updateUdmabuf(); });
    /* back from a terminal where the helper was installed, say */
    on<void(GParamSpec *)>(window, "notify::is-active", [this, window](GParamSpec *) {
        if (gtk_window_is_active(window) && m_host && m_host->untuned()) {
            m_host->recheck();
        }
    });

    /* the guests take their memory as they go */
    m_memoryTimer->setInterval(3000);
    QObject::connect(m_memoryTimer, &QTimer::timeout, &m_qt, [this]() { refreshMemory(); });
    m_memoryTimer->start();
    for (Vm *vm : store->vms()) {
        watch(vm);
    }
    QObject::connect(store, &VmStore::added, &m_qt, [this](Vm *vm) { watch(vm); });

    updateHost();
    updateUdmabuf();
    refreshMemory();
}

Notices::~Notices()
{
    stopCallbacks();
    /* out of the page's header bar, which outlives the window */
    if (GtkWidget *bar = gtk_widget_get_ancestor(m_button, ADW_TYPE_HEADER_BAR)) {
        adw_header_bar_remove(ADW_HEADER_BAR(bar), m_button);
    }
    g_object_unref(m_button);
}

void Notices::watch(Vm *vm)
{
    QObject::connect(vm->runner(), &VmRunner::stateChanged, &m_qt, [this]() { refreshMemory(); });
}

QList<Notices::Kind> Notices::shown() const
{
    QList<Kind> kinds;

    if (!m_hostText.isEmpty()) {
        kinds << Kind::HostTuning;
    }
    if (!m_udmabufText.isEmpty()) {
        kinds << Kind::Udmabuf;
    }
    if (!m_memoryText.isEmpty()) {
        kinds << Kind::Memory;
    }
    return kinds;
}

QString Notices::title(Kind kind) const
{
    switch (kind) {
    case Kind::HostTuning:
        return "Host Tuning Inactive";
    case Kind::Udmabuf:
        return m_udmabufTitle;
    case Kind::Memory:
        return MemoryText::title();
    }
    return {};
}

QString Notices::text(Kind kind) const
{
    switch (kind) {
    case Kind::HostTuning:
        return m_hostText;
    case Kind::Udmabuf:
        return m_udmabufText;
    case Kind::Memory:
        return m_memoryText;
    }
    return {};
}

void Notices::updateHost()
{
    /* tuning is on (the user's choice): it does not take effect */
    m_hostText = m_host && m_host->untuned()
                     ? HostTuningText::untuned(m_host->untunedCount(), m_host->untunedStatus().why())
                     : QString();
    update();
}

void Notices::updateUdmabuf()
{
    const QList<UdmabufWatch::Issue> issues = m_udmabuf->issues();
    QStringList lines;
    bool refused = false, device = false, blamed = false;

    for (const UdmabufWatch::Issue &issue : issues) {
        lines << UdmabufText::line(issue);
        refused |= issue.log.any();
        device |= issue.device();
        blamed |= issue.limitsBlamed();
    }
    m_udmabufTitle = issues.isEmpty() ? QString() : UdmabufText::title(refused, device);
    m_udmabufText = lines.join('\n');
    update();
    /*
     * Once per run, by itself, when there is something to do now: the
     * limits (or the device) still in the way, with host tuning on.  Off
     * by the user's choice, the notice is enough, as host tuning's says
     * nothing then; and buffers refused for another reason, or before the
     * limits were raised (in the log of a VM found running), call for no
     * fix.
     */
    if (!m_explained && HostSettings::enabled() && (blamed || device) &&
        m_udmabuf->limitsNow().low()) {
        m_explained = true;
        /*
         * After the turn of the event loop: a VM start that also offers to
         * set up the vitrine group (host tuning's question, which covers
         * this) offers it right after the answer that got here
         */
        QTimer::singleShot(0, &m_qt, [this]() {
            if ((!m_host || !m_host->groupOffered()) && !m_udmabuf->issues().isEmpty()) {
                explainUdmabuf();
            }
        });
    }
}

void Notices::refreshMemory()
{
    const HostMemory::Info host = HostMemory::read();
    QList<HostMemory::Vm> vms;
    qint64 guest = 0, held = 0;

    for (Vm *vm : m_store->vms()) {
        if (vm->runner()->isActive()) {
            const qint64 ram = VmConfig::memoryMiB(vm->args());
            vms << HostMemory::Vm{ram > 0 ? ram : 128, HostMemory::residentMiB(vm->runner()->pid())};
            guest += vms.last().guestMiB;
            held += vms.last().residentMiB;
        }
    }
    const bool tight = !vms.isEmpty() && HostMemory::tight(host, vms);
    const QString text = tight ? MemoryText::text(guest, held, host) : QString();
    if (text != m_memoryText) {
        m_memoryText = text;
        update();
    }
}

void Notices::update()
{
    const QList<Kind> kinds = shown();
    QStringList tip;

    if (kinds != m_rowKinds) {
        while (GtkWidget *row = gtk_widget_get_first_child(m_list)) {
            gtk_list_box_remove(GTK_LIST_BOX(m_list), row);
        }
        m_rows.clear();
        m_rowButtons.clear();
        for (const Kind kind : kinds) {
            GtkWidget *row = Gtk4::actionRow(title(kind));
            GtkWidget *button =
                Gtk4::rowTextButton(kind == Kind::Memory ? "_Details…" : "What to _Do…");

            adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 4);
            gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), false);
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), button);
            on<void()>(button, "clicked", [this, kind]() {
                gtk_popover_popdown(GTK_POPOVER(m_popover));
                explain(kind);
            });
            gtk_list_box_append(GTK_LIST_BOX(m_list), row);
            m_rows << row;
            m_rowButtons << button;
        }
        m_rowKinds = kinds;
    }
    for (qsizetype i = 0; i < kinds.size(); i++) {
        const Kind kind = kinds[i];
        Gtk4::setTitle(m_rows[i], title(kind));
        /* the memory's explanation: its first sentence here, the rest in Details */
        const QString subtitle = kind == Kind::Memory ? firstSentence(Gtk4::markup(text(kind)))
                                                      : Gtk4::escaped(text(kind));
        if (QString::fromUtf8(adw_action_row_get_subtitle(ADW_ACTION_ROW(m_rows[i]))) != subtitle) {
            adw_action_row_set_subtitle(ADW_ACTION_ROW(m_rows[i]), utf8(subtitle).constData());
        }
        gtk_widget_set_sensitive(m_rowButtons[i], kind != Kind::HostTuning || !m_rechecking);
        tip << title(kind);
    }
    gtk_widget_set_tooltip_text(m_button, utf8(tip.join('\n')).constData());
    if (kinds.isEmpty()) {
        gtk_popover_popdown(GTK_POPOVER(m_popover));
    }
    gtk_widget_set_visible(m_button, !kinds.isEmpty());
}

void Notices::explain(Kind kind)
{
    switch (kind) {
    case Kind::HostTuning:
        explainHost();
        break;
    case Kind::Udmabuf:
        explainUdmabuf();
        break;
    case Kind::Memory:
        explainMemory();
        break;
    }
}

void Notices::explainHost()
{
    if (!m_host || !m_host->untuned()) {
        updateHost();
        return;
    }
    /* the state now, not the one of the VM start: the helper installed or
       the group joined since */
    m_rechecking = true;
    update();
    m_host->recheck(&m_qt, guarded<void(const HostSettings::Status &)>(
                               [this](const HostSettings::Status &now) {
        m_rechecking = false;
        update();
        if (!m_host) {
            return;
        }
        if (now.active() || !m_host->untuned()) {
            /* the running VMs are being tuned: the notice goes with that */
            if (m_host->untuned()) {
                m_present(boxAlert(HostTuningText::active()));
            }
            return;
        }
        explainHostNow(m_host->untunedStatus());
    }));
}

void Notices::explainHostNow(const HostSettings::Status &status)
{
    QList<std::pair<QString, QString>> responses = {{"close", "_Close"}};

    if (status.needsGroup()) {
        responses << std::pair(QString("set-up"), QString("_Set Up…"));
    }
    responses << std::pair(QString("off"), QString("_Turn Off Tuning"));
    AdwDialog *dialog = boxAlert(HostTuningText::inactive(m_host->untunedCount(), status), responses);
    auto *alert = ADW_ALERT_DIALOG(dialog);
    adw_alert_dialog_set_response_appearance(alert, "off", ADW_RESPONSE_DESTRUCTIVE);
    /* opened by a click: Set Up may be the default; without it Close, not Turn Off Tuning */
    if (status.needsGroup()) {
        adw_alert_dialog_set_response_appearance(alert, "set-up", ADW_RESPONSE_SUGGESTED);
        adw_alert_dialog_set_default_response(alert, "set-up");
    }
    Gtk4::onResponse(dialog, guarded<void(const QString &)>([this](const QString &response) {
        if (response == "set-up") {
            setUpGroup(m_present);
        } else if (response == "off") {
            turnOffTuning();
        }
    }));
    m_present(dialog);
}

void Notices::offerGroup(const HostSettings::Status &status)
{
    AdwDialog *dialog = boxAlert(HostTuningText::offer(status),
                                 {{"not-now", "_Not Now"}, {"set-up", "_Set Up…"},
                                  {"off", "_Turn Off Tuning"}});
    auto *alert = ADW_ALERT_DIALOG(dialog);

    adw_alert_dialog_set_response_appearance(alert, "set-up", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_response_appearance(alert, "off", ADW_RESPONSE_DESTRUCTIVE);
    /* it comes up by itself, at a VM start: an Enter meant for the guest
       (its boot menu, its login) must not start the password dialog */
    adw_alert_dialog_set_default_response(alert, "not-now");
    Gtk4::onResponse(dialog, guarded<void(const QString &)>([this](const QString &response) {
        if (response == "set-up") {
            setUpGroup(m_present);
        } else if (response == "off") {
            turnOffTuning();
        }
        /* Not Now: not asked again in this run (HostSettings), the notice stays */
    }));
    m_present(dialog);
}

void Notices::explainUdmabuf()
{
    const QList<UdmabufWatch::Issue> issues = m_udmabuf->issues();

    if (issues.isEmpty()) {
        updateUdmabuf();
        return;
    }
    if (m_udmabufBox) {
        /* open already */
        return;
    }
    const Udmabuf::Limits now = m_udmabuf->limitsNow();
    const auto any = [&issues](bool (UdmabufWatch::Issue::*what)() const) {
        return std::any_of(issues.begin(), issues.end(),
                           [what](const UdmabufWatch::Issue &issue) { return (issue.*what)(); });
    };
    const bool refused = std::any_of(issues.begin(), issues.end(),
                                     [](const UdmabufWatch::Issue &issue) { return issue.log.any(); });
    const bool device = now.deviceErrno || any(&UdmabufWatch::Issue::device);
    const HostSettings::Status status = m_host ? m_host->untunedStatus() : HostSettings::Status();
    /* host tuning, where raising the limits helps; not without the helper
       installed: the explanation points to Installing */
    const bool raise = any(&UdmabufWatch::Issue::limitsBlamed) && !now.deviceErrno;
    QList<std::pair<QString, QString>> responses = {{"close", "_Close"}};
    QString action;

    if (raise && !HostSettings::enabled() && HostSettings::helperInstalled()) {
        action = "turn-on";
        responses << std::pair(action, QString("_Turn On Host Tuning"));
    } else if (raise && HostSettings::enabled() && m_host && m_host->untuned() && status.needsGroup()) {
        action = "set-up";
        responses << std::pair(action, QString("_Set Up…"));
    }
    AdwDialog *dialog = boxAlert({UdmabufText::title(refused, device),
                                  UdmabufText::explanation(issues, now)},
                                 responses);
    /* it may come up by itself, at a VM start: an Enter meant for the guest
       closes it, and nothing else */
    if (!action.isEmpty()) {
        adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), utf8(action).constData(),
                                                 ADW_RESPONSE_SUGGESTED);
    }
    Gtk4::onResponse(dialog, guarded<void(const QString &)>([this](const QString &response) {
        if (response == "set-up") {
            setUpGroup(m_present);
        } else if (response == "turn-on") {
            HostSettings::setEnabled(true);
            if (m_host) {
                m_host->preferencesChanged();
            }
        }
    }));
    m_udmabufBox = dialog;
    on<void()>(dialog, "closed", [this, dialog]() {
        if (m_udmabufBox == dialog) {
            m_udmabufBox = nullptr;
        }
    });
    m_present(dialog);
}

void Notices::explainMemory()
{
    if (!m_memoryText.isEmpty()) {
        m_present(boxAlert({MemoryText::title(), m_memoryText}));
    }
}

void Notices::setUpGroup(const Gtk4::Dialog::Presenter &present, std::function<void()> done)
{
    HostSettings *host = HostSettings::instance();

    if (!host) {
        if (done) {
            done();
        }
        return;
    }
    host->setUpGroup([present, done](HostSettings::Setup result, const QString &error,
                                     const HostSettings::Status &now) {
        switch (result) {
        case HostSettings::Setup::Cancelled:
            break;
        case HostSettings::Setup::Failed:
            /* pkexec's words, plain: rich text from here */
            present(boxAlert(HostTuningText::setUpFailed(error.toHtmlEscaped())));
            break;
        case HostSettings::Setup::Done:
            present(boxAlert(HostTuningText::setUpDone(now)));
            break;
        }
        if (done) {
            done();
        }
    });
}

void Notices::turnOffTuning()
{
    HostSettings::setEnabled(false);
    if (HostSettings *host = HostSettings::instance()) {
        host->preferencesChanged();
    }
}
