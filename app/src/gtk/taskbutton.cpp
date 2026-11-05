// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/taskbutton.h"

#include <QTimer>

#include <algorithm>
#include <cmath>
#include <utility>

#include "core/tasks.h"
#include "gtk/widgets.h"

using Gtk4::utf8;

namespace {

/* A turn of the ring while a task cannot tell how far it is, and the part that turns */
constexpr int kTurnMs = 1200;
constexpr double kTurningArc = 100;
/* The ring's diameter: a VM's icon in the list, and some */
constexpr int kRingSize = 24;

/* As the actions that start the job have it */
const char *kindIcon(Task::Kind kind)
{
    switch (kind) {
    case Task::Kind::Build:
        return "applications-engineering-symbolic";
    case Task::Kind::Clone:
        return "edit-copy-symbolic";
    case Task::Kind::Other:
        break;
    }
    return "system-run-symbolic";
}

/* The icon of a task in the popover: its kind's while it runs, then how it ended */
const char *stateIcon(const Task *task)
{
    switch (task->state()) {
    case Task::State::Running:
        break;
    case Task::State::Done:
        return "object-select-symbolic";
    case Task::State::Failed:
        return "dialog-error-symbolic";
    case Task::State::Stopped:
        return "process-stop-symbolic";
    }
    return kindIcon(task->kind());
}

/* "45%"; nothing while it cannot tell */
QString percent(double fraction)
{
    return fraction < 0 ? QString() : QString("%1%").arg(int(fraction * 100));
}

/* What a task shows beside its title: how far, or how it ended */
QString statusOf(const Task *task)
{
    switch (task->state()) {
    case Task::State::Running:
        return percent(task->fraction());
    case Task::State::Done:
        return "Done";
    case Task::State::Failed:
        return "Failed";
    case Task::State::Stopped:
        return "Stopped";
    }
    return {};
}

/* The style class of a colour: the theme's accent, success, error; none for dim */
enum class Tone { Accent, Success, Error, Dim };

const char *toneClass(Tone tone)
{
    switch (tone) {
    case Tone::Accent:
        return "accent";
    case Tone::Success:
        return "success";
    case Tone::Error:
        return "error";
    case Tone::Dim:
        break;
    }
    return "dim-label";
}

void setTone(GtkWidget *widget, Tone tone)
{
    for (const char *each : {"accent", "success", "error", "dim-label"}) {
        gtk_widget_remove_css_class(widget, each);
    }
    gtk_widget_add_css_class(widget, toneClass(tone));
}

/* What the ring of one task, or of several, shows */
struct Ring {
    enum class Inside { Icon, Check, Alert, Count };
    Inside inside = Inside::Icon;
    const char *icon = "system-run-symbolic";
    bool dimIcon = false;
    int count = 0;
    /* how far, -1 turning; or whole, once over */
    double fraction = -1;
    bool whole = false;
    Tone tone = Tone::Accent;
    /* one of several failed */
    bool dot = false;
};

Ring ringOf(const QList<Task *> &tasks)
{
    Ring ring;
    QList<const Task *> running;
    bool failed = false;
    bool done = false;

    for (const Task *task : tasks) {
        if (task->isRunning()) {
            running << task;
        }
        failed = failed || task->state() == Task::State::Failed;
        done = done || task->state() == Task::State::Done;
    }
    if (tasks.size() == 1) {
        const Task *task = tasks.first();
        ring.icon = kindIcon(task->kind());
        switch (task->state()) {
        case Task::State::Running:
            ring.fraction = task->fraction();
            break;
        case Task::State::Done:
            ring.inside = Ring::Inside::Check;
            ring.whole = true;
            ring.tone = Tone::Success;
            break;
        case Task::State::Failed:
            ring.inside = Ring::Inside::Alert;
            ring.whole = true;
            ring.tone = Tone::Error;
            break;
        case Task::State::Stopped:
            ring.dimIcon = true;
            ring.whole = true;
            ring.tone = Tone::Dim;
            break;
        }
        return ring;
    }
    ring.inside = Ring::Inside::Count;
    ring.count = int(tasks.size());
    if (!running.isEmpty()) {
        /* how far those running are, if they all can tell */
        double sum = 0;
        for (const Task *task : std::as_const(running)) {
            if (task->fraction() < 0) {
                sum = -1;
                break;
            }
            sum += task->fraction();
        }
        ring.fraction = sum < 0 ? -1 : sum / double(running.size());
        ring.dot = failed;
        return ring;
    }
    ring.whole = true;
    ring.tone = failed ? Tone::Error : done ? Tone::Success : Tone::Dim;
    return ring;
}

/* The ring: drawn in its tone's colour, the track in the text's, the icon or the count in it */
struct RingWidgets {
    GtkWidget *overlay;
    GtkWidget *area;
    GtkWidget *icon;
    GtkWidget *count;
    GtkWidget *dot;
};

}

