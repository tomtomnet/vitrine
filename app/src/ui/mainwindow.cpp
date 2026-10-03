// SPDX-License-Identifier: GPL-2.0-or-later
#include "mainwindow.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

#include "core/paths.h"
#include "core/qemuinfo.h"
#include "core/vmconfig.h"
#include "core/vmhardware.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "ui/clonedialog.h"
#include "ui/firmwarerepair.h"
#include "ui/icons.h"
#include "ui/importdialog.h"
#include "ui/memorymonitor.h"
#include "ui/newvmdialog.h"
#include "ui/perfmonitor.h"
#include "ui/preferencesdialog.h"
#include "ui/qemubuilddialog.h"
#include "ui/qemudocs.h"
#include "ui/referencepanel.h"
#include "ui/textdialog.h"
#include "ui/uiconfig.h"
#include "ui/updatenotifier.h"
#include "ui/usbaccess.h"
#include "ui/vmdetails.h"
#include "ui/vmpane.h"
#include "ui/vmwindow.h"
#include "ui/widgets.h"

enum { IdRole = Qt::UserRole, StateRole, StateColorRole };

/* A computer with a badge for the state */
static QIcon vmIcon(VmRunner::State state)
{
    static QHash<int, QIcon> cache;
    const QIcon base = Icons::themed({"computer"}, QStyle::SP_ComputerIcon);

    if (state == VmRunner::State::Stopped) {
        return base;
    }
    if (cache.contains(int(state))) {
        return cache.value(int(state));
    }

    QIcon icon;
    for (qreal dpr : {1.0, 2.0, 3.0}) {
        QPixmap pixmap = base.pixmap(QSize(32, 32), dpr);
        QPainter p(&pixmap);
        /* in the corner, its outline inside the icon too */
        const qreal outline = 1.5;
        const QRectF badge(32 - 15 - outline / 2, 32 - 15 - outline / 2, 15, 15);
        const QPointF c = badge.center();
        QColor color(0x3d, 0xae, 0xe9);

        if (state == VmRunner::State::Running) {
            color = QColor(0x27, 0xae, 0x60);
        } else if (state == VmRunner::State::Paused) {
            color = QColor(0xf6, 0x74, 0x00);
        }
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(Qt::white, outline));
        p.setBrush(color);
        p.drawEllipse(badge);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::white);
        if (state == VmRunner::State::Paused) {
            p.drawRect(QRectF(c.x() - 3.0, c.y() - 3.5, 2.2, 7));
            p.drawRect(QRectF(c.x() + 0.8, c.y() - 3.5, 2.2, 7));
        } else if (state == VmRunner::State::Running) {
            QPainterPath play;
            play.moveTo(c + QPointF(-2.5, -4));
            play.lineTo(c + QPointF(4, 0));
            play.lineTo(c + QPointF(-2.5, 4));
            play.closeSubpath();
            p.drawPath(play);
        } else {
            for (int i = -1; i <= 1; i++) {
                p.drawEllipse(c + QPointF(i * 3.7, 0), 1.1, 1.1);
            }
        }
        p.end();
        icon.addPixmap(pixmap);
    }
    cache.insert(int(state), icon);
    return icon;
}

/* The name in bold, the state under it */
/* QEMU shows the VM in a window of its own: SDL or GTK, its default being one of them */
static bool hasWindow(const Vm *vm)
{
    const VmConfig::Graphics g = VmConfig::graphics(vm->args());

    return g.custom != "-nographic" &&
           (g.display.isEmpty() || g.display == "sdl" || g.display == "gtk");
}

class VmItemDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QWidget *widget = opt.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        const QString name = opt.text;
        const QString state = index.data(StateRole).toString();
        const bool selected = opt.state & QStyle::State_Selected;
        const QPalette::ColorGroup group =
            !(opt.state & QStyle::State_Enabled) ? QPalette::Disabled
            : (opt.state & QStyle::State_Active) ? QPalette::Normal
                                                 : QPalette::Inactive;
        QFont bold = opt.font;
        QColor stateColor = index.data(StateColorRole).value<QColor>();

        opt.text.clear();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

        bold.setBold(true);
        const QRect rect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget)
                               .adjusted(4, 0, -4, 0);
        const QFontMetrics boldMetrics(bold);
        const QFontMetrics metrics(opt.font);
        int y = rect.top() + (rect.height() - boldMetrics.height() - metrics.height()) / 2;

        if (!stateColor.isValid() || selected) {
            stateColor = opt.palette.color(group, selected ? QPalette::HighlightedText
                                                           : QPalette::PlaceholderText);
        }
        painter->save();
        painter->setFont(bold);
        painter->setPen(opt.palette.color(group, selected ? QPalette::HighlightedText
                                                          : QPalette::Text));
        painter->drawText(QRect(rect.left(), y, rect.width(), boldMetrics.height()),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          boldMetrics.elidedText(name, Qt::ElideRight, rect.width()));
        y += boldMetrics.height();
        painter->setFont(opt.font);
        painter->setPen(stateColor);
        painter->drawText(QRect(rect.left(), y, rect.width(), metrics.height()),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          metrics.elidedText(state, Qt::ElideRight, rect.width()));
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        QFont bold = option.font;

        bold.setBold(true);
        size.setHeight(qMax(size.height(), QFontMetrics(bold).height() +
                                               QFontMetrics(option.font).height() + 12));
        return size;
    }
};

MainWindow::MainWindow(VmStore *store, QWidget *parent)
    : QMainWindow(parent), m_store(store), m_list(new QListWidget),
      m_right(new QStackedWidget), m_pane(new VmPane), m_details(m_pane->details()),
      m_splitter(new QSplitter), m_qemuStatus(new QLabel), m_perf(new PerfMonitor)
{
    auto *welcome = new QWidget;
    auto *welcomeLayout = new QVBoxLayout(welcome);
    auto *welcomeText = new QLabel(tr("<h2>No virtual machines yet</h2>"
                                      "<p>Create one to get started.</p>"));
    auto *create = new QPushButton;
    const QSettings settings(Paths::settingsPath(), QSettings::IniFormat);

    createActions();

    m_list->setObjectName("vms");
    m_list->setIconSize(QSize(32, 32));
    m_list->setItemDelegate(new VmItemDelegate(m_list));
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setMinimumWidth(200);
    /* the list is the side of the window, as the pages are the side of the settings */
    m_list->setFrameShape(QFrame::NoFrame);

    welcomeText->setAlignment(Qt::AlignCenter);
    create->setText(m_new->text().remove('&'));
    create->setIcon(m_new->icon());
    welcomeLayout->addStretch();
    welcomeLayout->addWidget(welcomeText);
    welcomeLayout->addWidget(create, 0, Qt::AlignCenter);
    welcomeLayout->addStretch();
    m_right->addWidget(welcome);
    m_right->addWidget(m_pane);

    m_splitter->addWidget(m_list);
    m_splitter->addWidget(m_right);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setSizes({260, 800});
    setCentralWidget(m_splitter);

    m_qemuStatus->setObjectName("qemuStatus");
    m_updates = new UpdateNotifier(this);
    connect(m_updates, &UpdateNotifier::buildQemuRequested, this, &MainWindow::buildQemu);
    statusBar()->addPermanentWidget(m_perf);
    statusBar()->addPermanentWidget(m_updates->button());
    statusBar()->addPermanentWidget(new MemoryMonitor(store, this));
    statusBar()->addPermanentWidget(m_qemuStatus);

    connect(create, &QPushButton::clicked, m_new, &QAction::trigger);
    connect(m_list, &QListWidget::currentItemChanged, this, &MainWindow::currentChanged);
    /* a double click starts the VM, or brings its window up */
    connect(m_list, &QListWidget::itemActivated, this, [this]() {
        if (m_start->isEnabled()) {
            start();
        } else if (m_showWindow->isEnabled()) {
            showWindow();
        }
    });
    connect(m_list, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        if (!m_list->itemAt(pos)) {
            return;
        }
        QMenu menu;
        menu.addActions({m_start, m_showWindow, m_pause, m_shutDown, m_reset, m_forceOff});
        menu.addSeparator();
        menu.addActions({m_settings, m_log, m_folder, m_command});
        menu.addSeparator();
        menu.addActions({m_clone, m_remove});
        menu.exec(m_list->viewport()->mapToGlobal(pos));
    });
    connect(m_details, &VmDetails::showLog, this, &MainWindow::showLog);
    connect(m_pane, &VmPane::startFromSnapshot, this, &MainWindow::startFrom);
    connect(store, &VmStore::added, this, &MainWindow::addVm);
    connect(store, &VmStore::removed, this, &MainWindow::removeItem);
    connect(QemuDocs::preferred(), &QemuDocs::changed, this, &MainWindow::updateStatus);

    for (Vm *vm : store->vms()) {
        addVm(vm);
    }
    if (!restoreGeometry(settings.value("mainwindow/geometry").toByteArray())) {
        /* room for the settings pages beside their list */
        resize(1060, 660);
    }
    restoreState(settings.value("mainwindow/state").toByteArray());
    m_splitter->restoreState(settings.value("mainwindow/splitter").toByteArray());
    select(settings.value("mainwindow/current").toString());
    if (!m_list->currentItem() && m_list->count() > 0) {
        m_list->setCurrentRow(0);
    }
    currentChanged();
    m_pane->setPage(VmPane::Page(settings.value("settings/page").toInt()));
    m_pane->setTab(VmPane::Tab(settings.value("mainwindow/tab").toInt()));
    updateStatus();
}

