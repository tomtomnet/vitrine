// SPDX-License-Identifier: GPL-2.0-or-later
#include <QApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QThread>

#include <cstdio>

#include "core/paths.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "ui/icons.h"
#include "ui/mainwindow.h"
#include "ui/qemudocs.h"

/*
 * One manager at a time: QEMU's QMP socket takes a single client, so a
 * second manager would wait forever to attach to the running VMs.  The
 * first one listens on manager.sock, and a second one asks it to show its
 * window, passing on its Wayland activation token so that it may take the
 * focus, then exits.
 */
static bool activateRunning(const QString &socketPath)
{
    QLocalSocket socket;

    socket.connectToServer(socketPath);
    if (!socket.waitForConnected(1000)) {
        return false;
    }
    socket.write("activate " + qgetenv("XDG_ACTIVATION_TOKEN") + '\n');
    socket.waitForBytesWritten(1000);
    socket.disconnectFromServer();
    return true;
}

static void listen(QLocalServer *server, const QString &socketPath, MainWindow *window)
{
    QLocalServer::removeServer(socketPath);
    server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server->listen(socketPath)) {
        qWarning("Cannot listen on %s: %s", qPrintable(socketPath),
                 qPrintable(server->errorString()));
        return;
    }
    QObject::connect(server, &QLocalServer::newConnection, window, [server, window]() {
        while (QLocalSocket *client = server->nextPendingConnection()) {
            QObject::connect(client, &QLocalSocket::disconnected, client,
                             &QObject::deleteLater);
            QObject::connect(client, &QLocalSocket::readyRead, window, [client, window]() {
                while (client->canReadLine()) {
                    const QByteArray line = client->readLine().trimmed();
                    if (line == "activate" || line.startsWith("activate ")) {
                        const QByteArray token = line.mid(9).trimmed();
                        if (!token.isEmpty()) {
                            /* the Wayland plugin activates with it */
                            qputenv("XDG_ACTIVATION_TOKEN", token);
                        }
                        window->bringToFront();
                    }
                }
            });
        }
    });
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    QApplication::setApplicationName("qemu-gui-manager");
    QApplication::setApplicationDisplayName(QObject::tr("QEMU GUI Manager"));
    QApplication::setApplicationVersion(QGM_VERSION);
    QApplication::setDesktopFileName("qemu-gui-manager");
    QApplication::setWindowIcon(Icons::app());

    const QString socketPath = Paths::runtimeDir() + "/manager.sock";
    QLockFile lock(Paths::runtimeDir() + "/manager.lock");
    if (!lock.tryLock(100)) {
        /* the other one may be starting still */
        for (int i = 0; i < 30; i++) {
            if (activateRunning(socketPath)) {
                return 0;
            }
            QThread::msleep(100);
        }
        fprintf(stderr, "qemu-gui-manager: another instance is running but does not "
                        "answer on %s\n", qPrintable(socketPath));
        return 1;
    }

    VmStore store(Paths::vmsDir());
    /* VMs outlive the manager: find those still running */
    for (Vm *vm : store.vms()) {
        vm->runner()->attach(vm->args());
    }
    QObject::connect(&store, &VmStore::added, &store,
                     [](Vm *vm) { vm->runner()->attach(vm->args()); });
    /* load the QEMU documentation in the background now */
    QemuDocs::preferred();

    MainWindow window(&store);
    QLocalServer server;
    listen(&server, socketPath, &window);
    window.show();
    return app.exec();
}
