// SPDX-License-Identifier: GPL-2.0-or-later
#include "buildswindow.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDirIterator>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <sys/stat.h>

#include "core/buildmanager.h"
#include "core/guesttools.h"
#include "core/hostkde.h"
#include "core/hostkdebuilder.h"
#include "core/paths.h"
#include "ui/banner.h"
#include "ui/buildtexts.h"
#include "ui/hostkdedialog.h"
#include "ui/icons.h"
#include "ui/taskbutton.h"
#include "ui/widgets.h"

using Builds::Component;
using Builds::Kind;
using State = BuildManager::State;
using StatusState = Builds::Status::State;

namespace {

/* What the items of the tree hold */
enum Role {
    IdRole = Qt::UserRole,      // the component's id, or the group's: host, guest, system
    ProgressRole,               // 0 to 1 while it builds, else none
};

enum Column { NameColumn, StatusColumn, BuiltColumn };

/* The lines of a log a window shows: the manager keeps more */
constexpr int kLogLines = 5000;
/* The source of a component building is shown again at most so often */
constexpr qint64 kSourceEveryMs = 2000;

/* The group a component shows in, by what it makes */
QString groupOf(Kind kind)
{
    switch (kind) {
    case Kind::Host:
        return QStringLiteral("host");
    case Kind::System:
        return QStringLiteral("system");
    default:
        return QStringLiteral("guest");
    }
}

/* The row of another release's guest tools: "release:44"; and back, "" for any other id */
QString releaseId(const QString &release)
{
    return "release:" + release;
}

QString releaseOf(const QString &id)
{
    return id.startsWith("release:") ? id.mid(8) : QString();
}

/* The guest tools' with their Fedora release */
QString groupTitle(const QString &group, const QString &guestRelease)
{
    return group == "host"       ? BuildsWindow::tr("Vitrine's QEMU")
           : group == "system"   ? BuildsWindow::tr("Host desktop")
           : guestRelease.isEmpty() ? BuildsWindow::tr("Guest tools")
                                 : BuildsWindow::tr("Guest tools for Fedora %1").arg(guestRelease);
}

/* A host package's icon: what it is to the user, rather than to a build */
QIcon systemIcon(const HostKde::Status &status)
{
    using Owner = HostKde::Status::Owner;
    using Offer = HostKde::Status::Offer;

    if (status.isOutdated() || status.owner == Owner::Foreign ||
        (status.owner == Owner::Vitrine && !status.locked)) {
        return Icons::keepColours(Icons::themed({"dialog-warning"}, QStyle::SP_MessageBoxWarning));
    }
    if (status.owner == Owner::Vitrine) {
        return Icons::upToDate();
    }
    if (status.offer != Offer::Offered) {
        return Icons::themed({"dialog-information", "help-about"}, QStyle::SP_MessageBoxInformation);
    }
    return {};
}

QString capitalized(const QString &text)
{
    return text.isEmpty() ? text : text.left(1).toUpper() + text.mid(1);
}

/* The repository's page, from its git URL */
QString webPage(const QString &url)
{
    return url.endsWith(".git") ? url.chopped(4) : url;
}

/* "gitlab.com/qemu-project/qemu" */
QString shortUrl(const QString &url)
{
    const QUrl u(webPage(url));
    return u.isValid() && !u.host().isEmpty() ? u.host() + u.path() : url;
}

/* "1.4 GB", as the size of files goes */
QString bytes(qint64 size)
{
    return QLocale().formattedDataSize(size, 1, QLocale::DataSizeTraditionalFormat);
}

/* What @path takes on disk, its folders' files too, links not followed */
qint64 diskUsage(const QString &path)
{
    struct stat st;
    qint64 total = 0;

    if (::lstat(QFile::encodeName(path).constData(), &st) != 0) {
        return 0;
    }
    if (!S_ISDIR(st.st_mode)) {
        return qint64(st.st_blocks) * 512;
    }
    QDirIterator it(path, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (::lstat(QFile::encodeName(it.next()).constData(), &st) == 0) {
            total += qint64(st.st_blocks) * 512;
        }
    }
    return total;
}

QIcon stateIcon(State run, StatusState state)
{
    switch (run) {
    case State::Queued:
    case State::Waiting:
        return Icons::themed({"chronometer", "alarm-symbolic", "view-history"},
                             QStyle::SP_FileDialogInfoView);
    case State::Building:
        return Icons::themed({"run-build", "run-build-install"}, QStyle::SP_BrowserReload);
    case State::Failed:
        return Icons::keepColours(
            Icons::themed({"emblem-error", "dialog-error"}, QStyle::SP_MessageBoxCritical));
    case State::Stopped:
    case State::Skipped:
        return Icons::themed({"process-stop"}, QStyle::SP_BrowserStop);
    default:
        break;
    }
    switch (state) {
    case StatusState::UpToDate:
        /* Breeze's check without its frame, the user's pick of the two */
        return Icons::upToDate();
    case StatusState::Outdated:
    case StatusState::NotBuilt:
        return Icons::keepColours(Icons::themed({"update-high", "system-software-update"},
                                                QStyle::SP_MessageBoxWarning));
    case StatusState::NoSources:
        break;
    }
    return Icons::keepColours(Icons::themed({"dialog-warning"}, QStyle::SP_MessageBoxWarning));
}

/*
 * The Status column: the text, then a thin bar under it while the
 * component builds and tells how far it is
 */
class StatusDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyledItemDelegate::paint(painter, option, index);
        const QVariant progress = index.data(ProgressRole);
        if (!progress.isValid() || progress.toDouble() < 0) {
            return;
        }
        const bool selected = option.state & QStyle::State_Selected;
        const int height = qMax(2, option.fontMetrics.height() / 7);
        QRect bar = option.rect.adjusted(option.fontMetrics.averageCharWidth() / 2, 0,
                                         -option.fontMetrics.averageCharWidth(), -2);
        QColor track = option.palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
        bar.setTop(bar.bottom() - height + 1);
        track.setAlphaF(0.2f);
        painter->save();
        painter->fillRect(bar, track);
        bar.setWidth(qRound(bar.width() * qBound(0.0, progress.toDouble(), 1.0)));
        painter->fillRect(bar, option.palette.color(selected ? QPalette::HighlightedText
                                                             : QPalette::Highlight));
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        /* room for the bar under the text, on every row alike */
        const QSize size = QStyledItemDelegate::sizeHint(option, index);
        return QSize(size.width(), size.height() + qMax(2, option.fontMetrics.height() / 7) + 2);
    }
};

}

QString BuildsWindow::describe(const StackBuilder::Build &build)
{
    return BuildTexts::describe(build);
}

QString BuildsWindow::explain(StackBuilder::State state, const StackBuilder::Build &build)
{
    return BuildTexts::explain(state, build);
}

QString BuildsWindow::describeGuestTools(const QString &release)
{
    return BuildTexts::describeGuestTools(release);
}

QString BuildsWindow::explain(GuestToolsBuilder::State state)
{
    return BuildTexts::explain(state);
}

QString BuildsWindow::duration(qint64 ms)
{
    return BuildTexts::duration(ms);
}

BuildsWindow *BuildsWindow::present(QWidget *from, const QString &component,
                                    const QString &guestRelease)
{
    static QPointer<BuildsWindow> open;
    QWidget *parent = from ? from->window() : nullptr;

    /* over the window that asks: one under a modal dialog would take no input */
    if (open && open->parentWidget() != parent) {
        open->close();
        open = nullptr;
    }
    if (!open) {
        open = new BuildsWindow(parent);
        open->setAttribute(Qt::WA_DeleteOnClose);
    }
    /* a VM's: its release's medium (a run keeps its own) */
    if (!guestRelease.isEmpty()) {
        BuildManager::instance()->setGuestRelease(guestRelease);
    }
    if (!component.isEmpty()) {
        open->select(component);
    }
    open->show();
    open->raise();
    open->activateWindow();
    return open;
}