void MainWindow::createActions()
{
    auto action = [this](const QString &text, const QStringList &icons,
                         QStyle::StandardPixmap fallback, const QKeySequence &shortcut,
                         void (MainWindow::*slot)()) {
        auto *a = new QAction(Icons::themed(icons, fallback), text, this);
        a->setShortcut(shortcut);
        connect(a, &QAction::triggered, this, slot);
        return a;
    };

    m_new = action(tr("&New…"), {"list-add", "document-new"}, QStyle::SP_FileDialogNewFolder,
                   QKeySequence::New, &MainWindow::newVm);
    m_new->setToolTip(tr("Create a virtual machine"));
    m_import = action(tr("&Import VM…"), {"document-import"}, QStyle::SP_DialogOpenButton,
                      QKeySequence(Qt::CTRL | Qt::Key_I), &MainWindow::importVm);
    m_import->setToolTip(tr("Create a virtual machine from a QEMU launch script"));
    m_build = action(tr("&Build QEMU…"), {"run-build", "run-build-install"},
                     QStyle::SP_BrowserReload, QKeySequence(Qt::CTRL | Qt::Key_B),
                     &MainWindow::buildQemu);
    m_settings = new QAction(Icons::themed({"configure", "preferences-system"},
                                           QStyle::SP_FileDialogDetailedView),
                             tr("&Settings"), this);
    m_settings->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_S));
    connect(m_settings, &QAction::triggered, this, [this]() { openSettings(current()); });
    m_start = action(tr("S&tart"), {"media-playback-start"}, QStyle::SP_MediaPlay,
                     QKeySequence(Qt::CTRL | Qt::Key_Return), &MainWindow::start);
    m_showWindow = action(tr("Show &Window"), {"window", "window-restore", "view-restore"},
                          QStyle::SP_TitleBarNormalButton, {}, &MainWindow::showWindow);
    m_showWindow->setToolTip(tr("Bring the window of the VM to the front"));
    m_pause = action(tr("&Pause"), {"media-playback-pause"}, QStyle::SP_MediaPause,
                     QKeySequence(Qt::CTRL | Qt::Key_P), &MainWindow::togglePause);
    m_shutDown = action(tr("Shut &Down"), {"system-shutdown"}, QStyle::SP_MediaStop,
                        QKeySequence(Qt::CTRL | Qt::Key_H), &MainWindow::shutDown);
    m_shutDown->setToolTip(tr("Ask the guest to shut down, as with the power button"));
    m_reset = action(tr("&Reset"), {"system-reboot", "view-refresh"}, QStyle::SP_BrowserReload,
                     {}, &MainWindow::reset);
    m_forceOff = action(tr("&Force Off"), {"process-stop"}, QStyle::SP_BrowserStop, {},
                        &MainWindow::forceOff);
    m_forceOff->setToolTip(tr("Stop the VM at once, as when pulling the plug"));
    m_clone = action(tr("&Clone…"), {"edit-copy"}, QStyle::SP_FileDialogNewFolder, {},
                     &MainWindow::cloneVm);
    m_clone->setToolTip(tr("A new VM with the same settings and copies of the disks"));
    m_remove = action(tr("Re&move…"), {"edit-delete", "user-trash"}, QStyle::SP_TrashIcon,
                      QKeySequence::Delete, &MainWindow::remove);
    m_log = action(tr("Show &Log"), {"text-x-generic", "document-preview"},
                   QStyle::SP_FileDialogContentsView, QKeySequence(Qt::CTRL | Qt::Key_L),
                   &MainWindow::showLog);
    m_folder = new QAction(Icons::themed({"folder-open"}, QStyle::SP_DirOpenIcon),
                           tr("Open &Folder"), this);
    connect(m_folder, &QAction::triggered, this, [this]() {
        if (Vm *vm = current()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(vm->dir()));
        }
    });
    m_command = action(tr("Show &Command Line"), {"utilities-terminal"},
                       QStyle::SP_ComputerIcon, {}, &MainWindow::showCommandLine);
    m_preferences = new QAction(Icons::themed({"preferences-other", "configure"},
                                              QStyle::SP_FileDialogDetailedView),
                                tr("&Preferences…"), this);
    m_preferences->setShortcut(QKeySequence::Preferences);
    connect(m_preferences, &QAction::triggered, this, [this]() {
        PreferencesDialog dialog(this);
        dialog.exec();
    });
    m_reference = new QAction(Icons::themed({"help-contents", "documentation"},
                                            QStyle::SP_DialogHelpButton),
                              tr("QEMU &Reference"), this);
    m_reference->setShortcut(QKeySequence::HelpContents);
    connect(m_reference, &QAction::triggered, this, [this]() { ReferenceWindow::open(this); });
    m_quit = new QAction(Icons::themed({"application-exit"}, QStyle::SP_DialogCloseButton),
                         tr("&Quit"), this);
    m_quit->setShortcut(QKeySequence::Quit);
    connect(m_quit, &QAction::triggered, this, &QWidget::close);

    QMenu *file = menuBar()->addMenu(tr("&File"));
    file->addAction(m_new);
    file->addAction(m_import);
    file->addSeparator();
    file->addAction(m_build);
    file->addAction(m_preferences);
    file->addAction(m_reference);
    file->addSeparator();
    file->addAction(m_quit);

    QMenu *machine = menuBar()->addMenu(tr("&Machine"));
    machine->addAction(m_settings);
    machine->addSeparator();
    machine->addActions({m_start, m_showWindow, m_pause, m_shutDown, m_reset, m_forceOff});
    machine->addSeparator();
    machine->addActions({m_log, m_folder, m_command});
    machine->addSeparator();
    machine->addActions({m_clone, m_remove});

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    help->addAction(m_reference);
    help->addAction(tr("Check for &Updates…"), this, [this]() { m_updates->checkNow(); });
    help->addSeparator();
    help->addAction(tr("&About Vitrine"), this, [this]() {
        QMessageBox::about(
            this, tr("About Vitrine"),
            tr("<h3>Vitrine %1</h3>"
               "<p>Creates and runs QEMU virtual machines. Each VM is its QEMU command "
               "line, which the settings edit and you can edit as text.</p>"
               "<p>Written by AI: Anthropic's Claude, through Claude Code.</p>"
               "<p>License: GPL-2.0-or-later</p>")
                .arg(QApplication::applicationVersion()));
    });
    help->addAction(tr("About &Qt"), qApp, &QApplication::aboutQt);

    QToolBar *toolbar = addToolBar(tr("Main Toolbar"));
    toolbar->setObjectName("toolbar");
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonFollowStyle);
    toolbar->addAction(m_new);
    toolbar->addSeparator();
    toolbar->addActions({m_start, m_pause, m_shutDown, m_forceOff});
}

