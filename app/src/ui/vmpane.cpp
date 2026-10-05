// SPDX-License-Identifier: GPL-2.0-or-later
#include "vmpane.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyleOptionViewItem>
#include <QStylePainter>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "ui/banner.h"
#include "ui/icons.h"
#include "ui/logview.h"
#include "ui/settingspages.h"
#include "ui/snapshotview.h"
#include "ui/vmdetails.h"
#include "ui/widgets.h"

/*
 * A page, in its scroll area.  The area takes the height for the width of
 * its widget as the least height it can have: the least height the page
 * needs, not the height it prefers, lets the page shrink before it
 * scrolls.
 */
class PageHolder : public QWidget
{
public:
    explicit PageHolder(SettingsPage *page)
    {
        auto *layout = new QVBoxLayout(this);

        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(page);
    }

    int heightForWidth(int width) const override
    {
        return layout()->totalMinimumHeightForWidth(width);
    }
};

/*
 * Advanced, under the simple pages: shows and hides the advanced ones.
 * Drawn as a row of the lists of pages, its arrow where their icons are
 * and its name where theirs are; a tool button centres both.
 */
class SectionButton : public QToolButton
{
public:
    explicit SectionButton(const QListWidget *list) : m_list(list)
    {
        setCheckable(true);
        setAutoRaise(true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    QSize sizeHint() const override
    {
        QRect icon, text;
        rowParts(QRect(0, 0, QWIDGETSIZE_MAX / 2, rowHeight()), &icon, &text);
        return QSize(text.left() + fontMetrics().horizontalAdvance(this->text()) +
                         textMargin() + m_list->spacing(),
                     rowHeight());
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QStylePainter painter(this);
        QStyleOptionToolButton panel;
        QStyleOption arrow;
        QRect icon, text;

        /* its hover and focus as a tool button's, not sunken while open */
        initStyleOption(&panel);
        panel.state &= ~QStyle::State_On;
        panel.text.clear();
        panel.icon = QIcon();
        panel.features &= ~QStyleOptionToolButton::Arrow;
        painter.drawComplexControl(QStyle::CC_ToolButton, panel);

        rowParts(QRect(m_list->spacing(), 0, width() - 2 * m_list->spacing(), height()), &icon,
                 &text);
        arrow.initFrom(this);
        arrow.rect = icon;
        painter.drawPrimitive(isChecked()        ? QStyle::PE_IndicatorArrowDown
                              : isRightToLeft() ? QStyle::PE_IndicatorArrowLeft
                                                : QStyle::PE_IndicatorArrowRight,
                              arrow);
        /* inside its rectangle as the style draws an item's text */
        painter.drawItemText(text.adjusted(textMargin(), 0, -textMargin(), 0),
                             Qt::AlignLeft | Qt::AlignVCenter, m_list->palette(), isEnabled(),
                             this->text(), QPalette::Text);
    }

private:
    /* Between an item's text and the sides of its text rectangle (QCommonStyle's) */
    int textMargin() const
    {
        return m_list->style()->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, m_list) + 1;
    }

    /* A row of the list as high as its items, with their spacing */
    int rowHeight() const
    {
        return (m_list->count() > 0 ? m_list->sizeHintForRow(0) : QToolButton::sizeHint().height()) +
               2 * m_list->spacing();
    }

    /* Where an item of the list in @row has its icon and its text */
    void rowParts(const QRect &row, QRect *icon, QRect *text) const
    {
        QStyleOptionViewItem item;

        item.initFrom(m_list);
        item.widget = m_list;
        item.rect = row;
        item.features = QStyleOptionViewItem::HasDisplay | QStyleOptionViewItem::HasDecoration;
        item.decorationSize = m_list->iconSize();
        item.decorationPosition = QStyleOptionViewItem::Left;
        item.decorationAlignment = Qt::AlignCenter;
        item.displayAlignment = Qt::AlignLeft | Qt::AlignVCenter;
        item.text = this->text();
        item.font = m_list->font();
        item.fontMetrics = QFontMetrics(item.font);
        item.direction = layoutDirection();
        *icon = m_list->style()->subElementRect(QStyle::SE_ItemViewItemDecoration, &item, m_list);
        *text = m_list->style()->subElementRect(QStyle::SE_ItemViewItemText, &item, m_list);
    }

    const QListWidget *m_list;
};

VmPane::VmPane(QWidget *parent)
    : QWidget(parent), m_check(new QTimer(this)), m_tabs(new QTabWidget),
      m_console(new QWidget), m_details(new VmDetails), m_side(new QWidget),
      m_list(new QListWidget), m_more(new SectionButton(m_list)), m_advanced(new QListWidget),
      m_title(new QLabel),
      m_stack(new QStackedWidget), m_snapshots(new SnapshotView), m_log(new LogView), m_running(new Banner(Banner::Information)),
      /* no mnemonic: the pages use D */
      m_discard(new QPushButton(Icons::themed({"edit-undo"}, QStyle::SP_DialogResetButton),
                                tr("Discard"))),
      m_apply(new QPushButton(Icons::themed({"dialog-ok-apply", "dialog-ok"},
                                            QStyle::SP_DialogApplyButton),
                              tr("&Apply")))
{
    auto *layout = new QVBoxLayout(this);
    auto *settings = new QWidget;
    auto *settingsLayout = new QHBoxLayout(settings);
    auto *line = new QFrame;
    auto *page = new QWidget;
    auto *pageLayout = new QVBoxLayout(page);
    auto *footer = new QHBoxLayout;

    /* no frame around the tabs: the pages have frames enough */
    m_tabs->setObjectName("vmTabs");
    m_tabs->setDocumentMode(true);
    m_tabs->addTab(m_console, tr("Console"));
    m_tabs->addTab(m_details, tr("Details"));
    m_tabs->addTab(settings, tr("Settings"));
    m_tabs->addTab(m_snapshots, tr("Snapshots"));
    m_tabs->addTab(m_log, tr("Logs"));
    {
        /* the same, as actions, for the window's bar and its View menu */
        auto *group = new QActionGroup(this);
        const QStringList texts = {tr("&Console"), tr("&Details"), tr("&Settings"),
                                   tr("S&napshots"), tr("L&ogs")};
        for (int i = 0; i < m_tabs->count(); i++) {
            auto *action = new QAction(texts.value(i, m_tabs->tabText(i)), group);
            action->setCheckable(true);
            action->setChecked(i == m_tabs->currentIndex());
            connect(action, &QAction::triggered, this, [this, i]() { setTab(Tab(i)); });
            m_tabActions << action;
        }
        connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
            if (QAction *action = m_tabActions.value(index)) {
                action->setChecked(true);
            }
        });
    }

    /* the pages down the side, as the settings dialog had them, on the tab:
       the simple ones, then Advanced, folded */
    m_list->setObjectName("pages");
    m_advanced->setObjectName("advancedPages");
    m_stack->setObjectName("pageStack");
    /* the pages' icons between the small and the large size: the toolbar's */
    const int icons = style()->pixelMetric(QStyle::PM_ToolBarIconSize, nullptr, this);
    for (QListWidget *list : {m_list, m_advanced}) {
        list->setIconSize(QSize(icons, icons));
        list->setSpacing(1);
        list->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        list->setFrameShape(QFrame::NoFrame);
        list->viewport()->setAutoFillBackground(false);
        list->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }
    m_more->setObjectName("advanced");
    m_more->setText(tr("Advanced"));
    m_more->setToolTip(tr("The machine, its boot, PCI devices, and the QEMU command line "
                          "itself"));
    m_advanced->hide();
    {
        auto *side = new QVBoxLayout(m_side);
        side->setContentsMargins(0, 0, 0, 0);
        side->setSpacing(0);
        side->addWidget(m_list);
        /* apart from the simple pages, as the groups of a layout are */
        side->addSpacing(style()->pixelMetric(QStyle::PM_LayoutVerticalSpacing, nullptr, this));
        side->addWidget(m_more);
        side->addWidget(m_advanced);
        side->addStretch();
    }
    line->setFrameShape(QFrame::VLine);
    line->setFrameShadow(QFrame::Sunken);
    /* the name of the page over it, above the titles of its parts */
    m_title->setObjectName("pageTitle");
    {
        QFont font = m_title->font();
        font.setBold(true);
        font.setPointSizeF(font.pointSizeF() * 1.3);
        m_title->setFont(font);
    }
    m_running->setObjectName("running");
    m_running->setText(tr("The VM is running: the changes apply the next time it starts."));
    m_discard->setObjectName("discard");
    m_discard->setToolTip(tr("Go back to the settings as they were saved"));
    m_apply->setObjectName("apply");
    footer->addStretch();
    footer->addWidget(m_discard);
    footer->addWidget(m_apply);
    pageLayout->addWidget(m_title);
    pageLayout->addWidget(m_running);
    pageLayout->addWidget(m_stack, 1);
    pageLayout->addLayout(footer);
    settingsLayout->setContentsMargins(0, 0, 0, 0);
    settingsLayout->setSpacing(0);
    settingsLayout->addWidget(m_side);
    settingsLayout->addWidget(line);
    settingsLayout->addWidget(page, 1);

    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_tabs);
    (new QVBoxLayout(m_console))->setContentsMargins(0, 0, 0, 0);
    connect(m_tabs, &QTabWidget::currentChanged, this,
            [this](int index) { emit tabChanged(Tab(qMax(0, index))); });

    /* Apply is for when there is something to apply */
    m_check->setSingleShot(true);
    m_check->setInterval(0);
    connect(m_check, &QTimer::timeout, this, &VmPane::updateFooter);
    /*
     * A page is chosen by selecting its row, with the mouse or the keys,
     * not by the row becoming current: keyboard focus coming into a list
     * makes its first row current, without selecting it
     */
    for (QListWidget *list : {m_list, m_advanced}) {
        connect(list, &QListWidget::itemSelectionChanged, this, [this, list]() {
            const QList<QListWidgetItem *> items = list->selectedItems();
            if (!items.isEmpty()) {
                pageChosen(list, list->row(items.first()));
            }
        });
    }
    connect(m_more, &QToolButton::toggled, this, &VmPane::showAdvanced);
    connect(m_apply, &QPushButton::clicked, this, &VmPane::apply);
    /*
     * Ctrl+S while the Settings tab shows, wherever the keys are in the
     * window (a page's field, the Arguments editor, the list of VMs or the
     * tabs after a click with the mouse): Apply.  Not on the other tabs,
     * where the VM's screen takes Ctrl+S for the guest.
     */
    auto *save = new QShortcut(QKeySequence::Save, this);
    save->setContext(Qt::WindowShortcut);
    save->setEnabled(m_tabs->currentWidget() == settings);
    connect(m_tabs, &QTabWidget::currentChanged, save,
            [this, save, settings]() { save->setEnabled(m_tabs->currentWidget() == settings); });
    connect(save, &QShortcut::activated, this, [this]() {
        /* the changes themselves, not Apply's state: it follows them a moment
           later, and Ctrl+S right after a key would find it off */
        if (isModified()) {
            apply();
        }
    });
    connect(m_discard, &QPushButton::clicked, this, &VmPane::discard);
    connect(m_snapshots, &SnapshotView::startRequested, this, &VmPane::startFromSnapshot);
    updateFooter();
}