BuildsWindow::BuildsWindow(QWidget *parent)
    : QDialog(parent), m_manager(BuildManager::instance()), m_memory(new Banner(Banner::Warning)),
      m_header(Widgets::note()), m_missing(Widgets::note()), m_tree(new QTreeWidget),
      m_tabs(new QTabWidget), m_source(Widgets::note()), m_patches(new QTreeWidget),
      m_patchText(new QPlainTextEdit), m_log(new QPlainTextEdit), m_background(Widgets::hint()),
      m_cache(new QLabel), m_clear(new QPushButton(tr("C&lear…"))), m_build(new QPushButton),
      m_all(new QPushButton), m_stop(new QPushButton(tr("&Stop"))),
      m_install(new QPushButton(tr("&Install…"))), m_revert(new QPushButton(tr("Re&vert…")))
{
    auto *layout = new QVBoxLayout(this);
    auto *split = m_split = new Splitter(Qt::Vertical);
    auto *sourceScroll = new QScrollArea;
    auto *patchSplit = new Splitter(Qt::Vertical);
    auto *bottom = new QHBoxLayout;
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    setObjectName("builds");
    setWindowTitle(tr("Builds"));
    m_memory->hide();
    m_memory->button()->setText(tr("Build &Anyway"));
    m_memory->button()->show();
    m_memory->button()->setToolTip(tr("Start it now: if the host runs short of memory, the "
                                      "kernel stops a program to free some, maybe a VM"));
    layout->addWidget(m_memory);
    m_header->setObjectName("buildsHeader");
    layout->addWidget(m_header);
    m_missing->setObjectName("buildsMissing");
    m_missing->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_missing->hide();
    layout->addWidget(m_missing);

    /* the components, by what they make */
    m_tree->setObjectName("components");
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({tr("Component"), tr("Status"), tr("Built")});
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setAllColumnsShowFocus(true);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setItemDelegateForColumn(StatusColumn, new StatusDelegate(m_tree));
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(NameColumn, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(StatusColumn, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(BuiltColumn, QHeaderView::ResizeToContents);
    split->addWidget(m_tree);

    /* the selected one: where it comes from, its patches, its log */
    m_source->setObjectName("source");
    m_source->setOpenExternalLinks(true);
    /* as high as its text, the room under it empty */
    auto *sourcePage = new QWidget;
    auto *sourceLayout = new QVBoxLayout(sourcePage);
    sourceLayout->addWidget(m_source);
    sourceLayout->addStretch(1);
    sourceScroll->setWidget(sourcePage);
    sourceScroll->setWidgetResizable(true);
    sourceScroll->setFrameShape(QFrame::NoFrame);
    m_tabs->addTab(sourceScroll, tr("S&ource"));

    m_patches->setObjectName("patches");
    m_patches->setColumnCount(2);
    m_patches->setHeaderLabels({tr("Patch"), tr("File")});
    m_patches->setUniformRowHeights(true);
    m_patches->setAllColumnsShowFocus(true);
    m_patches->header()->setStretchLastSection(false);
    m_patches->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_patches->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_patchText->setObjectName("patchText");
    m_patchText->setReadOnly(true);
    m_patchText->setFont(fixed);
    m_patchText->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_patchText->setPlaceholderText(tr("Select a patch to read it."));
    patchSplit->addWidget(m_patches);
    patchSplit->addWidget(m_patchText);
    patchSplit->setStretchFactor(0, 1);
    patchSplit->setStretchFactor(1, 1);
    m_tabs->addTab(patchSplit, tr("&Patches"));

    m_log->setObjectName("log");
    m_log->setReadOnly(true);
    m_log->setFont(fixed);
    m_log->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_log->setMaximumBlockCount(kLogLines);
    m_tabs->addTab(m_log, tr("&Log"));
    m_tabs->setObjectName("details");
    split->addWidget(m_tabs);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 3);
    layout->addWidget(split, 1);

    m_background->setText(tr("Closing this window does not stop the build."));
    m_background->hide();
    layout->addWidget(m_background);

    /* the cache on the left, what to build on the right */
    m_cache->setObjectName("cacheSize");
    m_clear->setObjectName("clearCache");
    m_build->setObjectName("build");
    m_all->setObjectName("buildAll");
    m_stop->setObjectName("stop");
    m_install->setObjectName("install");
    m_revert->setObjectName("revert");
    Widgets::setButtonIcon(m_clear, Icons::themed({"edit-clear-history", "edit-clear-all",
                                                   "edit-clear"},
                                                  QStyle::SP_DialogResetButton));
    Widgets::setButtonIcon(m_build, Icons::themed({"run-build", "run-build-install"},
                                                  QStyle::SP_BrowserReload));
    Widgets::setButtonIcon(m_all, Icons::themed({"update-high", "system-software-update"},
                                                QStyle::SP_BrowserReload));
    Widgets::setButtonIcon(m_stop, Icons::themed({"process-stop"}, QStyle::SP_BrowserStop));
    Widgets::setButtonIcon(m_install, Icons::themed({"run-install", "system-software-install"},
                                                    QStyle::SP_DialogApplyButton));
    Widgets::setButtonIcon(m_revert, Icons::themed({"edit-undo"}, QStyle::SP_ArrowBack));
    buttons->addButton(m_build, QDialogButtonBox::ActionRole);
    /* a host package's, beside its Build */
    buttons->addButton(m_install, QDialogButtonBox::ActionRole);
    buttons->addButton(m_revert, QDialogButtonBox::ActionRole);
    buttons->addButton(m_all, QDialogButtonBox::ActionRole);
    buttons->addButton(m_stop, QDialogButtonBox::ActionRole);
    bottom->addWidget(m_cache);
    bottom->addWidget(m_clear);
    bottom->addStretch();
    bottom->addWidget(buttons);
    layout->addLayout(bottom);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(m_build, &QPushButton::clicked, this, [this]() { buildSelected(); });
    connect(m_all, &QPushButton::clicked, m_manager, &BuildManager::buildOutdated);
    connect(m_stop, &QPushButton::clicked, m_manager, &BuildManager::cancel);
    connect(m_install, &QPushButton::clicked, this, [this]() {
        /* the group: every package built and ready, at once (one password) */
        if (selected() == "system") {
            HostKdeDialog::installAll(this, installableHostPackages());
        } else {
            HostKdeDialog::install(this, selected());
        }
    });
    connect(m_revert, &QPushButton::clicked, this,
            [this]() { HostKdeDialog::revert(this, {selected()}); });
    connect(m_clear, &QPushButton::clicked, this, &BuildsWindow::clearCache);
    /* another release's guest tools: shown instead (refused during a run, when their rows are
       off: back to what was shown); not from within the tree's own signal */
    connect(m_tree, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *current, QTreeWidgetItem *previous) {
        const QString release =
            current ? releaseOf(current->data(NameColumn, IdRole).toString()) : QString();
        if (release.isEmpty()) {
            return;
        }
        const QString back = previous ? previous->data(NameColumn, IdRole).toString() : QString();
        QTimer::singleShot(0, this, [this, release, back]() {
            if (m_manager->setGuestRelease(release)) {
                select("guest");
            } else {
                select(back);
            }
        });
    });
    connect(m_manager, &BuildManager::guestReleaseChanged, this, [this]() {
        fill();
        showDetails();
        updateButtons();
        updateHeader();
    });
    connect(m_memory->button(), &QPushButton::clicked, m_manager, &BuildManager::proceed);
    connect(m_tree, &QTreeWidget::currentItemChanged, this, &BuildsWindow::showDetails);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &BuildsWindow::contextMenu);
    connect(m_patches, &QTreeWidget::currentItemChanged, this, &BuildsWindow::showPatchText);
    connect(m_tabs, &QTabWidget::currentChanged, this, &BuildsWindow::showDetails);

    connect(m_manager, &BuildManager::progressChanged, this, [this](const QString &id) {
        refreshItem(id);
        updateHeader();
        /* the details of the one building: when its state changes, else now and then */
        if (id == selected() && (!m_sourceShown.isValid() ||
                                 m_sourceShown.elapsed() >= kSourceEveryMs)) {
            showSource();
        }
    });
    connect(m_manager, &BuildManager::statusChanged, this, [this]() {
        refreshItems();
        showDetails();
        updateButtons();
        updateHeader();
    });
    connect(m_manager, &BuildManager::runStarted, this, [this]() {
        m_showInstall = false;
        m_shownLog.clear();
        refreshItems();
        showDetails();
        updateButtons();
        updateHeader();
        showMissing();
    });
    connect(m_manager, &BuildManager::runFinished, this, [this]() {
        refreshItems();
        showDetails();
        updateButtons();
        updateHeader();
        showMissing();
        showMemory();
        updateCache();
    });
    connect(m_manager, &BuildManager::output, this, [this](const QString &id, const QString &text) {
        if (id == m_shownLog) {
            m_log->moveCursor(QTextCursor::End);
            m_log->insertPlainText(text);
            m_log->moveCursor(QTextCursor::End);
        }
    });
    connect(GuestToolsBuilder::instance(), &GuestToolsBuilder::waitingChanged, this,
            &BuildsWindow::showMemory);
    connect(HostKdeBuilder::instance(), &HostKdeBuilder::waitingChanged, this,
            &BuildsWindow::showMemory);

    /* a host package installed or reverted: the output in its Log tab */
    HostKdeMonitor *monitor = HostKdeMonitor::instance();
    connect(monitor, &HostKdeMonitor::started, this, [this, monitor]() {
        m_showInstall = true;
        m_installIds = monitor->busyWith();
        m_shownLog.clear();
        if (selected() == "system" || m_installIds.contains(selected())) {
            if (!m_installIds.contains(selected()) && !m_installIds.isEmpty()) {
                select(m_installIds.first());
            }
            m_tabs->setCurrentIndex(2);
        }
        showLog();
        updateButtons();
    });
    connect(monitor, &HostKdeMonitor::output, this, [this](const QString &text) {
        if (m_showInstall && m_installIds.contains(m_shownLog)) {
            m_log->moveCursor(QTextCursor::End);
            m_log->insertPlainText(text);
            m_log->moveCursor(QTextCursor::End);
        }
    });
    connect(monitor, &HostKdeMonitor::finished, this,
            [this](const QString &error, bool cancelled) {
        updateButtons();
        if (!error.isEmpty() && !cancelled) {
            Widgets::warn(this, tr("Host Desktop"), error);
        }
    });

    /* the run's task, in the main window's list: seen here once it is over */
    TaskButton::followSeen(this, []() { return BuildManager::instance()->task(); });
    /* vitrine back in front through another of its windows, this one in sight:
       what was built meanwhile, from a terminal too (this one's own
       activation: changeEvent()) */
    connect(qApp, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive && isVisible() && !isActiveWindow()) {
            lookAgain();
        }
    });

    fill();
    showMissing();
    showMemory();
    updateButtons();
    updateHeader();
    const QSettings settings(Paths::settingsPath(), QSettings::IniFormat);
    if (!restoreGeometry(settings.value("builds/geometry").toByteArray())) {
        resize(fontMetrics().averageCharWidth() * 120, fontMetrics().height() * 42);
    }
}

