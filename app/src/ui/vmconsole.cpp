// SPDX-License-Identifier: GPL-2.0-or-later
#include "vmconsole.h"

#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <utility>

#include "core/guesttools.h"
#include "core/vmconfig.h"
#include "core/vmhardware.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "nested/nestedview.h"
#include "ui/banner.h"
#include "ui/guesttoolsdialog.h"
#include "ui/icons.h"
#include "ui/vmdetails.h"
#include "vmview/vmview.h"

/* QEMU opens the display's socket with the others, before QMP answers:
   a few tries cover a slow start */
static constexpr int kAttachTries = 20;
static constexpr int kAttachRetryMs = 250;

static QString memoryText(qint64 mib)
{
    return mib % 1024 == 0 ? VmConsole::tr("%1 GiB").arg(mib / 1024)
                           : VmConsole::tr("%1 MiB").arg(mib);
}

/* QEMU drops input while the VM is paused, but for a guest that suspended
   itself to RAM, which a key wakes */
static bool runnerTakesInput(const VmRunner *runner)
{
    return runner->state() == VmRunner::State::Running || runner->isSuspended();
}

/* Bigger and bold, for titles */
static QFont scaled(QFont font, qreal factor)
{
    font.setBold(true);
    font.setPointSizeF(font.pointSizeF() * factor);
    return font;
}

