// SPDX-License-Identifier: GPL-2.0-or-later
#include "settingsupdates.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QResizeEvent>
#include <QTextLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtMath>

#include <memory>

#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "ui/banner.h"
#include "ui/vmpane.h"
#include "ui/widgets.h"

using SettingsUpdates::Change;
using SettingsUpdates::Record;

namespace {

/*
 * Argument lines, as written, in a box that wraps them anywhere and is as
 * tall as they are once wrapped at the width it has: no scrolling to see
 * them.  Its height for its width, in its layout, laid out as the box lays
 * them out: a count from the widths of the lines' characters could miss
 * by a line, as it did by a pixel.
 */
class LinesBox : public QPlainTextEdit
{
public:
    LinesBox()
    {
        QSizePolicy policy = sizePolicy();

        setReadOnly(true);
        setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        /* never needed, and its width would wrap the lines otherwise */
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        /* as tall as its lines, never more (sizeHint()) */
        policy.setVerticalPolicy(QSizePolicy::Maximum);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
    }

    bool hasHeightForWidth() const override { return true; }

    int heightForWidth(int width) const override
    {
        const qreal margin = document()->documentMargin();
        /* as QPlainTextDocumentLayout lays out its blocks, whole pixels a line */
        const qreal room = width - 2 * frameWidth() - 2 * margin;
        qreal height = 0;

        for (const QString &line : toPlainText().split('\n')) {
            QTextLayout layout(line, font());
            layout.setTextOption(document()->defaultTextOption());
            layout.beginLayout();
            for (QTextLine l = layout.createLine(); l.isValid(); l = layout.createLine()) {
                l.setLeadingIncluded(true);
                l.setLineWidth(room);
                height += l.height() + (l.leading() < 0 ? qCeil(l.leading()) : 0);
            }
            layout.endLayout();
        }
        /* and a pixel: QPlainTextEdit sees a line as hidden unless the
           lines and the margins leave one (adjustScrollbars()) */
        return qCeil(height + 2 * margin) + 1 + 2 * frameWidth();
    }

    /* As high as its lines at the width it has */
    QSize sizeHint() const override
    {
        return QSize(QPlainTextEdit::sizeHint().width(),
                     heightForWidth(qMax(width(), minimumWidth())));
    }

    /* At least as high as its lines unwrapped, not a scroll area's least */
    QSize minimumSizeHint() const override
    {
        return QSize(QPlainTextEdit::minimumSizeHint().width(), heightForWidth(QWIDGETSIZE_MAX / 2));
    }

    /*
     * @lines, as wide as the longest (the card's) where @maxWidth allows:
     * the window may still be narrower, and lines wrap then
     */
    void setLines(const QStringList &lines, int maxWidth)
    {
        ensurePolished();
        const QFontMetrics fm(font());
        const int margins = 2 * (frameWidth() + qCeil(document()->documentMargin()));
        const int room = qMax(fm.averageCharWidth() * 40, maxWidth - margins);
        int widest = 0;

        for (const QString &line : lines) {
            widest = qMax(widest, fm.horizontalAdvance(line));
        }
        setPlainText(lines.join('\n'));
        setMinimumWidth(qMin(widest + fm.averageCharWidth(), room) + margins);
        updateGeometry();
    }

protected:
    /* another width, maybe another height: the layout asks again */
    void resizeEvent(QResizeEvent *event) override
    {
        QPlainTextEdit::resizeEvent(event);
        if (event->size().width() != event->oldSize().width()) {
            updateGeometry();
        }
    }
};

}

/* "its line 12 is no option: ..." as a sentence of its own */
static QString sentence(const QString &text)
{
    QString out = text;

    if (!out.isEmpty()) {
        out[0] = out[0].toUpper();
        if (!out.endsWith('.')) {
            out += '.';
        }
    }
    return out;
}

static QStringList titles(const QList<Change> &changes)
{
    QStringList out;

    for (const Change &c : changes) {
        out << c.title;
    }
    return out;
}

static QStringList titles(const Record &record)
{
    QStringList out;

    for (const Record::Applied &applied : record.changes) {
        out << SettingsUpdates::title(applied.id);
    }
    return out;
}

/* The VmPane @from is on, if it shows @vm */
static VmPane *paneOf(QWidget *from, Vm *vm)
{
    for (QWidget *w = from; w; w = w->parentWidget()) {
        if (auto *pane = qobject_cast<VmPane *>(w)) {
            return pane->vm() == vm ? pane : nullptr;
        }
    }
    return nullptr;
}