static RingWidgets makeRing()
{
    RingWidgets ring;

    ring.overlay = gtk_overlay_new();
    ring.area = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(ring.area), kRingSize);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(ring.area), kRingSize);
    gtk_overlay_set_child(GTK_OVERLAY(ring.overlay), ring.area);
    ring.icon = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(ring.icon), 12);
    gtk_widget_set_halign(ring.icon, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(ring.icon, GTK_ALIGN_CENTER);
    gtk_overlay_add_overlay(GTK_OVERLAY(ring.overlay), ring.icon);
    ring.count = gtk_label_new(nullptr);
    gtk_widget_add_css_class(ring.count, "caption-heading");
    gtk_widget_set_halign(ring.count, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(ring.count, GTK_ALIGN_CENTER);
    gtk_overlay_add_overlay(GTK_OVERLAY(ring.overlay), ring.count);
    /* one of several failed: a dot on the ring, in the error colour */
    ring.dot = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(ring.dot), 8);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(ring.dot), 8);
    gtk_widget_set_halign(ring.dot, GTK_ALIGN_END);
    gtk_widget_set_valign(ring.dot, GTK_ALIGN_START);
    gtk_widget_add_css_class(ring.dot, "error");
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(ring.dot),
        [](GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer) {
            GdkRGBA color;
            gtk_widget_get_color(GTK_WIDGET(area), &color);
            gdk_cairo_set_source_rgba(cr, &color);
            cairo_arc(cr, width / 2.0, height / 2.0, std::min(width, height) / 2.0, 0, 2 * M_PI);
            cairo_fill(cr);
        },
        nullptr, nullptr);
    gtk_overlay_add_overlay(GTK_OVERLAY(ring.overlay), ring.dot);
    gtk_widget_set_valign(ring.overlay, GTK_ALIGN_CENTER);
    return ring;
}

