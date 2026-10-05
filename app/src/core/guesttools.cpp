// SPDX-License-Identifier: GPL-2.0-or-later
#include "guesttools.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>

#include <utility>

#include "core/gpucontexts.h"
#include "core/guestos.h"
#include "core/guesttoolsbuilder.h"
#include "core/paths.h"
#include "core/qmpclient.h"
#include "core/vmhardware.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"

namespace GuestTools {

const char kLabel[] = "VITRINETOOL";
const char kPortName[] = "org.vitrine.agent.0";
const char kPortId[] = "vitrine-agent-port";
const char kFedoraRelease[] = "44";
const int kStatsProtocol = 2;

/* No hello this long after the guest started: no agent */
static const int kGraceMs = 90000;
/* Connecting again to the agent's socket */
static const int kRetryMs = 3000;

bool offered(const VmConfig::Guest &guest, const QString &reportedOs, QString *why)
{
    QString reason;

    if (GuestOs::isFedora(guest.id) || reportedOs == "fedora") {
        return true;
    }
    if (!guest.id.isEmpty() || !reportedOs.isEmpty()) {
        const GuestOs::Os os = GuestOs::Catalogue::instance().find(guest.id);
        const QString name = !os.isNull()      ? os.name
                             : guest.id.isEmpty() ? reportedOs
                                                  : guest.id;
        reason = QObject::tr("The guest tools are for Fedora Linux; this VM runs %1.").arg(name);
    } else if (guest.os == "windows") {
        reason = QObject::tr("The guest tools are for Fedora Linux; this VM runs Windows.");
    } else if (guest.os == "other") {
        reason = QObject::tr("The guest tools are for Fedora Linux; this VM runs another "
                             "system.");
    } else {
        reason = QObject::tr("The guest tools are for Fedora Linux. If this VM runs it, choose "
                             "Fedora as its system in its settings (General).");
    }
    if (why) {
        *why = reason;
    }
    return false;
}

QString dataDir()
{
    return Paths::dataDir() + "/guest-tools";
}

Medium parseManifest(const QByteArray &json, const QString &image)
{
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    Medium m;

    if (o["mediumId"].toString().isEmpty() || o["tools"].toString().isEmpty()) {
        return m;
    }
    m.image = image;
    m.mediumId = o["mediumId"].toString();
    m.fedora = o["fedora"].toString();
    m.tools = o["tools"].toString();
    m.mesa = o["mesa"].toString();
    m.kwin = o["kwin"].toString();
    for (const QJsonValue &p : o["packages"].toArray()) {
        m.packages << QString("%1-%2.%3").arg(p["name"].toString(), p["evr"].toString(),
                                              p["arch"].toString());
    }
    return m;
}

Medium medium(const QString &release)
{
    const QString image = QString("%1/vitrine-guest-tools-fc%2.img").arg(dataDir(), release);
    QFile manifest(QString(image).replace(QRegularExpression("\\.img$"), ".json"));

    if (!QFileInfo::exists(image) || !manifest.open(QIODevice::ReadOnly)) {
        return {};
    }
    return parseManifest(manifest.readAll(), image);
}

/* The target of @qemu, qemu-system-TARGET; another name is the host's */
static QString targetOf(const QString &qemu)
{
    static const QRegularExpression target("^qemu-system-([a-z0-9_]+)");
    const QRegularExpressionMatch m = target.match(QFileInfo(qemu).fileName());

    return m.hasMatch() ? m.captured(1) : Paths::hostArch();
}

/* Machines without PCI, where a virtio-serial controller cannot go */
static bool hasPci(const ArgsFile &args)
{
    const QString machine = VmConfig::machineType(args);
    return machine != "isapc" && machine != "microvm" && machine != "none";
}

bool hasSerialController(const ArgsFile &args, const QString &qemu)
{
    const QString target = targetOf(qemu);

    return (target == "x86_64" || target == "aarch64") && hasPci(args);
}

bool addsAgentPort(const ArgsFile &args, const QString &qemu)
{
    return hasSerialController(args, qemu) && !args.toText().contains(kPortName);
}

bool canBootstrap(const ArgsFile &args, const QString &qemu)
{
    /* the tools are for x86-64 Fedora; SMBIOS type 11 carries the credentials */
    return targetOf(qemu) == "x86_64" && hasPci(args) &&
           !args.toText().contains("systemd.extra-unit.vitrine-tools-bootstrap");
}

QStringList agentPortArgs(const QString &socket)
{
    return {"-chardev",
            QString("socket,id=vitrine-agent,path=%1,server=on,wait=off")
                .arg(OptionValue::escape(socket)),
            "-device",
            QString("virtserialport,bus=vitrine-serial.0,chardev=vitrine-agent,name=%1,id=%2")
                .arg(kPortName, kPortId)};
}

QStringList mediumArgs(const QString &image)
{
    /* no bootindex: the firmware boots from the VM's own disks first, and
       the medium has no boot loader */
    return {"-drive",
            "if=none,id=vitrine-tools,format=raw,readonly=on,file=" + OptionValue::escape(image),
            "-device", "virtio-blk-pci,drive=vitrine-tools,id=vitrine-tools-disk"};
}

static QByteArray resource(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QByteArray bootstrapUnit()
{
    return resource(":/guest/bootstrap/vitrine-tools-bootstrap.service");
}

QByteArray bootstrapDropIn()
{
    return resource(":/guest/bootstrap/multi-user.target.conf");
}

QStringList bootstrapArgs()
{
    /* systemd-debug-generator writes them as a unit and a drop-in; the
       drop-in's name follows the "~" */
    auto credential = [](const QString &name, const QByteArray &content) {
        return QString("type=11,value=io.systemd.credential.binary:%1=%2")
            .arg(name, QString::fromLatin1(content.toBase64()));
    };
    return {"-smbios",
            credential("systemd.extra-unit.vitrine-tools-bootstrap.service", bootstrapUnit()),
            "-smbios",
            credential("systemd.unit-dropin.multi-user.target~vitrine-tools", bootstrapDropIn())};
}

static QString pendingKey(const QString &vmId)
{
    return "guesttools/pending/" + vmId;
}

static QString lastKey(const QString &vmId)
{
    return "guesttools/last/" + vmId;
}

Pending pending(const QString &vmId)
{
    const QString value =
        QSettings(Paths::settingsPath(), QSettings::IniFormat).value(pendingKey(vmId)).toString();

    return value == "bootstrap" ? Pending::Bootstrap
           : value == "medium"  ? Pending::Medium
                                : Pending::None;
}

void setPending(const QString &vmId, Pending pending)
{
    QSettings s(Paths::settingsPath(), QSettings::IniFormat);

    if (pending == Pending::None) {
        s.remove(pendingKey(vmId));
    } else {
        s.setValue(pendingKey(vmId), pending == Pending::Bootstrap ? "bootstrap" : "medium");
    }
}

static QHash<QString, QString> stringHash(const QJsonObject &o)
{
    QHash<QString, QString> h;

    for (auto it = o.begin(); it != o.end(); ++it) {
        h.insert(it.key(), it.value().isString() ? it.value().toString()
                                                 : it.value().toVariant().toString());
    }
    return h;
}

Report parseReport(const QJsonObject &s)
{
    Report r;
    const QJsonObject os = s["os"].toObject();
    const QJsonObject driver = s["driver"].toObject();

    r.tools = s["tools"].toString();
    r.agent = s["agent"].toString();
    r.osId = os["id"].toString();
    r.osVersion = os["version"].toString();
    r.osName = os["name"].toString();
    r.kernel = s["kernel"].toString();
    for (const QJsonValue &k : s["kernels"].toArray()) {
        r.kernels << Kernel{k["version"].toString(), k["headers"].toBool(),
                            k["driver"].toString()};
    }
    if (s["secureBoot"].isBool()) {
        r.secureBoot = s["secureBoot"].toBool();
    }
    r.desktops = stringHash(s["desktops"].toObject());
    r.driverPresent = driver["present"].toBool(true);
    r.driverLoaded = driver["loaded"].toBool();
    r.driverPatched = driver["patched"].toBool();
    r.taint = driver["taint"].toString();
    r.driverParams = stringHash(driver["params"].toObject());
    for (const QJsonValue &c : s["gpu"].toObject()["capsets"].toArray()) {
        r.capsets << c.toString();
    }
    r.packages = stringHash(s["packages"].toObject());
    r.kwinPatched = s["kwinPatched"].toBool();
    r.preempt = s["preempt"].toString();
    r.rebootNeeded = s["rebootNeeded"].toBool();
    r.installing = s["installing"].toBool();
    r.installedMedium = s["installed"].toObject()["medium"].toString();
    r.bootstrap = s["bootstrap"].toString();
    if (const QJsonObject last = s["lastInstall"].toObject(); last["ok"].isBool()) {
        r.lastMedium = last["medium"].toString();
        r.lastOk = last["ok"].toBool();
        r.lastError = last["error"].toString();
    }
    return r;
}

Message parseMessage(const QByteArray &line)
{
    static const QHash<QString, Message::Type> types{
        {"hello", Message::Type::Hello},       {"status", Message::Type::Status},
        {"progress", Message::Type::Progress}, {"result", Message::Type::Result},
        {"error", Message::Type::Error},       {"stats", Message::Type::Stats}};
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &error);
    const QJsonObject o = doc.object();
    Message m;

    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return m;
    }
    m.type = types.value(o["type"].toString(), Message::Type::Invalid);
    m.id = o["id"].isDouble() ? o["id"].toInteger() : -1;
    switch (m.type) {
    case Message::Type::Hello:
        m.protocol = o["protocol"].toInt();
        m.report = parseReport(o["status"].toObject());
        break;
    case Message::Type::Status:
        m.report = parseReport(o["status"].toObject());
        break;
    case Message::Type::Progress:
        m.step = o["step"].toString();
        m.current = o["current"].toInt();
        m.total = o["total"].toInt();
        break;
    case Message::Type::Result:
        m.command = o["cmd"].toString();
        m.ok = o["ok"].toBool();
        m.error = o["error"].toString();
        m.rebootNeeded = o["rebootNeeded"].toBool();
        break;
    case Message::Type::Error:
        m.error = o["error"].toString();
        break;
    case Message::Type::Stats:
        m.stats = o["stats"].toObject();
        break;
    case Message::Type::Invalid:
        break;
    }
    return m;
}

