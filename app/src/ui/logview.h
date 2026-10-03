// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QWidget>

class QPlainTextEdit;
class QPushButton;
class QTimer;

/*
 * The log of a VM, which grows as QEMU writes: while shown, the view reads
 * what was added, and follows the end if it was at the end.  QEMU appends
 * to the same file at each start, so only its last part is read.
 */
class LogView : public QWidget
{
    Q_OBJECT

public:
    explicit LogView(QWidget *parent = nullptr);

    void setPath(const QString &path);

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void reload();
    void poll();
    void append(const QByteArray &bytes);

    QPlainTextEdit *m_text;
    QPushButton *m_clear;
    QPushButton *m_open;
    QTimer *m_timer;
    QString m_path;
    qint64 m_offset = 0;
    QByteArray m_partial;       // a line QEMU has not ended yet
};
