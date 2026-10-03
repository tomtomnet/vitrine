// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QPointer>
#include <QWidget>

class Banner;
class GpuContexts;
class QemuDocs;
class QLabel;
class QTextBrowser;
class QTimer;
class Vm;

/* The summary of a VM, beside the list */
class VmDetails : public QWidget
{
    Q_OBJECT

public:
    explicit VmDetails(QWidget *parent = nullptr);

    void setVm(Vm *vm);
    /* Why the last start failed, shown until the next start; empty for none */
    void setError(const QString &error);
    void refresh();

signals:
    void showLog();

private:
    QString html() const;

    QPointer<Vm> m_vm;
    QemuDocs *m_docs = nullptr;
    QLabel *m_icon;
    QLabel *m_name;
    QLabel *m_state;
    Banner *m_note;
    Banner *m_error;
    QTextBrowser *m_text;
    QTimer *m_growing;
    /* whether the guest uses the native context it was given */
    GpuContexts *m_contexts;
    Banner *m_contextsNote;
};

/* "Running", "Powered off"... */
QString stateText(const Vm *vm);