TaskButton::TaskButton(TaskList *tasks, GtkWindow *window)
    : m_tasks(tasks), m_window(window), m_button(gtk_button_new()), m_compact(gtk_button_new()),
      m_popover(gtk_popover_new()), m_list(gtk_list_box_new()),
      m_clear(gtk_button_new_with_mnemonic("C_lear Finished"))
{
    const RingWidgets ring = makeRing();
    const RingWidgets compact = makeRing();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

    /* the sidebar's and the header bar's: kept until this goes */
    g_object_ref_sink(m_button);
    g_object_ref_sink(m_compact);
    m_ring = ring.area;
    m_ringIcon = ring.icon;
    m_ringCount = ring.count;
    m_compactRing = compact.area;
    m_compactIcon = compact.icon;
    m_compactCount = compact.count;
    g_object_set_data(G_OBJECT(m_ring), "dot", ring.dot);
    g_object_set_data(G_OBJECT(m_compactRing), "dot", compact.dot);
    for (GtkWidget *area : {m_ring, m_compactRing}) {
        gtk_drawing_area_set_draw_func(
            GTK_DRAWING_AREA(area),
            [](GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer self) {
                static_cast<const TaskButton *>(self)->draw(GTK_WIDGET(area), cr, width, height);
            },
            this, nullptr);
    }

    /* the bottom of the list: a VM's row, less tall */
    m_title = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(m_title), 0);
    gtk_label_set_ellipsize(GTK_LABEL(m_title), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(m_title, true);
    m_status = gtk_label_new(nullptr);
    gtk_widget_add_css_class(m_status, "numeric");
    gtk_box_append(GTK_BOX(box), ring.overlay);
    gtk_box_append(GTK_BOX(box), m_title);
    gtk_box_append(GTK_BOX(box), m_status);
    gtk_button_set_child(GTK_BUTTON(m_button), box);
    gtk_widget_set_name(m_button, "tasks");
    gtk_widget_add_css_class(m_button, "flat");
    gtk_widget_set_margin_start(m_button, 6);
    gtk_widget_set_margin_end(m_button, 6);
    gtk_widget_set_margin_top(m_button, 4);
    gtk_widget_set_margin_bottom(m_button, 4);
    gtk_button_set_child(GTK_BUTTON(m_compact), compact.overlay);
    gtk_widget_set_name(m_compact, "tasks-compact");
    gtk_widget_add_css_class(m_compact, "flat");

    /* the tasks, each with its progress, a row showing its details */
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(m_list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(m_list, "boxed-list");
    gtk_widget_set_size_request(m_list, 360, -1);
    gtk_widget_set_halign(m_clear, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(content), m_list);
    gtk_box_append(GTK_BOX(content), m_clear);
    gtk_widget_set_margin_start(content, 6);
    gtk_widget_set_margin_end(content, 6);
    gtk_widget_set_margin_top(content, 6);
    gtk_widget_set_margin_bottom(content, 6);
    /* as wide as a column of text: a popover wider than the monitor is taken down as it shows */
    GtkWidget *clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 400);
    adw_clamp_set_child(ADW_CLAMP(clamp), content);
    gtk_popover_set_child(GTK_POPOVER(m_popover), clamp);
    gtk_widget_set_parent(m_popover, m_button);

    on<void()>(m_button, "clicked", [this]() { activate(); });
    on<void()>(m_compact, "clicked", [this]() { activate(); });
    for (GtkWidget *button : {m_button, m_compact}) {
        /* a right click: the popover, with Clear Finished */
        GtkGesture *click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
        on<void(gint, gdouble, gdouble)>(click, "pressed", [this, button](gint, gdouble, gdouble) {
            if (gtk_widget_get_parent(m_popover) != button) {
                gtk_widget_unparent(m_popover);
                gtk_widget_set_parent(m_popover, button);
            }
            showPopover();
        });
        gtk_widget_add_controller(button, GTK_EVENT_CONTROLLER(click));
    }
    on<void(GtkListBoxRow *)>(m_list, "row-activated", [this](GtkListBoxRow *activated) {
        for (auto it = m_rows.cbegin(); it != m_rows.cend(); ++it) {
            if (it.value().row == GTK_WIDGET(activated)) {
                Task *task = const_cast<Task *>(it.key());
                gtk_popover_popdown(GTK_POPOVER(m_popover));
                task->showDetails();
                return;
            }
        }
    });
    on<void()>(m_clear, "clicked", [this]() { m_tasks->clearFinished(); });
    on<void()>(m_popover, "closed", [this]() { m_rows.clear(); });

    QObject::connect(tasks, &TaskList::added, &m_qt, [this]() { refresh(); });
    QObject::connect(tasks, &TaskList::taskChanged, &m_qt, [this](Task *task) {
        refresh();
        refreshRow(task);
    });
    QObject::connect(tasks, &TaskList::taskFinished, &m_qt, [this]() { lingerSuccesses(); });
    QObject::connect(tasks, &TaskList::removed, &m_qt, [this](Task *task) {
        m_lingering.remove(task);
        refresh();
    });
    /* the window in front: the successes start to go */
    on<void(GParamSpec *)>(window, "notify::is-active", [this](GParamSpec *) { lingerSuccesses(); });
    refresh();
}

TaskButton::~TaskButton()
{
    stopCallbacks();
    for (GtkWidget *area : {m_ring, m_compactRing}) {
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), nullptr, nullptr, nullptr);
    }
    if (m_tick) {
        gtk_widget_remove_tick_callback(m_ring, m_tick);
        gtk_widget_remove_tick_callback(m_compactRing, m_compactTick);
    }
    gtk_widget_unparent(m_popover);
    /* out of the list's bottom bar and the header bar, which outlive the window */
    for (GtkWidget *widget : {m_button, m_compact}) {
        if (GtkWidget *bar = gtk_widget_get_ancestor(widget, ADW_TYPE_HEADER_BAR)) {
            adw_header_bar_remove(ADW_HEADER_BAR(bar), widget);
        } else if (GtkWidget *view = gtk_widget_get_ancestor(widget, ADW_TYPE_TOOLBAR_VIEW)) {
            adw_toolbar_view_remove(ADW_TOOLBAR_VIEW(view), widget);
        }
        g_object_unref(widget);
    }
}

void TaskButton::setListShown(bool shown)
{
    m_listShown = shown;
    refresh();
}

QString TaskButton::title() const
{
    const QList<Task *> tasks = m_tasks->tasks();

    if (tasks.size() == 1) {
        return tasks.first()->title();
    }
    return tasks.isEmpty() ? QString() : QString("%1 tasks").arg(tasks.size());
}

