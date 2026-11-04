// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtk/vmconsole.h"

#include <QTimer>

#include <utility>

#include "core/nesteddesktop.h"
#include "core/vmconfig.h"
#include "core/vmhardware.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "gtkview/vmdisplay.h"

/* QEMU opens the display's socket with the others, before QMP answers:
   a few tries cover a slow start */
static constexpr int kAttachTries = 20;
static constexpr int kAttachRetryMs = 250;

/* QEMU drops input while the VM is paused, but for a guest that suspended
   itself to RAM, which a key wakes */
static bool runnerTakesInput(const VmRunner *runner)
{
    return runner->state() == VmRunner::State::Running || runner->isSuspended();
}

static NestedDisplay::Factory &nestedFactory()
{
    static NestedDisplay::Factory factory;
    return factory;
}

VmConsole::VmConsole(Vm *vm)
    : m_vm(vm), m_views(new ConsoleViews(&m_qt)), m_stack(gtk_stack_new()),
      m_screenBox(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0)),
      m_desktopBox(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0)), m_retryTimer(new QTimer(&m_qt)),
      m_statsTimer(new QTimer(&m_qt))
{
    /* its widgets live as long as the console, in the page or out of it */
    g_object_ref_sink(m_stack);
    gtk_widget_set_hexpand(m_stack, true);
    gtk_widget_set_vexpand(m_stack, true);
    /* nothing over the screen, which would resize the guest; no animation
       between the views, which would show both a moment */
    gtk_stack_set_transition_type(GTK_STACK(m_stack), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_stack_add_named(GTK_STACK(m_stack), m_screenBox, "screen");
    gtk_stack_add_named(GTK_STACK(m_stack), m_desktopBox, "desktop");
    QObject::connect(m_views, &ConsoleViews::viewChanged, &m_qt,
                     [this](ConsoleViews::View) { applyView(); });

    m_retryTimer->setSingleShot(true);
    m_retryTimer->setInterval(kAttachRetryMs);
    QObject::connect(m_retryTimer, &QTimer::timeout, &m_qt, [this]() { attach(); });
    m_statsTimer->setInterval(1000);
    QObject::connect(m_statsTimer, &QTimer::timeout, &m_qt, [this]() {
        if (m_display) {
            m_stats = m_display->takeStats();
        }
    });

    if (vm) {
        VmRunner *runner = vm->runner();
        QObject::connect(runner, &VmRunner::stateChanged, &m_qt, [this]() { update(); });
        QObject::connect(runner, &VmRunner::suspendedChanged, &m_qt, [this]() { update(); });
        QObject::connect(runner, &VmRunner::waitsForDisplayChanged, &m_qt, [this]() { update(); });
        /* a reboot: the boot again, and the nested desktop served anew */
        QObject::connect(runner, &VmRunner::guestReset, &m_qt, [this]() {
            if (m_nested) {
                renewNested();
            }
        });
    }
    update();
}

VmConsole::~VmConsole()
{
    QObject::disconnect(m_views, nullptr, &m_qt, nullptr);
    changed = nullptr;
    screenReady = nullptr;
    /* before the widgets: the render thread draws into the window, and
       the guest's window hangs from it */
    detach();
    if (NestedDisplay *nested = std::exchange(m_nested, nullptr)) {
        gtk_box_remove(GTK_BOX(m_desktopBox), nested->widget());
        delete nested;
    }
    g_object_unref(m_stack);
}

Vm *VmConsole::vm() const
{
    return m_vm;
}

VmConsole::Page VmConsole::page() const
{
    return m_page;
}

QString VmConsole::messageTitle() const
{
    return m_messageTitle;
}

QString VmConsole::messageText() const
{
    return m_messageText;
}

bool VmConsole::isWaiting() const
{
    return m_waiting;
}

bool VmConsole::canRetry() const
{
    return m_retry;
}

void VmConsole::retry()
{
    m_attempts = 0;
    attach();
}

GtkWidget *VmConsole::widget() const
{
    return m_stack;
}

VmDisplay *VmConsole::display() const
{
    return m_display;
}

bool VmConsole::hasScreen() const
{
    return desktopShown() || m_display;
}

bool VmConsole::isFullScreen() const
{
    return desktopShown() ? m_nested->isFullScreen() : m_display && m_display->isFullScreen();
}

void VmConsole::setFullScreen(bool on)
{
    if (desktopShown()) {
        m_nested->setFullScreen(on);
    } else if (m_display) {
        m_display->setFullScreen(on);
    }
}

void VmConsole::focusScreen()
{
    if (desktopShown()) {
        m_nested->focus();
    } else if (m_display) {
        m_display->focus();
    }
}

bool VmConsole::hasKeyboard() const
{
    return desktopShown() ? m_nested->hasKeyboard() : m_display && m_display->hasKeyboard();
}

bool VmConsole::takesInput() const
{
    if (desktopShown()) {
        return m_vm && runnerTakesInput(m_vm->runner());
    }
    return m_display && m_display->inputEnabled();
}

bool VmConsole::grabbed() const
{
    return !desktopShown() && m_display && m_display->grabbed();
}

QString VmConsole::grabState() const
{
    return m_display ? m_display->grabState() : QString();
}

void VmConsole::setGrab(bool on)
{
    if (m_display) {
        m_display->setGrab(on);
    }
}

void VmConsole::setCovered(bool on)
{
    m_covered = on;
    if (m_display) {
        m_display->setCovered(on);
    }
    if (m_nested) {
        m_nested->setCovered(on);
    }
}

void VmConsole::leaveScreen()
{
    if (m_display) {
        m_display->setFullScreen(false);
        m_display->setGrab(false);
    }
    if (m_nested) {
        m_nested->setFullScreen(false);
    }
}

const Stats::Summary &VmConsole::stats() const
{
    return m_stats;
}

bool VmConsole::hasViews() const
{
    return m_nested;
}

ConsoleViews::View VmConsole::currentView() const
{
    return m_views->view();
}

void VmConsole::showView(ConsoleViews::View view)
{
    if (m_nested) {
        m_views->choose(view);
    }
}

bool VmConsole::desktopShown() const
{
    return m_nested && m_views->view() == ConsoleViews::View::Desktop;
}

NestedDisplay *VmConsole::nestedView() const
{
    return m_nested;
}

void VmConsole::setNestedFactory(const NestedDisplay::Factory &factory)
{
    nestedFactory() = factory;
}

bool VmConsole::hasNestedSession() const
{
    return m_nested && (m_nested->connections() > 0 || m_nested->isMapped());
}

bool VmConsole::serveNested(const QString &socket, QString *error)
{
    if (m_nested && m_nested->socket() == socket) {
        return true;
    }
    dropNested();
    const NestedDisplay::Factory &factory = nestedFactory();
    NestedDisplay *nested = factory ? factory(socket, error) : NestedDisplay::create(socket, error);
    if (!nested) {
        return false;
    }
    m_nested = nested;
    gtk_box_append(GTK_BOX(m_desktopBox), nested->widget());
    nested->setCovered(m_covered);
    /* the guest's window came or went: Desktop, or Screen with its login screen */
    nested->mappedChanged.connect([this](bool mapped) {
        m_views->setDesktop(mapped);
        notifyChanged();
    });
    nested->connectionsChanged.connect([this](int) { notifyChanged(); });
    nested->keyboardChanged.connect([this]() { notifyChanged(); });
    nested->fullScreenChanged.connect([this](bool) { notifyChanged(); });
    /* the guest's Ctrl+Alt+F, or vitrine's while it has no window to ask */
    nested->fullScreenRequested.connect([this](bool on) {
        if (desktopShown()) {
            m_nested->setFullScreen(on);
        }
    });
    nested->fullScreenToggled.connect([this]() {
        if (desktopShown()) {
            m_nested->setFullScreen(!m_nested->isFullScreen());
        }
    });
    m_views->powerOn();
    applyView();
    return true;
}

/*
 * The guest restarted, its sessions with it: a new view serves the nested
 * desktop's socket, and their connections go with the old one.  QEMU keeps
 * one open across a reset it makes itself (the host's Reset): its window
 * would still count as shown - the library keeps one such state for all its
 * connections - so the next session's window would change nothing, and the
 * console would never show it.
 */
void VmConsole::renewNested()
{
    const QString socket = m_nested->socket();
    QString error;

    dropNested();
    if (!serveNested(socket, &error)) {
        qWarning("vitrine: %s", qPrintable(error));
    }
}

void VmConsole::dropNested()
{
    if (!m_nested) {
        return;
    }
    /* nestedView() is null while it goes */
    NestedDisplay *nested = std::exchange(m_nested, nullptr);
    gtk_box_remove(GTK_BOX(m_desktopBox), nested->widget());
    delete nested;
    m_views->powerOn();
    applyView();
}

/*
 * The view to show: the D-Bus display's page, the VM's screen, or the
 * guest's desktop.  The keys go to the one shown, and full screen, if on,
 * follows.
 */
void VmConsole::applyView()
{
    const ConsoleViews::View view = m_views->view();

    if (!m_nested) {
        gtk_stack_set_visible_child(GTK_STACK(m_stack), m_screenBox);
        notifyChanged();
        return;
    }
    const bool full = m_nested->isFullScreen() || (m_display && m_display->isFullScreen());
    if (view == ConsoleViews::View::Desktop) {
        if (m_display) {
            m_display->setFullScreen(false);
        }
        gtk_stack_set_visible_child(GTK_STACK(m_stack), m_desktopBox);
        m_nested->setInputEnabled(m_vm && runnerTakesInput(m_vm->runner()));
        m_nested->setFullScreen(full);
    } else {
        m_nested->setInputEnabled(false);
        m_nested->setFullScreen(false);
        gtk_stack_set_visible_child(GTK_STACK(m_stack), m_screenBox);
        if (m_display && full) {
            m_display->setFullScreen(true);
        }
    }
    notifyChanged();
    if (takesInput()) {
        notifyScreenReady();
    }
}

/* The page for the state of the VM */
void VmConsole::update()
{
    const VmRunner *runner = m_vm ? m_vm->runner() : nullptr;
    const VmRunner::State state = runner ? runner->state() : VmRunner::State::Stopped;

    switch (state) {
    case VmRunner::State::Stopped:
    case VmRunner::State::Stopping:
        /* the guest is gone, or going: its screen with it, and its desktop */
        detach();
        dropNested();
        setPage(Page::Home);
        return;
    case VmRunner::State::Starting:
        /*
         * Paused for its screen (VmRunner::waitsForDisplay): the view
         * attaches and tells QEMU the screen's size and refresh rate before
         * the guest runs; a view up stays up until the guest runs
         */
        if (!runner->waitsForDisplay() && !m_display) {
            detach();
            showMessage("Starting…", {}, true);
            return;
        }
        break;
    case VmRunner::State::Running:
    case VmRunner::State::Paused:
        /* paused, the screen keeps its last frame: the list and the page tell */
        break;
    }

    /*
     * The nested desktop of a QEMU found running: served again, for its
     * next session, as the last one ended with the vitrine that served it.
     * The guest may be anywhere, from its firmware to its login screen: the
     * view follows what its window does.
     */
    if (const QString socket = runner->nestedSocket(); !socket.isEmpty() && !m_nested) {
        QString error;
        if (!serveNested(socket, &error)) {
            qWarning("vitrine: %s", qPrintable(error));
        }
    }
    if (m_nested) {
        m_nested->setInputEnabled(desktopShown() && runnerTakesInput(runner));
    }
    if (!runner->displaySocket().isEmpty()) {
        if (m_display) {
            const bool took = m_display->inputEnabled();
            m_display->setInputEnabled(runnerTakesInput(runner));
            setPage(Page::Screen);
            if (!took && m_display->inputEnabled()) {
                notifyScreenReady();
            }
        } else if (!m_retryTimer->isActive()) {
            m_attempts = 0;
            attach();
        }
        return;
    }
    detach();
    if (VmConfig::screen(runner->runArgs()) == VmConfig::Screen::OwnWindow) {
        setPage(Page::OwnWindow);
    } else {
        showMessage("No Screen", "This VM runs without a display (-display none, -nographic or "
                                 "VNC, for example).");
    }
}

/* The view on the VM's display, again until it answers */
void VmConsole::attach()
{
    const QString socket = m_vm ? m_vm->runner()->displaySocket() : QString();
    QString error;

    m_retryTimer->stop();
    if (m_display || socket.isEmpty()) {
        return;
    }
    /*
     * A new view each time: a failed attach leaves the view half set up.
     * In the window first: a guest waiting for its screen gets the size of
     * the window around it.
     */
    auto *display = new VmDisplay;
    gtk_box_append(GTK_BOX(m_screenBox), GTK_WIDGET(display->widget()->gobj()));
    /* the nested desktop's: its card's console (two cards: the virtio card's) */
    if (!display->attach(socket, &error, m_vm->runner()->waitsForDisplay(),
                         m_nested ? NestedDesktop::screenConsole(m_vm->runner()->runArgs()) : -1)) {
        gtk_box_remove(GTK_BOX(m_screenBox), GTK_WIDGET(display->widget()->gobj()));
        delete display;
        if (++m_attempts < kAttachTries) {
            showMessage("Connecting to the Screen…", {}, true);
            m_retryTimer->start();
        } else {
            showMessage("Cannot Show the Screen", error, false, true);
            /* no screen to wait for */
            if (m_vm) {
                m_vm->runner()->displayReady();
            }
        }
        return;
    }
    m_display = display;
    m_attempts = 0;
    m_stats = {};
    display->setInputEnabled(runnerTakesInput(m_vm->runner()));
    display->setCovered(m_covered);
    display->grabChanged.connect([this]() { notifyChanged(); });
    display->fullScreenChanged.connect([this](bool) { notifyChanged(); });
    /* a guest paused for its screen runs once QEMU has the screen's size and refresh rate */
    display->screenInfoApplied.connect([this]() {
        if (m_vm) {
            m_vm->runner()->displayReady();
        }
    });
    m_statsTimer->start();
    setPage(Page::Screen);
    notifyChanged();
    if (m_display && m_display->inputEnabled()) {
        notifyScreenReady();
    }
}

void VmConsole::detach()
{
    m_retryTimer->stop();
    m_attempts = 0;
    if (!m_display) {
        return;
    }
    m_statsTimer->stop();
    m_stats = {};
    /* display() is null while it goes: what it signals on its way out must
       not lead back to it */
    VmDisplay *display = std::exchange(m_display, nullptr);
    gtk_box_remove(GTK_BOX(m_screenBox), GTK_WIDGET(display->widget()->gobj()));
    delete display;
    notifyChanged();
}

void VmConsole::showMessage(const QString &title, const QString &text, bool waiting, bool retry)
{
    const bool same = m_page == Page::Message && title == m_messageTitle &&
                      text == m_messageText && waiting == m_waiting && retry == m_retry;

    m_messageTitle = title;
    m_messageText = text;
    m_waiting = waiting;
    m_retry = retry;
    if (m_page != Page::Message) {
        setPage(Page::Message);
    } else if (!same) {
        notifyChanged();
    }
}

void VmConsole::setPage(Page page)
{
    if (page != Page::Message) {
        m_messageTitle.clear();
        m_messageText.clear();
        m_waiting = false;
        m_retry = false;
    }
    if (page != m_page) {
        m_page = page;
        notifyChanged();
    }
}

void VmConsole::notifyChanged()
{
    if (changed) {
        changed();
    }
}

void VmConsole::notifyScreenReady()
{
    if (screenReady) {
        screenReady();
    }
}
