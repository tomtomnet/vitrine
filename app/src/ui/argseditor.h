// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QPlainTextEdit>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>

#include "ui/qemudocs.h"

class QCompleter;
class QListWidget;
class QStandardItemModel;
class QTimer;

/* What is wrong with the arguments, per line (0-based) */
struct ArgsProblem {
    int line;
    QString message;
};
/* With @vmDir, the files the arguments read must exist (relative to it) */
QList<ArgsProblem> checkArgs(const QString &text, const QemuInfo *info,
                             const QString &vmDir = {});

class ArgsHighlighter : public QSyntaxHighlighter
{
public:
    explicit ArgsHighlighter(QTextDocument *document);

    /* Unknown options are underlined, once @docs are loaded */
    void setDocs(QemuDocs *docs);
    /* After the palette changed */
    void updateFormats(const QPalette &palette);

protected:
    void highlightBlock(const QString &text) override;

private:
    void highlightKeys(const QString &text, int from);

    QemuDocs *m_docs = nullptr;
    QTextCharFormat m_option;
    QTextCharFormat m_unknown;
    QTextCharFormat m_key;
    QTextCharFormat m_comment;
    QTextCharFormat m_directive;
    QTextCharFormat m_invalid;
};

/*
 * A plain-text editor for the arguments, one option per line, highlighted
 * and checked against the QEMU documentation, with completion of option
 * and device names.
 */
class ArgsEditor : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit ArgsEditor(QWidget *parent = nullptr);

    /* The documentation for highlighting and completion */
    void setDocs(QemuDocs *docs);
    QemuDocs *docs() const { return m_docs; }
    /* On a line of its own after the cursor's, or on the cursor's if blank */
    void insertLine(const QString &line);
    void goToLine(int line);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    enum class Context { None, Option, Device };

    void complete(bool force);
    void insertCompletion(const QString &completion);
    void fillModel(Context context);

    QemuDocs *m_docs = nullptr;
    ArgsHighlighter *m_highlighter;
    QCompleter *m_completer;
    QStandardItemModel *m_model;
    Context m_context = Context::None;
};

/*
 * The editor with the list of problems under it.  It follows the #qemu
 * line of the text: the documentation is that of the QEMU the VM runs.
 */
class ArgsEditorPane : public QWidget
{
    Q_OBJECT

public:
    explicit ArgsEditorPane(QWidget *parent = nullptr);

    ArgsEditor *editor() const { return m_editor; }
    /* Where relative paths point: QEMU runs in the VM folder */
    void setVmDir(const QString &dir) { m_vmDir = dir; }
    /* Checks the text now, rather than after typing pauses */
    void check();

signals:
    void docsChanged(QemuDocs *docs);

private:
    void setDocs(QemuDocs *docs);

    QemuDocs *m_docs = nullptr;
    QString m_vmDir;
    ArgsEditor *m_editor;
    QListWidget *m_problems;
    QTimer *m_timer;
};