VmPane::~VmPane()
{
    /* the lists go after the members: no page to load then */
    disconnect(m_list, nullptr, this, nullptr);
    disconnect(m_advanced, nullptr, this, nullptr);
}

Vm *VmPane::vm() const
{
    return m_vm;
}

void VmPane::setVm(Vm *vm)
{
    if (vm == m_vm && m_pages.isEmpty() == !vm) {
        m_details->setVm(vm);
        return;
    }
    if (m_vm) {
        disconnect(m_vm, nullptr, this, nullptr);
        disconnect(m_vm->runner(), nullptr, this, nullptr);
    }
    m_vm = vm;
    m_details->setVm(vm);
    m_snapshots->setVm(vm);
    m_log->setPath(vm ? vm->runner()->logPath() : QString());
    if (vm) {
        connect(vm, &Vm::changed, this, &VmPane::vmChanged);
        connect(vm->runner(), &VmRunner::stateChanged, this, &VmPane::updateFooter);
    }
    buildPages();
}

/* A list as tall as its items: the lists of pages do not scroll */
static void fitHeight(QListWidget *list)
{
    int height = 2 * list->frameWidth() + list->spacing();

    for (int i = 0; i < list->count(); i++) {
        height += list->sizeHintForRow(i) + 2 * list->spacing();
    }
    list->setFixedHeight(height);
}

