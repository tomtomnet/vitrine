// SPDX-License-Identifier: GPL-2.0-or-later
#include "usbguard.h"

#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QPointer>
#include <QSocketNotifier>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <string_view>
#include <utility>

#include <dirent.h>
#include <linux/netlink.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/optionvalue.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"

using Device = UsbHotplug::Device;
using Kind = UsbGuard::Holder::Kind;

static QPointer<UsbGuard> s_instance;

/* vendor:product, as lsusb has them */
static QString idText(quint16 vendor, quint16 product)
{
    return QString("%1:%2").arg(vendor, 4, 16, QChar('0')).arg(product, 4, 16, QChar('0'));
}

/* "vivo X200 Ultra", else its IDs */
static QString nameOf(const UsbDevice &d)
{
    const QString name = QString("%1 %2").arg(d.manufacturer, d.product).simplified();
    return name.isEmpty() ? idText(d.vendorId, d.productId) : name;
}

/* "vivo X200 Ultra (2d95:6013)", else its IDs */
static QString described(const UsbDevice &d)
{
    const QString name = QString("%1 %2").arg(d.manufacturer, d.product).simplified();
    const QString ids = idText(d.vendorId, d.productId);
    return name.isEmpty() ? ids : QString("%1 (%2)").arg(name, ids);
}

/* A filter for one device in particular: its node, or its bus and address */
static bool concrete(const Device &d)
{
    return !d.node.isEmpty() || (d.bus && d.address);
}

/* No field: QEMU takes what it opens first, no device in particular (as Device::matches) */
static bool empty(const Device &d)
{
    return !d.bus && !d.address && !d.vendorId && !d.productId && d.port.isEmpty() &&
           d.node.isEmpty();
}

/* 0x2d95 or 11669, as QEMU reads its numbers */
static int number(const QString &text)
{
    bool ok = false;
    const uint value = text.trimmed().toUInt(&ok, 0);
    return ok ? int(value) : 0;
}

/* The usb-host devices of @vm: as its QEMU has them once read, else as its run's arguments say */
static QList<Device> devicesOf(Vm *vm)
{
    UsbHotplug *hotplug = UsbHotplug::of(vm->runner());
    return hotplug->isKnown() ? hotplug->devices() : UsbGuard::filtersOf(vm->runner()->runArgs());
}

