// SPDX-License-Identifier: GPL-2.0-or-later
#include "clonedialog.h"

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include "core/tasks.h"
#include "core/vmcloner.h"
#include "core/vmstore.h"
#include "ui/icons.h"
#include "ui/taskbutton.h"
#include "ui/widgets.h"

static QString bytesText(qint64 bytes)
{
    return bytes >= (1LL << 30) ? CloneDialog::tr("%1 GiB").arg(double(bytes) / (1LL << 30), 0, 'f', 1)
           : bytes >= (1LL << 20) ? CloneDialog::tr("%1 MiB").arg(bytes >> 20)
                                  : CloneDialog::tr("%1 KiB").arg(qMax<qint64>(1, bytes >> 10));
}

CloneDialog::CloneDialog(VmStore *store, Vm *source, QWidget *parent)
    : QDialog(parent), m_source(source), m_cloner(new VmCloner(store, this)),
      m_name(new QLineEdit), m_error(Widgets::note()), m_progress(new QProgressBar),
      m_background(Widgets::hint())
{
    auto *layout = new QVBoxLayout(this);
    auto *form = Widgets::form();
    const QList<std::pair<QString, qint64>> files = VmCloner::filesToCopy(source);
    QStringList names;
    qint64 total = 0;
    QString name = tr("%1 clone").arg(source->name());

    setObjectName("cloneDialog");
    setWindowTitle(tr("Clone %1").arg(source->name()));

    /* a name no other VM has */
    auto taken = [store](const QString &n) {
        for (const Vm *vm : store->vms()) {
            if (vm->name() == n) {
                return true;
            }
        }
        return false;
    };
    for (int i = 2; taken(name); i++) {
        name = tr("%1 clone %2").arg(source->name()).arg(i);
    }
    m_name->setText(name);
    m_name->selectAll();
    form->addRow(tr("&Name:"), m_name);
    layout->addLayout(form);

    for (const auto &[file, bytes] : files) {
        names << QString("%1 (%2)").arg(QFileInfo(file).fileName(), bytesText(bytes));
        total += bytes;
    }
    layout->addWidget(Widgets::note(
        files.isEmpty()
            ? tr("The new VM gets the same settings. It has no disk to copy.")
            : tr("The new VM gets the same settings, and copies of what the VM writes, %1 "
                 "in all: %2. On btrfs and XFS, the copies take no time nor space until a "
                 "VM changes them.")
                  .arg(bytesText(total), names.join(", ").toHtmlEscaped())));
    layout->addWidget(Widgets::hint(
        tr("CD/DVD images and the other files the VM only reads are shared. The network "
           "cards get new addresses.")));

    m_error->setVisible(false);
    m_background->setTextFormat(Qt::PlainText);
    m_background->setText(tr("Closing this window does not stop the copy. %1 cannot start "
                             "until it is done.").arg(source->name()));
    layout->addWidget(m_progress);
    layout->addWidget(m_background);
    layout->addWidget(m_error);

    layout->addStretch();
    auto *buttons = new QDialogButtonBox;
    m_ok = buttons->addButton(tr("&Clone"), QDialogButtonBox::AcceptRole);
    m_stop = buttons->addButton(tr("&Stop"), QDialogButtonBox::ActionRole);
    m_cancel = buttons->addButton(QDialogButtonBox::Cancel);
    m_close = buttons->addButton(QDialogButtonBox::Close);
    m_ok->setDefault(true);
    /* as Machine > Clone, and as a build's Stop */
    Widgets::setButtonIcon(m_ok, Icons::themed({"edit-copy"}, QStyle::SP_FileDialogNewFolder));
    Widgets::setButtonIcon(m_stop, Icons::themed({"process-stop"}, QStyle::SP_BrowserStop));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &CloneDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &CloneDialog::reject);
    connect(m_stop, &QPushButton::clicked, m_cloner, &VmCloner::cancel);
    connect(m_name, &QLineEdit::textChanged, this, &CloneDialog::updateButtons);

    connect(m_cloner, &VmCloner::progress, this, [this](qint64 done, qint64 total) {
        /* in MiB: QProgressBar counts in int */
        m_progress->setMaximum(int(qMax<qint64>(1, total >> 20)));
        m_progress->setValue(int(done >> 20));
    });
    connect(m_cloner, &VmCloner::finished, this, &CloneDialog::finished);
    /* the copy's task: seen here once it is over, gone with it if the dialog is closed */
    TaskButton::followSeen(this, [this]() { return m_cloner->task(); });
    connect(TaskList::instance(), &TaskList::removed, this, [this](Task *task) {
        if (task == m_cloner->task()) {
            QTimer::singleShot(0, this, &CloneDialog::goIfDone);
        }
    });
    updateButtons();
    Widgets::resizeToWidth(this, 560);
}

CloneDialog::~CloneDialog()
{
    /* its task may stay: not with this dialog for its details */
    if (Task *task = m_cloner->task()) {
        task->setDetails({});
    }
}

Vm *CloneDialog::clone() const
{
    return m_clone;
}

bool CloneDialog::isCloning() const
{
    return m_cloner->isRunning();
}

void CloneDialog::accept()
{
    if (isCloning() || m_name->text().trimmed().isEmpty() || !m_source) {
        return;
    }
    m_error->setVisible(false);
    m_progress->setMaximum(0);
    m_progress->setValue(0);
    m_cloner->start(m_source, m_name->text().trimmed());
    /* refused at once: finished() said why */
    if (!isCloning()) {
        return;
    }
    if (Task *task = m_cloner->task()) {
        task->setDetails([this]() { present(); });
    }
    updateButtons();
    /* the progress and what closing does: room for them */
    Widgets::resizeToWidth(this, width());
}

void CloneDialog::reject()
{
    /* the copy goes on unseen: its task tells how far */
    if (isCloning()) {
        hide();
        return;
    }
    QDialog::reject();
}

void CloneDialog::finished(Vm *clone, const QString &error)
{
    const Task *task = m_cloner->task();

    updateButtons();
    if (clone) {
        m_clone = clone;
        m_made = true;
        if (isVisible()) {
            emit cloned(clone);
            QDialog::accept();
        }
        return;
    }
    /* Stop: as Cancel was */
    if (task && task->state() == Task::State::Stopped) {
        QDialog::reject();
        return;
    }
    m_error->setText(tr("<b>The VM could not be cloned:</b> %1").arg(error.toHtmlEscaped()));
    m_error->setVisible(true);
    Widgets::resizeToWidth(this, width());
}

void CloneDialog::present()
{
    /* the new VM is what there is to see */
    if (m_made) {
        if (m_clone) {
            emit cloned(m_clone);
        }
        return;
    }
    show();
    raise();
    activateWindow();
}

void CloneDialog::updateButtons()
{
    const bool cloning = isCloning();

    m_name->setEnabled(!cloning);
    m_ok->setVisible(!cloning);
    m_ok->setEnabled(!m_name->text().trimmed().isEmpty());
    m_cancel->setVisible(!cloning);
    m_stop->setVisible(cloning);
    m_close->setVisible(cloning);
    m_close->setDefault(cloning);
    m_ok->setDefault(!cloning);
    m_progress->setVisible(cloning);
    m_background->setVisible(cloning);
}

void CloneDialog::hideEvent(QHideEvent *event)
{
    QDialog::hideEvent(event);
    QTimer::singleShot(0, this, &CloneDialog::goIfDone);
}

void CloneDialog::goIfDone()
{
    const Task *task = m_cloner->task();

    if (isVisible() || isCloning() || (task && TaskList::instance()->tasks().contains(task))) {
        return;
    }
    deleteLater();
}
