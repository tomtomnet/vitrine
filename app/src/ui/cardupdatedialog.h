// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDialog>
#include <QPointer>
#include <QWidget>

#include "core/argsfile.h"
#include "core/cardupdate.h"

class Banner;
class QDialogButtonBox;
class QLabel;
class QPlainTextEdit;
class Vm;

/*
 * Update the 3D Card: what a VM lacks of vitrine's card (CardUpdate), in
 * plain words, and the argument lines before and after.  Apply saves
 * vm.args as the settings pages do, for the next start if the VM runs.
 */
class CardUpdateDialog : public QDialog
{
    Q_OBJECT

public:
    /*
     * Opens the dialog for @vm, from @from on a VmPane's page: the
     * changes of its settings not applied yet would be lost on Apply or
     * would undo the update, so it asks to apply or discard them first,
     * as starting the VM does
     */
    static void run(QWidget *from, Vm *vm);

    CardUpdateDialog(Vm *vm, QWidget *parent = nullptr);

    void accept() override;

private:
    /* The changes for the arguments of now, the notes after @lead */
    void fill(const QString &lead = {});

    QPointer<Vm> m_vm;
    /* The arguments the changes were found for */
    ArgsFile m_args;
    QList<CardUpdate::Change> m_changes;
    QLabel *m_intro;
    QLabel *m_list;
    QPlainTextEdit *m_before;
    QPlainTextEdit *m_after;
    QLabel *m_notes;
    QDialogButtonBox *m_buttons;
};

/*
 * Across the top of a VM's Details (and of its Console while it is off):
 * the offer of vitrine's 3D card to a VM that lacks it, with Update...
 * and Don't Ask Again
 */
class CardUpdateBanner : public QWidget
{
    Q_OBJECT

public:
    explicit CardUpdateBanner(QWidget *parent = nullptr);

    void setVm(Vm *vm);
    /* Asks again what the VM lacks if its arguments or QEMU changed */
    void refresh();
    /* The VM lacks vitrine's card, and the user said not to ask again */
    bool isDeclined() const;

    /* Don't Ask Again for @vmId, or ask again: all the banners follow */
    static void setDeclined(const QString &vmId, bool declined);

signals:
    /* isDeclined() may have changed */
    void declinedChanged();

private:
    QPointer<Vm> m_vm;
    Banner *m_banner;
    /* What m_changes were found for: the arguments and the QEMU */
    QString m_key;
    QList<CardUpdate::Change> m_changes;
};