Vm *MainWindow::current() const
{
    const QListWidgetItem *item = m_list->currentItem();
    return item ? m_store->find(item->data(IdRole).toString()) : nullptr;
}

void MainWindow::select(const QString &id)
{
    if (QListWidgetItem *item = itemOf(id)) {
        m_list->setCurrentItem(item);
    }
}

QListWidgetItem *MainWindow::itemOf(const QString &id) const
{
    for (int i = 0; i < m_list->count(); i++) {
        if (m_list->item(i)->data(IdRole).toString() == id) {
            return m_list->item(i);
        }
    }
    return nullptr;
}

void MainWindow::addVm(Vm *vm)
{
    auto *item = new QListWidgetItem(vm->name());

    item->setData(IdRole, vm->id());
    m_list->addItem(item);

    connect(vm, &Vm::changed, this, [this, vm]() {
        updateItem(vm);
        if (vm == current()) {
            m_details->refresh();
        }
    });
    connect(vm->runner(), &VmRunner::stateChanged, this,
            [this, vm](VmRunner::State state) { stateChanged(vm, state); });
    connect(vm->runner(), &VmRunner::sharesMounted, this,
            [this, vm](const QStringList &mounted, const QStringList &problems) {
        if (!mounted.isEmpty()) {
            statusBar()->showMessage(tr("%1 mounted its shared folders: %2")
                                         .arg(vm->name(), mounted.join(", ")), 10000);
        }
        if (!problems.isEmpty()) {
            auto *box = Widgets::messageBox(
                QMessageBox::Warning, tr("Shared Folders"),
                tr("%1 could not mount some of its shared folders:\n\n%2")
                    .arg(vm->name(), problems.join('\n')),
                QMessageBox::Ok, this);
            box->setAttribute(Qt::WA_DeleteOnClose);
            box->open();
        }
    });
    connect(vm->runner(), &VmRunner::failed, this,
            [this, vm](const QString &error) { failed(vm, error); });
    m_states[vm->id()] = vm->runner()->state();
    updateItem(vm);
    m_list->sortItems();
    if (m_list->count() == 1) {
        m_list->setCurrentItem(item);
    }
    currentChanged();
}

