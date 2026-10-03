// SPDX-License-Identifier: GPL-2.0-or-later
#include "vmwindow.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDir>
#include <QSaveFile>

#include "core/paths.h"

static const char kService[] = "org.kde.KWin";
static const char kPlugin[] = "qemu-gui-manager-raise";
static const int kTimeoutMs = 2000;

/* Plasma 6 has windowList() and activeWindow; Plasma 5 clientList() and activeClient */
static const char kScript[] = R"js(
(function () {
    const pid = %1;
    const windows = workspace.windowList ? workspace.windowList() : workspace.clientList();

    for (let i = 0; i < windows.length; i++) {
        const w = windows[i];

        if (w.pid !== pid || !w.normalWindow) {
            continue;
        }
        w.minimized = false;
        if ("activeWindow" in workspace) {
            workspace.activeWindow = w;
        } else {
            workspace.activeClient = w;
        }
        return;
    }
})();
)js";

static QDBusMessage call(const QString &path, const QString &interface, const QString &method,
                         const QVariantList &args = {})
{
    QDBusMessage message = QDBusMessage::createMethodCall(kService, path, interface, method);

    message.setArguments(args);
    return QDBusConnection::sessionBus().call(message, QDBus::Block, kTimeoutMs);
}

namespace VmWindow {

bool raise(qint64 pid, QString *error)
{
    const QDBusConnection bus = QDBusConnection::sessionBus();
    const QString path = Paths::runtimeDir() + "/raise-window.js";
    QSaveFile script(path);

    if (!bus.isConnected() || !bus.interface()->isServiceRegistered(kService)) {
        *error = QCoreApplication::translate(
            "VmWindow", "This desktop does not let an app bring the window of another to the "
                        "front: go to the window of the VM from the taskbar.");
        return false;
    }
    QDir().mkpath(Paths::runtimeDir());
    if (!script.open(QIODevice::WriteOnly) ||
        script.write(QString::fromLatin1(kScript).arg(pid).toUtf8()) < 0 || !script.commit()) {
        *error = script.errorString();
        return false;
    }

    /* one left by a call that did not end */
    call("/Scripting", "org.kde.kwin.Scripting", "unloadScript", {QString(kPlugin)});
    const QDBusMessage loaded =
        call("/Scripting", "org.kde.kwin.Scripting", "loadScript", {path, QString(kPlugin)});
    const int id = loaded.arguments().value(0, -1).toInt();
    if (loaded.type() == QDBusMessage::ErrorMessage || id < 0) {
        *error = QCoreApplication::translate("VmWindow", "KWin did not load the script: %1")
                     .arg(loaded.errorMessage());
        return false;
    }
    /* the object of the script: /Scripting/ScriptN in Plasma 6, /N in Plasma 5 */
    QDBusMessage ran = call(QString("/Scripting/Script%1").arg(id), "org.kde.kwin.Script", "run");
    if (ran.type() == QDBusMessage::ErrorMessage) {
        ran = call(QString("/%1").arg(id), "org.kde.kwin.Script", "run");
    }
    /* run answers once the script ran; then it goes, as KWin numbers the scripts after
       how many are loaded, and the next one would get the number of a stale one */
    call("/Scripting", "org.kde.kwin.Scripting", "unloadScript", {QString(kPlugin)});
    if (ran.type() == QDBusMessage::ErrorMessage) {
        *error = QCoreApplication::translate("VmWindow", "KWin did not run the script: %1")
                     .arg(ran.errorMessage());
        return false;
    }
    return true;
}

}
