// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <memory>

class QLocalServer;
class QLockFile;

/*
 * One manager at a time, whichever app it is (vitrine, vitrine-gtk):
 * QEMU's QMP socket takes a single client, so a second manager would wait
 * forever to attach to the running VMs.  The first one holds manager.lock
 * and listens on manager.sock; a later one asks it to show its window,
 * passing on its Wayland activation token so that it may take the focus,
 * then exits.  The line it sends: "activate TOKEN" (or "activate").
 */
class ManagerInstance : public QObject
{
    Q_OBJECT

public:
    /* @dir is Paths::runtimeDir() but for tests */
    explicit ManagerInstance(const QString &dir, QObject *parent = nullptr);
    ~ManagerInstance() override;

    QString lockPath() const;
    QString socketPath() const;

    /* Takes manager.lock, waiting a little: false if another manager holds it */
    bool lock();
    /*
     * The manager holding the lock shows its window, with @token: false if
     * it does not answer, after a few seconds of trying, as it may be
     * starting still
     */
    bool activateRunning(const QByteArray &token) const;
    /* Listens on manager.sock for later managers: activationRequested() */
    bool listen(QString *error = nullptr);

signals:
    /* A later manager asks for the window, with its activation token, if any */
    void activationRequested(const QByteArray &token);

private:
    QString m_dir;
    std::unique_ptr<QLockFile> m_lock;
    QLocalServer *m_server = nullptr;
};
