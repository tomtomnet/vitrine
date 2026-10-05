// SPDX-License-Identifier: GPL-2.0-or-later
#include "guestshutdown.h"

#include <QJsonObject>
#include <QPointer>
#include <QTimer>

#include <utility>

#include "core/guestagent.h"

/* A qemu-ga that runs answers guest-sync at once */
static int syncTimeoutMs = 3000;
/* It answers guest-shutdown only when shutdown(8) failed, which it waits for */
static int refuseTimeoutMs = 3000;

GuestShutdown::GuestShutdown(QObject *parent) : QObject(parent), m_refuse(new QTimer(this))
{
    m_refuse->setSingleShot(true);
    /* no refusal: the guest shuts down */
    connect(m_refuse, &QTimer::timeout, this, &GuestShutdown::settle);
}

GuestShutdown::~GuestShutdown()
{
    /* the answers dropAgent() brings about are void */
    m_step++;
    dropAgent();
}

void GuestShutdown::setGuestAgentTimeouts(int syncMs, int refuseMs)
{
    syncTimeoutMs = syncMs;
    refuseTimeoutMs = refuseMs;
}

void GuestShutdown::setToolsAgent(const Asker &ask)
{
    m_toolsAgent = ask;
}

void GuestShutdown::setGuestAgent(const QString &socket, const PortCheck &isOpen)
{
    m_socket = socket;
    m_portOpen = isOpen;
}

void GuestShutdown::setPowerButton(const std::function<void()> &press)
{
    m_powerButton = press;
}

void GuestShutdown::setRunning(const std::function<bool()> &running)
{
    m_running = running;
}

GuestShutdown::Way GuestShutdown::way() const
{
    return m_way;
}

bool GuestShutdown::isBusy() const
{
    return m_busy;
}

void GuestShutdown::start()
{
    if (m_busy) {
        return;
    }
    m_busy = true;
    tryToolsAgent();
}

void GuestShutdown::reset()
{
    m_step++;
    m_busy = false;
    m_refuse->stop();
    dropAgent();
    setWay(Way::None);
}

void GuestShutdown::tryToolsAgent()
{
    const quint64 step = ++m_step;
    /* the answer may come after this object went, with its runner */
    const bool asked = m_toolsAgent && m_toolsAgent([self = QPointer(this), step](bool took) {
        if (!self || step != self->m_step) {
            return;
        }
        if (took) {
            self->setWay(Way::ToolsAgent);
            self->settle();
        } else {
            self->tryGuestAgent();
        }
    });

    if (step != m_step) {
        /* it answered already */
        return;
    }
    if (asked) {
        setWay(Way::ToolsAgent);
    } else {
        tryGuestAgent();
    }
}

void GuestShutdown::tryGuestAgent()
{
    const quint64 step = ++m_step;

    if (m_socket.isEmpty()) {
        pressPowerButton();
        return;
    }
    if (!m_portOpen) {
        callGuestAgent();
        return;
    }
    /* a guest without qemu-ga leaves its port closed: no wait for its answer */
    m_portOpen([self = QPointer(this), step](bool open) {
        if (!self || step != self->m_step) {
            return;
        }
        if (open) {
            self->callGuestAgent();
        } else {
            self->pressPowerButton();
        }
    });
}

void GuestShutdown::callGuestAgent()
{
    const quint64 step = ++m_step;
    auto *agent = new GuestAgent(this);

    dropAgent();
    m_agent = agent;
    setWay(Way::GuestAgent);
    connect(agent, &GuestAgent::ready, this, [this, step, agent]() {
        if (step != m_step) {
            return;
        }
        agent->execute("guest-shutdown", {{"mode", "powerdown"}},
                       [this, step, agent](const QJsonValue &, const QString &error) {
            if (step != m_step) {
                return;
            }
            /* an answer that is no refusal (qemu-ga answers none on success),
               or QEMU closing the port on its way out, with the guest */
            if (error.isEmpty() || !agent->isConnected()) {
                settle();
                return;
            }
            qInfo("qemu-ga did not shut the guest down: %s", qPrintable(error));
            pressPowerButton();
        });
        m_refuse->start(refuseTimeoutMs);
    });
    connect(agent, &GuestAgent::failed, this, [this, step](const QString &error) {
        if (step != m_step) {
            return;
        }
        qInfo("qemu-ga: %s", qPrintable(error));
        pressPowerButton();
    });
    agent->connectToSocket(m_socket, syncTimeoutMs);
}

void GuestShutdown::pressPowerButton()
{
    m_step++;
    dropAgent();
    /* not for a guest that went meanwhile */
    if (!m_running || m_running()) {
        setWay(Way::PowerButton);
        if (m_powerButton) {
            m_powerButton();
        }
    }
    settle();
}

void GuestShutdown::settle()
{
    /* first: the answers dropAgent() brings about are void */
    m_step++;
    m_busy = false;
    m_refuse->stop();
    dropAgent();
}

void GuestShutdown::dropAgent()
{
    if (GuestAgent *agent = std::exchange(m_agent, nullptr)) {
        agent->disconnect(this);
        agent->disconnectFromSocket();
        agent->deleteLater();
    }
}

void GuestShutdown::setWay(Way way)
{
    if (m_way != way) {
        m_way = way;
        emit wayChanged(way);
    }
}
