// SPDX-License-Identifier: GPL-2.0-or-later
#include "importdialog.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>

#include "core/firmware.h"
#include "core/qemuinfo.h"
#include "core/vmconfig.h"
#include "core/vmstore.h"
#include "ui/banner.h"
#include "ui/widgets.h"

ImportDialog::ImportDialog(VmStore *store, const QemuInfo *info, QWidget *parent)
    : QDialog(parent), m_store(store), m_info(info)
{
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    auto *fileRow = new QHBoxLayout;
    auto *open = new QPushButton(tr("Open Script…"));
    auto *splitter = new Splitter(Qt::Vertical);
    const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    setWindowTitle(tr("Import a VM"));

    auto *intro = new QLabel(tr("Open the script you start the VM with, or paste its QEMU "
                                "command line. The new VM gets the options of the command. "
                                "Its disks stay where they are; its firmware files are "
                                "copied into its folder, to go wherever it goes."));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    m_script = new QPlainTextEdit;
    m_script->setFont(fixed);
    m_script->setPlaceholderText("qemu-system-x86_64 -machine q35 -m 8G …");
    m_script->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_preview = new QPlainTextEdit;
    m_preview->setFont(fixed);
    m_preview->setReadOnly(true);
    m_preview->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_preview->setPlaceholderText(tr("The arguments of the new VM"));
    splitter->addWidget(m_script);
    splitter->addWidget(m_preview);
    layout->addWidget(splitter, 1);

    m_missing = new Banner(Banner::Warning);
    m_missing->button()->setText(tr("Choose Their &Folder…"));
    m_missing->button()->show();
    m_missing->hide();
    layout->addWidget(m_missing);
    m_notes = new QLabel;
    m_notes->setWordWrap(true);
    m_notes->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_notes);

    m_baseDir = new QLineEdit(QDir::homePath());
    m_baseDir->setToolTip(tr("Where the script runs from: relative paths in the command are "
                             "relative to this folder"));
    m_name = new QLineEdit;
    fileRow->addWidget(m_baseDir);
    fileRow->addWidget(open);
    form->addRow(tr("Runs from:"), fileRow);
    form->addRow(tr("Name:"), m_name);
    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    m_create = buttons->addButton(tr("Create VM"), QDialogButtonBox::AcceptRole);
    layout->addWidget(buttons);

    connect(open, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(this, tr("Open Script"),
                                                          m_baseDir->text());
        if (!path.isEmpty()) {
            load(path);
        }
    });
    /* the script runs from where the files are */
    connect(m_missing->button(), &QPushButton::clicked, this, [this]() {
        const QString dir = QFileDialog::getExistingDirectory(
            this, tr("The Folder of %1").arg(m_result ? m_result->missing.value(0) : QString()),
            m_baseDir->text());
        if (!dir.isEmpty()) {
            m_baseDir->setText(dir);
        }
    });
    connect(m_script, &QPlainTextEdit::textChanged, this, &ImportDialog::update);
    connect(m_baseDir, &QLineEdit::textChanged, this, &ImportDialog::update);
    connect(m_name, &QLineEdit::textEdited, this, [this]() { m_nameEdited = true; });
    connect(m_name, &QLineEdit::textChanged, this, [this]() {
        m_create->setEnabled(m_result && !m_name->text().trimmed().isEmpty());
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &ImportDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    update();
    resize(760, 640);
}

void ImportDialog::load(const QString &path)
{
    QFile f(path);

    if (!f.open(QIODevice::ReadOnly)) {
        Widgets::warn(this, windowTitle(), f.errorString());
        return;
    }
    m_baseDir->setText(QFileInfo(path).absolutePath());
    if (!m_nameEdited) {
        /* start_fedora-kde.sh: fedora-kde */
        QString name = QFileInfo(path).completeBaseName();
        for (const char *prefix : {"start_", "start-", "run_", "run-"}) {
            if (name.startsWith(prefix) && name.size() > int(strlen(prefix))) {
                name = name.mid(int(strlen(prefix)));
            }
        }
        m_name->setText(name);
    }
    m_script->setPlainText(QString::fromUtf8(f.readAll()));
}

void ImportDialog::update()
{
    std::function<bool(const QString &)> takesValue;

    if (m_info && !m_info->options.isEmpty()) {
        takesValue = [this](const QString &name) {
            const QemuOptionDoc *o = m_info->option(name);
            return o ? o->takesValue : true;
        };
    }
    m_result = Importer::importScript(m_script->toPlainText(), m_baseDir->text(), takesValue);

    if (!m_result) {
        m_preview->clear();
        m_notes->setText(m_script->toPlainText().trimmed().isEmpty()
                             ? QString()
                             : tr("No command runs QEMU (qemu-system-…) in this text."));
    } else {
        const QString name = VmConfig::name(m_result->args);
        if (!name.isEmpty() && !m_nameEdited) {
            m_name->setText(name);
        }
        m_preview->setPlainText(m_result->args.toText());
        QStringList notes;
        for (const QString &note : std::as_const(m_result->notes)) {
            notes << "• " + note.toHtmlEscaped();
        }
        m_notes->setText(notes.join("<br>"));
    }
    if (m_result && !m_result->missing.isEmpty()) {
        m_missing->setText(tr("<b>Not found in %1:</b> %2. Choose the folder the script "
                              "runs from, where they are; otherwise the VM looks for them "
                              "in its own folder.")
                               .arg(m_baseDir->text().toHtmlEscaped(),
                                    m_result->missing.join(", ").toHtmlEscaped()));
        m_missing->show();
    } else {
        m_missing->hide();
    }
    m_create->setEnabled(m_result && !m_name->text().trimmed().isEmpty());
}

void ImportDialog::accept()
{
    QString error;
    ArgsFile args;

    if (!m_result) {
        return;
    }
    args = m_result->args;
    VmConfig::setName(args, m_name->text().trimmed());
    m_vm = m_store->create(m_name->text().trimmed(), &error);
    if (!m_vm || !FirmwareDb::copyIntoVm(args, m_vm->dir(), nullptr, &error) ||
        !m_vm->save(args, &error)) {
        if (m_vm) {
            m_store->remove(m_vm);
            m_vm = nullptr;
        }
        Widgets::warn(this, windowTitle(), tr("Cannot create the VM: %1").arg(error));
        return;
    }
    QDialog::accept();
}
