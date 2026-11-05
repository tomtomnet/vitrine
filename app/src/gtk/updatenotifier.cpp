// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/updatenotifier.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>

#include "core/paths.h"
#include "gtk/markup.h"
#include "gtk/widgets.h"
#include "ui/updatetext.h"

/* configured by CMake */
#if __has_include("buildinfo.h")
#include "buildinfo.h"
#else
#define VITRINE_COMMIT ""
#define VITRINE_SOURCE_DIR ""
#endif

using Gtk4::utf8;

static const qint64 kDaySecs = 24 * 3600;
/* Check for Updates again soon after: the answer of a moment ago */
static const qint64 kAgainSecs = 5 * 60;

static QSettings settings()
{
    return QSettings(Paths::settingsPath(), QSettings::IniFormat);
}

/* The dot of the news, in the accent colour, once for the display */
static void addStyle()
{
    static bool added = false;

    if (added || !gdk_display_get_default()) {
        return;
    }
    added = true;
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, ".vitrine-news { background-color: @accent_bg_color; "
                                           "border-radius: 9999px; min-width: 8px; "
                                           "min-height: 8px; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

UpdateNotifier::UpdateNotifier() : m_check(new UpdateCheck(&m_qt)), m_daily(new QTimer(&m_qt))
{
    const QList<UpdateCheck::Project> running = projects();
    const QSettings s = settings();

    QObject::connect(m_check, &UpdateCheck::finished, &m_qt,
                     [this](const QList<UpdateCheck::Result> &results) { finished(results); });
    /* the last answer, for what still runs: nothing asked of GitHub at start */
    for (const UpdateCheck::Result &r : UpdateCheck::fromJson(
             QJsonDocument::fromJson(s.value("updates/results").toByteArray()).array())) {
        for (const UpdateCheck::Project &p : running) {
            if (p.name == r.project.name && p.commit == r.project.commit) {
                m_results << r;
            }
        }
    }
    /* a while after the start, then every hour: at most once a day, see maybeCheck() */
    m_daily->setInterval(3600 * 1000);
    QObject::connect(m_daily, &QTimer::timeout, &m_qt, [this]() { maybeCheck(); });
    m_daily->start();
    QTimer::singleShot(15000, &m_qt, [this]() { maybeCheck(); });
}

/* A button gone with its window: out of the list */
static void buttonGone(gpointer self, GObject *gone)
{
    static_cast<QList<GtkWidget *> *>(self)->removeAll(reinterpret_cast<GtkWidget *>(gone));
}

UpdateNotifier::~UpdateNotifier()
{
    for (GtkWidget *button : std::as_const(m_buttons)) {
        g_object_weak_unref(G_OBJECT(button), &buttonGone, &m_buttons);
    }
}

QList<UpdateCheck::Project> UpdateNotifier::projects()
{
    static const QRegularExpression sha("^[0-9a-f]{40}$");
    QList<UpdateCheck::Project> list;

    /* built from a git checkout */
    if (sha.match(VITRINE_COMMIT).hasMatch()) {
        list << UpdateCheck::Project{"vitrine", "tomtomnet/vitrine", "main", VITRINE_COMMIT};
    }
    return list;
}

void UpdateNotifier::addMenuButton(GtkWidget *button)
{
    addStyle();
    m_buttons << button;
    g_object_weak_ref(G_OBJECT(button), &buttonGone, &m_buttons);
    updateButtons();
}

QString UpdateNotifier::newsText() const
{
    QStringList news;

    for (const UpdateCheck::Result &r : m_results) {
        if (r.newCommits > 0) {
            news << r.project.name + ": " + UpdateText::newCommits(r.newCommits);
        }
    }
    return news.join('\n');
}

void UpdateNotifier::updateButtons()
{
    const QString news = newsText();

    for (GtkWidget *button : std::as_const(m_buttons)) {
        if (news.isEmpty()) {
            gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), "open-menu-symbolic");
            gtk_widget_set_tooltip_text(button, "Main Menu");
            continue;
        }
        /* the menu's icon with a dot at its corner */
        GtkWidget *overlay = gtk_overlay_new();
        GtkWidget *dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_overlay_set_child(GTK_OVERLAY(overlay),
                              gtk_image_new_from_icon_name("open-menu-symbolic"));
        gtk_widget_add_css_class(dot, "vitrine-news");
        gtk_widget_set_halign(dot, GTK_ALIGN_END);
        gtk_widget_set_valign(dot, GTK_ALIGN_START);
        gtk_widget_set_can_target(dot, false);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), dot);
        gtk_menu_button_set_child(GTK_MENU_BUTTON(button), overlay);
        gtk_widget_set_tooltip_text(button, utf8("Main Menu\n" + news).constData());
    }
}