QByteArray commandLine(const QString &command, qint64 id)
{
    return QJsonDocument(QJsonObject{{"cmd", command}, {"id", id}})
               .toJson(QJsonDocument::Compact) +
           '\n';
}

State evaluate(const Inputs &in)
{
    if (!in.running) {
        if (in.pending != Pending::None) {
            return State::Pending;
        }
        /* what its last run showed: no tools, or older ones than the medium's */
        if (in.remembered == State::NotInstalled) {
            return State::NotInstalled;
        }
        if (in.remembered == State::Installed && in.medium.isValid() &&
            !in.rememberedTools.isEmpty() && in.rememberedTools != in.medium.tools) {
            return State::UpdateAvailable;
        }
        return State::Unknown;
    }
    if (in.installing) {
        return State::Installing;
    }
    if (in.failed) {
        return State::Failed;
    }
    if (in.agentSeen) {
        const Report &r = in.report;

        /* the agent of the tools in the guest answers before the install
           this run brought starts (it waits for the network) */
        if (r.bootstrap == "waiting" || r.bootstrap == "running") {
            return State::Installing;
        }
        if (r.bootstrap.isEmpty() && in.bootstrapRun && !in.bootstrapExpired &&
            in.medium.isValid() && r.lastMedium != in.medium.mediumId) {
            return State::Installing;
        }

        /* the installer of this medium failed, at boot or by hand */
        if (!r.lastOk.value_or(true) && in.medium.isValid() &&
            r.lastMedium == in.medium.mediumId) {
            return State::Failed;
        }
        if (r.tools.isEmpty()) {
            return State::NotInstalled;
        }
        if (in.medium.isValid() && !in.medium.fedora.isEmpty() &&
            (r.osId != "fedora" || r.osVersion != in.medium.fedora)) {
            return State::Unsupported;
        }
        if (r.rebootNeeded) {
            return State::RebootNeeded;
        }
        if (!r.driverPatched && r.driverPresent) {
            return State::DriverNotActive;
        }
        if (in.medium.isValid() &&
            (in.medium.tools != r.tools ||
             (!r.installedMedium.isEmpty() && r.installedMedium != in.medium.mediumId))) {
            return State::UpdateAvailable;
        }
        return State::Installed;
    }
    if (in.pending != Pending::None) {
        return State::Pending;
    }
    if (in.bootstrapRun && !in.waited) {
        /* the guest installs them before its desktop starts */
        return State::Installing;
    }
    return in.waited ? State::NotInstalled : State::Unknown;
}

