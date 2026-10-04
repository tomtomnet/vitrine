// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QGridLayout>
#include <QMessageBox>
#include <QSplitter>
#include <QString>

class QAbstractButton;
class QIcon;
class QLabel;
class QLineEdit;
class QSlider;
class QSpinBox;
class QToolButton;
class QWidget;

/*
 * Labels and fields in two columns, like QFormLayout, whose rows get the
 * height of wrapped notes wrong.  The fields keep their size, but for the
 * expanding ones: line edits, sliders, notes.
 */
class Form : public QGridLayout
{
public:
    Form();

    /* A label with a mnemonic focuses the field */
    void addRow(const QString &label, QWidget *field);
    void addRow(QWidget *label, QWidget *field);
    void addRow(const QString &label, QLayout *field);
    void addRow(QWidget *label, QLayout *field);
    /* A title over the rows that follow, where a group box would frame them */
    void addSection(const QString &title);

private:
    void addLabel(QWidget *label, Qt::Alignment vertical);
};

/*
 * Panes as far apart as the widgets of a layout, the gap between them
 * being the handle: no bar drawn, which Breeze would fill the gap with
 */
class Splitter : public QSplitter
{
public:
    explicit Splitter(Qt::Orientation orientation, QWidget *parent = nullptr);

protected:
    QSplitterHandle *createHandle() override;
};

/* What the dialogs and pages are made of */
namespace Widgets {

/* Asks before a destructive @action, which Enter does not trigger */
bool confirm(QWidget *parent, QMessageBox::Icon icon, const QString &title,
             const QString &text, const QString &action, const QIcon &actionIcon);
/*
 * @button of a dialog's buttons with @icon where the style puts icons on
 * them: as the standard buttons beside it (OK, Cancel, Close) have theirs
 */
void setButtonIcon(QAbstractButton *button, const QIcon &icon);

/*
 * A message box Qt draws itself.  With KDE's platform theme a QMessageBox
 * shows the theme's own box instead, and closing that one with the title
 * bar's X leaves this one open, unseen and modal: the window no longer takes
 * input.  The app's message boxes come from here or set DontUseNativeDialog.
 */
QMessageBox *messageBox(QMessageBox::Icon icon, const QString &title, const QString &text,
                        QMessageBox::StandardButtons buttons, QWidget *parent);
/* QMessageBox::information() and warning(), drawn by Qt: see messageBox() */
void inform(QWidget *parent, const QString &title, const QString &text);
void warn(QWidget *parent, const QString &title, const QString &text);

Form *form();
/* The height of a line of @widget's text: widths of fields go by it */
int em(const QWidget *widget);
/*
 * @window @width wide (its minimum width at least) and as high as its
 * content is at that width.  A window shown without a size takes the
 * height of the width it prefers, which wrapped text shorter there makes
 * too high at a wider width: the layout spreads the rest in the window.
 */
void resizeToWidth(QWidget *window, int width);
/* The title of a part of a page, flat: no frame around the part */
QLabel *heading(const QString &text);
/* A form label whose mnemonic focuses @buddy, for fields that are layouts */
QLabel *label(const QString &text, QWidget *buddy);
/* Wrapped, selectable text with links */
QLabel *note(const QString &text = {});
/* A note in a smaller, dimmer font, under a field */
QLabel *hint(const QString &text = {});
/*
 * A notice of the status bar, hidden until there is something to say: an
 * icon and a text, which a click explains.  The notices all look alike,
 * their icons the style's small size beside their text.
 */
QToolButton *statusButton(const QString &name, const QIcon &icon, const QString &text = {});
/* @edit with a Browse button for a file, or for a folder if @folder */
QWidget *browseRow(QLineEdit *edit, const QString &title, const QString &filter = {},
                   bool folder = false);
/* @slider moves @spin in steps of @unit, and follows it */
void link(QSlider *slider, QSpinBox *spin, int unit);
/* The RAM of this computer */
qint64 hostMemoryMiB();

}