void UpdateNotifier::revalidate()
{
    const QList<UpdateCheck::Project> running = projects();
    QList<UpdateCheck::Result> kept;

    for (UpdateCheck::Result r : std::as_const(m_results)) {
        for (const UpdateCheck::Project &p : running) {
            if (p.name != r.project.name) {
                continue;
            }
            if (p.commit != r.project.commit && p.commit == r.head) {
                /* built the newest one GitHub told of */
                r.project.commit = p.commit;
                r.newCommits = 0;
                r.subjects.clear();
            }
            /* another commit, unknown until the next check */
            if (p.commit == r.project.commit) {
                kept << r;
            }
        }
    }
    m_results = kept;
    save();
    updateButtons();
}

void UpdateNotifier::save()
{
    settings().setValue(
        "updates/results",
        QJsonDocument(UpdateCheck::toJson(m_results)).toJson(QJsonDocument::Compact));
}

void UpdateNotifier::maybeCheck()
{
    const QSettings s = settings();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QDateTime last = s.value("updates/last").toDateTime();
    const QDateTime notBefore = s.value("updates/notBefore").toDateTime();

    if (!s.value("updates/check", true).toBool() || m_check->isChecking()) {
        return;
    }
    /* a day since the last one (a clock put back does not stop it), and as GitHub asked */
    if ((last.isValid() && last <= now && last.secsTo(now) < kDaySecs) ||
        (notBefore.isValid() && now < notBefore)) {
        return;
    }
    start(false);
}

void UpdateNotifier::checkNow()
{
    const QSettings s = settings();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QDateTime last = s.value("updates/last").toDateTime();
    const QDateTime notBefore = s.value("updates/notBefore").toDateTime();

    if (m_check->isChecking()) {
        m_asked = true;
        return;
    }
    if (last.isValid() && last <= now && last.secsTo(now) < kAgainSecs && !m_results.isEmpty()) {
        showResults();
        return;
    }
    if (notBefore.isValid() && now < notBefore) {
        if (present) {
            present(Gtk4::alert("Updates", UpdateText::waitText(notBefore)));
        }
        return;
    }
    start(true);
}

void UpdateNotifier::start(bool asked)
{
    const QList<UpdateCheck::Project> list = projects();
    QSettings s = settings();

    /* now, whatever the answer: a failure too waits a day */
    s.setValue("updates/last", QDateTime::currentDateTimeUtc());
    m_asked = asked;
    if (list.isEmpty()) {
        if (asked && present) {
            present(Gtk4::alert("Updates", UpdateText::nothingToCheck()));
        }
        return;
    }
    m_check->check(list);
}

void UpdateNotifier::finished(const QList<UpdateCheck::Result> &results)
{
    QSettings s = settings();

    m_results = results;
    save();
    for (const UpdateCheck::Result &r : results) {
        if (r.limitedUntil.isValid()) {
            s.setValue("updates/notBefore", r.limitedUntil);
        }
    }
    updateButtons();
    if (m_asked) {
        m_asked = false;
        showResults();
        return;
    }
    /* news: said once, as they come */
    const QString news = newsText();
    if (!news.isEmpty() && news != m_told && notify) {
        m_told = news;
        notify(QString("Vitrine has new commits on GitHub (%1): Check for Updates… in the main "
                       "menu tells more.")
                   .arg(QString(news).replace('\n', ", ")));
    }
}

void UpdateNotifier::showResults()
{
    revalidate();
    if (!present) {
        return;
    }
    AdwDialog *dialog = adw_alert_dialog_new("Updates", nullptr);
    auto *alert = ADW_ALERT_DIALOG(dialog);

    adw_alert_dialog_set_body_use_markup(alert, true);
    adw_alert_dialog_set_body(
        alert, utf8(Markup::pango(UpdateText::results(m_results, VITRINE_SOURCE_DIR))).constData());
    adw_alert_dialog_set_prefer_wide_layout(alert, true);
    adw_alert_dialog_add_response(alert, "close", "_Close");
    adw_alert_dialog_set_close_response(alert, "close");
    adw_alert_dialog_set_default_response(alert, "close");
    m_told = newsText();
    present(dialog);
}