BuildsWindow::~BuildsWindow() = default;

void BuildsWindow::shown()
{
    /* the sources may have changed meanwhile, and the system (Plasma, the frameworks, Qt) */
    if (!m_manager->isRunning()) {
        m_manager->refresh();
    }
    HostKdeMonitor::instance()->refresh();
    updateCache();
}

void BuildsWindow::hideEvent(QHideEvent *event)
{
    QSettings settings(Paths::settingsPath(), QSettings::IniFormat);
    settings.setValue("builds/geometry", saveGeometry());
    QDialog::hideEvent(event);
}

void BuildsWindow::changeEvent(QEvent *event)
{
    QDialog::changeEvent(event);
    /* back in front: what was built meanwhile, from a terminal too */
    if (event->type() == QEvent::ActivationChange && isActiveWindow()) {
        lookAgain();
    }
}

void BuildsWindow::lookAgain()
{
    /* a run says itself what it builds; the host packages: one reading at
       a time, the main window's banner asks too */
    if (!m_manager->isRunning()) {
        m_manager->refresh();
        HostKdeMonitor::instance()->lookAgain();
    }
}

void BuildsWindow::fill()
{
    const QString was = selected();
    const QList<Component> components = m_manager->components();

    m_tree->clear();
    m_items.clear();
    m_groups.clear();
    m_releases.clear();
    for (const Component &c : components) {
        const QString group = groupOf(c.kind);
        QTreeWidgetItem *parent = m_groups.value(group);
        if (!parent) {
            parent = new QTreeWidgetItem(m_tree);
            parent->setText(NameColumn, groupTitle(group, m_manager->guestRelease()));
            parent->setData(NameColumn, IdRole, group);
            QFont bold = parent->font(NameColumn);
            bold.setBold(true);
            parent->setFont(NameColumn, bold);
            m_groups.insert(group, parent);
        }
        auto *item = new QTreeWidgetItem(parent);
        item->setData(NameColumn, IdRole, c.id);
        m_items.insert(c.id, item);
    }
    /* the guest tools of the other releases around this one's, the newest first */
    if (QTreeWidgetItem *guest = m_groups.value("guest")) {
        const QStringList releases = GuestTools::releases();
        const qsizetype shown = releases.indexOf(m_manager->guestRelease());
        int older = 0;
        for (const QString &r : m_manager->otherGuestReleases()) {
            auto *item = new QTreeWidgetItem;
            const int at = m_tree->indexOfTopLevelItem(guest);
            item->setText(NameColumn, groupTitle("guest", r));
            item->setData(NameColumn, IdRole, releaseId(r));
            QFont bold = item->font(NameColumn);
            bold.setBold(true);
            item->setFont(NameColumn, bold);
            m_tree->insertTopLevelItem(releases.indexOf(r) > shown ? at : at + 1 + older++, item);
            m_releases.insert(r, item);
        }
    }
    m_tree->expandAll();
    refreshItems();
    fitTree();
    if (!was.isEmpty()) {
        select(was);
    }
    if (!m_tree->currentItem() && !m_items.isEmpty()) {
        select(components.first().id);
    }
}

void BuildsWindow::refreshItems()
{
    /* a component more or less (the sources changed): anew */
    QStringList ids;
    for (const Component &c : m_manager->components()) {
        ids << c.id;
    }
    QStringList shown = m_items.keys();
    std::sort(shown.begin(), shown.end());
    QStringList sorted = ids;
    std::sort(sorted.begin(), sorted.end());
    QStringList releases = m_releases.keys();
    std::sort(releases.begin(), releases.end());
    QStringList others = m_manager->otherGuestReleases();
    std::sort(others.begin(), others.end());
    if (sorted != shown || releases != others) {
        fill();
        return;
    }
    refreshReleases();
    for (const QString &id : std::as_const(ids)) {
        const Component c = m_manager->component(id);
        const Builds::Status status = m_manager->status(id);
        /* "QEMU 11.1.50", "Mesa 26.2.3": what is built, else what its sources make */
        QString version = status.version;
        if (c.kind == Kind::Package && version.isEmpty()) {
            version = Builds::source(c).revision.section('-', 0, 0);
        }
        /* the Mesa for Flatpak apps: one for each runtime, in its Source */
        m_items.value(id)->setText(NameColumn, c.kind == Kind::Medium || c.kind == Kind::Flatpak ||
                                                       version.isEmpty()
                                                   ? c.title
                                                   : c.title + ' ' + version);
        refreshItem(id);
    }
}

