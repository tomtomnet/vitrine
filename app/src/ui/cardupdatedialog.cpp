// SPDX-License-Identifier: GPL-2.0-or-later
#include "cardupdatedialog.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFontDatabase>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QStyle>
#include <QTextLayout>
#include <QVBoxLayout>
#include <QtMath>

#include "core/paths.h"
#include "core/vmconfig.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "ui/banner.h"
#include "ui/icons.h"
#include "ui/qemudocs.h"
#include "ui/vmpane.h"
#include "ui/widgets.h"

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
      m_before(new LinesBox), m_after(new LinesBox), m_notes(Widgets::note()),
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
    Widgets::setButtonIcon(apply, Icons::themed({"dialog-ok-apply", "dialog-ok"},
                                                QStyle::SP_DialogApplyButton));
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

void CardUpdateDialog::fill(const QString &lead)
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
    static_cast<LinesBox *>(m_before)->setLines(diff.before, room);
    static_cast<LinesBox *>(m_after)->setLines(diff.after, room);

    if (!lead.isEmpty()) {
        notes << lead;
    }
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
        fill(tr("The VM's arguments changed meanwhile: these are the changes for them now."));
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
    /* the QEMU of the preferences, or a build of vitrine's, may offer otherwise */
    connect(QemuDocs::preferred(), &QemuDocs::changed, this, &CardUpdateBanner::refresh);
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