QString failureReason(const QString &output)
{
    /* the installer's message: what follows "ERROR: ", to the end */
    const qsizetype at = output.indexOf("ERROR: ");
    const QString message = at >= 0 ? output.mid(at + 7) : output;

    return message.simplified();
}

QString driverProblem(const Report &r)
{
    if (r.secureBoot.value_or(false)) {
        return r.driverLoaded
                   ? QObject::tr("Secure Boot is on, and the driver is not signed: turn Secure "
                                 "Boot off in the VM's firmware settings")
                   : QObject::tr("Secure Boot is on, and the kernel refused the driver, which "
                                 "is not signed: the VM has no graphics driver. Turn Secure Boot "
                                 "off in the VM's firmware settings");
    }
    for (const Kernel &k : r.kernels) {
        if (k.version != r.kernel) {
            continue;
        }
        if (k.driver == "no-headers") {
            return QObject::tr("the running kernel (%1) has no headers to build the driver "
                               "with: update the guest, then restart it")
                .arg(k.version);
        }
        if (k.driver == "installed") {
            return QObject::tr("the driver is built for the running kernel (%1) but not "
                               "loaded: restart the guest")
                .arg(k.version);
        }
        return QObject::tr("the driver did not build for the running kernel (%1)").arg(k.version);
    }
    return QObject::tr("the driver is not loaded");
}

}