void BuildsWindow::refreshItem(const QString &id)
{
    QTreeWidgetItem *item = m_items.value(id);
    const Component c = m_manager->component(id);
    const Builds::Status status = m_manager->status(id);
    const BuildManager::Progress p = m_manager->progress(id);
    const bool running = m_manager->isRunning();
    QString text;

    if (!item || !c.isValid()) {
        return;
    }

    /* how the run goes for it, else what is built */
    switch (p.state) {
    case State::Queued:
        text = tr("Queued");
        break;
    case State::Waiting:
        text = tr("Waiting for memory");
        break;
    case State::Building:
        text = p.phase.isEmpty() ? tr("Starting") : capitalized(p.phase);
        if (p.total > 0) {
            text = tr("%1, %2%").arg(text).arg(int(qint64(p.done) * 100 / p.total));
        }
        break;
    case State::Failed:
        text = tr("Failed: %1").arg(p.error);
        break;
    case State::Stopped:
        text = tr("Stopped");
        break;
    case State::Skipped:
        text = tr("Not built: a build before it failed");
        break;
    case State::Built:
    case State::UpToDate:
    case State::Restored:
        if (running) {
            text = p.state == State::Built      ? tr("Built")
                   : p.state == State::Restored ? tr("Its build before, brought back")
                                                : tr("Up to date");
        }
        break;
    case State::Idle:
        break;
    }
    if (text.isEmpty() && c.kind == Kind::System) {
        text = m_manager->systemStatus(id).summary();
    }
    if (text.isEmpty()) {
        switch (status.state) {
        case StatusState::NoSources:
            text = tr("No sources to build it from");
            break;
        case StatusState::NotBuilt:
            text = tr("Not built");
            break;
        case StatusState::Outdated: {
            const QString why = Builds::summary(status.changes);
            text = why.isEmpty() ? tr("Out of date") : tr("Out of date: %1").arg(why);
            break;
        }
        case StatusState::UpToDate:
            text = tr("Up to date");
            break;
        }
    }
    item->setText(StatusColumn, text);
    item->setToolTip(StatusColumn, text);
    item->setIcon(NameColumn, stateIcon(p.state, status.state));
    if (c.kind == Kind::System && p.state == State::Idle) {
        const QIcon icon = systemIcon(m_manager->systemStatus(id));
        if (!icon.isNull()) {
            item->setIcon(NameColumn, icon);
        }
    }
    item->setData(StatusColumn, ProgressRole,
                  p.state == State::Building && p.total > 0
                      ? QVariant(double(p.done) / p.total)
                      : QVariant());

    item->setText(BuiltColumn, status.built.isValid()
                                   ? QLocale().toString(status.built.toLocalTime().date(),
                                                        QLocale::ShortFormat)
                                   : QString());
    item->setToolTip(BuiltColumn, status.built.isValid()
                                      ? QLocale().toString(status.built.toLocalTime(),
                                                           QLocale::LongFormat)
                                      : QString());
    refreshGroup(groupOf(c.kind));
}

void BuildsWindow::refreshReleases()
{
    /* the release shown changes only between runs */
    const bool running = m_manager->isRunning();

    for (auto it = m_releases.cbegin(); it != m_releases.cend(); ++it) {
        QTreeWidgetItem *item = it.value();
        const QString release = it.key();
        const GuestToolsBuilder::State state = m_manager->guestToolsState(release);
        const QDateTime built = m_manager->guestToolsBuilt(release);
        using Tools = GuestToolsBuilder::State;
        const StatusState look = state == Tools::UpToDate   ? StatusState::UpToDate
                                 : state == Tools::Outdated ? StatusState::Outdated
                                 : state == Tools::NotBuilt ? StatusState::NotBuilt
                                                            : StatusState::NoSources;

        item->setText(StatusColumn, BuildTexts::releaseState(state));
        item->setIcon(NameColumn, stateIcon(State::Idle, look));
        item->setText(BuiltColumn, built.isValid()
                                       ? QLocale().toString(built.toLocalTime().date(),
                                                            QLocale::ShortFormat)
                                       : QString());
        item->setToolTip(BuiltColumn, built.isValid() ? QLocale().toString(built.toLocalTime(),
                                                                           QLocale::LongFormat)
                                                      : QString());
        item->setToolTip(NameColumn, running ? tr("Shown once the build is over")
                                             : BuildTexts::otherReleaseAbout(
                                                   release, m_manager->guestPackages(release)));
        item->setDisabled(running);
    }
}

void BuildsWindow::refreshGroup(const QString &group)
{
    QTreeWidgetItem *item = m_groups.value(group);
    int outdated = 0;
    int building = 0;
    int failed = 0;
    bool all = true;

    if (!item) {
        return;
    }
    for (int i = 0; i < item->childCount(); i++) {
        const QString id = item->child(i)->data(NameColumn, IdRole).toString();
        const StatusState state = m_manager->status(id).state;
        const BuildManager::Progress p = m_manager->progress(id);
        outdated += state != StatusState::UpToDate;
        all = all && state == StatusState::NotBuilt;
        building += p.state == State::Building || p.state == State::Waiting ||
                    p.state == State::Queued;
        failed += p.state == State::Failed;
    }
    item->setText(StatusColumn, building > 0 ? tr("Building")
                                : failed > 0 ? tr("Failed")
                                : outdated == 0 ? tr("Up to date")
                                : all ? tr("Not built")
                                      : tr("Out of date"));
    /* optional, and about the system rather than builds: what is installed */
    if (group == "system" && building == 0 && failed == 0) {
        QStringList installed;
        bool behind = false;
        for (int i = 0; i < item->childCount(); i++) {
            const QString id = item->child(i)->data(NameColumn, IdRole).toString();
            const HostKde::Status s = m_manager->systemStatus(id);
            if (s.owner == HostKde::Status::Owner::Vitrine) {
                installed << s.target.title;
            }
            behind = behind || s.isOutdated();
        }
        item->setText(StatusColumn, behind               ? tr("Out of date")
                                    : !installed.isEmpty() ? tr("Installed: %1").arg(installed.join(", "))
                                                           : tr("Optional"));
    }
}

void BuildsWindow::fitTree()
{
    /* all the rows at once, the details below taking the rest */
    int rows = 0;
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        rows++;
    }
    const int row = m_tree->sizeHintForRow(0) > 0 ? m_tree->sizeHintForRow(0)
                                                  : fontMetrics().height() + 8;
    const int height = m_tree->header()->sizeHint().height() + row * (rows + 1) +
                       2 * m_tree->frameWidth();
    const QList<int> sizes = m_split->sizes();
    const int total = sizes.value(0) + sizes.value(1);
    if (total > height) {
        m_split->setSizes({height, total - height});
    }
}

void BuildsWindow::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    fitTree();
    shown();
}

QString BuildsWindow::selected() const
{
    const QTreeWidgetItem *item = m_tree->currentItem();
    return item ? item->data(NameColumn, IdRole).toString() : QString();
}

void BuildsWindow::select(const QString &component)
{
    QTreeWidgetItem *item = m_items.value(component, m_groups.value(component));

    if (item) {
        m_tree->setCurrentItem(item);
        m_tree->scrollToItem(item);
    }
}

bool BuildsWindow::systemSelected() const
{
    const QString id = selected();
    return m_items.contains(id) && m_manager->component(id).kind == Kind::System;
}

QStringList BuildsWindow::installableHostPackages() const
{
    QStringList ids;

    if (const QTreeWidgetItem *group = m_groups.value("system")) {
        for (int i = 0; i < group->childCount(); i++) {
            const QString child = group->child(i)->data(NameColumn, IdRole).toString();
            if (m_manager->systemStatus(child).canInstall()) {
                ids << child;
            }
        }
    }
    return ids;
}

QStringList BuildsWindow::selectedComponents() const
{
    const QString id = selected();
    QStringList ids;

    if (const QTreeWidgetItem *group = m_groups.value(id)) {
        for (int i = 0; i < group->childCount(); i++) {
            const QString child = group->child(i)->data(NameColumn, IdRole).toString();
            /* of the host packages, those the research has a set for: building one
               installs nothing (Install… does, with the administrator's password),
               and the group's Build is how they are all built at once */
            if (id != "system" || m_manager->systemStatus(child).canBuild()) {
                ids << child;
            }
        }
        return ids;
    }
    return id.isEmpty() || !releaseOf(id).isEmpty() ? QStringList() : QStringList{id};
}

void BuildsWindow::showDetails()
{
    const QString id = selected();
    const bool component = m_items.contains(id);

    m_tabs->setTabEnabled(1, component && !Builds::patches(m_manager->component(id)).isEmpty());
    m_tabs->setTabEnabled(2, component);
    if (!m_tabs->isTabEnabled(m_tabs->currentIndex())) {
        m_tabs->setCurrentIndex(0);
    }
    showSource();
    showPatches();
    showLog();
    updateButtons();
}

