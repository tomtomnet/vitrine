// SPDX-License-Identifier: GPL-2.0-or-later
#include "clonedialog.h"

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/vmcloner.h"
#include "core/vmstore.h"
#include "ui/widgets.h"

static QString bytesText(qint64 bytes)
{
    return bytes >= (1LL << 30) ? CloneDialog::tr("%1 GiB").arg(double(bytes) / (1LL << 30), 0, 'f', 1)
           : bytes >= (1LL << 20) ? CloneDialog::tr("%1 MiB").arg(bytes >> 20)
                                  : CloneDialog::tr("%1 KiB").arg(qMax<qint64>(1, bytes >> 10));
}

CloneDialog::CloneDialog(VmStore *store, Vm *source, QWidget *parent)
    : QDialog(parent), m_source(source), m_cloner(new VmCloner(store, this)),
      m_name(new QLineEdit), m_error(Widgets::note()), m_progress(new QProgressBar)
{
    auto *layout = new QVBoxLayout(this);
    auto *form = Widgets::form();
    const QList<std::pair<QString, qint64>> files = VmCloner::filesToCopy(source);
    QStringList names;
    qint64 total = 0;
    QString name = tr("%1 clone").arg(source->name());

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

    m_progress->setVisible(false);
    m_error->setVisible(false);
    layout->addWidget(m_progress);
    layout->addWidget(m_error);

    layout->addStretch();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    m_ok = buttons->addButton(tr("&Clone"), QDialogButtonBox::AcceptRole);
    m_ok->setDefault(true);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &CloneDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &CloneDialog::reject);
    connect(m_name, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_ok->setEnabled(!text.trimmed().isEmpty() && !m_cloner->isRunning());
    });

    connect(m_cloner, &VmCloner::progress, this, [this](qint64 done, qint64 total) {
        /* in MiB: QProgressBar counts in int */
        m_progress->setMaximum(int(qMax<qint64>(1, total >> 20)));
        m_progress->setValue(int(done >> 20));
    });
    connect(m_cloner, &VmCloner::finished, this, [this](Vm *clone, const QString &error) {
        m_progress->setVisible(false);
        m_name->setEnabled(true);
        m_ok->setEnabled(true);
        if (clone) {
            m_clone = clone;
            QDialog::accept();
        } else if (error != tr("Cancelled")) {
            m_error->setText(tr("<b>The VM could not be cloned:</b> %1").arg(error.toHtmlEscaped()));
            m_error->setVisible(true);
        }
    });
    /* the height of the wrapped notes at that width */
    resize(560, layout->totalHeightForWidth(560));
}

void CloneDialog::accept()
{
    if (m_cloner->isRunning() || m_name->text().trimmed().isEmpty()) {
        return;
    }
    m_error->setVisible(false);
    m_name->setEnabled(false);
    m_ok->setEnabled(false);
    m_progress->setMaximum(0);
    m_progress->setVisible(true);
    m_cloner->start(m_source, m_name->text().trimmed());
}

void CloneDialog::reject()
{
    /* the half-made clone goes */
    m_cloner->cancel();
    QDialog::reject();
}
