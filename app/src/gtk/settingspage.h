// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gtk/gtkbase.h"

#include <QObject>
#include <QString>

#include <functional>

#include "core/argsfile.h"
#include "gtk/guarded.h"

/*
 * A page of a VM's settings (SettingsDialog), as the Qt app's
 * SettingsPage: all pages edit the same arguments.  A page shows them when
 * it is entered (load()) and writes back what the user changed when it is
 * left (save()), so that the other pages, the Arguments page above all,
 * see the changes.  Only what the user changed is written, to keep the
 * rest as written.  What the arguments need in the VM folder, the new
 * disks they name, the firmware copies, is done by commit() on Apply, so
 * that Discard leaves nothing behind.
 */
class SettingsPage : public Gtk4::Guarded
{
public:
    virtual ~SettingsPage();
    SettingsPage(const SettingsPage &) = delete;
    SettingsPage &operator=(const SettingsPage &) = delete;

    virtual QString title() const = 0;
    virtual const char *icon() const = 0;
    /* What the page shows: an AdwPreferencesPage, mostly */
    GtkWidget *widget() const { return m_widget; }

    virtual void load(const ArgsFile &args) = 0;
    virtual void save(ArgsFile &args) = 0;
    /* The user changed something since load() */
    virtual bool isModified() const = 0;
    virtual bool commit(const ArgsFile &args, const QString &vmDir, QString *error)
    {
        Q_UNUSED(args);
        Q_UNUSED(vmDir);
        Q_UNUSED(error);
        return true;
    }

    /* Called after each change the user makes: the dialog's Apply follows */
    void setChanged(std::function<void()> changed) { m_changed = std::move(changed); }
    /* Where it presents its alerts and small dialogs: the window's present() */
    void setPresenter(std::function<void(AdwDialog *)> presenter) { m_present = std::move(presenter); }

protected:
    /* @widget: the page, taken */
    explicit SettingsPage(GtkWidget *widget);

    /* After a change by the user: not while load() fills the page in */
    void changed();
    void present(AdwDialog *dialog) const;
    const std::function<void(AdwDialog *)> &presenter() const { return m_present; }
    /*
     * @function from GLib's loop, if the page is still there: for what
     * makes rows again from within a signal of one of them
     */
    void later(std::function<void()> function);
    /* Qt's connections, which go with the page */
    QObject *qt() { return &m_qt; }

    /* While load() fills the page in: its widgets' signals are not the user's */
    bool m_loading = false;

private:
    GtkWidget *m_widget;
    std::function<void()> m_changed;
    std::function<void(AdwDialog *)> m_present;
    QObject m_qt;
};

/* load() of a page: m_loading for the time of a scope */
class Loading
{
public:
    explicit Loading(bool &flag) : m_flag(flag), m_was(flag) { m_flag = true; }
    ~Loading() { m_flag = m_was; }
    Loading(const Loading &) = delete;
    Loading &operator=(const Loading &) = delete;

private:
    bool &m_flag;
    bool m_was;
};