QString TaskButton::status() const
{
    const QList<Task *> tasks = m_tasks->tasks();
    int failed = 0;
    int running = 0;
    bool done = false;

    if (tasks.size() == 1) {
        return statusOf(tasks.first());
    }
    for (const Task *task : tasks) {
        failed += task->state() == Task::State::Failed;
        running += task->isRunning();
        done = done || task->state() == Task::State::Done;
    }
    if (failed > 0) {
        return QString("%1 failed").arg(failed);
    }
    if (running > 0) {
        return percent(ringOf(tasks).fraction);
    }
    return tasks.isEmpty() ? QString() : done ? QString("Done") : QString("Stopped");
}

QString TaskButton::describe(const Task *task)
{
    const QString status = statusOf(task);
    QString line = task->title();

    if (!status.isEmpty()) {
        line += " · " + status;
    }
    if (!task->text().isEmpty()) {
        line += " — " + task->text();
    }
    return line;
}

void TaskButton::activate()
{
    const QList<Task *> tasks = m_tasks->tasks();

    if (tasks.size() == 1) {
        tasks.first()->showDetails();
    } else if (tasks.size() > 1) {
        /* over the button it was clicked on */
        GtkWidget *from = m_listShown ? m_button : m_compact;
        if (gtk_widget_get_parent(m_popover) != from) {
            gtk_widget_unparent(m_popover);
            gtk_widget_set_parent(m_popover, from);
        }
        showPopover();
    }
}

void TaskButton::showPopover()
{
    if (m_tasks->tasks().isEmpty()) {
        return;
    }
    fillPopover();
    gtk_popover_popup(GTK_POPOVER(m_popover));
}

bool TaskButton::spins() const
{
    const QList<Task *> tasks = m_tasks->tasks();
    const Ring ring = tasks.isEmpty() ? Ring() : ringOf(tasks);

    return !tasks.isEmpty() && ring.fraction < 0 && !ring.whole;
}

void TaskButton::refresh()
{
    const QList<Task *> tasks = m_tasks->tasks();
    const bool any = !tasks.isEmpty();
    const Ring ring = any ? ringOf(tasks) : Ring();
    QString tip;

    gtk_widget_set_visible(m_button, any);
    /* the bottom bar of the list, there only with tasks */
    if (GtkWidget *view = gtk_widget_get_ancestor(m_button, ADW_TYPE_TOOLBAR_VIEW)) {
        adw_toolbar_view_set_reveal_bottom_bars(ADW_TOOLBAR_VIEW(view), any);
    }
    gtk_widget_set_visible(m_compact, any && !m_listShown);

    for (GtkWidget *area : {m_ring, m_compactRing}) {
        auto *dot = static_cast<GtkWidget *>(g_object_get_data(G_OBJECT(area), "dot"));
        setTone(area, ring.tone);
        gtk_widget_set_visible(dot, ring.dot);
        gtk_widget_queue_draw(area);
    }
    for (GtkWidget *icon : {m_ringIcon, m_compactIcon}) {
        gtk_image_set_from_icon_name(GTK_IMAGE(icon), ring.icon);
        gtk_widget_set_visible(icon, ring.inside == Ring::Inside::Icon);
        gtk_widget_set_opacity(icon, ring.dimIcon ? 0.55 : 1);
    }
    for (GtkWidget *count : {m_ringCount, m_compactCount}) {
        gtk_label_set_text(GTK_LABEL(count), QByteArray::number(ring.count).constData());
        gtk_widget_set_visible(count, ring.inside == Ring::Inside::Count);
    }

    const QString status = this->status();
    gtk_label_set_text(GTK_LABEL(m_title), utf8(title()).constData());
    gtk_label_set_text(GTK_LABEL(m_status), utf8(status).constData());
    {
        int failed = 0, running = 0;
        bool done = false;
        for (const Task *task : tasks) {
            failed += task->state() == Task::State::Failed;
            running += task->isRunning();
            done = done || task->state() == Task::State::Done;
        }
        setTone(m_status, failed > 0                ? Tone::Error
                          : running == 0 && done     ? Tone::Success
                                                     : Tone::Dim);
    }
    if (tasks.size() == 1) {
        const Task *task = tasks.first();
        tip = "<b>" + task->title().toHtmlEscaped() + "</b>";
        if (!status.isEmpty()) {
            tip += " · " + status.toHtmlEscaped();
        }
        if (!task->text().isEmpty()) {
            tip += '\n' + task->text().toHtmlEscaped();
        }
    } else if (any) {
        QStringList lines{"<b>" + title().toHtmlEscaped() + "</b>"};
        for (const Task *task : tasks) {
            lines << describe(task).toHtmlEscaped();
        }
        tip = lines.join('\n');
    }
    for (GtkWidget *button : {m_button, m_compact}) {
        gtk_widget_set_tooltip_markup(button, any ? utf8(tip).constData() : nullptr);
        gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                       utf8(title()).constData(), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
                                       utf8(status).constData(), -1);
    }

    /* turning while those running cannot tell how far they are: each ring at
       its frame clock's pace, while it shows */
    const auto turn = [](GtkWidget *area, GdkFrameClock *, gpointer) -> gboolean {
        gtk_widget_queue_draw(area);
        return G_SOURCE_CONTINUE;
    };
    if (spins() && !m_tick) {
        m_tick = gtk_widget_add_tick_callback(m_ring, turn, nullptr, nullptr);
        m_compactTick = gtk_widget_add_tick_callback(m_compactRing, turn, nullptr, nullptr);
    } else if (!spins() && m_tick) {
        gtk_widget_remove_tick_callback(m_ring, std::exchange(m_tick, 0));
        gtk_widget_remove_tick_callback(m_compactRing, std::exchange(m_compactTick, 0));
    }

    if (gtk_widget_get_visible(m_popover)) {
        if (!any) {
            gtk_popover_popdown(GTK_POPOVER(m_popover));
        } else if (tasks.size() != m_rows.size()) {
            fillPopover();
        }
    }
}