void BuildsWindow::showSource()
{
    const QString id = selected();
    const Component c = m_manager->component(id);
    const Builds::Status status = m_manager->status(id);
    const BuildManager::Progress p = m_manager->progress(id);
    const auto row = [](const QString &label, const QString &value) {
        return "<tr><td style=\"padding-right: 1em\">" + label.toHtmlEscaped() +
               "</td><td>" + value + "</td></tr>";
    };
    QString html;
    QString rows;

    m_sourceShown.start();
    if (QTreeWidgetItem *group = m_groups.value(id)) {
        /* a group: what it makes, and of what */
        QStringList parts;
        for (int i = 0; i < group->childCount(); i++) {
            parts << group->child(i)->text(NameColumn).toHtmlEscaped() + ": " +
                         group->child(i)->text(StatusColumn).toHtmlEscaped();
        }
        html = BuildTexts::groupAbout(id, m_manager->guestRelease(),
                                      id == "guest"
                                          ? m_manager->guestPackages(m_manager->guestRelease())
                                          : QStringList());
        html += "<ul><li>" + parts.join("</li><li>") + "</li></ul>";
        m_source->setText(html);
        return;
    }
    if (!c.isValid()) {
        m_source->setText(m_items.isEmpty()
                              ? tr("This installation of Vitrine has nothing to build: no host/ "
                                   "or guest/ folder.")
                              : QString());
        return;
    }
    if (c.kind == Kind::System) {
        m_source->setText(systemSource(c));
        return;
    }

    /* what it is, and how it stands */
    html = "<p><b>" + c.title.toHtmlEscaped() + "</b>";
    switch (status.state) {
    case StatusState::UpToDate:
        html += " — " + tr("up to date: built from these sources.").toHtmlEscaped();
        break;
    case StatusState::NotBuilt:
        html += " — " + tr("not built yet.").toHtmlEscaped();
        break;
    case StatusState::NoSources:
        html += " — " + tr("no sources to build it from.").toHtmlEscaped();
        break;
    case StatusState::Outdated:
        html += " — " + tr("out of date. Changed since its build:").toHtmlEscaped();
        break;
    }
    html += "</p>";
    if (status.state == StatusState::Outdated && !status.changes.isEmpty()) {
        QStringList items;
        for (const Builds::Change &change : status.changes) {
            items << Builds::describe(change).toHtmlEscaped();
        }
        html += "<ul><li>" + items.join("</li><li>") + "</li></ul>";
    }
    if (p.state == State::Failed) {
        html += "<p>" + tr("Its last build failed: %1").arg(p.error).toHtmlEscaped() + "</p>";
    } else if (p.state == State::Building) {
        html += "<p>" + tr("Building: %1.").arg(p.phase.isEmpty() ? tr("starting") : p.phase)
                            .toHtmlEscaped() + "</p>";
    }

    /* where it comes from */
    const Builds::Source source = Builds::source(c);
    switch (c.kind) {
    case Kind::Host:
        rows += row(tr("Source:"),
                    QString("<a href=\"%1\">%2</a>, %3")
                        .arg(webPage(source.url).toHtmlEscaped(), shortUrl(source.url).toHtmlEscaped(),
                             tr("commit %1").arg("<code>" + source.revision.left(12).toHtmlEscaped() +
                                                 "</code>")));
        break;
    case Kind::Package:
        rows += row(tr("Source:"),
                    c.id == "tools"
                        ? tr("its folder %1 in Vitrine's guest/, version %2")
                              .arg("<code>" + c.values.value("spec").section('/', 0, 0).toHtmlEscaped() +
                                       "/</code>",
                                   source.revision.toHtmlEscaped())
                        : tr("%1 from %2, built with Fedora's spec %3, version %4")
                              .arg("<code>" + source.archive.toHtmlEscaped() + "</code>",
                                   source.archiveHost.toHtmlEscaped(),
                                   "<code>" + c.values.value("spec").section('/', -1).toHtmlEscaped() +
                                       "</code>",
                                   source.revision.toHtmlEscaped()));
        if (!source.url.isEmpty()) {
            rows += row(tr("Project:"), QString("<a href=\"%1\">%2</a>")
                                            .arg(source.url.toHtmlEscaped(),
                                                 shortUrl(source.url).toHtmlEscaped()));
        }
        break;
    case Kind::Flatpak: {
        QStringList parts;
        for (const Builds::FlatpakBranch &b : Builds::flatpakBranches(c)) {
            parts << tr("Mesa %1 for the runtimes %2 (commit %3)")
                         .arg(b.mesa.toHtmlEscaped(), b.branch.toHtmlEscaped(),
                              "<code>" + b.commit.left(12).toHtmlEscaped() + "</code>");
        }
        rows += row(tr("Source:"),
                    tr("<a href=\"%1\">%2</a>'s recipe, the one of Flathub's: %3")
                        .arg(source.url.toHtmlEscaped(), shortUrl(source.url).toHtmlEscaped(),
                             parts.join(", ")));
        break;
    }
    case Kind::Medium: {
        QStringList parts;
        QStringList others;
        for (const QString &dep : c.deps) {
            const Component d = m_manager->component(dep);
            if (d.isValid()) {
                (d.kind == Kind::Flatpak ? others : parts) << d.title.toHtmlEscaped();
            }
        }
        rows += row(tr("Made of:"),
                    others.isEmpty()
                        ? tr("the packages %1, and the installer").arg(parts.join(", "))
                        : tr("the packages %1; %2 and the installer")
                              .arg(parts.join(", "), others.join(", ")));
        break;
    }
    case Kind::System:
        /* systemSource() */
        break;
    }
    /* its patches, in a word: their number by origin */
    const QList<Builds::Patch> patches = Builds::patches(c);
    if (!patches.isEmpty()) {
        QStringList origins;
        QHash<QString, int> count;
        for (const Builds::Patch &patch : patches) {
            /* not built: the Patches tab shows them apart */
            if (patch.left) {
                continue;
            }
            if (!count.contains(patch.origin)) {
                origins << patch.origin;
            }
            count[patch.origin]++;
        }
        QStringList parts;
        for (const QString &origin : std::as_const(origins)) {
            parts << (origin.isEmpty() ? QString::number(count.value(origin))
                                       : tr("%1 from %2").arg(count.value(origin)).arg(origin))
                         .toHtmlEscaped();
        }
        rows += row(tr("Patches:"), parts.join(", "));
    }
    /* a library of QEMU's, or a package's (KWin's KWayland) */
    if ((c.kind == Kind::Host || c.kind == Kind::Package) && !c.deps.isEmpty()) {
        QStringList titles;
        for (const QString &dep : c.deps) {
            titles << m_manager->component(dep).title.toHtmlEscaped();
        }
        rows += row(tr("Built against:"), titles.join(", "));
    }
    /* QEMU and passt, which it starts for a VM's network */
    if (c.kind == Kind::Host && !c.runs.isEmpty()) {
        QStringList titles;
        for (const QString &dep : c.runs) {
            titles << m_manager->component(dep).title.toHtmlEscaped();
        }
        rows += row(tr("Runs with:"), titles.join(", "));
    }
    if (status.built.isValid()) {
        const QDateTime when = status.built.toLocalTime();
        QString built = QLocale().toString(when.date(), "d MMMM yyyy") + ", " +
                        QLocale().toString(when.time(), QLocale::ShortFormat);
        const QList<qint64> took = m_manager->durations(id);
        if (!took.isEmpty()) {
            built = tr("%1, in %2").arg(built, duration(took.last()));
        }
        rows += row(tr("Built:"), built.toHtmlEscaped());
    }
    if (!status.output.isEmpty()) {
        const qint64 used = size(status.output);
        rows += row(tr("Output:"), "<code>" + status.output.toHtmlEscaped() + "</code>" +
                                       (used >= 0 ? " — " + bytes(used).toHtmlEscaped() : QString()));
    }
    if (c.id == "qemu" && !Paths::customQemuBinary().isEmpty()) {
        rows += row(tr("In use:"), tr("no: the preferences choose another QEMU, %1")
                                       .arg(Paths::customQemuBinary())
                                       .toHtmlEscaped());
    }
    m_source->setText(html + "<table>" + rows + "</table>");
}