void MainWindow::stateChanged(Vm *vm, VmRunner::State state)
{
    const QString id = vm->id();

    if (state == VmRunner::State::Stopped) {
        /* for failed(), which follows */
        m_endedFrom[id] = m_states.value(id);
    } else {
        /* a new run, started here or found running (attach) */
        m_errors.remove(id);
        if (state != VmRunner::State::Starting) {
            m_starting.remove(id);
        }
    }
    m_states[id] = state;
    updateItem(vm);
    if (vm == current()) {
        m_details->setError(m_errors.value(id));
        m_details->refresh();
        updateActions();
    }
}

void MainWindow::failed(Vm *vm, const QString &error)
{
    const QString id = vm->id();
    const VmRunner::State state = vm->runner()->state();
    QList<FirmwareFiles::File> firmware;
    QString title, text;

    if (state != VmRunner::State::Stopped) {
        /* QEMU refused a command: the VM runs on */
        title = vm->name();
        text = tr("QEMU refused the command.");
    } else {
        const VmRunner::State from = m_endedFrom.value(id);

        m_errors[id] = error;
        updateItem(vm);
        if (vm == current()) {
            m_details->setError(error);
            updateActions();
        }
        if (m_starting.remove(id)) {
            title = tr("Cannot Start %1").arg(vm->name());
            text = tr("%1 could not start.").arg(vm->name());
            firmware = FirmwareRepair::named(vm, error);
        } else if (from != VmRunner::State::Stopped) {
            title = tr("%1 Stopped").arg(vm->name());
            text = tr("%1 stopped unexpectedly.").arg(vm->name());
        } else {
            title = vm->name();
            text = tr("Cannot connect to %1.").arg(vm->name());
        }
    }

    auto *box = Widgets::messageBox(QMessageBox::Warning, title, text, QMessageBox::Close, this);
    box->setInformativeText(error);
    box->setAttribute(Qt::WA_DeleteOnClose);
    if (QFileInfo::exists(vm->runner()->logPath())) {
        QPushButton *log = box->addButton(tr("Show &Log"), QMessageBox::ActionRole);
        connect(log, &QPushButton::clicked, this, [this, id]() {
            select(id);
            showLog();
        });
    }
    /* QEMU refused a firmware copy */
    if (!firmware.isEmpty()) {
        const bool vars = std::all_of(firmware.begin(), firmware.end(), [](const auto &f) {
            return f.role == FirmwareFiles::File::Role::Vars;
        });
        QPushButton *renew = box->addButton(vars ? tr("Reset UEFI &Variables…")
                                                 : tr("&Replace Firmware Files…"),
                                            QMessageBox::ActionRole);
        connect(renew, &QPushButton::clicked, this, [this, id, firmware]() {
            Vm *vm = m_store->find(id);
            if (vm && FirmwareRepair::reset(this, vm, firmware, tr("&Replace and Start"))) {
                select(id);
                start();
            }
        });
    }
    box->open();
}

