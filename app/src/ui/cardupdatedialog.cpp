// SPDX-License-Identifier: GPL-2.0-or-later
#include "cardupdatedialog.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFontDatabase>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QStyle>
#include <QVBoxLayout>
#include <QtMath>

#include "core/paths.h"
#include "core/vmconfig.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "ui/banner.h"
#include "ui/vmpane.h"
#include "ui/widgets.h"

/* Argument lines, as written, in a box that wraps them anywhere */
static QPlainTextEdit *linesBox()
{
    auto *box = new QPlainTextEdit;

    box->setReadOnly(true);
    box->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    box->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    return box;
}

/*
 * @lines in @box, as wide as the longest (the card's) where @maxWidth
 * allows, and as tall as they are once wrapped: no scrolling to see them
 */
static void setLines(QPlainTextEdit *box, const QStringList &lines, int maxWidth)
{
    const QFontMetrics fm(box->font());
    const int margins = 2 * (box->frameWidth() + qCeil(box->document()->documentMargin()));
    const int room = qMax(fm.averageCharWidth() * 40, maxWidth - margins);
    int width = 0;
    int rows = 0;

    for (const QString &line : lines) {
        const int advance = fm.horizontalAdvance(line);
        width = qMax(width, advance);
        rows += qMax(1, (advance + room - 1) / room);
    }
    box->setPlainText(lines.join('\n'));
    box->setMinimumWidth(qMin(width + fm.averageCharWidth(), room) + margins);
    box->setFixedHeight(qMax(rows, 1) * fm.lineSpacing() + margins + fm.descent());
}

void CardUpdateDialog::run(QWidget *from, Vm *vm)
{
    if (!vm) {
        return;
    }
    for (QWidget *w = from; w; w = w->parentWidget()) {
        if (auto *pane = qobject_cast<VmPane *>(w)) {
            if (pane->vm() == vm &&
                !pane->confirmChanges(tr("Apply them before updating the 3D card?"))) {
                return;
            }
            break;
        }
    }
    /* the settings just applied may have changed the card */
    if (CardUpdate::changes(vm->args()).isEmpty()) {
        return;
    }
    auto *dialog = new CardUpdateDialog(vm, from ? from->window() : nullptr);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}

CardUpdateDialog::CardUpdateDialog(Vm *vm, QWidget *parent)
    : QDialog(parent), m_vm(vm), m_intro(Widgets::note()), m_list(Widgets::note()),
      m_before(linesBox()), m_after(linesBox()), m_notes(Widgets::note()),
      m_buttons(new QDialogButtonBox(QDialogButtonBox::Cancel))
{
    auto *layout = new QVBoxLayout(this);
    QPushButton *apply = m_buttons->addButton(tr("&Apply"), QDialogButtonBox::AcceptRole);

    setWindowTitle(tr("Update the 3D Card"));
    /* always rich: guessing would show the entities of an escaped line */
    for (QLabel *label : {m_intro, m_list, m_notes}) {
        label->setTextFormat(Qt::RichText);
    }
    m_list->setObjectName("changes");
    m_before->setObjectName("before");
    m_after->setObjectName("after");
    m_notes->setObjectName("notes");
    apply->setObjectName("apply");
    apply->setDefault(true);
    layout->addWidget(m_intro);
    layout->addWidget(m_list);
    layout->addWidget(Widgets::heading(tr("Before")));
    layout->addWidget(m_before);
    layout->addWidget(Widgets::heading(tr("After")));
    layout->addWidget(m_after);
    layout->addWidget(m_notes);
    layout->addWidget(m_buttons);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &CardUpdateDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &CardUpdateDialog::reject);
    fill();
}

void CardUpdateDialog::fill()
{
    QStringList notes;
    QString items;

    if (!m_vm) {
        return;
    }
    m_args = m_vm->args();
    m_changes = CardUpdate::changes(m_args);
    const ArgsFile after = CardUpdate::apply(m_args, m_changes);
    const CardUpdate::LineDiff diff = CardUpdate::diff(m_args, after);

    m_intro->setText(tr("New Linux VMs get these settings for their 3D card, which %1 lacks:")
                         .arg(m_vm->name().toHtmlEscaped()));
    for (const CardUpdate::Change &c : std::as_const(m_changes)) {
        items += "<li>" + CardUpdate::describe(c).toHtmlEscaped() + "</li>";
    }
    m_list->setText("<ul>" + items + "</ul>");
    /* most of the screen at most, beside the dialog's margins */
    const QScreen *screen = parentWidget() ? parentWidget()->screen() : this->screen();
    const int room = screen->availableGeometry().width() * 4 / 5 -
                     2 * style()->pixelMetric(QStyle::PM_LayoutLeftMargin);
    setLines(m_before, diff.before, room);
    setLines(m_after, diff.after, room);

    notes << tr("The other lines and keys stay as they are.");
    if (m_vm->runner()->isActive()) {
        notes << tr("The VM is running: the changes apply the next time it starts.");
    }
    /* what the system's QEMU, standing in for vitrine's, lacks */
    if (VmRunner::needsQemuBuild(after) && !VmRunner::needsQemuBuild(m_args)) {
        notes << tr("The VM then waits for Vitrine's QEMU, which is not built yet: "
                    "File > Build QEMU builds it.");
    }
    m_notes->setText(notes.join(' ').toHtmlEscaped());
    if (QPushButton *apply = findChild<QPushButton *>("apply")) {
        apply->setEnabled(!m_changes.isEmpty());
    }
    adjustSize();
}

