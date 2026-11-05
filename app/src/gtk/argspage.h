// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/settingspage.h"

#include <QList>
#include <QString>

#include <memory>

class QemuDocs;
class QTimer;
class ReferencePanel;
struct QemuInfo;

/*
 * The Arguments page: the VM's QEMU command line as text, one option per
 * line, as the Qt app's ArgumentsPage edits it, with the Qt editor's
 * tools: the text highlighted (options, unknown ones underlined, keys,
 * comments, vitrine's directives), the problems with it under it
 * (checkArgs(), a click going to the line), option and device names
 * completed (Ctrl+Space, or while typing them), and the QEMU reference
 * beside it, whose Insert adds a line.  The documentation is that of the
 * QEMU of the text's #qemu line, else the preferred one.
 */
class ArgumentsPage : public SettingsPage
{
public:
    explicit ArgumentsPage(const QString &vmDir);
    ~ArgumentsPage() override;

    QString title() const override { return "Arguments"; }
    const char *icon() const override { return "utilities-terminal-symbolic"; }
    void load(const ArgsFile &args) override;
    void save(ArgsFile &args) override;
    bool isModified() const override;
    void addNarrowSetters(AdwBreakpoint *narrow) override;

    QString text() const;
    void setText(const QString &text);
    /* Checks the text now, rather than after typing pauses */
    void check();
    /* The problems listed, as shown: "Line 3: Unknown option -foo" */
    QStringList problems() const;
    void goToLine(int line);
    /* On a line of its own after the cursor's, or on the cursor's if blank: the reference's Insert */
    void insertLine(const QString &line);
    ReferencePanel *reference() const { return m_reference.get(); }
    /* For tests: the tags on line @line from character @offset, by name; the completions shown */
    QStringList tagsAt(int line, int offset) const;
    QStringList completions() const;
    /* Completion at the cursor, as Ctrl+Space asks it */
    void complete(bool force);
    void placeCursor(int line, int offset);

private:
    enum class Context { None, Option, Device };

    void setDocs(QemuDocs *docs);
    void highlight();
    void highlightLine(int line, const QString &text, const QemuInfo *info);
    void tag(int line, int from, int length, const char *name);
    void highlightKeys(int line, const QString &text, int from);
    void updateColors();
    void fillCompletions(Context context);
    void insertCompletion(const QString &completion);
    void hideCompletion();
    bool completionKey(guint keyval, GdkModifierType state);

    QString m_vmDir;
    QString m_loaded;
    QemuDocs *m_docs = nullptr;
    QTimer *m_timer;
    QStringList m_problems;
    std::unique_ptr<ReferencePanel> m_reference;

    GtkTextBuffer *m_buffer;
    GtkWidget *m_view;
    GtkWidget *m_problemsScroll;
    GtkWidget *m_problemsList;
    AdwOverlaySplitView *m_split;

    /* completion: the names of the context, and those shown for the prefix typed */
    GtkWidget *m_completion;
    GtkWidget *m_completionScroll;
    GtkWidget *m_completionList;
    Context m_context = Context::None;
    QList<std::pair<QString, QString>> m_names;
    QStringList m_shown;
    QString m_prefix;
    /* a key typed that completion follows, and completion's own writing */
    bool m_typed = false;
    bool m_completing = false;
    gulong m_darkHandler = 0;
    gulong m_accentHandler = 0;
};
