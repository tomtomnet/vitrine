// SPDX-License-Identifier: GPL-2.0-or-later
#include "windowfocus.h"

#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSaveFile>

#include "core/paths.h"

static const char kService[] = "org.kde.KWin";
static const char kScripting[] = "org.kde.kwin.Scripting";
static const char kPath[] = "/org/vitrine/WindowFocus";
static const char kPlugin[] = "vitrine-window-focus";
static const int kTimeoutMs = 2000;

/*
 * Plasma 6 has windowActivated and activeWindow, Plasma 5 clientActivated
 * and activeClient.  The pid goes as a string: a script's number may reach
 * D-Bus as an int or a double, and one signature takes it either way.
 */
static const char kScript[] = R"js(
(function () {
    // vitrine (WindowFocus): the process of the active window, for focus
    // priority between VMs in windows of QEMU's own
    function report(w) {
        callDBus("%1", "%2", "org.vitrine.WindowFocus", "windowActivated",
                 String(w && w.pid > 0 ? w.pid : 0));
    }
    if (workspace.windowActivated) {
        workspace.windowActivated.connect(report);
        report(workspace.activeWindow);
    } else {
        workspace.clientActivated.connect(report);
        report(workspace.activeClient);
    }
})();
)js";

const char *WindowFocus::objectPath()
{
    return kPath;
}

const char *WindowFocus::pluginName()
{
    return kPlugin;
}

QString WindowFocus::script(const QString &service)
{
    return QString::fromLatin1(kScript).arg(service, QString::fromLatin1(kPath));
}

WindowFocus::WindowFocus(const QDBusConnection &bus, QObject *parent)
    : QObject(parent), m_bus(bus)
{
}

WindowFocus::~WindowFocus()
{
    if (m_running || m_sent) {
        /* waited for, a little: a message only queued may not leave before
           the connection closes, and the script would call a name gone */
        QDBusMessage message =
            QDBusMessage::createMethodCall(kService, "/Scripting", kScripting, "unloadScript");
        message.setArguments({QString(kPlugin)});
        m_bus.call(message, QDBus::Block, 500);
    }
    if (!m_path.isEmpty()) {
        QFile::remove(m_path);
    }
}

void WindowFocus::setWatching(bool on)
{
    if (on == m_watching) {
        return;
    }
    m_watching = on;
    if (!on) {
        unload();
        return;
    }
    if (!m_bus.isConnected() || !m_bus.interface()) {
        return;
    }
    if (!m_registered) {
        m_registered = m_bus.registerObject(kPath, this, QDBusConnection::ExportScriptableSlots);
        if (!m_registered) {
            qWarning("vitrine: cannot serve %s on the session bus: %s", kPath,
                     qPrintable(m_bus.lastError().message()));
            return;
        }
    }
    if (!m_kwin) {
        /* KWin restarted (a crash, a compositor restart): its scripts went
           with it, the new one gets ours */
        m_kwin = new QDBusServiceWatcher(kService, m_bus, QDBusServiceWatcher::WatchForOwnerChange,
                                         this);
        connect(m_kwin, &QDBusServiceWatcher::serviceOwnerChanged, this,
                [this](const QString &, const QString &, const QString &owner) {
            /* the old KWin's script went with it */
            m_owner = owner;
            m_sent = false;
            m_generation++;
            setRunning(false);
            if (m_watching && !owner.isEmpty()) {
                load();
            }
        });
    }
    readOwner();
    load();
}

void WindowFocus::readOwner()
{
    const QDBusReply<QString> owner = m_bus.interface()->serviceOwner(kService);
    m_owner = owner.isValid() ? owner.value() : QString();
}

void WindowFocus::load()
{
    static const QRegularExpression unique("^:[0-9]+\\.[0-9]+$");
    const QString service = m_bus.baseService();

    /* not KWin's desktop: nothing reports the active window */
    if (m_owner.isEmpty() || !unique.match(service).hasMatch()) {
        return;
    }
    QDir().mkpath(Paths::runtimeDir());
    m_path = Paths::runtimeDir() + "/window-focus.js";
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly) || file.write(script(service).toUtf8()) < 0 ||
        !file.commit()) {
        qWarning("vitrine: cannot write %s: %s", qPrintable(m_path), qPrintable(file.errorString()));
        return;
    }
    /* one a vitrine that ended without its unload left: KWin refuses a
       second script of the same name */
    QDBusMessage stale = QDBusMessage::createMethodCall(kService, "/Scripting", kScripting,
                                                        "unloadScript");
    stale.setArguments({QString(kPlugin)});
    m_bus.call(stale, QDBus::NoBlock);
    QDBusMessage load = QDBusMessage::createMethodCall(kService, "/Scripting", kScripting, "loadScript");
    load.setArguments({m_path, QString(kPlugin)});
    m_sent = true;
    const int generation = ++m_generation;
    auto *loading = new QDBusPendingCallWatcher(m_bus.asyncCall(load, kTimeoutMs), this);
    connect(loading, &QDBusPendingCallWatcher::finished, this, [this, loading, generation]() {
        const QDBusMessage reply = loading->reply();
        const int id = reply.arguments().value(0, -1).toInt();

        loading->deleteLater();
        /* unloaded or loaded again meanwhile: that request has it */
        if (!m_watching || generation != m_generation) {
            return;
        }
        if (reply.type() == QDBusMessage::ErrorMessage || id < 0) {
            qWarning("vitrine: KWin did not load the focus script: %s",
                     qPrintable(reply.errorMessage()));
            return;
        }
        /* the script's object: /Scripting/ScriptN (Plasma 6); else
           Scripting.start() runs whatever is loaded and not running */
        const QDBusMessage run = QDBusMessage::createMethodCall(
            kService, QString("/Scripting/Script%1").arg(id), "org.kde.kwin.Script", "run");
        auto *running = new QDBusPendingCallWatcher(m_bus.asyncCall(run, kTimeoutMs), this);
        connect(running, &QDBusPendingCallWatcher::finished, this,
                [this, running, generation]() {
            running->deleteLater();
            if (!m_watching || generation != m_generation) {
                return;
            }
            if (running->isError()) {
                m_bus.call(QDBusMessage::createMethodCall(kService, "/Scripting", kScripting, "start"),
                           QDBus::NoBlock);
            }
            setRunning(true);
        });
    });
}

void WindowFocus::unload()
{
    const bool sent = m_running || m_sent;

    /* a load under way: its reply is stale now, and this request, after
       it on the same connection, takes the script away again */
    m_generation++;
    m_sent = false;
    if (sent && m_bus.isConnected()) {
        QDBusMessage message =
            QDBusMessage::createMethodCall(kService, "/Scripting", kScripting, "unloadScript");
        message.setArguments({QString(kPlugin)});
        m_bus.call(message, QDBus::NoBlock);
    }
    setRunning(false);
}

void WindowFocus::setRunning(bool running)
{
    if (running != m_running) {
        m_running = running;
        emit runningChanged(running);
    }
}

void WindowFocus::windowActivated(const QString &pid)
{
    bool ok = false;
    const qint64 value = pid.toLongLong(&ok);

    /* KWin's script's report only: no other client of the bus has a say in
       which VM is real-time */
    if (!calledFromDBus() || m_owner.isEmpty() || message().service() != m_owner || !ok ||
        value < 0 || !m_watching) {
        return;
    }
    emit activated(value);
}
