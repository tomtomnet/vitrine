// SPDX-License-Identifier: GPL-2.0-or-later
#include "logview.h"

#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "ui/icons.h"

static const int kPollMs = 500;
/* the end of a long log: the runs before the last are rarely read */
static const qint64 kTailBytes = 1 << 20;
static const int kMaxLines = 20000;

LogView::LogView(QWidget *parent)
    : QWidget(parent), m_text(new QPlainTextEdit),
      m_clear(new QPushButton(Icons::themed({"edit-clear-history", "edit-clear"},
                                            QStyle::SP_DialogResetButton),
                              tr("C&lear"))),
      m_open(new QPushButton(Icons::themed({"document-open", "text-x-generic"},
                                           QStyle::SP_FileIcon),
                             tr("&Open Externally"))),
      m_timer(new QTimer(this))
{
    auto *layout = new QVBoxLayout(this);
    auto *buttons = new QHBoxLayout;

    m_text->setObjectName("log");
    m_text->setReadOnly(true);
    m_text->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_text->setMaximumBlockCount(kMaxLines);
    m_text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_text->setPlaceholderText(tr("Nothing yet: QEMU writes here once the VM starts."));
    m_clear->setToolTip(tr("Empty the log file"));
    buttons->addStretch();
    buttons->addWidget(m_clear);
    buttons->addWidget(m_open);
    layout->addWidget(m_text, 1);
    layout->addLayout(buttons);

    m_timer->setInterval(kPollMs);
    connect(m_timer, &QTimer::timeout, this, &LogView::poll);
    connect(m_clear, &QPushButton::clicked, this, [this]() {
        QFile f(m_path);
        /* QEMU goes on at the new end: it appends */
        if (f.open(QIODevice::ReadWrite)) {
            f.resize(0);
        }
        reload();
    });
    connect(m_open, &QPushButton::clicked, this, [this]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_path));
    });
}

void LogView::setPath(const QString &path)
{
    if (path == m_path) {
        return;
    }
    m_path = path;
    if (isVisible()) {
        reload();
    } else {
        m_text->clear();
        m_offset = 0;
        m_partial.clear();
    }
}

void LogView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    reload();
    m_timer->start();
}

void LogView::hideEvent(QHideEvent *event)
{
    m_timer->stop();
    QWidget::hideEvent(event);
}

void LogView::reload()
{
    QFile f(m_path);

    m_text->clear();
    m_offset = 0;
    m_partial.clear();
    if (m_path.isEmpty() || !f.open(QIODevice::ReadOnly)) {
        m_clear->setEnabled(false);
        m_open->setEnabled(false);
        return;
    }
    /* from a line start */
    if (f.size() > kTailBytes) {
        f.seek(f.size() - kTailBytes);
        f.readLine();
    }
    m_offset = f.pos();
    append(f.readAll());
    m_offset = f.pos();
    m_text->verticalScrollBar()->setValue(m_text->verticalScrollBar()->maximum());
}

void LogView::poll()
{
    const QFileInfo info(m_path);
    QFile f(m_path);

    if (!info.exists() || info.size() < m_offset) {
        /* gone, emptied or started over */
        if (m_offset > 0 || !m_text->document()->isEmpty() || info.exists()) {
            reload();
        }
        return;
    }
    if (info.size() == m_offset || !f.open(QIODevice::ReadOnly) || !f.seek(m_offset)) {
        return;
    }
    append(f.readAll());
    m_offset = f.pos();
}

/* the complete lines of @bytes, and what came before them */
void LogView::append(const QByteArray &bytes)
{
    QScrollBar *bar = m_text->verticalScrollBar();
    const bool atEnd = bar->value() == bar->maximum();
    QByteArray text = m_partial + bytes;
    const qsizetype end = text.lastIndexOf('\n');

    m_clear->setEnabled(true);
    m_open->setEnabled(true);
    if (end < 0) {
        m_partial = text;
        return;
    }
    m_partial = text.mid(end + 1);
    text.truncate(end);
    m_text->appendPlainText(QString::fromUtf8(text));
    if (atEnd) {
        bar->setValue(bar->maximum());
    }
}