/* The pages of the VM, new, on the page shown before */
void VmPane::buildPages()
{
    const Page page = m_page;

    m_pages.clear();
    m_current = -1;
    {
        /* the page shown in the end is loaded below, once */
        const QSignalBlocker blocker(m_list), advancedBlocker(m_advanced);

        m_list->clear();
        m_advanced->clear();
        /* the scroll areas, with the pages */
        while (m_stack->count() > 0) {
            QWidget *old = m_stack->widget(0);
            m_stack->removeWidget(old);
            delete old;
        }
        if (m_vm) {
            /* in the order of Page */
            m_pages = {new GeneralPage(m_vm),  new HardwarePage,
                       new DisplayPage,         new StoragePage(m_vm->dir()),
                       new SharesPage,          new UsbPage,
                       new NetworkPage(m_vm),   new MachinePage,
                       new BootPage(m_vm),      new PciPage,
                       new ArgumentsPage(m_vm->dir())};
        }
        /* scrolled, not the window grown, when a page does not fit */
        for (SettingsPage *page : std::as_const(m_pages)) {
            auto *scroll = new QScrollArea;
            scroll->setWidget(new PageHolder(page));
            scroll->setWidgetResizable(true);
            scroll->setFrameShape(QFrame::NoFrame);
            /* down, never across: the window is no narrower than the widest page */
            scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            scroll->setMinimumWidth(scroll->widget()->minimumSizeHint().width() +
                                    scroll->verticalScrollBar()->sizeHint().width());
            (m_stack->count() < FirstAdvanced ? m_list : m_advanced)
                ->addItem(new QListWidgetItem(page->icon(), page->title()));
            m_stack->addWidget(scroll);
            watchEdits(page);
        }
        if (!m_pages.isEmpty()) {
            const int width = qMax(m_list->sizeHintForColumn(0),
                                   m_advanced->sizeHintForColumn(0)) +
                              2 * m_list->frameWidth() + 16;
            m_list->setFixedWidth(width);
            m_advanced->setFixedWidth(width);
            /* a row as high as the lists' now that they have some */
            m_more->updateGeometry();
            m_side->setFixedWidth(qMax(width, m_more->sizeHint().width()));
            fitHeight(m_list);
            fitHeight(m_advanced);
        }
        m_more->setVisible(!m_pages.isEmpty());
    }
    m_args = m_vm ? m_vm->args() : ArgsFile();
    m_loaded = m_args.toText();
    if (!m_pages.isEmpty()) {
        selectPage(qBound(0, int(page), int(m_pages.size()) - 1));
    }
    updateFooter();
}