void MainWindow::removeItem(const QString &id)
{
    /* its changes go with it */
    if (m_pane->vm() && m_pane->vm()->id() == id) {
        m_pane->setVm(nullptr);
    }
    m_errors.remove(id);
    m_states.remove(id);
    m_endedFrom.remove(id);
    m_starting.remove(id);
    delete itemOf(id);
    currentChanged();
}

void MainWindow::updateItem(Vm *vm)
{
    QListWidgetItem *item = itemOf(vm->id());
    const VmRunner::State state = vm->runner()->state();
    const bool dark = palette().color(QPalette::Base).lightnessF() < 0.5;
    QColor color;

    if (!item) {
        return;
    }
    if (m_errors.contains(vm->id()) && state == VmRunner::State::Stopped) {
        item->setData(StateRole, tr("Stopped with an error"));
        color = dark ? QColor(0xff, 0x6b, 0x6b) : QColor(0xda, 0x44, 0x53);
    } else {
        item->setData(StateRole, stateText(vm));
        if (state == VmRunner::State::Running) {
            color = dark ? QColor(0x5f, 0xd3, 0x8d) : QColor(0x1e, 0x8a, 0x4c);
        } else if (state == VmRunner::State::Paused) {
            color = dark ? QColor(0xff, 0xa9, 0x4d) : QColor(0xc0, 0x5a, 0x00);
        }
    }
    item->setData(StateColorRole, color);
    item->setIcon(vmIcon(state));
    if (item->text() != vm->name()) {
        item->setText(vm->name());
        m_list->sortItems();
    }
    updateStatus();
}

void MainWindow::currentChanged()
{
    /* the changes not applied to the VM shown: asked about after the click */
    if (m_pane->vm() && current() != m_pane->vm() && m_pane->isModified()) {
        if (!m_leaving) {
            m_leaving = true;
            QTimer::singleShot(0, this, &MainWindow::leaveVm);
        }
        return;
    }
    showCurrent();
}

void MainWindow::leaveVm()
{
    Vm *shown = m_pane->vm();

    m_leaving = false;
    if (shown && current() != shown &&
        !m_pane->confirmChanges(tr("Apply them before going to another VM?"))) {
        /* Cancel: back to it */
        select(shown->id());
        return;
    }
    showCurrent();
}

void MainWindow::showCurrent()
{
    Vm *vm = current();

    m_right->setCurrentIndex(m_list->count() == 0 ? 0 : 1);
    m_pane->setVm(vm);
    m_perf->setVm(vm);
    m_details->setError(vm ? m_errors.value(vm->id()) : QString());
    updateActions();
}

void MainWindow::updateActions()
{
    Vm *vm = current();
    const VmRunner::State state = vm ? vm->runner()->state() : VmRunner::State::Stopped;
    const bool paused = state == VmRunner::State::Paused;

    m_settings->setEnabled(vm);
    m_start->setEnabled(vm && state == VmRunner::State::Stopped);
    m_showWindow->setEnabled(vm && (state == VmRunner::State::Running || paused) &&
                             hasWindow(vm));
    m_pause->setEnabled(vm && (state == VmRunner::State::Running || paused));
    m_pause->setText(paused ? tr("&Resume") : tr("&Pause"));
    m_pause->setIcon(paused ? Icons::themed({"media-playback-start"}, QStyle::SP_MediaPlay)
                            : Icons::themed({"media-playback-pause"}, QStyle::SP_MediaPause));
    m_shutDown->setEnabled(vm && state == VmRunner::State::Running);
    m_reset->setEnabled(vm && (state == VmRunner::State::Running || paused));
    m_forceOff->setEnabled(vm && state != VmRunner::State::Stopped);
    m_clone->setEnabled(vm && state == VmRunner::State::Stopped);
    m_remove->setEnabled(vm && state == VmRunner::State::Stopped);
    m_log->setEnabled(vm);
    m_folder->setEnabled(vm);
    m_command->setEnabled(vm);
}