void CardUpdateDialog::accept()
{
    QString error;

    if (!m_vm) {
        reject();
        return;
    }
    /* vm.args edited meanwhile, say by hand: shown again, not applied unseen */
    if (m_vm->args().toText() != m_args.toText()) {
        fill();
        m_notes->setText(tr("The VM's arguments changed meanwhile: these are the changes "
                            "for them now.").toHtmlEscaped());
        return;
    }
    if (!m_vm->save(CardUpdate::apply(m_args, m_changes), &error)) {
        Widgets::warn(this, tr("Cannot Update the 3D Card"), error);
        return;
    }
    QDialog::accept();
}

/* Every banner, to follow Don't Ask Again wherever it was said */
static QList<QPointer<CardUpdateBanner>> &banners()
{
    static QList<QPointer<CardUpdateBanner>> list;
    return list;
}

CardUpdateBanner::CardUpdateBanner(QWidget *parent)
    : QWidget(parent), m_banner(new Banner(Banner::Information))
{
    auto *layout = new QVBoxLayout(this);
    QPushButton *never = m_banner->addButton(tr("Don't Ask Again"));

    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_banner);
    m_banner->setText(tr("This VM's 3D card lacks Vitrine's settings for native context and "
                         "smooth frames.")
                          .toHtmlEscaped());
    m_banner->button()->setText(tr("Update…"));
    m_banner->button()->setObjectName("updateCard");
    m_banner->button()->show();
    never->setObjectName("dontAskAgain");
    never->setToolTip(tr("The Details tab can offer it again"));
    connect(m_banner->button(), &QPushButton::clicked, this,
            [this]() { CardUpdateDialog::run(this, m_vm); });
    connect(never, &QPushButton::clicked, this, [this]() {
        if (m_vm) {
            setDeclined(m_vm->id(), true);
        }
    });
    banners().removeAll(nullptr);
    banners() << this;
    hide();
}

void CardUpdateBanner::setVm(Vm *vm)
{
    if (m_vm == vm) {
        refresh();
        return;
    }
    if (m_vm) {
        disconnect(m_vm, nullptr, this, nullptr);
    }
    m_vm = vm;
    if (vm) {
        connect(vm, &Vm::changed, this, &CardUpdateBanner::refresh);
    }
    m_key.clear();
    refresh();
}

void CardUpdateBanner::refresh()
{
    const bool declined = isDeclined();

    if (!m_vm) {
        m_key.clear();
        m_changes.clear();
        hide();
    } else {
        /*
         * Asked again only when it may answer otherwise: a chosen QEMU is
         * run to ask it, and Details refreshes every few seconds
         */
        const ArgsFile &args = m_vm->args();
        QString qemu = VmConfig::qemuBinary(args);
        if (qemu.isEmpty()) {
            qemu = Paths::customQemuBinary();
        }
        const QString key =
            QStringList{args.toText(), qemu,
                        QString::number(QFileInfo(qemu).lastModified().toMSecsSinceEpoch()),
                        Paths::stackQemu()}
                .join('\n');
        if (key != m_key) {
            m_key = key;
            m_changes = CardUpdate::changes(args);
        }
        setVisible(!m_changes.isEmpty() && !CardUpdate::isDeclined(m_vm->id()));
    }
    if (isDeclined() != declined) {
        emit declinedChanged();
    }
}

bool CardUpdateBanner::isDeclined() const
{
    return m_vm && !m_changes.isEmpty() && CardUpdate::isDeclined(m_vm->id());
}

void CardUpdateBanner::setDeclined(const QString &vmId, bool declined)
{
    CardUpdate::setDeclined(vmId, declined);
    for (const QPointer<CardUpdateBanner> &banner : QList(banners())) {
        if (banner && banner->m_vm && banner->m_vm->id() == vmId) {
            banner->refresh();
            emit banner->declinedChanged();
        }
    }
}