void BuildsWindow::showPatches()
{
    const QString id = selected();
    const Component c = m_manager->component(id);
    QTreeWidgetItem *group = nullptr;
    QString origin;
    int number = 0;

    if (id == m_shownPatches) {
        return;
    }
    m_shownPatches = id;
    m_patches->clear();
    m_patchText->clear();
    const QList<Builds::Patch> list = Builds::patches(c);
    m_tabs->setTabText(1, list.isEmpty() ? tr("&Patches") : tr("&Patches (%1)").arg(list.size()));
    for (const Builds::Patch &patch : list) {
        /* their origins, in the order they apply */
        if (!group || patch.origin != origin) {
            origin = patch.origin;
            /* the host's KWin's are sets, one of them for this computer */
            if (c.kind == Kind::System) {
                number = 0;
            }
            group = new QTreeWidgetItem(m_patches);
            QFont bold = group->font(0);
            bold.setBold(true);
            group->setFont(0, bold);
        }
        auto *item = new QTreeWidgetItem(group);
        item->setText(0, tr("%1. %2").arg(++number).arg(Builds::subject(c, patch)));
        item->setText(1, patch.name());
        item->setToolTip(0, item->text(0));
        item->setToolTip(1, patch.path);
        item->setData(0, Qt::UserRole, patch.path);
        const int count = group->childCount();
        const QString patches = count == 1 ? tr("1 patch") : tr("%1 patches").arg(count);
        group->setText(0, origin.isEmpty() ? patches : origin + " · " + patches);
    }
    m_patches->expandAll();
}

void BuildsWindow::showPatchText()
{
    const QTreeWidgetItem *item = m_patches->currentItem();
    const Component c = m_manager->component(m_shownPatches);
    const QString path = item ? item->data(0, Qt::UserRole).toString() : QString();

    if (path.isEmpty()) {
        m_patchText->clear();
        return;
    }
    for (const Builds::Patch &patch : Builds::patches(c)) {
        if (patch.path == path) {
            m_patchText->setPlainText(Builds::text(c, patch));
            return;
        }
    }
}

void BuildsWindow::showLog()
{
    const QString id = m_items.contains(selected()) ? selected() : QString();

    if (id == m_shownLog && !id.isEmpty()) {
        return;
    }
    m_shownLog = id;
    QString text = id.isEmpty() ? QString() : m_manager->log(id);
    if (m_showInstall && m_installIds.contains(id)) {
        text = HostKdeMonitor::instance()->log();
    }
    /* the end of it: the lines the window keeps */
    qsizetype at = text.size();
    for (int i = 0; i <= kLogLines && at > 0; i++) {
        at = text.lastIndexOf('\n', at - 1);
        if (at < 0) {
            break;
        }
    }
    if (at > 0) {
        text = text.mid(at + 1);
    }
    m_log->setPlainText(text);
    m_log->setPlaceholderText(id.isEmpty() ? QString() : tr("No build of it yet."));
    m_log->moveCursor(QTextCursor::End);
}

void BuildsWindow::showMissing()
{
    const StackBuilder *stack = StackBuilder::instance();
    QStringList guest;
    QString text;

    if (m_manager->isRunning()) {
        m_missing->hide();
        return;
    }
    if (!stack->missing().isEmpty()) {
        text = tr("Missing for the build of Vitrine's QEMU: %1.")
                   .arg(stack->missing().join(", ").toHtmlEscaped());
        if (!stack->installCommand().isEmpty()) {
            text += "<br>" + tr("Install them with: <code>%1</code>")
                                 .arg(stack->installCommand().toHtmlEscaped());
        }
    }
    /* what this host lacks now for the guest tools' */
    const QList<GuestToolsBuilder::Requirement> needed = GuestToolsBuilder::missing();
    for (const GuestToolsBuilder::Requirement &r : needed) {
        guest << (r.command == "podman" ? tr("podman, which runs the build in a Fedora container")
                                        : r.command.toHtmlEscaped());
    }
    if (!guest.isEmpty()) {
        if (!text.isEmpty()) {
            text += "<br>";
        }
        text += tr("Missing for the build of the guest tools: %1.").arg(guest.join(", "));
        text += "<br>" + tr("Install them with: <code>%1</code>")
                             .arg(GuestToolsBuilder::installCommand(needed).toHtmlEscaped());
    }
    m_missing->setText(text);
    m_missing->setVisible(!text.isEmpty());
}

void BuildsWindow::showMemory()
{
    const auto gib = [](qint64 mib) {
        return QLocale().toString(double(mib) / 1024, 'f', mib % 1024 == 0 ? 0 : 1);
    };

    if (!m_manager->isWaiting()) {
        m_memory->hide();
        return;
    }
    m_memory->setText(tr("<b>Waiting for memory.</b> Building %1 takes up to %2 GiB, and %3 GiB "
                         "is free: close VMs or other programs. The build goes on by itself once "
                         "%4 GiB is free, what the host keeps free included.")
                          .arg(m_manager->component(m_manager->current()).title.toHtmlEscaped(),
                               gib(GuestToolsBuilder::memoryCapMiB()),
                               gib(m_manager->availableMiB()), gib(m_manager->neededMiB())));
    m_memory->show();
}

void BuildsWindow::updateButtons()
{
    const bool running = m_manager->isRunning();
    const QString id = selected();
    const QStringList ids = selectedComponents();
    QStringList todo;
    bool built = false;

    for (const QString &c : ids) {
        const StatusState state = m_manager->status(c).state;
        if (state == StatusState::NotBuilt || state == StatusState::Outdated) {
            todo << c;
        }
        built = built || state == StatusState::Outdated || state == StatusState::UpToDate;
    }
    /* "Update QEMU", "Build Guest Tools" */
    const QString title = m_groups.contains(id) ? (id == "host"     ? tr("Vitrine's QEMU")
                                                   : id == "system" ? tr("Host Desktop")
                                                                    : tr("Guest Tools"))
                                                : m_manager->component(id).title;
    m_build->setText(title.isEmpty()            ? tr("&Build")
                     : built && !todo.isEmpty() ? tr("&Update %1").arg(title)
                                                : tr("&Build %1").arg(title));
    m_build->setEnabled(!running && !todo.isEmpty());
    const QStringList plan = todo.isEmpty() ? QStringList() : m_manager->plan(todo);
    QStringList titles;
    for (const QString &c : plan) {
        titles << m_manager->component(c).title;
    }
    m_build->setToolTip(plan.isEmpty() ? QString() : tr("Builds %1").arg(titles.join(", ")));

    const QStringList outdated = m_manager->outdated();
    bool anyBuilt = false;
    titles.clear();
    for (const QString &c : outdated) {
        titles << m_manager->component(c).title;
    }
    for (const Component &c : m_manager->components()) {
        anyBuilt = anyBuilt || m_manager->status(c.id).state == StatusState::UpToDate ||
                   m_manager->status(c.id).state == StatusState::Outdated;
    }
    m_all->setText(anyBuilt ? tr("Update &All") : tr("Build &All"));
    m_all->setEnabled(!running && !outdated.isEmpty());
    m_all->setToolTip(outdated.isEmpty() ? tr("All is up to date")
                                         : tr("Builds %1").arg(titles.join(", ")));
    m_stop->setVisible(running);
    m_background->setVisible(running);
    m_clear->setEnabled(!running);

    /* a host package: installed by hand, after warnings; the Host desktop group: all those
       ready at once, one password (Install All…) */
    const bool host = systemSelected();
    const bool hostGroup = id == "system" && m_groups.contains(id);
    m_install->setVisible(host || hostGroup);
    m_install->setText(hostGroup ? tr("&Install All…") : tr("&Install…"));
    m_revert->setVisible(host);
    if (hostGroup) {
        const QStringList ready = installableHostPackages();
        QStringList readyTitles;
        for (const QString &c : ready) {
            readyTitles << m_manager->systemStatus(c).target.title;
        }
        const bool busy = HostKdeMonitor::instance()->isBusy() ||
                          (running && ready.contains(m_manager->current()));
        m_install->setEnabled(!busy && !ready.isEmpty());
        m_install->setToolTip(ready.isEmpty()
                                  ? tr("No build is waiting to be installed")
                                  : tr("Install %1 on this computer at once, with the "
                                       "administrator's password")
                                        .arg(HostKdeDialog::names(readyTitles)));
    }
    if (host) {
        const HostKde::Status status = m_manager->systemStatus(id);
        const bool busy = HostKdeMonitor::instance()->isBusy() || (running && m_manager->current() == id);
        QString why;
        m_install->setEnabled(!busy && status.canInstall(&why));
        m_install->setToolTip(!why.isEmpty() ? why
                                             : tr("Install it on this computer, with the "
                                                  "administrator's password"));
        why.clear();
        m_revert->setEnabled(!busy && status.canRevert(&why));
        m_revert->setToolTip(!why.isEmpty() ? why
                                             : tr("Put the distribution's %1 back").arg(status.target.title));
        if (!running && !status.canBuild(&why)) {
            m_build->setToolTip(why);
        }
    }
}