void TaskButton::fillPopover()
{
    bool finished = false;

    while (GtkWidget *row = gtk_widget_get_first_child(m_list)) {
        gtk_list_box_remove(GTK_LIST_BOX(m_list), row);
    }
    m_rows.clear();
    for (Task *task : m_tasks->tasks()) {
        Row row;
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

        row.row = gtk_list_box_row_new();
        row.icon = gtk_image_new();
        row.title = gtk_label_new(nullptr);
        row.text = gtk_label_new(nullptr);
        row.status = gtk_label_new(nullptr);
        row.progress = gtk_progress_bar_new();
        gtk_label_set_xalign(GTK_LABEL(row.title), 0);
        gtk_label_set_ellipsize(GTK_LABEL(row.title), PANGO_ELLIPSIZE_END);
        gtk_label_set_xalign(GTK_LABEL(row.text), 0);
        gtk_label_set_wrap(GTK_LABEL(row.text), true);
        gtk_label_set_wrap_mode(GTK_LABEL(row.text), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_lines(GTK_LABEL(row.text), 3);
        gtk_label_set_ellipsize(GTK_LABEL(row.text), PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class(row.text, "dim-label");
        gtk_widget_add_css_class(row.text, "caption");
        gtk_widget_add_css_class(row.status, "numeric");
        gtk_widget_set_hexpand(texts, true);
        gtk_widget_set_valign(row.status, GTK_ALIGN_START);
        gtk_progress_bar_set_pulse_step(GTK_PROGRESS_BAR(row.progress), 0.1);
        gtk_box_append(GTK_BOX(texts), row.title);
        gtk_box_append(GTK_BOX(texts), row.text);
        gtk_box_append(GTK_BOX(line), row.icon);
        gtk_box_append(GTK_BOX(line), texts);
        gtk_box_append(GTK_BOX(line), row.status);
        gtk_box_append(GTK_BOX(box), line);
        gtk_box_append(GTK_BOX(box), row.progress);
        gtk_widget_set_margin_start(box, 12);
        gtk_widget_set_margin_end(box, 12);
        gtk_widget_set_margin_top(box, 8);
        gtk_widget_set_margin_bottom(box, 8);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row.row), box);
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row.row), true);
        gtk_list_box_append(GTK_LIST_BOX(m_list), row.row);
        m_rows.insert(task, row);
        refreshRow(task);
        finished = finished || !task->isRunning();
    }
    gtk_widget_set_visible(m_clear, finished);
}