VmConsole::VmConsole(Vm *vm, QWidget *parent)
    : QWidget(parent), m_vm(vm), m_pages(new QStackedWidget), m_name(new QLabel),
      m_state(new QLabel), m_error(new Banner(Banner::Warning)), m_start(new QPushButton),
      m_summary(new QLabel), m_messageTitle(new QLabel), m_messageText(new QLabel),
      m_messageButton(new QPushButton), m_screen(new QWidget),
      m_viewPages(new QStackedWidget), m_dbusPage(new QWidget),
      m_screenLayout(new QVBoxLayout(m_dbusPage)), m_desktopPage(new QWidget),
      m_desktopLayout(new QVBoxLayout(m_desktopPage)), m_ownWindowText(new QLabel),
      m_views(new ConsoleViews(this)), m_retry(new QTimer(this)), m_statsTimer(new QTimer(this))
{
    auto *layout = new QVBoxLayout(this);

    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_pages);

    /* Home: what the VM is, and Start */
    {
        auto *home = new QWidget;
        auto *column = new QVBoxLayout(home);
        auto *header = new QHBoxLayout;
        auto *titles = new QVBoxLayout;
        const int large = style()->pixelMetric(QStyle::PM_LargeIconSize, nullptr, this);
        /* at the head of the page: twice a large icon */
        auto *icon = new IconLabel(Icons::themed({"computer"}, QStyle::SP_ComputerIcon), 2 * large);
        auto *settings = new QLabel(QString("<a href=\"settings\">%1</a>").arg(tr("Settings")));

        m_name->setFont(scaled(m_name->font(), 1.6));
        m_name->setTextInteractionFlags(Qt::TextSelectableByMouse);
        titles->setSpacing(0);
        titles->addStretch();
        titles->addWidget(m_name);
        titles->addWidget(m_state);
        titles->addStretch();
        header->addWidget(icon);
        header->addLayout(titles, 1);

        m_error->button()->setText(tr("Show &Log"));
        m_error->button()->show();
        m_error->hide();
        connect(m_error->button(), &QPushButton::clicked, this, &VmConsole::showLogRequested);

        m_start->setObjectName("consoleStart");
        m_start->setText(tr("Start"));
        m_start->setIcon(Icons::themed({"media-playback-start"}, QStyle::SP_MediaPlay));
        m_start->setIconSize(QSize(large, large));
        m_start->setFont(scaled(m_start->font(), 1.3));
        m_start->setMinimumHeight(56);
        m_start->setMinimumWidth(200);
        connect(m_start, &QPushButton::clicked, this, &VmConsole::startRequested);

        m_summary->setWordWrap(true);
        m_summary->setTextFormat(Qt::PlainText);
        settings->setTextFormat(Qt::RichText);
        connect(settings, &QLabel::linkActivated, this, &VmConsole::settingsRequested);

        column->addStretch(2);
        column->addLayout(header);
        column->addWidget(m_error);
        auto *tools = new GuestToolsBanner;
        tools->setVm(vm);
        column->addWidget(tools);
        column->addSpacing(16);
        column->addWidget(m_start, 0, Qt::AlignLeft);
        column->addSpacing(16);
        column->addWidget(m_summary);
        column->addWidget(settings);
        column->addStretch(3);

        /* a column in the middle, as wide as it reads well */
        auto *page = new QWidget;
        auto *row = new QHBoxLayout(page);
        home->setMaximumWidth(560);
        row->addStretch();
        row->addWidget(home, 4);
        row->addStretch();
        m_pages->addWidget(page);
    }

    /* Message: starting, connecting, cannot connect, no screen */
    {
        auto *page = new QWidget;
        auto *column = new QVBoxLayout(page);

        m_messageTitle->setFont(scaled(m_messageTitle->font(), 1.3));
        m_messageTitle->setAlignment(Qt::AlignCenter);
        m_messageText->setAlignment(Qt::AlignCenter);
        m_messageText->setWordWrap(true);
        m_messageText->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_messageButton->setText(tr("&Try Again"));
        connect(m_messageButton, &QPushButton::clicked, this, [this]() {
            m_attempts = 0;
            attach();
        });
        column->addStretch();
        column->addWidget(m_messageTitle);
        column->addWidget(m_messageText);
        column->addWidget(m_messageButton, 0, Qt::AlignCenter);
        column->addStretch();
        m_pages->addWidget(page);
    }

    /* Screen: the view's widget, alone; with the nested desktop, the view chosen (the
       Console tab's menu, the View menu, Ctrl+Alt+1/2/3) */
    {
        auto *column = new QVBoxLayout(m_screen);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(0);
        m_screenLayout->setContentsMargins(0, 0, 0, 0);
        m_desktopLayout->setContentsMargins(0, 0, 0, 0);
        m_viewPages->addWidget(m_dbusPage);
        m_viewPages->addWidget(m_desktopPage);
        column->addWidget(m_viewPages, 1);
        m_pages->addWidget(m_screen);
    }
    connect(m_views, &ConsoleViews::viewChanged, this, &VmConsole::applyView);

    /* OwnWindow */
    {
        auto *page = new QWidget;
        auto *column = new QVBoxLayout(page);
        auto *title = new QLabel(tr("Shown in its own window"));
        auto *show = new QPushButton(Icons::themed({"window", "window-restore", "view-restore"},
                                                   QStyle::SP_TitleBarNormalButton),
                                     tr("Show &Window"));

        title->setFont(scaled(title->font(), 1.3));
        title->setAlignment(Qt::AlignCenter);
        m_ownWindowText->setAlignment(Qt::AlignCenter);
        m_ownWindowText->setWordWrap(true);
        m_ownWindowText->setText(tr("QEMU shows this VM in a window of its own, which stays "
                                    "open without vitrine."));
        connect(show, &QPushButton::clicked, this, &VmConsole::showWindowRequested);
        column->addStretch();
        column->addWidget(title);
        column->addWidget(m_ownWindowText);
        column->addWidget(show, 0, Qt::AlignCenter);
        column->addStretch();
        m_pages->addWidget(page);
    }

    m_retry->setSingleShot(true);
    m_retry->setInterval(kAttachRetryMs);
    connect(m_retry, &QTimer::timeout, this, &VmConsole::attach);
    /* the view's numbers pile up until taken */
    m_statsTimer->setInterval(1000);
    connect(m_statsTimer, &QTimer::timeout, this, [this]() {
        if (m_view) {
            m_stats = m_view->takeStats();
            emit statsChanged();
        }
    });

    if (vm) {
        connect(vm, &Vm::changed, this, &VmConsole::updateHome);
        connect(vm->runner(), &VmRunner::stateChanged, this, &VmConsole::update);
        connect(vm->runner(), &VmRunner::suspendedChanged, this, &VmConsole::update);
        connect(vm->runner(), &VmRunner::waitsForDisplayChanged, this, &VmConsole::update);
        /* a reboot: the boot again, and the nested desktop served anew */
        connect(vm->runner(), &VmRunner::guestReset, this, [this]() {
            if (m_nested) {
                renewNested();
            }
        });
        /*
         * What the guest shows, as the guest tools' agent says.  Not an
         * agent answering (qemu-ga's guest-get-osinfo, the agent's hello):
         * both start early in the boot, before the tools' install at boot
         * and long before the login screen.
         */
        connect(GuestToolsMonitor::of(vm), &GuestToolsMonitor::changed, this,
                &VmConsole::followGuest);
    }
    update();
}