void MainWindow::updateStatus()
{
    const QemuDocs *docs = QemuDocs::preferred();
    const QemuInfo *info = docs->info();
    int running = 0;

    for (Vm *vm : m_store->vms()) {
        running += vm->runner()->isActive();
    }
    if (info) {
        m_qemuStatus->setText(tr("QEMU %1").arg(info->version));
        m_qemuStatus->setToolTip(docs->binary());
    } else {
        m_qemuStatus->setText(docs->status());
        m_qemuStatus->setToolTip(QString());
    }
    statusBar()->showMessage(running == 0 ? QString()
                                          : tr("%n running", nullptr, running));
}

void MainWindow::newVm()
{
    NewVmDialog dialog(m_store, this);

    if (dialog.exec() != QDialog::Accepted || !dialog.vm()) {
        return;
    }
    select(dialog.vm()->id());
    if (!dialog.warnings().isEmpty()) {
        Widgets::inform(this, tr("VM Created"), dialog.warnings().join("\n\n"));
    }
    if (dialog.openSettings()) {
        openSettings(dialog.vm(), VmPane::Arguments);
    }
}

void MainWindow::importVm()
{
    ImportDialog dialog(m_store, QemuDocs::preferred()->info(), this);

    if (dialog.exec() == QDialog::Accepted && dialog.vm()) {
        select(dialog.vm()->id());
    }
}

void MainWindow::buildQemu()
{
    if (!m_buildDialog) {
        /* not modal: the VMs stay at hand during a build */
        m_buildDialog = new QemuBuildDialog(this);
        connect(m_buildDialog, &QemuBuildDialog::built, m_updates, &UpdateNotifier::revalidate);
        connect(m_buildDialog, &QemuBuildDialog::qemuChanged, this, [this]() {
            QemuDocs::reloadPreferred();
            m_details->refresh();
        });
    }
    m_buildDialog->show();
    m_buildDialog->raise();
    m_buildDialog->activateWindow();
}

void MainWindow::bringToFront()
{
    setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
    show();
    raise();
    activateWindow();
}

void MainWindow::openSettings(Vm *vm, int page)
{
    if (!vm) {
        return;
    }
    select(vm->id());
    if (page >= 0) {
        m_pane->setPage(VmPane::Page(page));
    }
    m_pane->setTab(VmPane::Settings);
}

void MainWindow::startFrom(const QString &snapshot)
{
    if (Vm *vm = current()) {
        m_loadvm.insert(vm->id(), snapshot);
        start();
    }
}

void MainWindow::start()
{
    Vm *vm = current();

    if (!vm) {
        return;
    }
    /* for this start only */
    const QString snapshot = m_loadvm.take(vm->id());
    /* it starts with the settings saved */
    if (vm == m_pane->vm() && !m_pane->confirmChanges(tr("Apply them before it starts?"))) {
        return;
    }
    if (VmConfig::qemuBinary(vm->args()).isEmpty() && Paths::qemuBinary().isEmpty()) {
        Widgets::warn(this, tr("QEMU Not Found"),
                      tr("%1 is not in PATH. Choose the QEMU to use in the "
                         "preferences, or build one with File > Build QEMU.")
                          .arg(Paths::qemuSystemName()));
        return;
    }
    if (m_askingUsb.contains(vm->id())) {
        return;
    }
    /* firmware copies a power cut damaged, say: new ones, if the user wants */
    if (!FirmwareRepair::checkBeforeStart(this, vm)) {
        return;
    }
    m_errors.remove(vm->id());
    m_details->setError({});

    /* the VM gets its USB devices only if it can open them from the start */
    const QString id = vm->id();
    m_askingUsb.insert(id);
    statusBar()->showMessage(tr("Checking the access to the USB devices of %1…").arg(vm->name()));
    UsbAccess::request(vm, this, [this, id, snapshot](const QString &warning) {
        Vm *vm = m_store->find(id);

        m_askingUsb.remove(id);
        statusBar()->clearMessage();
        if (!vm || vm->runner()->isActive()) {
            return;
        }
        m_starting.insert(id);
        ArgsFile args = vm->args();
        if (!snapshot.isEmpty()) {
            args.add("loadvm", snapshot);
        }
        vm->runner()->start(args);
        if (!warning.isEmpty()) {
            auto *box = Widgets::messageBox(QMessageBox::Warning, tr("USB Passthrough"), warning,
                                            QMessageBox::Ok, this);
            box->setAttribute(Qt::WA_DeleteOnClose);
            box->open();
        }
    });
}

