// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QObject>
#include <QString>

#include <functional>

class GuestAgent;
class QTimer;

/*
 * Shut Down, the gentlest way the guest takes.  Each way is tried when the
 * one before cannot take the request, refuses it or does not answer:
 *
 *  1. the guest tools' agent, which powers the guest off at once
 *     (GuestToolsMonitor gives the runner the way to ask it);
 *  2. QEMU's guest agent, qemu-ga, which most Linux guests have (Fedora
 *     installs it in VMs): guest-shutdown, as virsh shutdown --mode=agent;
 *  3. the ACPI power button, which a guest may answer with a question on
 *     its screen: Plasma shows its logout screen and waits for a click.
 *
 * The VM's runner gives it the ways its run has; tests give stand-ins.
 */
class GuestShutdown : public QObject
{
    Q_OBJECT

public:
    enum class Way {
        None,           // not asked in this run
        ToolsAgent,     // the guest tools' agent
        GuestAgent,     // qemu-ga
        PowerButton,    // the ACPI power button
    };
    Q_ENUM(Way)

    /*
     * Asks the guest tools' agent: false if it cannot now (none answers);
     * else it calls @answer later, with true once the agent took the
     * request, false if the agent refused it or did not answer in time
     */
    using Asker = std::function<bool(const std::function<void(bool took)> &answer)>;
    /* Tells @answer whether the guest opened qemu-ga's port: its qemu-ga runs */
    using PortCheck = std::function<void(const std::function<void(bool open)> &answer)>;

    explicit GuestShutdown(QObject *parent = nullptr);
    ~GuestShutdown() override;

    void setToolsAgent(const Asker &ask);
    /* The socket of qemu-ga's port, empty if the run has none; @isOpen, if
       given, spares the wait for a qemu-ga the guest does not run */
    void setGuestAgent(const QString &socket, const PortCheck &isOpen = {});
    void setPowerButton(const std::function<void()> &press);
    /* Whether the guest still runs, which a request needs (default: yes) */
    void setRunning(const std::function<bool()> &running);

    /* Asks the guest from the first way on; not again while a request is
       on its way, but again once the last one took a way */
    void start();
    /* The run ended, or the guest is shutting down: what the request left
       is dropped, and the way is None again */
    void reset();
    /* The way the request took, or is trying */
    Way way() const;
    /* A request on its way: a way tried, not settled yet */
    bool isBusy() const;

    /*
     * Tests: how long qemu-ga has to answer guest-sync, and how long it
     * has to refuse guest-shutdown, which it answers only when it fails
     */
    static void setGuestAgentTimeouts(int syncMs, int refuseMs);

signals:
    /* Each way as it is tried, for each request, and None once reset */
    void wayChanged(GuestShutdown::Way way);

private:
    void tryToolsAgent();
    void tryGuestAgent();
    void callGuestAgent();
    void pressPowerButton();
    void settle();
    void dropAgent();
    void setWay(Way way);

    Asker m_toolsAgent;
    QString m_socket;
    PortCheck m_portOpen;
    std::function<void()> m_powerButton;
    std::function<bool()> m_running;
    Way m_way = Way::None;
    bool m_busy = false;
    /* each step of each request: the answers of an earlier one are void */
    quint64 m_step = 0;
    GuestAgent *m_agent = nullptr;
    /* guest-shutdown sent: no refusal by then, the guest shuts down */
    QTimer *m_refuse;
};