VmConsole::~VmConsole()
{
    /* before the widgets, their windows among them */
    delete std::exchange(m_view, nullptr);
    delete std::exchange(m_nested, nullptr);
}

Vm *VmConsole::vm() const
{
    return m_vm;
}

VmConsole::Page VmConsole::page() const
{
    return Page(m_pages->currentIndex());
}

VmView *VmConsole::view() const
{
    return m_view;
}

bool VmConsole::isFullScreen() const
{
    return desktopShown() ? m_nested->isFullScreen() : m_view && m_view->isFullScreen();
}

void VmConsole::setFullScreen(bool on)
{
    if (desktopShown()) {
        m_nested->setFullScreen(on);
    } else if (m_view) {
        m_view->setFullScreen(on);
    }
}

void VmConsole::focusScreen()
{
    if (desktopShown()) {
        m_nested->focus();
    } else if (m_view) {
        m_view->focus();
    }
}

bool VmConsole::hasKeyboard() const
{
    return desktopShown() ? m_nested->hasKeyboard() : m_view && m_view->hasKeyboard();
}

bool VmConsole::takesInput() const
{
    if (desktopShown()) {
        return m_vm && runnerTakesInput(m_vm->runner());
    }
    return m_view && m_view->inputEnabled();
}

QWidget *VmConsole::screenWidget() const
{
    return desktopShown() ? m_nested->widget() : m_view ? m_view->widget() : nullptr;
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

DesktopView *VmConsole::nestedView() const
{
    return m_nested;
}

static DesktopView::Factory &desktopViewFactory()
{
    static DesktopView::Factory factory;
    return factory;
}

void VmConsole::setDesktopViewFactory(const DesktopView::Factory &factory)
{
    desktopViewFactory() = factory;
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
    const DesktopView::Factory &factory = desktopViewFactory();
    DesktopView *nested = factory ? factory(socket, error, this)
                                  : NestedView::create(socket, error, this);
    if (!nested) {
        return false;
    }
    m_nested = nested;
    m_desktopLayout->addWidget(nested->widget());
    /* the guest's window came or went: Desktop, or Screen with its login screen */
    connect(nested, &DesktopView::mappedChanged, this, [this](bool mapped) {
        m_views->setDesktop(mapped);
        emit changed();
    });
    connect(nested, &DesktopView::connectionsChanged, this, &VmConsole::changed);
    connect(nested, &DesktopView::keyboardChanged, this, &VmConsole::changed);
    connect(nested, &DesktopView::fullScreenChanged, this, &VmConsole::changed);
    /* the guest's Ctrl+Alt+F, or vitrine's while it has no window to ask */
    connect(nested, &DesktopView::fullScreenRequested, this, [this](bool on) {
        if (desktopShown()) {
            m_nested->setFullScreen(on);
        }
    });
    connect(nested, &DesktopView::fullScreenToggled, this, [this]() {
        if (desktopShown()) {
            m_nested->setFullScreen(!m_nested->isFullScreen());
        }
    });
    m_views->powerOn();
    applyView();
    /* a QEMU found running: what its guest shows, if its agent said already */
    followGuest();
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

void VmConsole::followGuest()
{
    if (m_nested && m_vm) {
        const GuestToolsMonitor *tools = GuestToolsMonitor::of(m_vm);
        m_views->setGuest(tools->hasAgent(), tools->screenShown());
    }
}

void VmConsole::dropNested()
{
    if (!m_nested) {
        return;
    }
    /* nestedView() is null while it goes */
    delete std::exchange(m_nested, nullptr);
    m_views->powerOn();
    applyView();
}

/*
 * The view to show: the D-Bus display's page on QEMU's console of the view
 * (attached again on the other console when it changes), or the guest's
 * desktop.  The keys go to the one shown, and full screen, if on, follows.
 */
void VmConsole::applyView()
{
    const ConsoleViews::View view = m_views->view();

    if (!m_nested) {
        m_viewPages->setCurrentWidget(m_dbusPage);
        return;
    }
    const bool full = m_nested->isFullScreen() || (m_view && m_view->isFullScreen());
    if (view == ConsoleViews::View::Desktop) {
        if (m_view) {
            m_view->setFullScreen(false);
        }
        m_viewPages->setCurrentWidget(m_desktopPage);
        m_nested->setInputEnabled(m_vm && runnerTakesInput(m_vm->runner()));
        m_nested->setFullScreen(full);
    } else {
        m_nested->setInputEnabled(false);
        m_nested->setFullScreen(false);
        m_viewPages->setCurrentWidget(m_dbusPage);
        /* the D-Bus display shows one console: the other one, attached again */
        if (m_view && m_view->console() != ConsoleViews::qemuConsole(view)) {
            detach();
            attach();
        }
        if (m_view && full) {
            m_view->setFullScreen(true);
        }
    }
    emit changed();
    if (takesInput()) {
        emit screenReady();
    }
}



void VmConsole::setError(const QString &error)
{
    m_error->setText(tr("The last run ended with an error: %1")
                         .arg(error.section('\n', 0, 0).toHtmlEscaped()));
    m_error->setVisible(!error.isEmpty());
}

const Stats::Summary &VmConsole::stats() const
{
    return m_stats;
}

/* The page for the state of the VM */
void VmConsole::update()
{
    const VmRunner *runner = m_vm ? m_vm->runner() : nullptr;
    const VmRunner::State state = runner ? runner->state() : VmRunner::State::Stopped;

    updateHome();
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
        if (!runner->waitsForDisplay() && !m_view) {
            detach();
            showMessage(tr("Starting…"), {});
            return;
        }
        break;
    case VmRunner::State::Running:
    case VmRunner::State::Paused:
        /* paused, the screen keeps its last frame: the list and the status
           bar tell */
        break;
    }

    /*
     * The nested desktop of a QEMU found running: served again, for its
     * next session, as the last one ended with the vitrine that served it.
     * The guest may be anywhere, from its firmware to its login screen: the
     * view follows what its agent says, once it answers.
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
        if (m_view) {
            const bool took = m_view->inputEnabled();
            m_view->setInputEnabled(runnerTakesInput(runner));
            setPage(Page::Screen);
            if (!took && m_view->inputEnabled()) {
                emit screenReady();
            }
        } else if (!m_retry->isActive()) {
            m_attempts = 0;
            attach();
        }
        return;
    }
    detach();
    if (VmConfig::screen(runner->runArgs()) == VmConfig::Screen::OwnWindow) {
        setPage(Page::OwnWindow);
    } else {
        showMessage(tr("No screen"),
                    tr("This VM runs without a display (-display none, -nographic or "
                       "VNC, for example). Its serial console and log are on the "
                       "Logs tab."));
    }
}

void VmConsole::updateHome()
{
    if (!m_vm) {
        return;
    }
    const ArgsFile &args = m_vm->args();
    const qint64 mib = VmConfig::memoryMiB(args);
    const VmConfig::Cpus cpus = VmConfig::cpus(args);
    QStringList parts;
    QStringList disks;

    m_name->setText(m_vm->name());
    m_state->setText(stateText(m_vm));
    m_start->setEnabled(m_vm->runner()->state() == VmRunner::State::Stopped);

    if (mib > 0) {
        parts << tr("%1 of memory").arg(memoryText(mib));
    }
    parts << (cpus.count > 1 ? tr("%1 processors").arg(cpus.count) : tr("1 processor"));
    switch (VmConfig::screen(args)) {
    case VmConfig::Screen::Embedded:
        parts << tr("screen in this window");
        break;
    case VmConfig::Screen::OwnWindow:
        parts << tr("screen in its own window");
        break;
    case VmConfig::Screen::None:
        parts << tr("no screen");
        break;
    }
    for (const VmConfig::Disk &disk : VmConfig::disks(args)) {
        if (!disk.cdrom && !disk.file.isEmpty()) {
            disks << QFileInfo(disk.file).fileName();
        }
    }
    QString text = parts.join(tr(", ")) + '.';
    if (!disks.isEmpty()) {
        text += '\n' + tr("Disks: %1").arg(disks.join(tr(", ")));
    }
    m_summary->setText(text);
}

/* The view on the VM's display, again until it answers */
void VmConsole::attach()
{
    const QString socket = m_vm ? m_vm->runner()->displaySocket() : QString();
    QString error;

    m_retry->stop();
    if (m_view || socket.isEmpty()) {
        return;
    }
    /* a new view each time: a failed attach leaves the view half set up */
    auto *view = new VmView(this);
    m_screenLayout->addWidget(view->widget(), 1);
    if (!view->attach(socket, &error, m_vm->runner()->waitsForDisplay(),
                      m_nested ? ConsoleViews::qemuConsole(m_views->view()) : -1)) {
        delete view;
        if (++m_attempts < kAttachTries) {
            showMessage(tr("Connecting to the screen…"), {});
            m_retry->start();
        } else {
            showMessage(tr("Cannot show the screen"), error, tr("&Try Again"));
            /* no screen to wait for */
            if (m_vm) {
                m_vm->runner()->displayReady();
            }
        }
        return;
    }
    m_view = view;
    m_attempts = 0;
    m_stats = {};
    view->setInputEnabled(runnerTakesInput(m_vm->runner()));
    connect(view, &VmView::grabChanged, this, &VmConsole::changed);
    connect(view, &VmView::fullScreenChanged, this, &VmConsole::changed);
    /* a guest paused for its screen runs once QEMU has the screen's size and refresh rate */
    connect(view, &VmView::screenInfoApplied, this, [this]() {
        if (m_vm) {
            m_vm->runner()->displayReady();
        }
    });
    m_statsTimer->start();
    setPage(Page::Screen);
    emit changed();
    if (m_view && m_view->inputEnabled()) {
        emit screenReady();
    }
}

void VmConsole::detach()
{
    m_retry->stop();
    m_attempts = 0;
    if (!m_view) {
        return;
    }
    m_statsTimer->stop();
    m_stats = {};
    /* view() is null while it goes: what it signals on its way out must
       not lead back to it */
    delete std::exchange(m_view, nullptr);
    emit statsChanged();
    emit changed();
}

void VmConsole::showMessage(const QString &title, const QString &text, const QString &button)
{
    m_messageTitle->setText(title);
    m_messageText->setText(text);
    m_messageText->setVisible(!text.isEmpty());
    m_messageButton->setText(button);
    m_messageButton->setVisible(!button.isEmpty());
    setPage(Page::Message);
}

void VmConsole::setPage(Page page)
{
    if (m_pages->currentIndex() != int(page)) {
        m_pages->setCurrentIndex(int(page));
        emit changed();
    }
}