SettingsUpdatesBanner::SettingsUpdatesBanner(Place place, QWidget *parent)
    : QWidget(parent), m_place(place), m_offer(new Banner(Banner::Information)),
      m_updated(new Banner(Banner::Information)), m_problem(new Banner(Banner::Information))
{
    auto *layout = new QVBoxLayout(this);

    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_offer);
    layout->addWidget(m_updated);
    layout->addWidget(m_problem);
    m_offer->setObjectName("settingsUpdatesOffer");
    m_offer->button()->setText(tr("Review…"));
    m_offer->button()->show();
    connect(m_offer->button(), &QPushButton::clicked, this, [this]() {
        if (m_vm) {
            SettingsUpdatesDialog::run(this, m_vm);
        }
    });
    if (place == Place::Home) {
        m_notNow = m_offer->addButton(tr("Not Now"));
        connect(m_notNow, &QPushButton::clicked, this, [this]() {
            if (m_vm) {
                SettingsUpdatesMonitor::of(m_vm)->setNotNow();
            }
        });
    }
    m_updated->setObjectName("settingsUpdatesDone");
    m_updated->button()->setText(tr("Undo…"));
    m_updated->button()->show();
    connect(m_updated->button(), &QPushButton::clicked, this, [this]() {
        if (m_vm) {
            SettingsUpdatesDialog::undo(this, m_vm);
        }
    });
    m_problem->setObjectName("settingsUpdatesProblem");
    m_offer->hide();
    m_updated->hide();
    m_problem->hide();
    hide();
}

QPushButton *SettingsUpdatesBanner::reviewButton() const
{
    return m_offer->button();
}

QPushButton *SettingsUpdatesBanner::undoButton() const
{
    return m_updated->button();
}

void SettingsUpdatesBanner::setVm(Vm *vm)
{
    if (m_vm == vm) {
        update();
        return;
    }
    if (m_vm) {
        disconnect(SettingsUpdatesMonitor::of(m_vm), nullptr, this, nullptr);
    }
    m_vm = vm;
    if (vm) {
        SettingsUpdatesMonitor *monitor = SettingsUpdatesMonitor::of(vm);
        connect(monitor, &SettingsUpdatesMonitor::changed, this, &SettingsUpdatesBanner::update);
        /* its QEMU, the preferences' one, may be another since */
        monitor->refresh();
    }
    update();
}

void SettingsUpdatesBanner::update()
{
    if (!m_vm) {
        hide();
        return;
    }
    SettingsUpdatesMonitor *monitor = SettingsUpdatesMonitor::of(m_vm);
    const QList<Change> offered = monitor->offered();
    const QList<Change> declined = monitor->declined();
    const QList<Record> records = monitor->records();
    const bool off = !m_vm->runner()->isActive();

    QString offer;
    if (m_place == Place::Home) {
        /* one line, while there is something to take and it was not put off */
        if (!offered.isEmpty() && !monitor->notNow() && off) {
            offer = offered.size() == 1
                        ? tr("This VM lacks a setting that new VMs get.")
                        : tr("This VM lacks %1 settings that new VMs get.").arg(offered.size());
        }
    } else if (!offered.isEmpty()) {
        offer = (offered.size() == 1 ? tr("This VM lacks a setting that new VMs get: %1.")
                                     : tr("This VM lacks %1 settings that new VMs get: %2.")
                                           .arg(offered.size()))
                    .arg(SettingsUpdates::listed(titles(offered)).toHtmlEscaped());
        if (!off) {
            offer += ' ' + tr("Shut it down to apply them.").toHtmlEscaped();
        }
    } else if (!declined.isEmpty()) {
        offer = tr("Not asked again for this VM: %1.")
                    .arg(SettingsUpdates::listed(titles(declined)).toHtmlEscaped());
    }
    m_offerText = offer;
    m_offer->setText(offer);
    m_offer->setVisible(!offer.isEmpty());

    QString updated;
    if (m_place == Place::Details && !records.isEmpty()) {
        const Record &last = records.last();
        updated = tr("Settings updated on %1: %2. vm.args as it was before is %3.")
                      .arg(last.time.toString("d MMMM yyyy, HH:mm"),
                           SettingsUpdates::listed(titles(last)).toHtmlEscaped(),
                           last.backup.toHtmlEscaped());
        m_updated->button()->setEnabled(off);
    }
    m_updateText = updated;
    m_updated->setText(updated);
    m_updated->setVisible(!updated.isEmpty());

    QString problem;
    if (m_place == Place::Details && !monitor->problem().isEmpty()) {
        problem = tr("Vitrine cannot check this VM's settings for newer ones. %1")
                      .arg(sentence(monitor->problem()).toHtmlEscaped());
    }
    m_problemText = problem;
    m_problem->setText(problem);
    m_problem->setVisible(!problem.isEmpty());

    setVisible(!offer.isEmpty() || !updated.isEmpty() || !problem.isEmpty());
}

