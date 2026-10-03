// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDialog>

class QPlainTextEdit;

/* Read-only text to copy, such as a command line */
class TextDialog : public QDialog
{
    Q_OBJECT

public:
    static void showText(QWidget *parent, const QString &title, const QString &text);

private:
    TextDialog(QWidget *parent, const QString &title);

    QPlainTextEdit *m_text;
};
