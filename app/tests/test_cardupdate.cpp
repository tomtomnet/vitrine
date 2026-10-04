// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include "core/cardupdate.h"
#include "core/paths.h"
#include "core/vmtemplate.h"

using namespace CardUpdate;

namespace QTest {
template <>
char *toString(const Change &c)
{
    return qstrdup(qPrintable(QString("{%1 %2=%3}").arg(int(c.kind)).arg(c.key, c.value)));
}
}

/* The card of vitrine's QEMU, all of it, as the template writes it */
static const char kFullCard[] = "virtio-gpu-gl-pci,hostmem=4G,blob=on,drm_native_context=on,"
                                "x-host-vblank=on,x-vblank-lead=3000,x-vblank-lead-auto=on";

static Change add(const QString &key, const QString &value)
{
    return {Change::AddProperty, key, value};
}

static const Change kVenus{Change::RemoveVenus, "venus", "off"};
static const Change kPat{Change::HonorGuestPat, "honor-guest-pat", "auto"};
static const Change kMemory{Change::SharedMemory, {}, {}};

/* The vblank properties, which the user's VM lacks */
static QList<Change> vblank()
{
    return {add("x-host-vblank", "on"), add("x-vblank-lead", "3000"),
            add("x-vblank-lead-auto", "on")};
}

/* A Linux VM on a PC with @card, KVM and shared memory: only the card to update */
static ArgsFile vm(const QString &card, const QString &extra = {})
{
    return ArgsFile::parse("#guest linux\n"
                           "-machine q35,memory-backend=mem\n"
                           "-accel kvm,honor-guest-pat=on\n"
                           "-object memory-backend-memfd,id=mem,size=4G,share=on\n"
                           "-vga none\n"
                           "-device " + card + "\n"
                           "-display dbus,p2p=yes,gl=on\n" + extra);
}

/* The keys @changes add to the card */
static QStringList added(const QList<Change> &changes)
{
    QStringList keys;

    for (const Change &c : changes) {
        if (c.kind == Change::AddProperty) {
            keys << c.key;
        }
    }
    return keys;
}