/*
 * What a change does to vm.args, its lines as they are and as they become,
 * wrapped within @width: a line of vm.args has no spaces to wrap at
 */
static QWidget *linesOf(const Change &change, int width)
{
    auto *box = new QWidget;
    auto *column = new QVBoxLayout(box);
    const auto part = [column, width](const QString &heading, const QStringList &lines) {
        auto *text = new LinesBox;
        column->addWidget(Widgets::hint(heading.toHtmlEscaped()));
        text->setLines(lines, width);
        column->addWidget(text);
    };

    column->setContentsMargins(0, 0, 0, 0);
    if (change.before.isEmpty()) {
        part(SettingsUpdatesDialog::tr("Added to vm.args:"), change.after);
    } else if (change.after.isEmpty()) {
        part(SettingsUpdatesDialog::tr("Taken out of vm.args:"), change.before);
    } else {
        part(SettingsUpdatesDialog::tr("In vm.args now:"), change.before);
        part(SettingsUpdatesDialog::tr("Then:"), change.after);
    }
    return box;
}

void SettingsUpdatesDialog::run(QWidget *from, Vm *vm)
{
    if (!vm) {
        return;
    }
    if (VmPane *pane = paneOf(from, vm);
        pane && !pane->confirmChanges(tr("Apply them before the newer settings?"))) {
        return;
    }
    auto *dialog = new SettingsUpdatesDialog(vm, from ? from->window() : nullptr);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}

void SettingsUpdatesDialog::undo(QWidget *from, Vm *vm)
{
    if (!vm) {
        return;
    }
    SettingsUpdatesMonitor *monitor = SettingsUpdatesMonitor::of(vm);
    const QList<Record> records = monitor->records();
    if (records.isEmpty()) {
        return;
    }
    const Record last = records.last();
    QWidget *window = from ? from->window() : nullptr;
    std::unique_ptr<QMessageBox> box(Widgets::messageBox(
        QMessageBox::Question, tr("Undo the Update"),
        tr("These settings go back as they were before the update of %1: %2. The other lines of "
           "vm.args stay as they are now.")
            .arg(last.time.toString("d MMMM, HH:mm"), SettingsUpdates::listed(titles(last))),
        QMessageBox::NoButton, window));
    QPointer<Vm> guard(vm);
    QPushButton *undoButton = box->addButton(tr("&Undo"), QMessageBox::AcceptRole);
    box->addButton(QMessageBox::Cancel);
    box->setDefaultButton(undoButton);
    box->exec();
    if (box->clickedButton() != undoButton || !guard) {
        return;
    }
    if (VmPane *pane = paneOf(from, vm);
        pane && !pane->confirmChanges(tr("Apply them before undoing the update?"))) {
        return;
    }
    QStringList failed;
    QString error;
    if (!monitor->undo(&failed, &error)) {
        Widgets::warn(window, tr("Cannot Undo the Update"), sentence(error));
    } else if (!failed.isEmpty()) {
        Widgets::inform(window, tr("Some Settings Stay as They Are"),
                        tr("Their lines changed since the update, and were left as they are: %1. "
                           "vm.args as it was before the update is %2, in the VM's folder.")
                            .arg(SettingsUpdates::listed(failed), last.backup));
    }
}