using namespace GuestTools;

GuestToolsMonitor *GuestToolsMonitor::of(Vm *vm)
{
    if (auto *m = vm->findChild<GuestToolsMonitor *>(QString(), Qt::FindDirectChildrenOnly)) {
        return m;
    }
    return new GuestToolsMonitor(vm);
}

GuestToolsMonitor::GuestToolsMonitor(Vm *vm)
    : QObject(vm), m_vm(vm), m_socket(new QLocalSocket(this)), m_retry(new QTimer(this)),
      m_grace(new QTimer(this)), m_contexts(new GpuContexts(this)), m_poll(new QTimer(this)),
      m_shutdownFallback(new QTimer(this)), m_bootstrapTimer(new QTimer(this))
{
    /* "not-installed", or "installed" and the version, as the last run left it */
    const QString last = QSettings(Paths::settingsPath(), QSettings::IniFormat)
                             .value(lastKey(vm->id())).toString();
    m_in.remembered = last == "not-installed"         ? State::NotInstalled
                      : last.startsWith("installed ") ? State::Installed
                                                      : State::Unknown;
    m_in.rememberedTools = last.section(' ', 1);
    m_in.medium = GuestTools::medium();

    m_retry->setSingleShot(true);
    m_retry->setInterval(kRetryMs);
    m_grace->setSingleShot(true);
    m_grace->setInterval(kGraceMs);
    /* the guest's 3D contexts, which tell whether it draws through native context */
    m_poll->setInterval(10000);
    connect(m_retry, &QTimer::timeout, this, &GuestToolsMonitor::connectSocket);
    connect(m_grace, &QTimer::timeout, this, [this]() {
        m_in.waited = true;
        emit changed();
    });
    connect(m_poll, &QTimer::timeout, this, [this]() {
        m_contexts->update(m_vm->runner()->qmp());
        /* an install the agent did not start (by hand, the bootstrap) shows there */
        if (m_in.agentSeen && ++m_polls % 3 == 0) {
            requestStatus();
        }
    });
    connect(m_contexts, &GpuContexts::changed, this, [this]() {
        /* the guest draws: its desktop is up, the agent had its chance */
        const GpuContexts::Status s = m_contexts->status();
        if ((s == GpuContexts::Status::Virgl || s == GpuContexts::Status::InUse) &&
            !m_in.agentSeen && m_in.running) {
            m_grace->start(qMin(m_grace->remainingTime(), 20000));
        }
        emit changed();
    });
    connect(m_socket, &QLocalSocket::readyRead, this, &GuestToolsMonitor::read);
    connect(m_socket, &QLocalSocket::connected, this, [this]() { requestStatus(); });
    connect(m_socket, &QLocalSocket::disconnected, this, [this]() {
        /* an answer that will not come */
        m_statsId = -1;
        if (m_in.running) {
            m_retry->start();
        }
    });
    connect(m_socket, &QLocalSocket::errorOccurred, this, [this]() {
        if (m_in.running && m_socket->state() == QLocalSocket::UnconnectedState) {
            m_retry->start();
        }
    });
    connect(vm->runner(), &VmRunner::stateChanged, this, &GuestToolsMonitor::runnerChanged);
    connect(this, &GuestToolsMonitor::changed, this, &GuestToolsMonitor::remember);
    /* a new medium, built from the app: the VMs with older tools offer it */
    connect(GuestToolsBuilder::instance(), &GuestToolsBuilder::built, this, [this]() {
        m_in.medium = GuestTools::medium();
        emit changed();
    });
    m_bootstrapTimer->setSingleShot(true);
    m_bootstrapTimer->setInterval(20 * 60 * 1000);
    connect(m_bootstrapTimer, &QTimer::timeout, this, [this]() {
        m_in.bootstrapExpired = true;
        emit changed();
    });

    /* Shut Down goes through the agent while there is one, before qemu-ga
       and the power button, which Plasma answers with its logout screen */
    m_shutdownFallback->setSingleShot(true);
    m_shutdownFallback->setInterval(5000);
    connect(m_shutdownFallback, &QTimer::timeout, this, [this]() { answerShutdown(false); });
    vm->runner()->setShutdownHandler(
        [guard = QPointer<GuestToolsMonitor>(this)](const std::function<void(bool)> &answer) {
        return guard && guard->shutDownThroughAgent(answer);
    });
    runnerChanged();
}