void VmPane::setConsole(QWidget *console)
{
    m_console->layout()->addWidget(console);
}

void VmPane::pageChosen(QListWidget *list, int row)
{
    QListWidget *other = list == m_list ? m_advanced : m_list;

    if (row < 0) {
        return;
    }
    {
        /* one page current in both lists; set through the view, so that
           focus coming in leaves it so rather than making row 0 current */
        const QSignalBlocker block(other);
        other->setCurrentIndex(QModelIndex());
        other->clearSelection();
    }
    switchTo(list == m_list ? row : FirstAdvanced + row);
}

void VmPane::selectPage(int page)
{
    if (page >= FirstAdvanced) {
        m_more->setChecked(true);
        m_advanced->setCurrentRow(page - FirstAdvanced);
    } else {
        m_list->setCurrentRow(page);
    }
}

void VmPane::showAdvanced(bool on)
{
    m_advanced->setVisible(on);
}

VmPane::Tab VmPane::tab() const
{
    return Tab(qMax(0, m_tabs->currentIndex()));
}

void VmPane::setTab(Tab tab)
{
    m_tabs->setCurrentIndex(qBound(0, int(tab), int(Logs)));
}

void VmPane::setTabBarShown(bool shown)
{
    /* the tab widget lays its pages out without it */
    m_tabs->tabBar()->setVisible(shown);
}

VmPane::Page VmPane::page() const
{
    return m_page;
}

void VmPane::setPage(Page page)
{
    m_page = Page(qBound(0, int(page), int(Arguments)));
    if (m_page < m_pages.size()) {
        selectPage(m_page);
    }
}

void VmPane::switchTo(int page)
{
    if (page < 0 || page >= m_pages.size()) {
        return;
    }
    /* what the page left changed goes to the arguments, for the next page */
    if (m_current >= 0 && m_pages[m_current]->isModified()) {
        m_pages[m_current]->save(m_args);
    }
    m_current = page;
    m_page = Page(page);
    m_pages[page]->load(m_args);
    m_stack->setCurrentIndex(page);
    m_title->setText(m_pages[page]->title());
    /* in line with the page, which has margins of its own */
    m_title->setContentsMargins(m_pages[page]->layout()->contentsMargins().left(), 0, 0, 0);
    updateFooter();
}

bool VmPane::isModified() const
{
    return m_vm && (m_args.toText() != m_loaded ||
                    (m_current >= 0 && m_pages[m_current]->isModified()));
}

