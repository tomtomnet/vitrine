// SPDX-License-Identifier: GPL-2.0-or-later
#include "textdialog.h"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "ui/icons.h"

TextDialog::TextDialog(QWidget *parent, const QString &title)
    : QDialog(parent), m_text(new QPlainTextEdit)
{
    auto *layout = new QVBoxLayout(this);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    QPushButton *copy = buttons->addButton(tr("&Copy"), QDialogButtonBox::ActionRole);

    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(title);
    m_text->setReadOnly(true);
    m_text->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    layout->addWidget(m_text);
    layout->addWidget(buttons);

    copy->setIcon(Icons::themed({"edit-copy"}, QStyle::SP_FileIcon));
    connect(copy, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(m_text->toPlainText());
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    resize(900, 560);
}

void TextDialog::showText(QWidget *parent, const QString &title, const QString &text)
{
    auto *dialog = new TextDialog(parent, title);
    dialog->m_text->setPlainText(text);
    dialog->show();
}