bool GuestToolsMonitor::shutDownThroughAgent(const std::function<void(bool took)> &answer)
{
    if (!m_in.running || !m_in.agentSeen ||
        m_socket->state() != QLocalSocket::ConnectedState) {
        return false;
    }
    m_shutdownAnswer = answer;
    send("shutdown");
    m_shutdownFallback->start();
    return true;
}

void GuestToolsMonitor::answerShutdown(bool took)
{
    m_shutdownFallback->stop();
    if (const auto answer = std::exchange(m_shutdownAnswer, {})) {
        answer(took);
    }
}

void GuestToolsMonitor::remember()
{
    QString value;

    if (!m_in.running) {
        return;
    }
    switch (state()) {
    case State::NotInstalled:
        value = "not-installed";
        break;
    case State::Installed:
    case State::UpdateAvailable:
    case State::RebootNeeded:
    case State::DriverNotActive:
    case State::MesaNotActive:
        value = "installed " + m_in.report.tools;
        break;
    default:
        return;
    }
    if (m_in.remembered != (value == "not-installed" ? State::NotInstalled : State::Installed) ||
        m_in.rememberedTools != value.section(' ', 1)) {
        QSettings(Paths::settingsPath(), QSettings::IniFormat).setValue(lastKey(m_vm->id()), value);
        m_in.remembered = value == "not-installed" ? State::NotInstalled : State::Installed;
        m_in.rememberedTools = value.section(' ', 1);
    }
}