/* vm.args saved, here or by hand: the pages show it, unless they have changes */
void VmPane::vmChanged()
{
    if (!isModified()) {
        m_args = m_vm->args();
        m_loaded = m_args.toText();
        if (m_current >= 0) {
            m_pages[m_current]->load(m_args);
        }
    }
    m_check->start();
}

void VmPane::updateFooter()
{
    const bool modified = isModified();

    m_apply->setEnabled(modified);
    m_discard->setEnabled(modified);
    m_running->setVisible(m_vm && m_vm->runner()->isActive());
}

bool VmPane::confirmChanges(const QString &question)
{
    if (!isModified()) {
        return true;
    }

    QMessageBox box(QMessageBox::Question, tr("Unsaved Changes"),
                    tr("The settings of %1 have changes that are not applied.")
                        .arg(m_vm->name()),
                    QMessageBox::NoButton, window());
    QPushButton *apply = box.addButton(tr("&Apply"), QMessageBox::AcceptRole);
    QPushButton *discard = box.addButton(tr("&Discard"), QMessageBox::DestructiveRole);

    /* as the page's own Apply and Discard */
    Widgets::setButtonIcon(apply, m_apply->icon());
    Widgets::setButtonIcon(discard, m_discard->icon());

    box.addButton(QMessageBox::Cancel);
    box.setInformativeText(question);
    /* the KDE dialog would choose the default button itself */
    box.setOption(QMessageBox::Option::DontUseNativeDialog);
    box.setDefaultButton(apply);
    box.exec();
    if (box.clickedButton() == apply) {
        return this->apply();
    }
    if (box.clickedButton() == discard) {
        this->discard();
        return true;
    }
    return false;
}

bool VmPane::apply()
{
    QString error;

    if (!m_vm) {
        return false;
    }
    if (m_current >= 0 && m_pages[m_current]->isModified()) {
        m_pages[m_current]->save(m_args);
    }
    /* new disks, firmware copies: now that the arguments are final */
    for (SettingsPage *page : std::as_const(m_pages)) {
        if (!page->commit(m_args, m_vm->dir(), &error)) {
            Widgets::warn(this, tr("Cannot Save the Settings"), error);
            return false;
        }
    }
    if (m_args.toText() != m_vm->args().toText() && !m_vm->save(m_args, &error)) {
        Widgets::warn(this, tr("Cannot Save the Settings"), error);
        return false;
    }
    m_args = m_vm->args();
    m_loaded = m_args.toText();
    if (m_current >= 0) {
        m_pages[m_current]->load(m_args);
    }
    updateFooter();
    return true;
}

void VmPane::discard()
{
    if (!m_vm) {
        return;
    }
    m_args = m_vm->args();
    m_loaded = m_args.toText();
    if (m_current >= 0) {
        m_pages[m_current]->load(m_args);
    }
    updateFooter();
}

/*
 * Checks again for something to apply after any change on @page.  The
 * connections go to the timer, which checks later, and goes before the
 * pages when the pane is deleted: their models still signal while they
 * are destroyed.
 */
void VmPane::watchEdits(SettingsPage *page)
{
    QTimer *timer = m_check;
    const auto check = qOverload<>(&QTimer::start);

    for (QLineEdit *w : page->findChildren<QLineEdit *>()) {
        connect(w, &QLineEdit::textChanged, timer, check);
    }
    for (QSpinBox *w : page->findChildren<QSpinBox *>()) {
        connect(w, &QSpinBox::valueChanged, timer, check);
    }
    for (QComboBox *w : page->findChildren<QComboBox *>()) {
        connect(w, &QComboBox::currentIndexChanged, timer, check);
    }
    /* the pages' own handlers of a click, run first, may open dialogs */
    for (QAbstractButton *w : page->findChildren<QAbstractButton *>()) {
        connect(w, &QAbstractButton::toggled, timer, check);
        connect(w, &QAbstractButton::clicked, timer, check);
    }
    for (QPlainTextEdit *w : page->findChildren<QPlainTextEdit *>()) {
        connect(w, &QPlainTextEdit::textChanged, timer, check);
    }
    for (QAbstractItemView *w : page->findChildren<QAbstractItemView *>()) {
        if (QAbstractItemModel *model = w->model()) {
            connect(model, &QAbstractItemModel::dataChanged, timer, check);
            connect(model, &QAbstractItemModel::rowsInserted, timer, check);
            connect(model, &QAbstractItemModel::rowsRemoved, timer, check);
            connect(model, &QAbstractItemModel::modelReset, timer, check);
        }
    }
}
