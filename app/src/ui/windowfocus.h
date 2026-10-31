// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDBusConnection>
#include <QDBusContext>
#include <QObject>
#include <QString>

class QDBusServiceWatcher;

/*
 * Which window of this computer's desktop is active, for focus priority
 * (HostSettings::windowActivated()) when VMs show in windows of QEMU's own -
 * SDL's, GTK's, the nested desktop's own - whose focus vitrine cannot see.
 * KWin, KDE Plasma's window manager, runs a script that reports the process
 * of each window it activates back to this object, over the session bus,
 * as the research's vm_focus_priority.py has it.  The script calls this
 * connection's unique name, so no name is claimed on the bus, and only the
 * calls of KWin's own connection count: another client of the bus has no
 * say in which VM is real-time.  On another desktop nothing is reported.
 * The script runs only while watching; KWin restarted meanwhile gets it
 * again.  Nothing is sent before the first setWatching(true).
 */
class WindowFocus : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.vitrine.WindowFocus")

public:
    /* On @bus: the session bus (KWin's), or a test's */
    explicit WindowFocus(const QDBusConnection &bus, QObject *parent = nullptr);
    /* The script goes with it */
    ~WindowFocus() override;

    /* KWin's script loaded and running (true), or gone */
    void setWatching(bool on);
    bool watching() const { return m_watching; }
    /* KWin took the script and runs it */
    bool running() const { return m_running; }

    /* The script, reporting to @service (a unique name, ":1.42") */
    static QString script(const QString &service);
    /* The object and interface it calls, and KWin's name for it */
    static const char *objectPath();
    static const char *pluginName();

signals:
    /* The window of process @pid is the active one now (0: none) */
    void activated(qint64 pid);
    /* running() changed */
    void runningChanged(bool running);

public slots:
    /* The script's report: the process of the active window, in decimal */
    Q_SCRIPTABLE void windowActivated(const QString &pid);

private:
    void load();
    void unload();
    void setRunning(bool running);
    /* KWin's connection now, or "" */
    void readOwner();

    QDBusConnection m_bus;
    QDBusServiceWatcher *m_kwin = nullptr;
    QString m_owner;            // KWin's unique name
    QString m_path;             // the script's file
    bool m_watching = false;
    bool m_registered = false;
    bool m_running = false;
    bool m_sent = false;        // a load went to KWin since the last unload
    int m_generation = 0;       // of the last load or unload: older replies are stale
};