QString GuestToolsMonitor::reportedOs() const
{
    if (!m_in.report.osId.isEmpty()) {
        return m_in.report.osId;
    }
    /* the installer installs them in Fedora only */
    return m_in.remembered == State::Installed ? "fedora" : QString();
}

State GuestToolsMonitor::state() const
{
    State s = evaluate(m_in);

    /* tools in, but the guest draws through virgl: not their Mesa */
    if (s == State::Installed && m_contexts->status() == GpuContexts::Status::Virgl) {
        s = State::MesaNotActive;
    }
    return s;
}

void GuestToolsMonitor::runnerChanged()
{
    const VmRunner::State s = m_vm->runner()->state();
    const bool running = s == VmRunner::State::Running || s == VmRunner::State::Paused;

    if (s == VmRunner::State::Starting) {
        /* what this start brings, from the command line it is computed with */
        m_startPending = GuestTools::pending(m_vm->id());
        m_started = true;
    }
    if (running && !m_in.running) {
        /* a new run, started here or found running */
        const Pending started = m_started ? m_startPending : Pending::None;

        const State remembered = m_in.remembered;
        const QString rememberedTools = m_in.rememberedTools;

        m_in = Inputs{};
        m_pendingAsked = false;
        m_in.remembered = remembered;
        m_in.rememberedTools = rememberedTools;
        m_in.running = true;
        m_in.bootstrapRun = started == Pending::Bootstrap;
        if (started != Pending::None) {
            /* the command line had it: what is pending is done */
            GuestTools::setPending(m_vm->id(), Pending::None);
        }
        m_in.pending = GuestTools::pending(m_vm->id());
        m_in.medium = GuestTools::medium();
        m_step.clear();
        m_error.clear();
        m_contexts->reset();
        m_grace->start();
        m_poll->start();
        if (m_in.bootstrapRun) {
            m_bootstrapTimer->start();
        }
        if (QmpClient *qmp = m_vm->runner()->qmp()) {
            connect(qmp, &QmpClient::qmpEvent, this, &GuestToolsMonitor::qmpEvent,
                    Qt::UniqueConnection);
        }
        connectSocket();
    } else if (!m_vm->runner()->isActive()) {
        m_started = false;
        m_in.running = false;
        m_in.agentSeen = false;
        m_in.installing = false;
        m_in.pending = GuestTools::pending(m_vm->id());
        m_in.medium = GuestTools::medium();
        m_retry->stop();
        m_grace->stop();
        m_poll->stop();
        m_shutdownFallback->stop();
        m_shutdownAnswer = {};
        m_bootstrapTimer->stop();
        m_socket->abort();
        m_buffer.clear();
        agentGone();
    }
    emit changed();
}

void GuestToolsMonitor::connectSocket()
{
    const QString path = m_vm->runner()->agentSocket();

    if (!m_in.running || m_socket->state() != QLocalSocket::UnconnectedState) {
        return;
    }
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        /* a VM started without the port (an older vitrine): look again later */
        m_retry->start();
        return;
    }
    m_buffer.clear();
    m_socket->connectToServer(path);
}

void GuestToolsMonitor::read()
{
    m_buffer += m_socket->readAll();
    for (qsizetype nl; (nl = m_buffer.indexOf('\n')) >= 0;) {
        const QByteArray line = m_buffer.left(nl);
        m_buffer.remove(0, nl + 1);
        handle(parseMessage(line));
    }
    /* no line is that long: garbage, or not the agent */
    if (m_buffer.size() > (1 << 20)) {
        m_buffer.clear();
    }
}

