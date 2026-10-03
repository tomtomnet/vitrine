// SPDX-License-Identifier: GPL-2.0-or-later
#include "vmconsole.h"

#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "core/vmconfig.h"
#include "core/vmhardware.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "ui/banner.h"
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
      m_screenLayout(new QVBoxLayout(m_screen)), m_paused(new Banner(Banner::Information)),
      m_ownWindowText(new QLabel), m_retry(new QTimer(this)), m_statsTimer(new QTimer(this))
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
        auto *icon = new QLabel;
        auto *settings = new QLabel(QString("<a href=\"settings\">%1</a>").arg(tr("Settings")));

        icon->setPixmap(Icons::themed({"computer"}, QStyle::SP_ComputerIcon).pixmap(64, 64));
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
        m_start->setIconSize(QSize(32, 32));
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

    /* Screen: the view's widget goes under the banner */
    m_screenLayout->setContentsMargins(0, 0, 0, 0);
    m_screenLayout->setSpacing(0);
    m_paused->setText(tr("The VM is paused."));
    m_paused->button()->setText(tr("&Resume"));
    m_paused->button()->show();
    m_paused->hide();
    connect(m_paused->button(), &QPushButton::clicked, this, &VmConsole::resumeRequested);
    m_screenLayout->addWidget(m_paused);
    m_pages->addWidget(m_screen);

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
        m_ownWindowText->setText(tr("QEMU shows this VM in a window of its own (SDL display), "
                                    "which stays open without vitrine."));
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
    }
    update();
}

VmConsole::~VmConsole()
{
    /* before the widgets, its window among them */
    delete m_view;
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
    return m_view && m_view->isFullScreen();
}

void VmConsole::setFullScreen(bool on)
{
    if (m_view) {
        m_view->setFullScreen(on);
    }
}

void VmConsole::focusScreen()
{
    if (m_view) {
        m_view->focus();
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
    m_paused->setVisible(state == VmRunner::State::Paused);
    switch (state) {
    case VmRunner::State::Stopped:
    case VmRunner::State::Stopping:
        /* the guest is gone, or going: its screen with it */
        detach();
        setPage(Page::Home);
        return;
    case VmRunner::State::Starting:
        detach();
        showMessage(tr("Starting…"), {});
        return;
    case VmRunner::State::Running:
    case VmRunner::State::Paused:
        break;
    }

    if (!runner->displaySocket().isEmpty()) {
        if (m_view) {
            setPage(Page::Screen);
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
    if (!view->attach(socket, &error)) {
        delete view;
        if (++m_attempts < kAttachTries) {
            showMessage(tr("Connecting to the screen…"), {});
            m_retry->start();
        } else {
            showMessage(tr("Cannot show the screen"), error, tr("&Try Again"));
        }
        return;
    }
    m_view = view;
    m_attempts = 0;
    m_stats = {};
    connect(view, &VmView::grabChanged, this, &VmConsole::changed);
    connect(view, &VmView::fullScreenChanged, this, &VmConsole::changed);
    m_statsTimer->start();
    setPage(Page::Screen);
    emit changed();
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
    delete m_view;
    m_view = nullptr;
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
