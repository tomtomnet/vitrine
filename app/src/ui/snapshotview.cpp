// SPDX-License-Identifier: GPL-2.0-or-later
#include "snapshotview.h"

#include <QCollator>
#include <QDateTime>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "core/snapshots.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "ui/banner.h"
#include "ui/icons.h"
#include "ui/widgets.h"

enum Column { Name, Taken, Kind, State, Clock };

/* A cell that sorts by a key, such as the time a snapshot was taken, not by its text */
class SortItem : public QTableWidgetItem
{
public:
    SortItem(const QString &text, const QVariant &key) : QTableWidgetItem(text)
    {
        setData(Qt::UserRole, key);
    }

    bool operator<(const QTableWidgetItem &other) const override
    {
        const QVariant a = data(Qt::UserRole);
        const QVariant b = other.data(Qt::UserRole);

        /* names as people sort them: "snap 2" before "snap 10", whatever the case */
        if (a.typeId() == QMetaType::QString) {
            static const QCollator collator = [] {
                QCollator c;
                c.setNumericMode(true);
                c.setCaseSensitivity(Qt::CaseInsensitive);
                return c;
            }();
            return collator.compare(a.toString(), b.toString()) < 0;
        }
        return a.toLongLong() < b.toLongLong();
    }
};

static QString clockText(qint64 ms)
{
    const qint64 s = ms / 1000;

    return QString("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QChar('0'))
        .arg(s % 60, 2, 10, QChar('0'));
}

SnapshotView::SnapshotView(QWidget *parent)
    : QWidget(parent), m_drives(new Banner(Banner::Warning)),
      m_error(new Banner(Banner::Warning)), m_notice(new Banner(Banner::Information)),
      m_table(new QTableWidget(0, 5)),
      m_progress(Widgets::hint()),
      m_take(new QPushButton(Icons::themed({"camera-photo", "document-save"},
                                           QStyle::SP_DialogSaveButton),
                             tr("&Take…"))),
      m_restore(new QPushButton(Icons::themed({"edit-undo", "document-revert"},
                                              QStyle::SP_BrowserReload),
                                tr("&Restore"))),
      m_start(new QPushButton(Icons::themed({"media-playback-start"}, QStyle::SP_MediaPlay),
                              tr("Start &From It"))),
      m_delete(new QPushButton(Icons::themed({"edit-delete", "list-remove"},
                                             QStyle::SP_TrashIcon),
                               tr("De&lete…")))
{
    auto *layout = new QVBoxLayout(this);
    auto *tableRow = new QHBoxLayout;
    auto *buttons = new QVBoxLayout;

    m_table->setObjectName("snapshots");
    m_table->setHorizontalHeaderLabels(
        {tr("Name"), tr("Taken"), tr("Kind"), tr("Running state"), tr("VM clock")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 5; column++) {
        m_table->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    }
    m_table->verticalHeader()->hide();
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setWordWrap(false);
    /* the latest first; a click on a column sorts by it, and again the other way */
    m_table->horizontalHeaderItem(Taken)->setData(Qt::InitialSortOrderRole, Qt::DescendingOrder);
    m_table->setSortingEnabled(true);
    m_table->sortByColumn(Taken, Qt::DescendingOrder);
    m_take->setObjectName("take");
    m_restore->setObjectName("restore");
    m_start->setObjectName("startFrom");
    m_delete->setObjectName("delete");
    m_drives->setObjectName("drives");
    m_error->setObjectName("snapshotError");
    m_notice->setObjectName("snapshotNotice");
    m_progress->setObjectName("progress");
    m_restore->setToolTip(tr("Take the VM back to where it was"));
    m_start->setToolTip(tr("Start the VM where it was when the snapshot was taken"));

    buttons->addWidget(m_take);
    buttons->addSpacing(buttons->spacing() * 2);
    buttons->addWidget(m_restore);
    buttons->addWidget(m_start);
    buttons->addWidget(m_delete);
    buttons->addStretch();
    tableRow->addWidget(m_table, 1);
    tableRow->addLayout(buttons);

    layout->addWidget(Widgets::note(
        tr("A snapshot keeps the disks of the VM as they are, inside their qcow2 files, and "
           "takes no space until the VM writes over what it keeps. Taken while the VM runs, it "
           "keeps the running state too, its memory, and the VM can go back to that moment.")));
    layout->addWidget(m_drives);
    layout->addWidget(m_error);
    layout->addWidget(m_notice);
    layout->addLayout(tableRow, 1);
    layout->addWidget(m_progress);
    m_drives->hide();
    m_error->hide();
    m_notice->hide();
    m_progress->hide();

    connect(m_table, &QTableWidget::itemSelectionChanged, this, &SnapshotView::updateButtons);
    connect(m_take, &QPushButton::clicked, this, &SnapshotView::take);
    connect(m_restore, &QPushButton::clicked, this, &SnapshotView::restore);
    connect(m_delete, &QPushButton::clicked, this, &SnapshotView::remove);
    connect(m_start, &QPushButton::clicked, this, [this]() {
        if (!selected().isEmpty()) {
            emit startRequested(selected());
        }
    });
    updateButtons();
}

void SnapshotView::setVm(Vm *vm)
{
    if (vm == m_vm) {
        return;
    }
    if (m_vm) {
        disconnect(m_vm, nullptr, this, nullptr);
        disconnect(m_vm->runner(), nullptr, this, nullptr);
    }
    if (m_snapshots) {
        m_snapshots->disconnect(this);
        if (m_snapshots->isBusy()) {
            /* QEMU or qemu-img is at it: it goes once they are done */
            connect(m_snapshots, &VmSnapshots::finished, m_snapshots, &QObject::deleteLater);
        } else {
            delete m_snapshots;
        }
        m_snapshots = nullptr;
    }
    m_vm = vm;
    m_table->setRowCount(0);
    m_error->hide();
    m_notice->hide();
    m_progress->hide();
    unsetCursor();
    if (vm) {
        m_snapshots = new VmSnapshots(vm->runner(), this);
        m_snapshots->setVm(vm->args(), vm->dir());
        connect(m_snapshots, &VmSnapshots::listed, this, [this](const QString &error) {
            if (!error.isEmpty()) {
                m_error->setText(error.toHtmlEscaped());
                m_error->show();
            }
            fill();
        });
        connect(m_snapshots, &VmSnapshots::finished, this, [this](const QString &error) {
            m_progress->hide();
            unsetCursor();
            m_error->setText(error.toHtmlEscaped());
            m_error->setVisible(!error.isEmpty());
            updateButtons();
        });
        connect(m_snapshots, &VmSnapshots::busyChanged, this, &SnapshotView::updateButtons);
        connect(m_snapshots, &VmSnapshots::notice, this, [this](const QString &text) {
            m_notice->setText(text.toHtmlEscaped());
            m_notice->show();
        });
        connect(vm, &Vm::changed, this, [this]() {
            m_snapshots->setVm(m_vm->args(), m_vm->dir());
            updateDrives();
        });
        /* QEMU or qemu-img, a new list */
        connect(vm->runner(), &VmRunner::stateChanged, this, [this]() {
            updateDrives();
            updateButtons();
            if (isVisible()) {
                m_snapshots->refresh();
            }
        });
        if (isVisible()) {
            m_snapshots->refresh();
        }
    }
    updateDrives();
    updateButtons();
}

void SnapshotView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (m_snapshots && !m_snapshots->isBusy()) {
        m_snapshots->refresh();
    }
}

void SnapshotView::fill()
{
    const QString current = selected();
    const QList<VmSnapshots::Snapshot> list = m_snapshots->snapshots();
    const QLocale locale;

    /* rows would move as they go in: sorted once all are in, by the column chosen */
    m_table->setSortingEnabled(false);
    m_table->setRowCount(0);
    for (const VmSnapshots::Snapshot &s : list) {
        const int row = m_table->rowCount();
        const bool state = s.stateBytes > 0;
        QStringList files;
        for (const QString &file : s.files) {
            files << QFileInfo(file).fileName();
        }
        m_table->insertRow(row);
        m_table->setItem(row, Name, new SortItem(s.name, s.name));
        m_table->setItem(row, Taken, new SortItem(locale.toString(s.date, QLocale::ShortFormat),
                                                  s.date.toMSecsSinceEpoch()));
        m_table->setItem(row, Kind, new SortItem(state ? tr("Disks and running state")
                                                       : tr("Disks only"),
                                                 state ? 1 : 0));
        m_table->setItem(row, State,
                         new SortItem(state ? locale.formattedDataSize(s.stateBytes, 1,
                                                                       QLocale::DataSizeIecFormat)
                                            : QString("—"),
                                      s.stateBytes));
        m_table->setItem(row, Clock, new SortItem(state ? clockText(s.vmClockMs) : QString("—"),
                                                  state ? s.vmClockMs : -1));
        for (int column = 0; column < 5; column++) {
            m_table->item(row, column)->setToolTip(tr("In %1").arg(files.join(", ")));
        }
        if (s.name == current) {
            m_table->selectRow(row);
        }
    }
    m_table->setSortingEnabled(true);
    updateButtons();
}

void SnapshotView::updateButtons()
{
    const bool busy = !m_snapshots || m_snapshots->isBusy();
    const VmRunner::State state = m_vm ? m_vm->runner()->state() : VmRunner::State::Stopped;
    const bool live = state == VmRunner::State::Running || state == VmRunner::State::Paused;
    const bool stopped = state == VmRunner::State::Stopped;
    const QString name = selected();
    bool withState = false;

    if (m_snapshots) {
        for (const VmSnapshots::Snapshot &s : m_snapshots->snapshots()) {
            withState |= s.name == name && s.stateBytes > 0;
        }
    }
    m_take->setEnabled(!busy && (live || stopped) && !m_cannotTake);
    /* a running VM goes back to the running state, not the disks alone */
    m_restore->setEnabled(!busy && !name.isEmpty() && (stopped || (live && withState)));
    m_restore->setToolTip(live && !name.isEmpty() && !withState
                              ? tr("This snapshot holds the disks only: stop the VM to go back "
                                   "to it")
                              : tr("Take the VM back to where it was"));
    m_start->setEnabled(!busy && !name.isEmpty() && stopped && withState);
    m_delete->setEnabled(!busy && !name.isEmpty() && (live || stopped));
}

/* The files that cannot keep snapshots, said above the table */
void SnapshotView::updateDrives()
{
    QStringList cannot;
    int can = 0;
    const bool live = m_vm && m_vm->runner()->isActive();

    m_cannotTake = false;
    if (!m_vm) {
        m_drives->hide();
        return;
    }
    for (const VmSnapshots::Drive &drive : VmSnapshots::drives(m_vm->args(), m_vm->dir())) {
        if (drive.canSnapshot()) {
            can++;
        } else {
            cannot << (drive.pflash ? tr("the UEFI variables (%1)").arg(QFileInfo(drive.file).fileName())
                                    : QFileInfo(drive.file).fileName());
        }
    }
    if (can == 0) {
        m_cannotTake = true;
        m_drives->setText(tr("The VM has no qcow2 disk to keep snapshots in. Only qcow2 files "
                             "can hold them; qemu-img convert makes one of a disk."));
    } else if (!cannot.isEmpty() && live) {
        m_drives->setText(tr("Snapshots leave out %1, which is not in qcow2, and hold the "
                             "disks only: QEMU cannot save the running state then.")
                              .arg(cannot.join(", ").toHtmlEscaped()));
    } else if (!cannot.isEmpty()) {
        m_drives->setText(tr("Snapshots leave out %1, which is not in qcow2.")
                              .arg(cannot.join(", ").toHtmlEscaped()));
    }
    m_drives->setVisible(can == 0 || !cannot.isEmpty());
}

QString SnapshotView::selected() const
{
    const QList<QTableWidgetItem *> items = m_table->selectedItems();

    return items.isEmpty() ? QString() : m_table->item(items.first()->row(), 0)->text();
}

void SnapshotView::busy(const QString &what)
{
    m_error->hide();
    m_notice->hide();
    m_progress->setText(what);
    m_progress->show();
    setCursor(Qt::BusyCursor);
}

void SnapshotView::take()
{
    const bool live = m_vm && m_vm->runner()->isActive();
    QString name = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
    bool ok = false;

    for (;;) {
        name = QInputDialog::getText(
            this, tr("Take a Snapshot"),
            live ? tr("Name of the snapshot, with the running state of the VM:")
                 : tr("Name of the snapshot of the disks:"),
            QLineEdit::Normal, name, &ok);
        if (!ok || !m_snapshots) {
            return;
        }
        if (VmSnapshots::isValidName(name)) {
            break;
        }
        m_error->setText(tr("A snapshot needs a name, not only digits, which QEMU would take "
                            "for the number of another, and no spaces around it."));
        m_error->show();
    }
    for (const VmSnapshots::Snapshot &s : m_snapshots->snapshots()) {
        if (s.name == name &&
            !Widgets::confirm(this, QMessageBox::Question, tr("Replace %1?").arg(name),
                              tr("A snapshot of this name exists: the new one replaces it."),
                              tr("&Replace"))) {
            return;
        }
    }
    busy(live ? tr("Taking the snapshot: QEMU pauses the VM while it saves the running "
                   "state…")
              : tr("Taking the snapshot…"));
    m_snapshots->take(name);
}

void SnapshotView::restore()
{
    const QString name = selected();
    const bool live = m_vm && m_vm->runner()->isActive();

    if (name.isEmpty() ||
        !Widgets::confirm(this, QMessageBox::Warning, tr("Go Back to %1?").arg(name),
                          live ? tr("The VM goes back to where it was when the snapshot was "
                                    "taken: what it did since is lost.")
                               : tr("The disks go back to what they held when the snapshot "
                                    "was taken: what was written since is lost."),
                          tr("&Restore"))) {
        return;
    }
    busy(tr("Going back to %1…").arg(name));
    m_snapshots->restore(name);
}

void SnapshotView::remove()
{
    const QString name = selected();

    if (name.isEmpty() ||
        !Widgets::confirm(this, QMessageBox::Question, tr("Delete %1?").arg(name),
                          tr("The VM can no longer go back to it. The space it takes in the "
                             "qcow2 files is reused."),
                          tr("&Delete"))) {
        return;
    }
    busy(tr("Deleting %1…").arg(name));
    m_snapshots->remove(name);
}