SettingsUpdatesDialog::SettingsUpdatesDialog(Vm *vm, QWidget *parent)
    : QDialog(parent), m_vm(vm), m_note(new Banner(Banner::Information)), m_list(new QWidget),
      m_rows(new QVBoxLayout(m_list)),
      m_buttons(new QDialogButtonBox(QDialogButtonBox::NoButton))
{
    auto *layout = new QVBoxLayout(this);
    auto *scroll = new QScrollArea;
    QLabel *intro = Widgets::note(
        tr("Settings vitrine gives the VMs it makes now, which this VM was made without. Pick "
           "those to take: vitrine saves vm.args as a dated copy beside it first, changes only "
           "their lines, and Undo on the Details puts them back."));

    intro->setTextFormat(Qt::PlainText);
    setWindowTitle(tr("Newer Settings for %1").arg(vm->name()));
    setObjectName("settingsUpdatesDialog");
    m_rows->setContentsMargins(0, 0, 0, 0);
    scroll->setWidget(m_list);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    /* everything in it wraps: as wide as the dialog */
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QPushButton *apply = m_buttons->addButton(tr("&Apply"), QDialogButtonBox::AcceptRole);
    m_buttons->addButton(tr("&Not Now"), QDialogButtonBox::RejectRole);
    apply->setDefault(true);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &SettingsUpdatesDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &SettingsUpdatesDialog::reject);

    layout->addWidget(intro);
    layout->addWidget(m_note);
    layout->addWidget(scroll, 1);
    layout->addWidget(m_buttons);
    m_note->hide();

    /* vm.args edited, the VM started or stopped, a choice made on its Details */
    connect(SettingsUpdatesMonitor::of(vm), &SettingsUpdatesMonitor::changed, this,
            &SettingsUpdatesDialog::rebuild);
    rebuild();
    /* as tall as its list at its width, up to half the screen or so: it scrolls past that */
    const int width = Widgets::em(this) * 40;
    const QMargins margins = layout->contentsMargins();
    const int room = width - margins.left() - margins.right() -
                     scroll->verticalScrollBar()->sizeHint().width();
    const int most = screen() ? screen()->availableGeometry().height() * 55 / 100 : 600;
    const int list = m_list->hasHeightForWidth() ? m_list->heightForWidth(room)
                                                  : m_list->sizeHint().height();
    scroll->setMinimumHeight(qMin(list, most));
    Widgets::resizeToWidth(this, width);
}

QStringList SettingsUpdatesDialog::offered() const
{
    QStringList out;

    for (const Change &c : m_changes) {
        out << c.id;
    }
    return out;
}

QStringList SettingsUpdatesDialog::declined() const
{
    QStringList out;

    for (const Change &c : m_declined) {
        out << c.id;
    }
    return out;
}

QPushButton *SettingsUpdatesDialog::applyButton() const
{
    for (QAbstractButton *b : m_buttons->buttons()) {
        if (m_buttons->buttonRole(b) == QDialogButtonBox::AcceptRole) {
            return qobject_cast<QPushButton *>(b);
        }
    }
    return nullptr;
}

void SettingsUpdatesDialog::rebuild()
{
    if (!m_vm) {
        QDialog::reject();
        return;
    }
    SettingsUpdatesMonitor *monitor = SettingsUpdatesMonitor::of(m_vm);
    const QList<Change> offered = monitor->offered();
    const QList<Change> declined = monitor->declined();

    if (m_built && offered == m_changes && declined == m_declined) {
        update();
        return;
    }
    /* changed by something else than a choice made here: vm.args edited */
    if (m_built && !m_ownChange && offered != m_changes) {
        m_stale = true;
    }
    m_ownChange = false;
    m_built = true;
    m_changes = offered;
    m_declined = declined;
    m_checks.clear();
    /* later: the button that made the list change may be on one of them */
    while (QLayoutItem *item = m_rows->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            w->hide();
            w->deleteLater();
        }
        delete item;
    }

    if (offered.isEmpty()) {
        QLabel *nothing = Widgets::note(tr("Nothing left to take."));
        nothing->setTextFormat(Qt::PlainText);
        m_rows->addWidget(nothing);
    }
    for (const Change &c : offered) {
        auto *row = new QWidget;
        auto *column = new QVBoxLayout(row);
        auto *check = new QCheckBox(c.title);
        QLabel *reason = Widgets::hint(c.reason);
        auto *toggle = new QToolButton;
        QWidget *lines = linesOf(c, Widgets::em(this) * 36);
        auto *since =
            new QLabel(tr("New VMs have it since %1.").arg(c.since.toString("d MMMM yyyy")));
        auto *declineButton = new QPushButton(tr("Don't Ask Again"));
        auto *actions = new QHBoxLayout;
        const QString id = c.id;

        column->setContentsMargins(0, 0, 0, Widgets::em(this) / 2);
        check->setObjectName("check-" + id);
        check->setChecked(m_checked.value(id, true));
        connect(check, &QCheckBox::toggled, this, [this, id](bool on) {
            m_checked.insert(id, on);
            update();
        });
        m_checks.insert(id, check);
        reason->setTextFormat(Qt::PlainText);
        reason->setContentsMargins(Widgets::em(this) * 3 / 2, 0, 0, 0);
        lines->setContentsMargins(Widgets::em(this) * 3 / 2, 0, 0, 0);
        lines->hide();
        since->setTextFormat(Qt::PlainText);
        since->setContentsMargins(Widgets::em(this) * 3 / 2, 0, 0, 0);
        since->hide();
        toggle->setText(tr("Lines of vm.args"));
        toggle->setCheckable(true);
        toggle->setAutoRaise(true);
        toggle->setArrowType(Qt::RightArrow);
        toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        connect(toggle, &QToolButton::toggled, this, [toggle, lines, since](bool on) {
            toggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
            lines->setVisible(on);
            since->setVisible(on);
        });
        declineButton->setObjectName("decline-" + id);
        declineButton->setFlat(true);
        declineButton->setToolTip(tr("Not suggested again for this VM: Review… on its Details "
                                     "shows it again, with Ask Again"));
        connect(declineButton, &QPushButton::clicked, this, [this, id]() { decline(id); });
        actions->setContentsMargins(Widgets::em(this), 0, 0, 0);
        actions->addWidget(toggle);
        actions->addStretch();
        actions->addWidget(declineButton);
        column->addWidget(check);
        column->addWidget(reason);
        column->addLayout(actions);
        column->addWidget(lines);
        column->addWidget(since);
        m_rows->addWidget(row);
    }
    if (!declined.isEmpty()) {
        m_rows->addSpacing(Widgets::em(this) / 2);
        m_rows->addWidget(Widgets::heading(tr("Not Asked Again")));
        for (const Change &c : declined) {
            auto *row = new QWidget;
            auto *line = new QHBoxLayout(row);
            auto *title = new QLabel(c.title);
            auto *again = new QPushButton(tr("Ask Again"));
            const QString id = c.id;

            line->setContentsMargins(0, 0, 0, 0);
            title->setTextFormat(Qt::PlainText);
            title->setWordWrap(true);
            again->setObjectName("again-" + id);
            connect(again, &QPushButton::clicked, this, [this, id]() { askAgain(id); });
            line->addWidget(title, 1);
            line->addWidget(again);
            m_rows->addWidget(row);
        }
    }
    m_rows->addStretch();
    update();
}