void BuildsWindow::updateHeader()
{
    QString text;

    if (m_manager->isRunning()) {
        const QString id = m_manager->current();
        const QStringList queue = m_manager->queue();
        const QString title = m_manager->component(id.isEmpty() ? queue.value(0) : id).title;
        text = queue.size() > 1 ? tr("Building %1 (%2 of %3)")
                                      .arg(title)
                                      .arg(qMax<qsizetype>(1, queue.indexOf(id) + 1))
                                      .arg(queue.size())
                                : tr("Building %1").arg(title);
        if (m_manager->isWaiting()) {
            text = tr("%1: waiting for memory.").arg(text);
        } else {
            text += '.';
        }
        m_header->setText(text.toHtmlEscaped());
        return;
    }
    const QStringList outdated = m_manager->outdated();
    if (m_manager->components().isEmpty()) {
        text = tr("This installation of Vitrine has nothing to build: no host/ or guest/ folder.");
    } else if (outdated.isEmpty()) {
        text = tr("All is built from this version's sources.");
    } else {
        text = outdated.size() == 1 ? tr("1 component to build.")
                                    : tr("%1 components to build.").arg(outdated.size());
    }
    if (!m_manager->error().isEmpty()) {
        text = tr("The last build failed: %1.").arg(m_manager->error()) + ' ' + text;
    }
    m_header->setText(text.toHtmlEscaped());
}

QString BuildsWindow::systemSource(const Component &c)
{
    using Offer = HostKde::Status::Offer;
    using Owner = HostKde::Status::Owner;
    const HostKde::Status host = m_manager->systemStatus(c.id);
    const HostKde::Target &target = host.target;
    const QString title = c.title;
    const HostKde::System system = HostKdeMonitor::instance()->system();
    const Builds::Status status = m_manager->status(c.id);
    const BuildManager::Progress p = m_manager->progress(c.id);
    const auto row = [](const QString &label, const QString &value) {
        return "<tr><td style=\"padding-right: 1em\">" + label.toHtmlEscaped() +
               "</td><td>" + value + "</td></tr>";
    };
    const auto para = [](const QString &text, bool bold = false) {
        return bold ? "<p><b>" + text.toHtmlEscaped() + "</b></p>"
                    : "<p>" + text.toHtmlEscaped() + "</p>";
    };
    QString html = "<p><b>" + title.toHtmlEscaped() + "</b> — " +
                   host.summary().toHtmlEscaped() + ".</p>";
    QString rows;
    QString why;

    html += para(BuildTexts::systemAbout(target) + ' ' +
                 tr("Optional: Vitrine builds it only when asked, and installs it only with the "
                    "administrator's password."));
    if (p.state == State::Failed) {
        html += para(tr("Its last build failed: %1").arg(p.error));
    } else if (p.state == State::Building) {
        html += para(tr("Building: %1.").arg(p.phase.isEmpty() ? tr("starting") : p.phase));
    }

    /* what stands in the way, or wants doing */
    if (host.offer == Offer::Unsupported) {
        html += para(host.why);
    } else if (host.offer == Offer::NoSet) {
        QStringList sets;
        for (const HostKde::Set &set : HostKde::sets(c.dir, target.package)) {
            QStringList versions;
            for (const QString &pattern : set.versions) {
                versions << HostKde::versionText(pattern);
            }
            sets << tr("%1 for %2 %3").arg(set.series, title, versions.join(", "));
        }
        html += para(tr("The research has no host set for %1 %2. Its sets: %3.")
                         .arg(title, host.version, sets.isEmpty() ? tr("none") : sets.join(", ")));
    }
    if (host.isOutdated()) {
        QStringList moved;
        for (const QString &line : host.outdated) {
            const QStringList f = line.split(' ');
            moved << tr("%1 %2, built for %3")
                         .arg(HostKde::packageTitle(f.value(0)), f.value(2), f.value(1));
        }
        html += para(target.package == "kwin"
                         ? tr("Vitrine's KWin no longer matches this computer (%1). At the next login "
                              "the desktop would run it with the new libraries, and might not start: "
                              "build and install the matching KWin, or revert, before you log out.")
                               .arg(moved.join(", "))
                         : tr("Vitrine's %1 no longer matches this computer (%2): programs use it with "
                              "the newer libraries, which upstream does not support. Build and install "
                              "the matching one, or revert.")
                               .arg(title, moved.join(", ")),
                     true);
    } else if (host.owner == Owner::Foreign) {
        html += para(host.installed.isLocal()
                         ? tr("The %1 installed, %2, is not Vitrine's: it was installed from a file. "
                              "Vitrine leaves it alone: Install… replaces it only after a warning, "
                              "and Revert… is not offered for it.")
                               .arg(title, host.installed.evr())
                         : tr("%1 is version-locked at %2, not by Vitrine. Install… replaces it and "
                              "its lock only after a warning.")
                               .arg(title, host.lockedAt.isEmpty() ? host.installed.evr()
                                                                   : host.lockedAt));
    }
    if (host.offer == Offer::Offered && host.build.isValid() && !host.canInstall(&why) &&
        !host.differs.isEmpty()) {
        html += para(why);
    }
    if (host.owner == Owner::Vitrine && !host.isOutdated() && host.build.isValid() &&
        host.build.rpm(target.package).digest != host.installed.digest && host.differs.isEmpty()) {
        html += para(tr("Another build than the one installed is ready (%1 %2, built %3): Install… "
                        "replaces it.")
                         .arg(title, host.build.evr,
                              QLocale().toString(host.build.built.toLocalTime().date(), "d MMMM yyyy")));
    }
    if (!host.heldBack.isEmpty()) {
        html += para((target.package == "qt6-qtbase"
                          ? tr("Updates wait: the repositories have Qt base %1, and its lock keeps "
                               "this computer at %2, with the rest of Qt (its modules need the same "
                               "version).")
                                .arg(host.heldBack, host.installed.evr())
                          : tr("An update waits: the repositories have %1 %2, and its lock keeps "
                               "this computer at %3.")
                                .arg(title, host.heldBack, host.installed.evr())) +
                     ' ' +
                     (host.offer == Offer::Offered
                          ? tr("Building it again takes the newer one.")
                          : tr("The research has no set for it yet: Revert… lets the update in.")));
    }
    if (host.nextLogin) {
        html += para(target.package == "kwin"
                         ? tr("The running desktop still runs the KWin it started with: the one "
                              "installed now runs from the next login.")
                         : tr("Programs started before it was installed or reverted keep what they "
                              "loaded; the desktop gets it at the next login."));
    }

    /* where it comes from, and what this computer has */
    const QString from = host.plasmaFrom == "beta"      ? tr("the KDE SIG's beta COPR")
                         : host.plasmaFrom == "testing" ? tr("Fedora's updates-testing")
                                                        : tr("Fedora's repositories");
    rows += row(tr("Source:"), (target.package == "kwin"
                                    ? tr("KWin's source RPM from %1, the version of this computer's "
                                         "Plasma")
                                          .arg(from)
                                    : tr("the source RPM of %1 from %2, its newest version there")
                                          .arg(target.package, from))
                                   .toHtmlEscaped());
    rows += row(tr("Recipe:"),
                tr("the research's %1 at commit %2, in a Fedora container")
                    .arg("<code>kde-rpms.sh</code>",
                         "<code>" + c.values.value("research").left(12).toHtmlEscaped() + "</code>"));
    if (!host.series.isEmpty()) {
        int count = 0;
        for (const Builds::Patch &patch : Builds::patches(c)) {
            count += !patch.left && patch.origin.startsWith(host.series + ',');
        }
        rows += row(tr("Set:"), (count == 1 ? tr("%1, 1 patch (the Patches tab)").arg(host.series)
                                            : tr("%1, %2 patches (the Patches tab)")
                                                  .arg(host.series)
                                                  .arg(count))
                                    .toHtmlEscaped());
    }
    QStringList computer;
    if (!system.fedora.isEmpty()) {
        computer << tr("Fedora %1").arg(system.fedora);
    }
    if (!host.plasma.isEmpty()) {
        computer << tr("Plasma %1").arg(host.plasma);
    }
    if (const HostKde::Package kf = system.package("kf6-kcoreaddons"); kf.isValid()) {
        computer << tr("KDE Frameworks %1").arg(kf.version);
    }
    if (const HostKde::Package qt = system.package("qt6-qtbase"); qt.isValid()) {
        computer << tr("Qt %1").arg(qt.version);
    }
    if (!computer.isEmpty()) {
        rows += row(tr("This computer:"), computer.join(", ").toHtmlEscaped());
    }
    if (host.installed.isValid()) {
        QString installed;
        switch (host.owner) {
        case Owner::Vitrine:
            installed = host.locked ? tr("Vitrine's %1 %2, version-locked").arg(title, host.installed.evr())
                                    : tr("Vitrine's %1 %2, not version-locked")
                                          .arg(title, host.installed.evr());
            break;
        case Owner::Foreign:
            installed = host.installed.isLocal()
                            ? tr("%1 %2, not Vitrine's: installed from a file")
                                  .arg(title, host.installed.evr())
                            : tr("%1 %2, from %3").arg(title, host.installed.evr(), host.installed.repo);
            if (host.locked) {
                installed += tr(", version-locked");
            }
            break;
        case Owner::Distribution:
            installed = tr("the distribution's %1 %2").arg(title, host.installed.evr());
            break;
        }
        if (host.names.size() > 1) {
            installed += tr(" (%1 packages: %2)").arg(host.names.size()).arg(host.names.join(", "));
        }
        rows += row(tr("Installed:"), installed.toHtmlEscaped());
    }
    if (status.built.isValid()) {
        const QDateTime when = status.built.toLocalTime();
        QString built = QLocale().toString(when.date(), "d MMMM yyyy") + ", " +
                        QLocale().toString(when.time(), QLocale::ShortFormat);
        const QList<qint64> took = m_manager->durations(c.id);
        if (!took.isEmpty()) {
            built = tr("%1, in %2").arg(built, duration(took.last()));
        }
        const HostKde::Build &b = host.build.isValid() ? host.build : host.other;
        if (b.isValid()) {
            built = tr("%1 %2, %3").arg(title, b.evr, built);
        }
        rows += row(tr("Built:"), built.toHtmlEscaped());
        if (b.isValid() && target.package != "qt6-qtbase" && b.buildroot.contains("qt6-qtbase")) {
            rows += row(tr("Built against:"),
                        tr("Qt %1").arg(b.buildroot.value("qt6-qtbase").section('-', 0, 0))
                            .toHtmlEscaped());
        }
    }
    if (!status.output.isEmpty()) {
        const qint64 used = size(status.output);
        rows += row(tr("Output:"), "<code>" + status.output.toHtmlEscaped() + "</code>" +
                                       (used >= 0 ? " — " + bytes(used).toHtmlEscaped() : QString()));
    }
    rows += row(tr("A build needs:"),
                (target.heavy ? tr("up to %1 GiB of memory, in its container")
                                    .arg(GuestToolsBuilder::memoryCapMiB() / 1024)
                              : tr("a GiB or two of memory, in its container"))
                    .toHtmlEscaped());
    const QString after =
        target.package == "kwin"
            ? tr("Installing replaces the compositor, as root, version-locked: after each Plasma or "
                 "Qt update it must be built and installed again, or reverted to the distribution's "
                 "KWin. It takes effect at the next login.")
        : target.package == "qt6-qtbase"
            ? tr("Installing replaces all of Qt base's packages, as root, version-locked: Qt updates "
                 "wait until it is built and installed again for them, or reverted. Programs started "
                 "after it use it; the desktop, from the next login.")
            : tr("Installing replaces it, as root, version-locked: when the KDE Frameworks move to a "
                 "new release, it must be built and installed again, or reverted. Programs started "
                 "after it use it; the desktop, from the next login.");
    return html + "<table>" + rows + "</table>" + para(after);
}