static QByteArray readAll(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

/* Where the link @name of the folder @dir points, as written; empty for none */
static QByteArray linkAt(int dir, const char *name)
{
    char buffer[PATH_MAX];
    const ssize_t n = ::readlinkat(dir, name, buffer, sizeof(buffer));
    return n > 0 ? QByteArray(buffer, qsizetype(n)) : QByteArray();
}

/* Who the process @pid of @proc is */
static UsbGuard::Holder holderOf(const QString &proc, qint64 pid)
{
    const QString dir = QString("%1/%2").arg(proc).arg(pid);
    const QList<QByteArray> argv = readAll(dir + "/cmdline").split('\0');
    UsbGuard::Holder who;

    who.pid = pid;
    who.program = QFileInfo(QFile::decodeName(argv.value(0))).fileName();
    if (who.program.isEmpty()) {
        who.program = QString::fromUtf8(readAll(dir + "/comm")).trimmed();
    }
    who.kind = UsbGuard::kindOf(who.program);
    /* -name guest=fedora,debug-threads=on, or -name fedora */
    for (qsizetype i = 1; who.kind == Kind::Qemu && i + 1 < argv.size(); i++) {
        if (argv[i] == "-name" || argv[i] == "--name") {
            const OptionValue name(QString::fromUtf8(argv[i + 1]));
            who.vmName = name.get("guest", name.implied());
            break;
        }
    }
    return who;
}

/* What tells an issue from another: its VM, device and holders */
static QString keyOf(const UsbGuard::Issue &issue)
{
    QStringList parts{issue.vmId, issue.vmName, issue.device.devNode(), nameOf(issue.device),
                      issue.claimedElsewhere ? "elsewhere" : ""};
    for (const UsbGuard::Holder &who : issue.holders) {
        parts << QString("%1 %2 %3").arg(who.pid).arg(who.program, who.vmName);
    }
    return parts.join('|');
}

/* For the VM's log, a line */
static QString logLine(const UsbGuard::Issue &issue)
{
    const UsbDevice &d = issue.device;
    QStringList who;

    if (issue.holders.isEmpty()) {
        return UsbGuard::tr("USB: %1, at bus %2, address %3, has an interface claimed by a "
                            "program of another user or of the system (usbfs)")
            .arg(described(d), QString::number(d.bus), QString::number(d.device));
    }
    for (const UsbGuard::Holder &h : issue.holders) {
        who << h.text();
    }
    return UsbGuard::tr("USB: %1, at bus %2, address %3, is held by %4 too")
        .arg(described(d), QString::number(d.bus), QString::number(d.device), who.join(", "));
}

/* the names in one go: a "%2" in a program's or a VM's name stays as it is */
QString UsbGuard::Holder::text() const
{
    if (kind == Kind::Qemu && !vmName.isEmpty()) {
        return tr("%1's QEMU (pid %2)").arg(vmName, QString::number(pid));
    }
    return tr("%1 (pid %2)").arg(program.isEmpty() ? tr("a program") : program,
                                 QString::number(pid));
}

QString UsbGuard::Issue::text() const
{
    QStringList names;

    if (holders.isEmpty()) {
        return tr("A program of another user, or of the system, claims %1, which %2 passes "
                  "through")
            .arg(nameOf(device), vmName);
    }
    for (const Holder &who : holders) {
        names << who.text();
    }
    const QString who = names.size() == 1
                            ? names.first()
                            : tr("%1 and %2").arg(names.mid(0, names.size() - 1).join(", "),
                                                  names.last());
    return (holders.size() == 1 ? tr("%1 holds %2, which %3 passes through")
                                : tr("%1 hold %2, which %3 passes through"))
        .arg(who, nameOf(device), vmName);
}

QList<Device> UsbGuard::filtersOf(const ArgsFile &args)
{
    QList<Device> list;

    for (int i : args.indexesOf("device")) {
        const OptionValue v = args.valueAt(i);
        Device d;

        if (v.implied() != "usb-host") {
            continue;
        }
        d.id = v.get("id");
        d.bus = number(v.get("hostbus"));
        d.address = number(v.get("hostaddr"));
        d.vendorId = quint16(number(v.get("vendorid")));
        d.productId = quint16(number(v.get("productid")));
        d.port = v.get("hostport");
        d.node = v.get("hostdevice");
        list << d;
    }
    return list;
}

bool UsbGuard::overlap(const Device &a, const Device &b, const QList<UsbDevice> &hosts)
{
    if (empty(a) || empty(b)) {
        return false;
    }
    /* one device in particular, plugged in: whether the other takes it too */
    for (const auto &[one, other] : {std::pair(&a, &b), std::pair(&b, &a)}) {
        if (!concrete(*one)) {
            continue;
        }
        for (const UsbDevice &h : hosts) {
            if (one->matches(h)) {
                return other->matches(h);
            }
        }
    }
    /* else a device could match both unless a field of theirs is apart */
    const auto agree = [](auto x, auto y) { return !x || !y || x == y; };
    return agree(a.bus, b.bus) && agree(a.address, b.address) && agree(a.vendorId, b.vendorId) &&
           agree(a.productId, b.productId) &&
           (a.port.isEmpty() || b.port.isEmpty() || a.port == b.port) &&
           (a.node.isEmpty() || b.node.isEmpty() || a.node == b.node);
}

QString UsbGuard::startConflict(VmStore *store, const Vm *vm, const ArgsFile &args)
{
    const QList<Device> mine = filtersOf(args);
    QList<UsbDevice> hosts;
    bool read = false;

    if (mine.isEmpty() || !store) {
        return {};
    }
    for (Vm *other : store->vms()) {
        if (other == vm || !other->runner()->isActive()) {
            continue;
        }
        for (const Device &b : devicesOf(other)) {
            for (const Device &a : mine) {
                /* read once, only when another VM passes devices through */
                if (!read) {
                    hosts = UsbHotplug::hostDevices();
                    read = true;
                }
                if (!overlap(a, b, hosts)) {
                    continue;
                }
                /* named by a device both take, plugged in, else by the IDs */
                QString device;
                for (const UsbDevice &h : std::as_const(hosts)) {
                    if (a.matches(h) && b.matches(h)) {
                        device = described(h);
                        break;
                    }
                }
                if (device.isEmpty()) {
                    const quint16 vendor = a.vendorId ? a.vendorId : b.vendorId;
                    const quint16 product = a.productId ? a.productId : b.productId;
                    device = vendor && product ? idText(vendor, product) : tr("a USB device");
                }
                const QString name = vm ? vm->name() : QString();
                QString text =
                    concrete(b)
                        ? tr("USB passthrough: %1 has %2, which %3 would take too. Take it back "
                             "from %1 first (USB Devices), or shut %1 down.")
                              .arg(other->name(), device, name)
                        : tr("USB passthrough: %1, which runs, passes %2 through too. Shut it "
                             "down first, or take the device out of the USB settings of %1 or "
                             "%3.")
                              .arg(other->name(), device, name);
                /* identical devices: a rule by IDs cannot tell them apart */
                const qsizetype same = concrete(a) ? 0
                                                   : std::count_if(hosts.cbegin(), hosts.cend(),
                                                                   [&a](const UsbDevice &h) {
                                                                       return a.matches(h);
                                                                   });
                if (same > 1) {
                    text += ' ' + tr("%1 devices with these IDs are plugged in, which USB "
                                     "settings cannot tell apart: give one to each VM from USB "
                                     "Devices instead.")
                                      .arg(same);
                }
                return text;
            }
        }
    }
    return {};
}

Vm *UsbGuard::attachConflict(VmStore *store, const Vm *vm, const UsbDevice &host, QString *why,
                             const QList<UsbDevice> *hosts)
{
    if (!store) {
        return nullptr;
    }
    for (Vm *other : store->vms()) {
        if (other == vm || !other->runner()->isActive()) {
            continue;
        }
        for (const Device &d : devicesOf(other)) {
            if (!d.matches(host)) {
                continue;
            }
            if (why) {
                /* a rule by IDs that takes identical devices: not this one, maybe */
                qsizetype same = 1;
                if (!concrete(d)) {
                    const QList<UsbDevice> plugged = hosts ? *hosts : UsbHotplug::hostDevices();
                    same = std::count_if(plugged.cbegin(), plugged.cend(),
                                         [&d](const UsbDevice &h) { return d.matches(h); });
                }
                *why = same > 1 ? tr("%1 passes devices with these IDs through: its QEMU may "
                                     "take this one too.")
                                      .arg(other->name())
                                : tr("%1 has it: take it back there first.").arg(other->name());
            }
            return other;
        }
    }
    return nullptr;
}

UsbGuard::Scan UsbGuard::scan(const QString &sysfs, const QString &dev, const QString &proc)
{
    const QByteArray procPath = QFile::encodeName(proc);
    const QByteArray prefix = QFile::encodeName(dev) + "/bus/usb/";
    Scan s;

    s.hosts = HostDevices::usbDevices(sysfs);
    s.hosts.removeIf([](const UsbDevice &d) { return d.isHub || d.bus <= 0 || d.device <= 0; });
    if (s.hosts.isEmpty()) {
        return s;
    }
    DIR *pids = ::opendir(procPath.constData());
    if (!pids) {
        return s;
    }
    while (const dirent *entry = ::readdir(pids)) {
        char *end = nullptr;
        const long long pid = std::strtoll(entry->d_name, &end, 10);
        QSet<QByteArray> nodes;

        if (pid <= 0 || *end != '\0') {
            continue;
        }
        /* another user's: not to be read */
        DIR *fds = ::opendir((procPath + '/' + entry->d_name + "/fd").constData());
        if (!fds) {
            continue;
        }
        while (const dirent *fd = ::readdir(fds)) {
            /* the link alone: nothing is looked up where it points (a file on a
               network file system may wait for its server) */
            const QByteArray target = fd->d_name[0] == '.' ? QByteArray()
                                                           : linkAt(::dirfd(fds), fd->d_name);
            if (target.startsWith(prefix) && !target.endsWith(" (deleted)")) {
                nodes.insert(target);
            }
        }
        ::closedir(fds);
        if (nodes.isEmpty()) {
            continue;
        }
        const Holder who = holderOf(proc, pid);
        for (const QByteArray &node : std::as_const(nodes)) {
            s.held[QFile::decodeName(node)] << who;
        }
    }
    ::closedir(pids);
    return s;
}

QList<UsbGuard::Issue> UsbGuard::issuesOf(const QList<Run> &runs, const Scan &scan,
                                          const QString &dev, const QHash<qint64, QString> &qemus)
{
    QList<Issue> out;
    /* /dev/bus/usb/003/012, under @dev */
    const auto nodeOf = [&dev](const UsbDevice &h) { return dev + h.devNode().mid(4); };
    const auto holds = [&scan, &nodeOf](const UsbDevice &h, qint64 pid) {
        const QList<Holder> who = scan.held.value(nodeOf(h));
        return pid > 0 && std::any_of(who.cbegin(), who.cend(),
                                      [pid](const Holder &w) { return w.pid == pid; });
    };

    for (const Run &run : runs) {
        QList<const UsbDevice *> passed;
        for (const Device &d : run.devices) {
            QList<const UsbDevice *> matching, its;
            for (const UsbDevice &h : scan.hosts) {
                if (d.matches(h)) {
                    matching << &h;
                    if (holds(h, run.pid)) {
                        its << &h;
                    }
                }
            }
            /* of identical devices, the one its QEMU holds; all of them while it holds none */
            for (const UsbDevice *h : its.isEmpty() ? matching : its) {
                if (!passed.contains(h)) {
                    passed << h;
                }
            }
        }
        for (const UsbDevice *h : std::as_const(passed)) {
            Issue issue;
            bool own = false;

            for (Holder who : scan.held.value(nodeOf(*h))) {
                if (who.pid == run.pid) {
                    own = true;
                    continue;
                }
                if (const auto it = qemus.constFind(who.pid); it != qemus.cend()) {
                    who.kind = Kind::Qemu;
                    who.vmName = it.value();
                    who.vitrine = true;
                }
                issue.holders << who;
            }
            issue.claimedElsewhere = issue.holders.isEmpty() && !own &&
                                     h->interfaceDrivers.contains("usbfs");
            if (issue.holders.isEmpty() && !issue.claimedElsewhere) {
                continue;
            }
            issue.vmId = run.id;
            issue.vmName = run.name;
            issue.device = *h;
            out << issue;
        }
    }
    return out;
}

UsbGuard::Holder::Kind UsbGuard::kindOf(const QString &program)
{
    /* a comm has 15 characters at most: gvfs-mtp-volume for gvfs-mtp-volume-monitor */
    if (program == "adb") {
        return Kind::Adb;
    }
    if (program == "gvfsd-mtp" || program.startsWith("gvfs-mtp-volume")) {
        return Kind::Mtp;
    }
    if (program == "gvfsd-gphoto2" || program.startsWith("gvfs-gphoto2-vo")) {
        return Kind::Gphoto2;
    }
    if (program.startsWith("qemu-system-") || program.startsWith("qemu-kvm")) {
        return Kind::Qemu;
    }
    return Kind::Other;
}

UsbGuard::UsbGuard(VmStore *store, QObject *parent)
    : QObject(parent), m_store(store), m_settle(new QTimer(this)), m_recheck(new QTimer(this)),
      m_watcher(new QFutureWatcher<Scan>(this))
{
    if (!s_instance) {
        s_instance = this;
    }
    m_settle->setSingleShot(true);
    connect(m_settle, &QTimer::timeout, this, &UsbGuard::startScan);
    connect(m_recheck, &QTimer::timeout, this, &UsbGuard::startScan);
    connect(m_watcher, &QFutureWatcher<Scan>::finished, this, [this]() {
        scanned(m_watcher->result());
        if (std::exchange(m_again, false)) {
            startScan();
        }
    });
    if (store) {
        for (Vm *vm : store->vms()) {
            watchVm(vm);
        }
        connect(store, &VmStore::added, this, &UsbGuard::watchVm);
    }
    vmsChanged();
}

UsbGuard::~UsbGuard()
{
    /* a scan still running ends by itself: it reads files, nothing of this */
    listen(false);
}

UsbGuard *UsbGuard::instance()
{
    return s_instance;
}

void UsbGuard::setDelays(int settleMs, int recheckMs)
{
    m_settleMs = settleMs;
    m_recheckMs = recheckMs;
    if (m_recheck->isActive()) {
        m_recheck->start(m_recheckMs);
    }
}

void UsbGuard::watchVm(Vm *vm)
{
    const QPointer<UsbGuard> self(this);
    UsbHotplug *hotplug = UsbHotplug::of(vm->runner());

    /* the checks live in the VM's runner and hotplug, which go with it */
    vm->runner()->setStartCheck([self, vm](const ArgsFile &args) {
        return self ? startConflict(self->m_store, vm, args) : QString();
    });
    hotplug->setAttachCheck([self, vm](const UsbDevice &host) {
        QString why;
        if (self) {
            attachConflict(self->m_store, vm, host, &why);
        }
        return why;
    });
    /* read at the start, given or taken back a device, stopped */
    connect(hotplug, &UsbHotplug::changed, this, &UsbGuard::vmsChanged);
}

void UsbGuard::vmsChanged()
{
    const bool any = !runs().isEmpty();

    listen(any);
    if (any) {
        settle();
    } else {
        /* nothing passed through: nothing read, nothing kept */
        m_settle->stop();
        m_recheck->stop();
        m_last = {};
        m_unsure.clear();
        m_confirmed.clear();
    }
    prune();
}

void UsbGuard::prune()
{
    QSet<QString> running;
    QSet<QString> told;

    for (const Run &run : runs()) {
        running.insert(run.id);
    }
    const qsizetype before = m_issues.size();
    m_issues.removeIf([&running](const Issue &issue) { return !running.contains(issue.vmId); });
    /* told again in a new run */
    for (const QString &key : std::as_const(m_told)) {
        if (running.contains(key.section(' ', 0, 0))) {
            told.insert(key);
        }
    }
    m_told = told;
    if (m_issues.isEmpty()) {
        m_recheck->stop();
    }
    if (m_issues.size() != before) {
        emit changed();
    }
}

QList<UsbGuard::Run> UsbGuard::runs() const
{
    QList<Run> list;

    if (!m_store) {
        return list;
    }
    for (Vm *vm : m_store->vms()) {
        VmRunner *runner = vm->runner();
        if (!runner->isActive()) {
            continue;
        }
        const UsbHotplug *hotplug = UsbHotplug::of(runner);
        if (hotplug->isKnown() && !hotplug->devices().isEmpty()) {
            list << Run{vm->id(), vm->name(), runner->pid(), hotplug->devices()};
        }
    }
    return list;
}

QHash<qint64, QString> UsbGuard::qemus() const
{
    QHash<qint64, QString> map;

    if (!m_store) {
        return map;
    }
    for (Vm *vm : m_store->vms()) {
        if (vm->runner()->isActive() && vm->runner()->pid() > 0) {
            map.insert(vm->runner()->pid(), vm->name());
        }
    }
    return map;
}

void UsbGuard::listen(bool on)
{
    if (on == (m_uevents >= 0)) {
        return;
    }
    if (!on) {
        delete std::exchange(m_notifier, nullptr);
        ::close(std::exchange(m_uevents, -1));
        return;
    }
    /* the kernel's uevents, which any user may read (udev's own are another group) */
    const int fd = ::socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                            NETLINK_KOBJECT_UEVENT);
    sockaddr_nl address = {};
    address.nl_family = AF_NETLINK;
    address.nl_groups = 1;
    if (fd < 0) {
        /* without: the scans as the VMs change, and while one holds */
        return;
    }
    if (::bind(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return;
    }
    m_uevents = fd;
    m_notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &UsbGuard::readUevents);
}