void TaskButton::refreshRow(Task *task)
{
    const auto it = m_rows.constFind(task);

    if (it == m_rows.cend()) {
        return;
    }
    const Row &row = it.value();
    const bool running = task->isRunning();

    gtk_image_set_from_icon_name(GTK_IMAGE(row.icon), stateIcon(task));
    setTone(row.icon, task->state() == Task::State::Done     ? Tone::Success
                      : task->state() == Task::State::Failed ? Tone::Error
                      : running                             ? Tone::Accent
                                                            : Tone::Dim);
    gtk_label_set_text(GTK_LABEL(row.title), utf8(task->title()).constData());
    gtk_label_set_text(GTK_LABEL(row.text), utf8(task->text()).constData());
    gtk_widget_set_visible(row.text, !task->text().isEmpty());
    gtk_label_set_text(GTK_LABEL(row.status), utf8(statusOf(task)).constData());
    setTone(row.status, task->state() == Task::State::Done     ? Tone::Success
                        : task->state() == Task::State::Failed ? Tone::Error
                                                               : Tone::Dim);
    gtk_widget_set_visible(row.progress, running);
    if (running) {
        if (task->fraction() < 0) {
            gtk_progress_bar_pulse(GTK_PROGRESS_BAR(row.progress));
        } else {
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(row.progress), task->fraction());
        }
    }
    gtk_widget_set_tooltip_text(row.row, utf8(describe(task)).constData());
    bool finished = false;
    for (const Task *each : m_tasks->tasks()) {
        finished = finished || !each->isRunning();
    }
    gtk_widget_set_visible(m_clear, finished);
}

void TaskButton::lingerSuccesses()
{
    const bool front = m_window && gtk_window_is_active(m_window);

    for (Task *task : m_tasks->tasks()) {
        if (task->state() != Task::State::Done && task->state() != Task::State::Stopped) {
            continue;
        }
        QTimer *timer = m_lingering.value(task);
        if (!timer) {
            /* the task's: gone with it */
            timer = new QTimer(task);
            timer->setSingleShot(true);
            timer->setInterval(m_linger);
            QObject::connect(timer, &QTimer::timeout, task, &Task::dismiss);
            m_lingering.insert(task, timer);
        }
        /* at a stretch: away from the front, counted again from the start */
        if (front && !timer->isActive()) {
            timer->start();
        } else if (!front) {
            timer->stop();
        }
    }
}

void TaskButton::draw(GtkWidget *area, cairo_t *cr, int width, int height) const
{
    const QList<Task *> tasks = m_tasks->tasks();

    if (tasks.isEmpty()) {
        return;
    }
    const Ring ring = ringOf(tasks);
    const double size = std::min(width, height);
    const double stroke = std::max(2.0, size / 12.0);
    const double r = (size - stroke) / 2;
    const double cx = width / 2.0, cy = height / 2.0;
    GdkRGBA color, text;

    gtk_widget_get_color(area, &color);
    gtk_widget_get_color(gtk_widget_get_parent(area), &text);
    cairo_set_line_width(cr, stroke);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    if (ring.whole) {
        gdk_cairo_set_source_rgba(cr, &color);
        cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
        cairo_stroke(cr);
    } else {
        GdkRGBA track = text;
        track.alpha *= 0.18f;
        gdk_cairo_set_source_rgba(cr, &track);
        cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
        cairo_stroke(cr);
        gdk_cairo_set_source_rgba(cr, &color);
        /* from the top, clockwise */
        const double top = -M_PI / 2;
        if (ring.fraction < 0) {
            const double turned = double(g_get_monotonic_time() / 1000 % kTurnMs) / kTurnMs * 2 * M_PI;
            cairo_arc(cr, cx, cy, r, top + turned, top + turned + kTurningArc * M_PI / 180);
            cairo_stroke(cr);
        } else if (ring.fraction > 0) {
            cairo_arc(cr, cx, cy, r, top, top + ring.fraction * 2 * M_PI);
            cairo_stroke(cr);
        }
    }
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_width(cr, stroke * 1.2);
    gdk_cairo_set_source_rgba(cr, &color);
    switch (ring.inside) {
    case Ring::Inside::Check:
        cairo_move_to(cr, cx - 0.42 * r, cy + 0.02 * r);
        cairo_line_to(cr, cx - 0.12 * r, cy + 0.32 * r);
        cairo_line_to(cr, cx + 0.44 * r, cy - 0.28 * r);
        cairo_stroke(cr);
        break;
    case Ring::Inside::Alert:
        cairo_move_to(cr, cx, cy - 0.42 * r);
        cairo_line_to(cr, cx, cy + 0.1 * r);
        cairo_stroke(cr);
        cairo_arc(cr, cx, cy + 0.4 * r, stroke * 0.7, 0, 2 * M_PI);
        cairo_fill(cr);
        break;
    case Ring::Inside::Icon:
    case Ring::Inside::Count:
        /* the overlays' */
        break;
    }
}