class TestCardUpdate : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;

    static bool script(const QString &path, const QByteArray &body)
    {
        QFile f(path);

        if (!QDir().mkpath(QFileInfo(path).path()) || !f.open(QIODevice::WriteOnly) ||
            f.write("#!/bin/sh\n" + body + "\n") < 0) {
            return false;
        }
        f.close();
        return f.setPermissions(QFileDevice::ReadOwner | QFileDevice::ExeOwner);
    }

    /* A QEMU that answers for @card and kvm-accel with @properties and @accel */
    QString fakeQemu(const QString &name, const QString &card, const QStringList &properties,
                     const QStringList &accel)
    {
        const QString path = m_tmp.filePath(name + "/" + Paths::qemuSystemName());
        QByteArray body = "case \"$1 $2\" in\n";

        body += "'-device " + card.toUtf8() + ",help')\n  echo '" + card.toUtf8() + " options:'\n";
        for (const QString &p : properties) {
            body += "  echo '  " + p.toUtf8() + "=<bool>'\n";
        }
        body += "  ;;\n'-object kvm-accel,help')\n  echo 'kvm-accel options:'\n";
        for (const QString &p : accel) {
            body += "  echo '  " + p.toUtf8() + "=<bool>'\n";
        }
        body += "  ;;\nesac";
        return script(path, body) ? path : QString();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        if (Paths::hostArch() != "x86_64") {
            QSKIP("the 3D card of Linux on a PC");
        }
        /* settings and stack in here: vitrine's QEMU not built, none chosen */
        qputenv("XDG_DATA_HOME", m_tmp.filePath("data").toUtf8());
        qputenv("XDG_CONFIG_HOME", m_tmp.filePath("config").toUtf8());
        qputenv("XDG_CACHE_HOME", m_tmp.filePath("cache").toUtf8());
        Paths::setQemuBinary({});
    }

    /*
     * The user's desktop VM: made while Fedora's QEMU stood in for
     * vitrine's (no native context, no vblank, -accel kvm, venus=off),
     * then given drm_native_context=on by hand
     */
    void userVm()
    {
        const QString before =
            "# The QEMU command line of this VM, one option per line.\n"
            "# Lines starting with # are comments; #share lines are shared folders.\n"
            "\n"
            "-name Fedora KDE,debug-threads=on\n"
            "#guest linux,desktop=kde\n"
            "#env MESA_DEBUG=1\n"
            "\n"
            "# System\n"
            "-machine q35,memory-backend=mem,dump-guest-core=off\n"
            "-accel kvm\n"
            "-cpu host,topoext=on\n"
            "-smp 8,sockets=1,cores=4,threads=2\n"
            "-object memory-backend-memfd,id=mem,size=8G,share=on\n"
            "\n"
            "# Firmware\n"
            "-drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd\n"
            "-drive if=pflash,format=raw,unit=1,file=OVMF_VARS.fd\n"
            "\n"
            "# Display\n"
            "-vga none\n"
            "-device virtio-gpu-gl-pci,hostmem=4G,blob=on,venus=off,drm_native_context=on\n"
            "-display dbus,p2p=yes,gl=on\n"
            "\n"
            "# Storage\n"
            "-object iothread,id=iodisk\n"
            "-drive file=disk.qcow2,format=qcow2,if=none,id=disk0,discard=unmap\n"
            "-device virtio-blk-pci,drive=disk0,iothread=iodisk\n"
            "\n"
            "# Network\n"
            "-nic none\n"
            "-netdev user,id=net0,hostfwd=tcp:127.0.0.1:10022-:22\n"
            "-device virtio-net-pci,netdev=net0\n"
            "# my own: a serial console\n"
            "-serial file:serial.log\n"
            "-global kvm-pit.lost_tick_policy=discard\n";
        const ArgsFile args = ArgsFile::parse(before);
        const QList<Change> found = changes(args);

        QCOMPARE(found, vblank() << kVenus << kPat);
        const ArgsFile after = apply(args, found);
        QString expected = before;
        expected.replace("-accel kvm\n", "-accel kvm,honor-guest-pat=auto\n");
        expected.replace("-device virtio-gpu-gl-pci,hostmem=4G,blob=on,venus=off,"
                         "drm_native_context=on\n",
                         QString("-device %1\n").arg(kFullCard));
        QCOMPARE(after.toText(), expected);
        QVERIFY(changes(after).isEmpty());

        /* what the dialog shows */
        const LineDiff d = diff(args, after);
        QCOMPARE(d.before, QStringList({"-accel kvm",
                                        "-device virtio-gpu-gl-pci,hostmem=4G,blob=on,venus=off,"
                                        "drm_native_context=on"}));
        QCOMPARE(d.after, QStringList({"-accel kvm,honor-guest-pat=auto",
                                       QString("-device %1").arg(kFullCard)}));
    }

    /* As the template made it before it adapted to Fedora's QEMU: nothing to do */
    void templateVm()
    {
        QVERIFY(changes(vm(kFullCard)).isEmpty());
        /* and what the template writes is what the update would */
        QString card = "virtio-gpu-gl-pci";
        for (const auto &[key, value] : VmTemplate::cardProperties()) {
            card += QString(",%1=%2").arg(key, value);
        }
        QCOMPARE(card, kFullCard);
        QCOMPARE(apply(vm("virtio-gpu-gl-pci"), changes(vm("virtio-gpu-gl-pci"))).toText(),
                 vm(kFullCard).toText());
    }

    /*
     * The research launcher's VM (host/run-vm.sh): all of it, with Venus
     * off and the swap targets it sets itself, which stay
     */
    void researchVm()
    {
        const QString before =
            "-name f44-kde-test,debug-threads=on\n"
            "-machine q35,dump-guest-core=off\n"
            "-accel kvm,honor-guest-pat=on\n"
            "-vga none\n"
            "-cpu host\n"
            "-smp 8,sockets=1,cores=4,threads=2\n"
            "-m 8G\n"
            "-object memory-backend-memfd,id=mem,size=8G,share=on\n"
            "-machine memory-backend=mem\n"
            "-drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd\n"
            "-drive if=pflash,format=raw,file=f44-kde-test-OVMF_VARS.fd\n"
            "-object iothread,id=iodisk\n"
            "-drive file=f44-kde-test.qcow2,if=none,id=d0,cache=none,discard=unmap\n"
            "-device virtio-blk-pci,drive=d0,iothread=iodisk\n"
            "-device virtio-gpu-gl-pci,hostmem=4G,blob=on,drm_native_context=on,venus=off,"
            "x-host-vblank=on,x-vblank-lead=3000,x-vblank-lead-auto=on,"
            "x-vblank-swap-target=4500,x-vblank-swap-target-zc=3500\n"
            "-display sdl,gl=on\n"
            "-device virtio-keyboard-pci\n"
            "-device virtio-tablet-pci\n"
            "-netdev user,id=n0,hostfwd=tcp:127.0.0.1:2222-:22\n"
            "-device virtio-net-pci,netdev=n0\n"
            "-device virtio-rng-pci\n"
            "-audio driver=pipewire,model=virtio\n";
        ArgsFile args = ArgsFile::parse(before);

        /* venus=off alone is not worth a banner */
        QVERIFY(changes(args).isEmpty());

        /* without honor-guest-pat, Venus goes with it */
        args = ArgsFile::parse(QString(before).replace("-accel kvm,honor-guest-pat=on\n",
                                                       "-accel kvm\n"));
        QCOMPARE(changes(args), QList<Change>({kVenus, kPat}));
        QCOMPARE(apply(args, changes(args)).toText(),
                 QString(before).replace(",venus=off", "").replace("pat=on", "pat=auto"));
    }

    /* Each property the card lacks, and none it sets, whatever to */
    void eachProperty_data()
    {
        QTest::addColumn<QString>("key");
        for (const auto &[key, value] : VmTemplate::cardProperties()) {
            QTest::newRow(qPrintable(key)) << key;
        }
    }

    void eachProperty()
    {
        QFETCH(QString, key);
        QString value;
        for (const auto &p : VmTemplate::cardProperties()) {
            if (p.first == key) {
                value = p.second;
            }
        }
        QString full = kFullCard;
        const QString item = QString(",%1=%2").arg(key, value);

        /* absent: offered, and put back where the template has it */
        const ArgsFile lacking = vm(QString(full).remove(item));
        QCOMPARE(changes(lacking), QList<Change>({add(key, value)}));
        QCOMPARE(apply(lacking, changes(lacking)).toText(), vm(full).toText());

        /* another value, or a bare key: the user's */
        for (const QString &other : {QString("%1=1").arg(key), QString("%1=off").arg(key),
                                     key, "no" + key}) {
            const ArgsFile set = vm(QString(full).replace(item, "," + other));
            QVERIFY2(!added(changes(set)).contains(key), qPrintable(other));
        }
        /* or set with -global, for any of the card's types */
        const ArgsFile global =
            vm(QString(full).remove(item),
               QString("-global virtio-gpu-base.%1=%2\n").arg(key, value == "on" ? "off" : "1"));
        QVERIFY(!added(changes(global)).contains(key));
        const ArgsFile globalLong =
            vm(QString(full).remove(item),
               QString("-global driver=virtio-gpu-gl-pci,property=%1,value=0\n").arg(key));
        QVERIFY(!added(changes(globalLong)).contains(key));
    }

    /* What the user set by hand stays theirs, and what goes with it */
    void explicitValues()
    {
        /* the host's vblank off: no lead either */
        QCOMPARE(changes(vm("virtio-gpu-gl-pci,hostmem=4G,blob=on,drm_native_context=on,"
                            "x-host-vblank=off")),
                 QList<Change>());
        QCOMPARE(changes(vm("virtio-gpu-gl-pci,x-host-vblank=off")),
                 QList<Change>({add("hostmem", "4G"), add("blob", "on"),
                                add("drm_native_context", "on")}));
        /* native context off: the rest still */
        QCOMPARE(changes(vm("virtio-gpu-gl-pci,hostmem=4G,blob=on,drm_native_context=off")),
                 vblank());
        /* a hostmem of its own */
        QCOMPARE(changes(vm("virtio-gpu-gl-pci,hostmem=8G,blob=on,drm_native_context=on")),
                 vblank());
        /* a lead fixed by hand stays fixed; the template's own may follow */
        QCOMPARE(changes(vm("virtio-gpu-gl-pci,hostmem=4G,blob=on,drm_native_context=on,"
                            "x-host-vblank=on,x-vblank-lead=2000")),
                 QList<Change>());
        QCOMPARE(changes(vm("virtio-gpu-gl-pci,hostmem=4G,blob=on,drm_native_context=on,"
                            "x-host-vblank=on,x-vblank-lead=3000")),
                 QList<Change>({add("x-vblank-lead-auto", "on")}));
        /* with -global too, where the card's line wins */
        QCOMPARE(changes(vm("virtio-gpu-gl-pci,hostmem=4G,blob=on,drm_native_context=on,"
                            "x-host-vblank=on",
                            "-global virtio-gpu-gl-pci.x-vblank-lead=5000\n")),
                 QList<Change>());
        QCOMPARE(changes(vm("virtio-gpu-gl-pci,hostmem=4G,blob=on,drm_native_context=on,"
                            "x-host-vblank=on",
                            "-global driver=virtio-gpu-base,property=x-vblank-lead,"
                            "value=2000\n")),
                 QList<Change>());
        QCOMPARE(changes(vm("virtio-gpu-gl-pci,hostmem=4G,blob=on,drm_native_context=on,"
                            "x-host-vblank=on",
                            "-global virtio-gpu-gl-pci.x-vblank-lead=3000\n")),
                 QList<Change>({add("x-vblank-lead-auto", "on")}));
        QCOMPARE(changes(vm("virtio-gpu-gl-pci,hostmem=4G,blob=on,drm_native_context=on,"
                            "x-host-vblank=on,x-vblank-lead=3000",
                            "-global virtio-gpu-gl-pci.x-vblank-lead=5000\n")),
                 QList<Change>({add("x-vblank-lead-auto", "on")}));
        const ArgsFile globalLead =
            vm("virtio-gpu-gl-pci,hostmem=4G,blob=on,drm_native_context=on,x-host-vblank=on",
               "-global virtio-gpu-gl-pci.x-vblank-lead=3000\n"
               "-global virtio-gpu-gl-pci.x-vblank-lead=5000\n");
        QCOMPARE(changes(globalLead), QList<Change>());
        QCOMPARE(apply(globalLead, {add("x-vblank-lead-auto", "on")}).toText(),
                 vm("virtio-gpu-gl-pci,hostmem=4G,blob=on,drm_native_context=on,"
                    "x-host-vblank=on,x-vblank-lead-auto=on",
                    "-global virtio-gpu-gl-pci.x-vblank-lead=3000\n"
                    "-global virtio-gpu-gl-pci.x-vblank-lead=5000\n")
                     .toText());
        /* the host's vblank on in any form, QEMU's default: the lead goes with it */
        const QList<Change> lead = {add("x-vblank-lead", "3000"),
                                    add("x-vblank-lead-auto", "on")};
        for (const QString &on : {QString("x-host-vblank"), QString("x-host-vblank=yes"),
                                  QString("x-host-vblank=off,x-host-vblank=on")}) {
            QCOMPARE(changes(vm("virtio-gpu-gl-pci,hostmem=4G,blob=on,drm_native_context=on," +
                                on)),
                     lead);
        }
        QCOMPARE(changes(vm("virtio-gpu-gl-pci,hostmem=4G,blob=on,drm_native_context=on",
                            "-global virtio-gpu-gl-pci.x-host-vblank=on\n")),
                 lead);
        /* and off in any form: no lead */
        for (const QString &off : {QString("nox-host-vblank"), QString("x-host-vblank=no"),
                                   QString("x-host-vblank=on,x-host-vblank=false")}) {
            QCOMPARE(changes(vm("virtio-gpu-gl-pci,hostmem=4G,blob=on,drm_native_context=on," +
                                off)),
                     QList<Change>());
        }
        QCOMPARE(changes(vm("virtio-gpu-gl-pci,hostmem=4G,blob=on,drm_native_context=on",
                            "-global virtio-gpu-gl-pci.x-host-vblank=off\n")),
                 QList<Change>());
        QCOMPARE(changes(vm("virtio-gpu-gl-pci,hostmem=4G,blob=on,drm_native_context=on",
                            "-global driver=virtio-gpu-base,property=x-host-vblank,"
                            "value=off\n")),
                 QList<Change>());
        /* the card's line wins over -global */
        QCOMPARE(changes(vm("virtio-gpu-gl-pci,hostmem=4G,blob=on,drm_native_context=on,"
                            "x-host-vblank=on",
                            "-global virtio-gpu-gl-pci.x-host-vblank=off\n")),
                 lead);
        /* Venus on is the user's */
        const ArgsFile venus = vm("virtio-gpu-gl-pci,venus=on,hostmem=4G,blob=on");
        QCOMPARE(changes(venus), QList<Change>({add("drm_native_context", "on")}) + vblank());
        QCOMPARE(apply(venus, changes(venus)).toText(),
                 vm("virtio-gpu-gl-pci,venus=on,hostmem=4G,blob=on,drm_native_context=on,"
                    "x-host-vblank=on,x-vblank-lead=3000,x-vblank-lead-auto=on")
                     .toText());
    }

    void venusRemoved()
    {
        /* alone: nothing */
        QCOMPARE(changes(vm(QString(kFullCard) + ",venus=off")), QList<Change>());
        QCOMPARE(changes(vm(QString(kFullCard) + ",novenus")), QList<Change>());

        /* with the rest: after the card's properties, before -accel */
        ArgsFile args = vm("virtio-gpu-gl-pci,venus=off,hostmem=4G,blob=on,drm_native_context=on",
                           "-m 4G\n");
        args = ArgsFile::parse(args.toText().replace("-accel kvm,honor-guest-pat=on",
                                                     "-accel kvm"));
        QCOMPARE(changes(args), vblank() << kVenus << kPat);
        QCOMPARE(apply(args, changes(args)).toText(),
                 vm(kFullCard, "-m 4G\n").toText().replace("pat=on", "pat=auto"));

        /* every form of off, and nothing else */
        args = vm("virtio-gpu-gl-pci,novenus,venus=no,blob=on,id=gpu");
        const ArgsFile after = apply(args, changes(args));
        QCOMPARE(after.lines[5].value,
                 "virtio-gpu-gl-pci,hostmem=4G,blob=on,drm_native_context=on,x-host-vblank=on,"
                 "x-vblank-lead=3000,x-vblank-lead-auto=on,id=gpu");
    }

    void accel()
    {
        const QString card = kFullCard;
        auto withAccel = [&card](const QString &lines) {
            return ArgsFile::parse("#guest linux\n" + lines +
                                   "-object memory-backend-memfd,id=mem,size=4G\n"
                                   "-device " + card + "\n");
        };

        /* -accel kvm: the property added */
        ArgsFile args = withAccel("-machine q35,memory-backend=mem\n-accel kvm\n");
        QCOMPARE(changes(args), QList<Change>({kPat}));
        QCOMPARE(apply(args, changes(args)).toText(),
                 withAccel("-machine q35,memory-backend=mem\n-accel kvm,honor-guest-pat=auto\n")
                     .toText());
        /* -enable-kvm and -machine accel=: -accel takes over */
        args = withAccel("-machine q35,memory-backend=mem\n-enable-kvm\n");
        QCOMPARE(changes(args), QList<Change>({kPat}));
        QCOMPARE(apply(args, changes(args)).toText(),
                 withAccel("-machine q35,memory-backend=mem\n-accel kvm,honor-guest-pat=auto\n")
                     .toText());
        args = withAccel("-machine q35,accel=kvm,memory-backend=mem\n");
        QCOMPARE(apply(args, changes(args)).toText(),
                 withAccel("-machine q35,memory-backend=mem\n-accel kvm,honor-guest-pat=auto\n")
                     .toText());
        args = withAccel("-machine q35,memory-backend=mem\n-accel kvm,kernel-irqchip=split\n");
        QCOMPARE(apply(args, changes(args)).toText(),
                 withAccel("-machine q35,memory-backend=mem\n"
                           "-accel kvm,kernel-irqchip=split,honor-guest-pat=auto\n")
                     .toText());

        /* set by hand, either way; or no KVM */
        QVERIFY(changes(withAccel("-machine q35,memory-backend=mem\n"
                                  "-accel kvm,honor-guest-pat=off\n")).isEmpty());
        QVERIFY(changes(withAccel("-machine q35,memory-backend=mem\n"
                                  "-accel kvm,honor-guest-pat=auto\n")).isEmpty());
        QVERIFY(changes(withAccel("-machine q35,memory-backend=mem\n-accel tcg\n")).isEmpty());
        QVERIFY(changes(withAccel("-machine q35,memory-backend=mem\n")).isEmpty());
        /* a QEMU without it */
        args = withAccel("-machine q35,memory-backend=mem\n-accel kvm\n");
        QVERIFY(changes(args, Offers{std::nullopt, QStringList({"kernel-irqchip"})}).isEmpty());
        QCOMPARE(changes(args, Offers{std::nullopt, QStringList({"honor-guest-pat"})}),
                 QList<Change>({kPat}));
    }

    void memory()
    {
        const QString card = QString("-device %1\n").arg(kFullCard);
        const QString top = "#guest linux\n-accel kvm,honor-guest-pat=on\n";

        /* QEMU's own RAM: a memfd backend of the same size */
        ArgsFile args = ArgsFile::parse(top + "-machine q35\n-m 6G\n" + card);
        QCOMPARE(changes(args), QList<Change>({kMemory}));
        QCOMPARE(apply(args, changes(args)).toText(),
                 top + "-machine q35,memory-backend=mem\n-m 6G\n" + card +
                     "-object memory-backend-memfd,id=mem,size=6G,share=on\n");
        /* a RAM backend becomes a memfd one */
        args = ArgsFile::parse(top + "-machine q35,memory-backend=ram0\n"
                                     "-object memory-backend-ram,id=ram0,size=4G\n" + card);
        QCOMPARE(changes(args), QList<Change>({kMemory}));
        QCOMPARE(apply(args, changes(args)).toText(),
                 top + "-machine q35,memory-backend=ram0\n"
                       "-object memory-backend-memfd,id=ram0,size=4G,share=on\n" + card);

        /* set up otherwise on purpose, or left to QEMU */
        for (const QString &memory :
             {QString("-machine q35,memory-backend=mem\n"
                      "-object memory-backend-memfd,id=mem,size=4G,share=off\n"),
              QString("-machine q35,memory-backend=mem\n"
                      "-object memory-backend-ram,id=mem,size=4G,share=off,prealloc=on\n"),
              QString("-machine q35,memory-backend=mem\n"
                      "-object memory-backend-ram,id=mem,size=4G,share=on\n"),
              QString("-machine q35,memory-backend=mem\n"
                      "-object memory-backend-ram,id=mem,size=4G,noshare\n"),
              QString("-machine q35,memory-backend=mem\n"
                      "-object memory-backend-file,id=mem,size=4G,mem-path=/dev/hugepages\n"),
              QString("-machine q35\n-m 4G\n-mem-path /dev/hugepages\n"),
              QString("-machine q35\n-m 4G\n"
                      "-object memory-backend-ram,id=n0,size=4G\n-numa node,memdev=n0\n"),
              QString("-machine q35\n")}) {
            QVERIFY2(changes(ArgsFile::parse(top + memory + card)).isEmpty(),
                     qPrintable(memory));
        }
    }

    /* Only for Linux on a PC with one 3D card of the template's kind */
    void exclusions_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<bool>("offered");
        const QString card = "-device virtio-gpu-gl-pci\n";

        QTest::newRow("linux") << "#guest linux\n" + card << true;
        QTest::newRow("desktop alone") << "#guest desktop=kde\n" + card << true;
        QTest::newRow("windows") << "#guest windows\n" + card << false;
        QTest::newRow("other") << "#guest other\n" + card << false;
        QTest::newRow("freebsd") << "#guest freebsd\n" + card << false;
        QTest::newRow("no directive") << card << true;
        QTest::newRow("no directive, hyper-v") << "-cpu host,hv-relaxed,hv-time\n" + card
                                               << false;
        QTest::newRow("virtio-vga-gl") << "#guest linux\n-device virtio-vga-gl\n" << true;
        QTest::newRow("virtio-gpu-gl") << "#guest linux\n-device virtio-gpu-gl\n" << true;
        QTest::newRow("mmio") << "#guest linux\n-device virtio-gpu-gl-device\n" << false;
        QTest::newRow("2d") << "#guest linux\n-device virtio-vga\n" << false;
        QTest::newRow("std") << "#guest linux\n-vga std\n" << false;
        QTest::newRow("qxl") << "#guest linux\n-device qxl-vga\n" << false;
        QTest::newRow("two cards")
            << "#guest linux\n" + card + "-device virtio-gpu-gl-pci,id=second\n" << false;
        QTest::newRow("vga beside") << "#guest linux\n-vga std\n" + card << false;
        QTest::newRow("nographic") << "#guest linux\n-nographic\n" + card << false;
        QTest::newRow("arm") << "#guest linux\n-machine virt\n" + card << false;
    }

    void exclusions()
    {
        QFETCH(QString, text);
        QFETCH(bool, offered);
        const ArgsFile args = ArgsFile::parse(text);

        QCOMPARE(!changes(args).isEmpty(), offered);
        QCOMPARE(!changes(args, Offers()).isEmpty(), offered);
        if (!offered) {
            QCOMPARE(apply(args, vblank()).toText(), args.toText());
        }
    }

    /* The card's other keys stay; the template's go where it writes them */
    void order()
    {
        ArgsFile args = vm("virtio-vga-gl,id=gpu0,bus=pcie.0");
        QCOMPARE(apply(args, changes(args)).lines[5].value,
                 "virtio-vga-gl,id=gpu0,bus=pcie.0,hostmem=4G,blob=on,drm_native_context=on,"
                 "x-host-vblank=on,x-vblank-lead=3000,x-vblank-lead-auto=on");
        args = vm("virtio-gpu-gl-pci,blob=on,hostmem=4G,xres=1920");
        QCOMPARE(apply(args, changes(args)).lines[5].value,
                 "virtio-gpu-gl-pci,blob=on,hostmem=4G,drm_native_context=on,x-host-vblank=on,"
                 "x-vblank-lead=3000,x-vblank-lead-auto=on,xres=1920");
        args = vm("virtio-gpu-gl-pci,drm_native_context=on,id=gpu,x-vblank-lead-auto=on");
        QCOMPARE(apply(args, changes(args)).lines[5].value,
                 "virtio-gpu-gl-pci,hostmem=4G,blob=on,drm_native_context=on,x-host-vblank=on,"
                 "x-vblank-lead=3000,id=gpu,x-vblank-lead-auto=on");

        /* changes made for other arguments: nothing twice, nothing undone */
        args = vm(kFullCard);
        QCOMPARE(apply(args, vblank() << kPat << kMemory).toText(), args.toText());
        args = ArgsFile::parse(vm(kFullCard).toText()
                                   .replace("-accel kvm,honor-guest-pat=on", "-accel tcg")
                                   .replace(",share=on", ",share=off"));
        QCOMPARE(apply(args, {kPat, kMemory}).toText(), args.toText());
    }

    /* A QEMU the user chose offers only what it has; one that does not answer, nothing */
    void chosenQemu()
    {
        const auto restore = qScopeGuard([]() { Paths::setQemuBinary({}); });
        const QStringList fork = {"blob", "hostmem", "venus", "drm_native_context",
                                  "x-host-vblank", "x-vblank-lead", "x-vblank-lead-auto"};
        const QString research =
            fakeQemu("research", "virtio-gpu-gl-pci", fork, {"honor-guest-pat"});
        const QString fedora =
            fakeQemu("fedora", "virtio-gpu-gl-pci", {"blob", "hostmem", "venus"},
                     {"kernel-irqchip"});
        const QString vga = fakeQemu("vga", "virtio-vga-gl", fork, {"honor-guest-pat"});
        const QString broken = m_tmp.filePath("broken/" + Paths::qemuSystemName());
        QVERIFY(!research.isEmpty() && !fedora.isEmpty() && !vga.isEmpty());
        QVERIFY(script(broken, "exit 1"));
        const QString lacking =
            "#guest linux\n-machine q35\n-accel kvm\n-m 4G\n-device virtio-gpu-gl-pci\n";
        const QList<Change> all = QList<Change>({add("hostmem", "4G"), add("blob", "on"),
                                                 add("drm_native_context", "on")}) +
                                  vblank() + QList<Change>({kPat, kMemory});

        /* vitrine's, not built: all of it */
        QVERIFY(Paths::stackQemu().isEmpty());
        QCOMPARE(changes(ArgsFile::parse(lacking)), all);

        /* the #qemu line's */
        QCOMPARE(changes(ArgsFile::parse("#qemu " + research + "\n" + lacking)), all);
        QCOMPARE(changes(ArgsFile::parse("#qemu " + fedora + "\n" + lacking)),
                 QList<Change>({add("hostmem", "4G"), add("blob", "on"), kMemory}));
        QCOMPARE(changes(ArgsFile::parse("#qemu " + broken + "\n" + lacking)), QList<Change>());
        QCOMPARE(changes(ArgsFile::parse("#qemu " + m_tmp.filePath("nowhere/qemu") + "\n" +
                                         lacking)),
                 QList<Change>());
        /* asked about the card the VM has */
        QCOMPARE(changes(ArgsFile::parse("#qemu " + vga + "\n" +
                                         QString(lacking).replace("virtio-gpu-gl-pci",
                                                                  "virtio-vga-gl"))),
                 all);
        QCOMPARE(changes(ArgsFile::parse("#qemu " + research + "\n" +
                                         QString(lacking).replace("virtio-gpu-gl-pci",
                                                                  "virtio-vga-gl"))),
                 QList<Change>({kPat, kMemory}));

        /* the preferences' */
        Paths::setQemuBinary(fedora);
        QCOMPARE(changes(ArgsFile::parse(lacking)),
                 QList<Change>({add("hostmem", "4G"), add("blob", "on"), kMemory}));
        Paths::setQemuBinary(broken);
        QCOMPARE(changes(ArgsFile::parse(lacking)), QList<Change>());
        /* the #qemu line first */
        QCOMPARE(changes(ArgsFile::parse("#qemu " + research + "\n" + lacking)), all);

        /* vitrine's, built, chosen by its path: taken for itself when it does not answer */
        const QString stack = Paths::stackDir();
        const auto unlink = qScopeGuard([stack]() { QFile::remove(stack + "/current"); });
        QVERIFY(stack.startsWith(m_tmp.path()));
        QVERIFY(script(stack + "/0123456789abcdef/bin/" + Paths::qemuSystemName(), "exit 1"));
        QVERIFY(QFile::link("0123456789abcdef", stack + "/current"));
        QVERIFY(!Paths::stackQemu().isEmpty());
        Paths::setQemuBinary(Paths::stackQemu());
        QCOMPARE(changes(ArgsFile::parse(lacking)), all);
        Paths::setQemuBinary({});
        QCOMPARE(changes(ArgsFile::parse(lacking)), all);
    }

    /* A QEMU is asked once until it changes, even when it does not answer */
    void askedOnce()
    {
        const auto restore = qScopeGuard([]() { Paths::setQemuBinary({}); });
        const QString count = m_tmp.filePath("counted/count");
        const QString failing = m_tmp.filePath("counted/" + Paths::qemuSystemName());
        const QString lacking =
            "#guest linux\n-machine q35\n-accel kvm\n-m 4G\n-device virtio-gpu-gl-pci\n";
        const auto calls = [&count]() {
            QFile f(count);
            return f.open(QIODevice::ReadOnly) ? f.readAll().count('\n') : 0;
        };

        QVERIFY(script(failing, "echo >> '" + count.toUtf8() + "'\nexit 1"));
        Paths::setQemuBinary(failing);
        QCOMPARE(changes(ArgsFile::parse(lacking)), QList<Change>());
        QCOMPARE(calls(), 1);
        /* by Details and the Console, and after an edit of the arguments */
        QCOMPARE(changes(ArgsFile::parse(lacking)), QList<Change>());
        QCOMPARE(changes(ArgsFile::parse(QString(lacking).replace("-m 4G", "-m 2G"))),
                 QList<Change>());
        QCOMPARE(calls(), 1);
        /* another card is asked about */
        QCOMPARE(changes(ArgsFile::parse(QString(lacking).replace("virtio-gpu-gl-pci",
                                                                  "virtio-vga-gl"))),
                 QList<Change>());
        QCOMPARE(calls(), 2);

        /* a new binary there is asked */
        QVERIFY(QFile::remove(failing));
        QCOMPARE(fakeQemu("counted", "virtio-gpu-gl-pci",
                          {"blob", "hostmem", "drm_native_context", "x-host-vblank",
                           "x-vblank-lead", "x-vblank-lead-auto"},
                          {"honor-guest-pat"}),
                 failing);
        QCOMPARE(changes(ArgsFile::parse(lacking)),
                 QList<Change>({add("hostmem", "4G"), add("blob", "on"),
                                add("drm_native_context", "on")}) +
                     vblank() + QList<Change>({kPat, kMemory}));
    }

    void plainWords()
    {
        for (const auto &[key, value] : VmTemplate::cardProperties()) {
            const QString text = describe(add(key, value));
            QVERIFY2(text.endsWith(QString("(%1=%2)").arg(key, value)), qPrintable(text));
            QVERIFY2(text.size() > key.size() + value.size() + 10, qPrintable(text));
        }
        QVERIFY(describe(kVenus).contains("venus=off"));
        QVERIFY(describe(kPat).contains("honor-guest-pat=auto"));
        QVERIFY(describe(kMemory).contains("memfd"));
    }

    void diffLines()
    {
        const ArgsFile a = ArgsFile::parse("-a 1\n-b 2\n-c 3\n");
        const ArgsFile b = ArgsFile::parse("-a 1\n-b 2,x=y\n-c 3\n-d 4\n");
        const LineDiff d = diff(a, b);

        QCOMPARE(d.before, QStringList({"-b 2"}));
        QCOMPARE(d.after, QStringList({"-b 2,x=y", "-d 4"}));
        QCOMPARE(diff(a, a).before, QStringList());
        QCOMPARE(diff(a, a).after, QStringList());
    }

    /* Don't Ask Again, per VM, in vitrine's settings */
    void declined()
    {
        QVERIFY(Paths::settingsPath().startsWith(m_tmp.path()));
        QVERIFY(!isDeclined("fedora-kde"));
        setDeclined("fedora-kde", true);
        QVERIFY(isDeclined("fedora-kde"));
        QVERIFY(!isDeclined("other"));
        setDeclined("fedora-kde", false);
        QVERIFY(!isDeclined("fedora-kde"));
    }
};

QTEST_GUILESS_MAIN(TestCardUpdate)
#include "test_cardupdate.moc"