void UsbGuard::readUevents()
{
    /* "add@/devices/...", then KEY=VALUE fields, each ending with a NUL */
    static constexpr std::string_view kUsb("\0SUBSYSTEM=usb\0", 15);
    char buffer[8192];
    bool usb = false;

    /* what is there, without waiting: the socket does not block */
    for (;;) {
        const ssize_t n = ::recv(m_uevents, buffer, sizeof(buffer), 0);
        if (n > 0) {
            usb |= std::string_view(buffer, size_t(n)).find(kUsb) != std::string_view::npos;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        /* events lost for want of room: some may have been USB's */
        usb |= n < 0 && errno == ENOBUFS;
        break;
    }
    if (usb) {
        hostChanged();
    }
}

void UsbGuard::hostChanged()
{
    if (!runs().isEmpty()) {
        settle();
    }
}

void UsbGuard::settle()
{
    /* the first change of a burst sets the time: a stream of them does not put it off */
    if (!m_settle->isActive()) {
        m_settle->start(m_settleMs);
    }
}

void UsbGuard::startScan()
{
    if (m_watcher->isRunning()) {
        m_again = true;
        return;
    }
    if (runs().isEmpty()) {
        return;
    }
    m_watcher->setFuture(QtConcurrent::run(&UsbGuard::scan, UsbHotplug::sysfsRoot(),
                                           UsbHotplug::devRoot(), m_procRoot));
}

void UsbGuard::scanNow()
{
    scanned(scan(UsbHotplug::sysfsRoot(), UsbHotplug::devRoot(), m_procRoot));
}

void UsbGuard::scanned(const Scan &result)
{
    QSet<QString> unsure;

    m_last = result;
    for (const Issue &issue : issuesOf(runs(), m_last, UsbHotplug::devRoot(), qemus())) {
        if (issue.claimedElsewhere) {
            unsure.insert(issue.vmId + ' ' + issue.device.devNode());
        }
    }
    m_confirmed = QSet<QString>(unsure).intersect(m_unsure);
    m_unsure = unsure;
    update();
    /* one found once: looked at again soon */
    if (m_unsure != m_confirmed) {
        settle();
    }
}

void UsbGuard::update()
{
    QList<Issue> now = issuesOf(runs(), m_last, UsbHotplug::devRoot(), qemus());
    QStringList keys, before;

    /* those that ended since the scan hold nothing (a QEMU of a run before, adb stopped) */
    for (Issue &issue : now) {
        issue.holders.removeIf([this](const Holder &who) {
            return !QFileInfo::exists(QString("%1/%2").arg(m_procRoot).arg(who.pid));
        });
    }
    now.removeIf([this](const Issue &issue) {
        return issue.holders.isEmpty() &&
               (!issue.claimedElsewhere ||
                !m_confirmed.contains(issue.vmId + ' ' + issue.device.devNode()));
    });
    for (const Issue &issue : std::as_const(now)) {
        keys << keyOf(issue);
    }
    for (const Issue &issue : std::as_const(m_issues)) {
        before << keyOf(issue);
    }
    m_issues = now;
    /* held: looked at again every few seconds, which tells when they let go */
    if (m_issues.isEmpty()) {
        m_recheck->stop();
    } else if (!m_recheck->isActive()) {
        m_recheck->start(m_recheckMs);
    }
    if (keys != before) {
        emit changed();
    }

    /* each holder told once while it holds: the VM's log, notice() */
    QSet<QString> told;
    for (const Issue &issue : std::as_const(now)) {
        const QString at = issue.vmId + ' ' + issue.device.devNode() + ' ';
        QStringList whos;
        bool fresh = false;

        if (issue.holders.isEmpty()) {
            whos << at + '-';
        }
        for (const Holder &who : issue.holders) {
            whos << at + QString::number(who.pid);
        }
        for (const QString &who : std::as_const(whos)) {
            fresh |= !m_told.contains(who);
            told.insert(who);
        }
        if (!fresh) {
            continue;
        }
        if (Vm *vm = m_store ? m_store->find(issue.vmId) : nullptr) {
            vm->runner()->appendNote(logLine(issue));
        }
        emit notice(issue.text() + '.');
    }
    m_told = told;
}