void GuestToolsMonitor::handle(const Message &m)
{
    switch (m.type) {
    case Message::Type::Hello:
        m_protocol = m.protocol;
        m_statsId = -1;
        m_statsRefused = false;
        [[fallthrough]];
    case Message::Type::Status:
        m_in.agentSeen = true;
        m_in.report = m.report;
        m_in.installing = m.report.installing;
        m_in.medium = GuestTools::medium();
        m_grace->stop();
        if (!m.report.tools.isEmpty() && !m.report.installing) {
            m_in.failed = false;
            /* in: no install at the next start after all - but for one
               asked for in this run, an update or an install again */
            if (m_in.pending == Pending::Bootstrap && !m_pendingAsked) {
                m_in.pending = Pending::None;
                GuestTools::setPending(m_vm->id(), Pending::None);
            }
        }
        break;
    case Message::Type::Progress:
        m_in.installing = true;
        m_in.failed = false;
        m_step = m.step;
        m_current = m.current;
        m_total = m.total;
        break;
    case Message::Type::Result:
        emit commandFinished(m.command, m.ok, m.error);
        if (m.command == "shutdown") {
            answerShutdown(m.ok);
        }
        if (m.command == "install-from-medium") {
            m_in.installing = false;
            m_in.failed = !m.ok;
            m_error = m.error;
            m_in.report.rebootNeeded = m.rebootNeeded;
            emit installFinished(m.ok, m.error, m.rebootNeeded);
            requestStatus();
        }
        break;
    case Message::Type::Error:
        if (m.id >= 0 && m.id == m_statsId) {
            /* an agent busy installing the tools, or one that says it has
               stats and has not: asked again after its next hello */
            m_statsId = -1;
            m_statsRefused = true;
            qInfo("guest agent of %s, stats: %s", qPrintable(m_vm->id()), qPrintable(m.error));
            break;
        }
        /* a command this agent does not know (an older one): shutdown goes
           the next way by its timer */
        qInfo("guest agent of %s: %s", qPrintable(m_vm->id()), qPrintable(m.error));
        break;
    case Message::Type::Stats:
        /* every second while the status bar shows them: no change of state */
        if (m.id == m_statsId) {
            m_statsId = -1;
        }
        emit statsReceived(m.stats);
        return;
    case Message::Type::Invalid:
        return;
    }
    emit changed();
}

void GuestToolsMonitor::qmpEvent(const QString &name, const QJsonObject &data)
{
    if (name == "VSERPORT_CHANGE" && data["id"].toString() == kPortId &&
        !data["open"].toBool()) {
        /* the agent went: the guest restarts, or the agent does */
        guestRestarted();
    } else if (name == "RESET") {
        guestRestarted();
    }
}

void GuestToolsMonitor::guestRestarted()
{
    if (!m_in.running) {
        return;
    }
    m_in.agentSeen = false;
    m_in.waited = false;
    m_in.installing = false;
    agentGone();
    m_contexts->reset();
    m_grace->start();
    emit changed();
}

void GuestToolsMonitor::agentGone()
{
    m_protocol = 0;
    m_statsId = -1;
    m_statsRefused = false;
}

qint64 GuestToolsMonitor::send(const QString &command)
{
    if (m_socket->state() != QLocalSocket::ConnectedState) {
        return -1;
    }
    m_socket->write(commandLine(command, m_nextId));
    return m_nextId++;
}

void GuestToolsMonitor::requestStatus()
{
    send("status");
}

bool GuestToolsMonitor::reportsStats() const
{
    return m_in.agentSeen && m_protocol >= kStatsProtocol && !m_statsRefused &&
           m_socket->state() == QLocalSocket::ConnectedState;
}

