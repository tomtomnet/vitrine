// SPDX-License-Identifier: GPL-2.0-or-later
#include "vmpane.h"

#include <QAbstractButton>
#include <QAbstractItemView>
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
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTimer>
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

VmPane::VmPane(QWidget *parent)
    : QWidget(parent), m_check(new QTimer(this)), m_tabs(new QTabWidget),
      m_details(new VmDetails), m_list(new QListWidget), m_title(new QLabel),
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
    m_tabs->addTab(m_details, tr("Details"));
    m_tabs->addTab(settings, tr("Settings"));
    m_tabs->addTab(m_snapshots, tr("Snapshots"));
    m_tabs->addTab(m_log, tr("Logs"));

    /* the pages down the side, as the settings dialog had them, on the tab */
    m_list->setObjectName("pages");
    m_stack->setObjectName("pageStack");
    m_list->setIconSize(QSize(22, 22));
    m_list->setSpacing(1);
    m_list->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->viewport()->setAutoFillBackground(false);
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
    settingsLayout->addWidget(m_list);
    settingsLayout->addWidget(line);
    settingsLayout->addWidget(page, 1);

    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_tabs);

    /* Apply is for when there is something to apply */
    m_check->setSingleShot(true);
    m_check->setInterval(0);
    connect(m_check, &QTimer::timeout, this, &VmPane::updateFooter);
    connect(m_list, &QListWidget::currentRowChanged, this, &VmPane::switchTo);
    connect(m_apply, &QPushButton::clicked, this, &VmPane::apply);
    connect(m_discard, &QPushButton::clicked, this, &VmPane::discard);
    connect(m_snapshots, &SnapshotView::startRequested, this, &VmPane::startFromSnapshot);
    updateFooter();
}

VmPane::~VmPane()
{
    /* the list goes after the members: no page to load then */
    disconnect(m_list, nullptr, this, nullptr);
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

/* The pages of the VM, new, on the page shown before */
void VmPane::buildPages()
{
    const Page page = m_page;

    m_pages.clear();
    m_current = -1;
    {
        /* the page shown in the end is loaded below, once */
        const QSignalBlocker blocker(m_list);

        m_list->clear();
        /* the scroll areas, with the pages */
        while (m_stack->count() > 0) {
            QWidget *old = m_stack->widget(0);
            m_stack->removeWidget(old);
            delete old;
        }
        if (m_vm) {
            m_pages = {new GeneralPage(m_vm), new SystemPage(m_vm),
                       new DisplayPage,        new StoragePage(m_vm->dir()),
                       new SharesPage,         new PciPage,
                       new UsbPage,            new ArgumentsPage(m_vm->dir())};
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
            m_list->addItem(new QListWidgetItem(page->icon(), page->title()));
            m_stack->addWidget(scroll);
            watchEdits(page);
        }
        if (!m_pages.isEmpty()) {
            m_list->setFixedWidth(m_list->sizeHintForColumn(0) + 2 * m_list->frameWidth() + 16);
        }
    }
    m_args = m_vm ? m_vm->args() : ArgsFile();
    m_loaded = m_args.toText();
    if (!m_pages.isEmpty()) {
        m_list->setCurrentRow(qBound(0, int(page), int(m_pages.size()) - 1));
    }
    updateFooter();
}

VmPane::Tab VmPane::tab() const
{
    return Tab(qMax(0, m_tabs->currentIndex()));
}

void VmPane::setTab(Tab tab)
{
    m_tabs->setCurrentIndex(qBound(0, int(tab), int(Logs)));
}

VmPane::Page VmPane::page() const
{
    return m_page;
}

void VmPane::setPage(Page page)
{
    m_page = Page(qBound(0, int(page), int(Arguments)));
    if (m_page < m_list->count()) {
        m_list->setCurrentRow(m_page);
    }
}

void VmPane::switchTo(int row)
{
    if (row < 0 || row >= m_pages.size()) {
        return;
    }
    /* what the page left changed goes to the arguments, for the next page */
    if (m_current >= 0 && m_pages[m_current]->isModified()) {
        m_pages[m_current]->save(m_args);
    }
    m_current = row;
    m_page = Page(row);
    m_pages[row]->load(m_args);
    m_stack->setCurrentIndex(row);
    m_title->setText(m_pages[row]->title());
    /* in line with the page, which has margins of its own */
    m_title->setContentsMargins(m_pages[row]->layout()->contentsMargins().left(), 0, 0, 0);
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