void MainWindow::togglePause()
{
    if (Vm *vm = current()) {
        if (vm->runner()->state() == VmRunner::State::Paused) {
            vm->runner()->resume();
        } else {
            vm->runner()->pause();
        }
    }
}

void MainWindow::shutDown()
{
    if (Vm *vm = current()) {
        vm->runner()->powerdown();
        statusBar()->showMessage(tr("Asked %1 to shut down").arg(vm->name()), 5000);
    }
}

void MainWindow::reset()
{
    Vm *vm = current();

    if (vm && Widgets::confirm(this, QMessageBox::Warning, tr("Reset %1?").arg(vm->name()),
                      tr("The VM restarts at once: the guest loses its unsaved work."),
                      tr("&Reset"))) {
        vm->runner()->reset();
    }
}

void MainWindow::forceOff()
{
    Vm *vm = current();

    if (!vm) {
        return;
    }
    if (vm->runner()->state() == VmRunner::State::Stopping) {
        /* the guest is off already, or on its way */
        vm->runner()->forceOff();
        return;
    }
    if (Widgets::confirm(this, QMessageBox::Warning, tr("Force Off %1?").arg(vm->name()),
                tr("The VM stops at once, as when pulling the plug: the guest loses its "
                   "unsaved work."),
                tr("&Force Off"))) {
        vm->runner()->forceOff();
    }
}

void MainWindow::cloneVm()
{
    Vm *vm = current();

    if (!vm || vm->runner()->isActive()) {
        return;
    }
    if (vm == m_pane->vm() && !m_pane->confirmChanges(tr("Apply them before cloning it?"))) {
        return;
    }
    CloneDialog dialog(m_store, vm, this);
    if (dialog.exec() == QDialog::Accepted && dialog.clone()) {
        select(dialog.clone()->id());
    }
}

void MainWindow::remove()
{
    Vm *vm = current();
    QString error, text;

    if (!vm || vm->runner()->isActive()) {
        return;
    }
    text = tr("The folder of %1, disks included, goes to the trash, where you can still "
              "restore it from.").arg(vm->name());
    if (vm == m_pane->vm() && m_pane->isModified()) {
        text += ' ' + tr("The changes to its settings that are not applied are lost.");
    }
    if (Widgets::confirm(this, QMessageBox::Question, tr("Remove %1?").arg(vm->name()), text,
                         tr("&Move to Trash")) &&
        !m_store->remove(vm, &error)) {
        Widgets::warn(this, tr("Cannot Remove the VM"), error);
    }
}

void MainWindow::showLog()
{
    if (current()) {
        m_pane->setTab(VmPane::Logs);
    }
}

void MainWindow::showWindow()
{
    Vm *vm = current();
    QString error;

    if (vm && vm->runner()->pid() > 0 && !VmWindow::raise(vm->runner()->pid(), &error)) {
        statusBar()->showMessage(error, 10000);
    }
}

void MainWindow::showCommandLine()
{
    if (Vm *vm = current()) {
        TextDialog::showText(this, tr("%1 — Command Line").arg(vm->name()),
                             UiConfig::commandText(vm->runner()->commandLine(vm->args()),
                                                   vm->dir()));
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    QSettings settings(Paths::settingsPath(), QSettings::IniFormat);
    const Vm *vm = current();

    if (!m_pane->confirmChanges(tr("Apply them before closing?"))) {
        event->ignore();
        return;
    }
    settings.setValue("mainwindow/geometry", saveGeometry());
    settings.setValue("mainwindow/state", saveState());
    settings.setValue("mainwindow/splitter", m_splitter->saveState());
    settings.setValue("mainwindow/current", vm ? vm->id() : QString());
    settings.setValue("mainwindow/tab", int(m_pane->tab()));
    settings.setValue("settings/page", int(m_pane->page()));
    QMainWindow::closeEvent(event);
}