qint64 BuildsWindow::size(const QString &path)
{
    if (m_sizes.contains(path)) {
        return m_sizes.value(path);
    }
    m_sizes.insert(path, -1);
    auto *watcher = new QFutureWatcher<qint64>(this);
    connect(watcher, &QFutureWatcher<qint64>::finished, this, [this, watcher, path]() {
        m_sizes.insert(path, watcher->result());
        watcher->deleteLater();
        showSource();
    });
    watcher->setFuture(QtConcurrent::run([path]() { return diskUsage(path); }));
    return -1;
}

void BuildsWindow::updateCache()
{
    const QStringList dirs = m_manager->cacheDirs();
    auto *watcher = new QFutureWatcher<qint64>(this);

    /* the folders' sizes again, and those of the outputs */
    m_sizes.clear();
    m_cache->setText(tr("Build cache: counting…"));
    m_cache->setToolTip(tr("Sources and build trees, downloads, the compiler cache:\n%1")
                            .arg(dirs.join('\n')));
    connect(watcher, &QFutureWatcher<qint64>::finished, this, [this, watcher]() {
        m_cacheSize = watcher->result();
        m_cache->setText(m_cacheSize > 0 ? tr("Build cache: %1").arg(bytes(m_cacheSize))
                                         : tr("Build cache: empty"));
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([dirs]() {
        qint64 total = 0;
        for (const QString &dir : dirs) {
            total += diskUsage(dir);
        }
        return total;
    }));
}

void BuildsWindow::clearCache()
{
    QString error;

    if (!Widgets::confirm(this, QMessageBox::Warning, tr("Clear the Build Cache"),
                          tr("The sources, the build trees and the compiler cache go (%1): the "
                             "next build of each component downloads its sources and compiles "
                             "all of it again. What is built stays.")
                              .arg(m_cacheSize >= 0 ? bytes(m_cacheSize) : tr("all of it")),
                          tr("C&lear"), m_clear->icon())) {
        return;
    }
    if (!m_manager->clearCache(&error)) {
        Widgets::warn(this, tr("Clear the Build Cache"), error);
    }
    updateCache();
}

void BuildsWindow::buildSelected(bool clean)
{
    QStringList todo;

    for (const QString &id : selectedComponents()) {
        const StatusState state = m_manager->status(id).state;
        if (state == StatusState::NotBuilt || state == StatusState::Outdated) {
            todo << id;
        }
    }
    if (!todo.isEmpty()) {
        m_manager->build(todo, clean);
    }
}

void BuildsWindow::contextMenu(const QPoint &at)
{
    QTreeWidgetItem *item = m_tree->itemAt(at);
    QMenu menu(this);

    if (!item || !releaseOf(item->data(NameColumn, IdRole).toString()).isEmpty()) {
        return;
    }
    m_tree->setCurrentItem(item);
    const QString id = selected();
    const Component c = m_manager->component(id);
    const Builds::Status status = m_manager->status(id);

    menu.addAction(m_build->icon(), m_build->text(), this, [this]() { buildSelected(); })
        ->setEnabled(m_build->isEnabled());
    /* build trees in doubt: from nothing (the host's; the packages build from nothing anyway) */
    if (c.kind == Kind::Host || id == "host") {
        menu.addAction(tr("Build from &Scratch"), this, [this]() { buildSelected(true); })
            ->setEnabled(m_build->isEnabled());
    }
    if (c.kind == Kind::System) {
        menu.addAction(m_install->icon(), m_install->text(), this,
                       [this, id]() { HostKdeDialog::install(this, id); })
            ->setEnabled(m_install->isEnabled());
        menu.addAction(m_revert->icon(), m_revert->text(), this,
                       [this, id]() { HostKdeDialog::revert(this, {id}); })
            ->setEnabled(m_revert->isEnabled());
    }
    if (!status.output.isEmpty()) {
        menu.addSeparator();
        const QString folder = QFileInfo(status.output).isDir()
                                   ? status.output
                                   : QFileInfo(status.output).absolutePath();
        menu.addAction(Icons::themed({"folder-open"}, QStyle::SP_DirOpenIcon),
                       tr("&Open Output Folder"), this,
                       [folder]() { QDesktopServices::openUrl(QUrl::fromLocalFile(folder)); });
        menu.addAction(Icons::themed({"edit-copy"}, QStyle::SP_FileIcon), tr("&Copy Its Path"),
                       this, [status]() { QApplication::clipboard()->setText(status.output); });
    }
    menu.exec(m_tree->viewport()->mapToGlobal(at));
}