void GuestToolsMonitor::requestStats()
{
    /* one at a time; an answer lost (the guest froze) is not waited for long */
    if (!reportsStats() || (m_statsId >= 0 && m_statsAsked.elapsed() < 5000)) {
        return;
    }
    m_statsId = send("stats");
    m_statsAsked.start();
}

void GuestToolsMonitor::installFromMedium()
{
    m_error.clear();
    m_in.failed = false;
    send("install-from-medium");
}

void GuestToolsMonitor::requestReboot()
{
    send("request-reboot");
}


void GuestToolsMonitor::setPending(Pending pending)
{
    GuestTools::setPending(m_vm->id(), pending);
    m_in.pending = pending;
    m_pendingAsked = m_in.running && pending != Pending::None;
    emit changed();
}

QString GuestToolsMonitor::text() const
{
    const Report &r = m_in.report;
    auto code = [](const QString &s) { return "<code>" + s.toHtmlEscaped() + "</code>"; };

    switch (state()) {
    case State::Unknown:
    case State::Installed:
        return {};
    case State::NotInstalled:
        if (!m_in.running) {
            return tr("The guest tools were not installed when this VM last ran: vitrine's "
                      "graphics driver, Mesa with native context and KWin make a Fedora guest "
                      "smoother.");
        }
        if (m_in.bootstrapRun) {
            return tr("The guest did not install the guest tools at its start: that needs "
                      "Fedora %1 with systemd 256 or later. To install them by hand, attach "
                      "the tools medium and run %2 in the guest.")
                .arg(kFedoraRelease, code("sudo bash /run/media/$USER/" + QString(kLabel) +
                                          "/install"));
        }
        return tr("The guest tools are not installed in this VM: vitrine's graphics driver, "
                  "Mesa with native context and KWin make a Fedora guest smoother.");
    case State::Pending:
        if (m_in.pending == Pending::Medium) {
            return tr("The tools medium is attached at the next start of the VM.");
        }
        return m_in.running ? tr("The guest tools install at the next start of the VM: shut "
                                 "it down, then start it.")
                            : tr("The guest tools install at the next start of the VM.");
    case State::Installing:
        if (m_step.isEmpty()) {
            return tr("Installing the guest tools in the guest…");
        }
        return m_total > 0 ? tr("Installing the guest tools: %1 (step %2 of %3)…")
                                 .arg(m_step.toHtmlEscaped())
                                 .arg(m_current)
                                 .arg(m_total)
                           : tr("Installing the guest tools: %1…").arg(m_step.toHtmlEscaped());
    case State::Failed: {
        const QString why = failureReason(m_error.isEmpty() ? r.lastError : m_error);
        return tr("The guest tools could not be installed: %1")
            .arg(why.isEmpty() ? tr("see the guest's journal (journalctl -t vitrine-guest-tools)")
                               : why.toHtmlEscaped());
    }
    case State::RebootNeeded:
        return tr("Restart the guest to finish installing the guest tools.");
    case State::DriverNotActive:
        return tr("The guest tools are installed, but the guest runs the stock graphics "
                  "driver: %1.")
            .arg(driverProblem(r).toHtmlEscaped());
    case State::MesaNotActive:
        return tr("The guest tools are installed, but the guest draws through virgl, not "
                  "native context: its Mesa is not the one of the tools.");
    case State::UpdateAvailable:
        if (!m_in.running) {
            return tr("Guest tools %1 are available; this guest had %2 when it last ran.")
                .arg(m_in.medium.tools.toHtmlEscaped(), m_in.rememberedTools.toHtmlEscaped());
        }
        return tr("Guest tools %1 are available; this guest has %2.")
            .arg(m_in.medium.tools.toHtmlEscaped(), r.tools.toHtmlEscaped());
    case State::Unsupported:
        return tr("The guest tools are for Fedora %1; this guest is %2.")
            .arg(m_in.medium.fedora.toHtmlEscaped(),
                 (r.osName.isEmpty() ? r.osId : r.osName).toHtmlEscaped());
    }
    return {};
}