void SettingsUpdatesDialog::update()
{
    m_noteText.clear();
    if (m_vm) {
        const QString blocked = SettingsUpdatesMonitor::of(m_vm)->blocked();
        if (m_vm->runner()->isActive()) {
            m_noteText = tr("Shut %1 down to apply them.").arg(m_vm->name());
        } else if (!blocked.isEmpty()) {
            m_noteText = sentence(blocked);
        } else if (m_stale) {
            m_noteText = tr("vm.args changed meanwhile: the list is up to date again.");
        }
    }
    m_note->setText(m_noteText.toHtmlEscaped());
    m_note->setVisible(!m_noteText.isEmpty());

    bool any = false;
    for (const Change &c : std::as_const(m_changes)) {
        any |= m_checked.value(c.id, true);
    }
    if (QPushButton *apply = applyButton()) {
        apply->setEnabled(any && m_vm &&
                          SettingsUpdatesMonitor::of(m_vm)->blocked().isEmpty());
    }
}

void SettingsUpdatesDialog::accept()
{
    QList<Change> chosen;
    QString error;
    bool stale = false;

    if (!m_vm) {
        QDialog::reject();
        return;
    }
    for (const Change &c : std::as_const(m_changes)) {
        if (m_checked.value(c.id, true)) {
            chosen << c;
        }
    }
    if (chosen.isEmpty()) {
        return;
    }
    if (SettingsUpdatesMonitor::of(m_vm)->apply(chosen, &error, &stale)) {
        QDialog::accept();
        return;
    }
    if (stale) {
        /* the monitor checked again: the list is new, and says so */
        m_stale = true;
        update();
        return;
    }
    Widgets::warn(this, tr("Cannot Apply the Settings"), sentence(error));
}

void SettingsUpdatesDialog::reject()
{
    if (m_vm) {
        SettingsUpdatesMonitor::of(m_vm)->setNotNow();
    }
    QDialog::reject();
}

void SettingsUpdatesDialog::decline(const QString &id)
{
    if (m_vm) {
        m_checked.remove(id);
        m_ownChange = true;
        SettingsUpdatesMonitor::of(m_vm)->setDeclined(id, true);
    }
}

void SettingsUpdatesDialog::askAgain(const QString &id)
{
    if (m_vm) {
        m_ownChange = true;
        SettingsUpdatesMonitor::of(m_vm)->setDeclined(id, false);
    }
}
