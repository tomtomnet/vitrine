// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QComboBox>

#include "core/guestos.h"

class QCheckBox;
class QFrame;
class QLineEdit;
class QStandardItemModel;
class QTreeView;
class SystemFilter;

/*
 * The system a VM runs, chosen as in virt-manager from libosinfo's list
 * (GuestOs): a field that shows it with its logo and opens the list, by
 * families (Linux, Windows, BSD, macOS, others), distributions and
 * releases, searched by words of their names (fedora 44, win 11).  The
 * releases no longer supported are left out unless asked for, but for the
 * one shown.  A row of each family, "Another distribution", stands for a
 * system the list does not have: the family alone.
 */
class OsChooser : public QComboBox
{
    Q_OBJECT

public:
    enum Role {
        IdRole = Qt::UserRole + 1,  // the system's id; empty for a family alone
        FamilyRole,                 // #guest's family: linux, windows, other
        SearchRole,                 // the words it is found by
        EolRole,                    // no longer supported
    };

    explicit OsChooser(QWidget *parent = nullptr);

    /* Shows @id of the catalogue, or none and @family alone (linux,
       windows, other; none: not known); no signal */
    void setSystem(const QString &id, const QString &family = {});
    QString id() const { return m_id; }
    /* The family of the system, linux, windows or other; empty if not known */
    QString family() const { return m_family; }

    void showPopup() override;
    void hidePopup() override;
    /* The list, while shown: its search field, its rows, the check box for
       the releases no longer supported */
    QFrame *popup() const { return m_popup; }
    QLineEdit *searchField() const { return m_search; }
    QTreeView *tree() const { return m_tree; }
    QCheckBox *oldReleases() const { return m_old; }

signals:
    /* The user chose a system, another one or the same */
    void systemChosen();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void makePopup();
    /* The rows, from the catalogue, with the system shown among them */
    void fill();
    void filter();
    /* @index of the list: a system is chosen, a group opens or closes */
    void activate(const QModelIndex &index);
    /* The first system the list shows, else an invalid index */
    QModelIndex firstSystem(const QModelIndex &parent = {}) const;

    QString m_id;
    QString m_family;
    QFrame *m_popup = nullptr;
    QLineEdit *m_search = nullptr;
    QTreeView *m_tree = nullptr;
    QCheckBox *m_old = nullptr;
    QStandardItemModel *m_model = nullptr;
    SystemFilter *m_filter = nullptr;
};
