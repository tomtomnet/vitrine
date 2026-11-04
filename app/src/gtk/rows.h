// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/gtkbase.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

/*
 * The pieces the dialogs are made of, libadwaita's boxed lists: groups of
 * rows to enter text, choose from a list, count, switch, and act; their
 * values as Qt strings; and choosing a file or a folder.  Widgets come
 * floating, for the group or box they go into.
 */
namespace Gtk4 {

/* A note in a dialog: dim, wrapping, left aligned */
GtkWidget *caption(const QString &text);

AdwPreferencesGroup *group(const QString &title = {}, const QString &description = {});

GtkWidget *entryRow(const QString &title, const QString &text = {});
GtkWidget *spinRow(const QString &title, double min, double max, double step,
                   const QString &subtitle = {});
GtkWidget *switchRow(const QString &title, const QString &subtitle = {});
GtkWidget *comboRow(const QString &title, const QStringList &items, const QString &subtitle = {});
GtkWidget *actionRow(const QString &title, const QString &subtitle = {});
/* A row that is a button, as at the end of a list: "Add Hard Disk…" */
GtkWidget *buttonRow(const QString &title, const char *icon = nullptr);
/* A button with an icon, flat, centered in a row: a row's suffix */
GtkWidget *rowButton(const char *icon, const QString &tooltip);
/* The same with a label, for an action of a row */
GtkWidget *rowTextButton(const QString &label, const QString &tooltip = {});

/* A spin row's value shown with @unit, "4096 MiB", and read back with it or without */
void setUnit(GtkWidget *spinRow, const QString &unit);
int value(GtkWidget *spinRow);
void setValue(GtkWidget *spinRow, double value);
/* Its range, the value kept within it */
void setRange(GtkWidget *spinRow, double min, double max);

/* An editable's text, as written */
QString text(GtkWidget *editable);
void setText(GtkWidget *editable, const QString &text);

/* A combo row's entries: their place, -1 for none */
void setItems(GtkWidget *comboRow, const QStringList &items);
int selected(GtkWidget *comboRow);
void setSelected(GtkWidget *comboRow, int index);
/* The text of entry @index, empty if there is none */
QString itemText(GtkWidget *comboRow, int index);

/* A row's subtitle, title and the like: markup, so escaped here */
void setSubtitle(GtkWidget *row, const QString &text);
void setTitle(GtkWidget *row, const QString &text);

struct FileFilter {
    QString name;
    /* e.g. "*.iso" */
    QStringList patterns;
};

/*
 * GTK's file chooser, for the window of @parent: @chosen gets the path,
 * not called if the user cancels.  @start: a file or folder it opens at.
 */
void chooseFile(GtkWidget *parent, const QString &title, const QList<FileFilter> &filters,
                const QString &start, std::function<void(const QString &path)> chosen);
void chooseFolder(GtkWidget *parent, const QString &title, const QString &start,
                  std::function<void(const QString &path)> chosen);

}
