// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "core/hostdevices.h"
#include "core/nesteddesktop.h"
#include "core/paths.h"
#include "core/settingsupdates.h"
#include "core/vmstore.h"
#include "core/vmtemplate.h"

using namespace SettingsUpdates;

/* What every Linux VM of the template has had after its display, since 2026-10-03 */
static const char kTail[] = "\n"
                            "# Input\n"
                            "-device virtio-keyboard-pci\n"
                            "-device virtio-tablet-pci\n"
                            "\n"
                            "# Storage\n"
                            "-object iothread,id=iodisk\n"
                            "-drive file=disk.qcow2,format=qcow2,if=none,id=disk0,discard=unmap\n"
                            "-device virtio-blk-pci,drive=disk0,iothread=iodisk\n"
                            "\n"
                            "# Network\n"
                            "-netdev passt,id=net0,vhost-user=on,tcp-ports=127.0.0.1/10022:22\n"
                            "-device virtio-net-pci,netdev=net0\n"
                            "\n"
                            "# Sound\n"
                            "-audio driver=pipewire,model=virtio\n";

static const char kHeader[] = "# The QEMU command line of this VM, one option per line.\n"
                              "# Lines starting with # are comments; #share lines are shared "
                              "folders.\n"
                              "\n";

/* qemu-gui-manager's template, until 2026-10-03 13:37: no #guest yet */
static QString eraImport()
{
    return QString(kHeader) +
           "-name Fedora\n"
           "\n"
           "# System\n"
           "-machine q35,memory-backend=mem\n"
           "-accel kvm\n"
           "-cpu host\n"
           "-smp 4\n"
           "-object memory-backend-memfd,id=mem,size=4G,share=on\n"
           "\n"
           "# Display\n"
           "-device virtio-vga-gl\n"
           "-display sdl,gl=on\n"
           "\n"
           "# Storage\n"
           "-drive file=disk.qcow2,format=qcow2,if=virtio,discard=unmap\n";
}

/* The research launcher's machine, 2026-10-03 13:37 to 13:53: the card without VGA, no topoext */
static QString eraResearch()
{
    return QString(kHeader) +
           "-name Fedora,debug-threads=on\n"
           "#guest linux\n"
           "\n"
           "# System\n"
           "-machine q35,memory-backend=mem,dump-guest-core=off\n"
           "-accel kvm,honor-guest-pat=on\n"
           "-cpu host\n"
           "-smp 8,sockets=1,cores=4,threads=2\n"
           "-object memory-backend-memfd,id=mem,size=8G,share=on\n"
           "\n"
           "# Display\n"
           "-vga none\n"
           "-device virtio-gpu-gl-pci,hostmem=4G,blob=on,drm_native_context=on,venus=off,"
           "x-host-vblank=on,x-vblank-lead=3000,x-vblank-lead-auto=on\n"
           "-display dbus,p2p=yes,gl=on\n" +
           kTail;
}

/* 2026-10-05 18:25 to 2026-10-22: virtio-vga-gl, no hpet=off, the 4 GiB window */
static QString eraOctober(const QString &machine = "q35,memory-backend=mem,dump-guest-core=off",
                          const QString &hostmem = "4G")
{
    return QString(kHeader) +
           "-name Fedora,debug-threads=on\n"
           "#guest linux,desktop=kde\n"
           "\n"
           "# System\n"
           "-machine " + machine + "\n"
           "-accel kvm,honor-guest-pat=auto\n"
           "-cpu host,topoext=on\n"
           "-smp 8,sockets=1,cores=4,threads=2\n"
           "-object memory-backend-memfd,id=mem,size=8G,share=on\n"
           "\n"
           "# Display\n"
           "-device virtio-vga-gl,hostmem=" + hostmem +
           ",blob=on,drm_native_context=on,x-host-vblank=on,x-vblank-lead=3000,"
           "x-vblank-lead-auto=on\n"
           "-display dbus,p2p=yes,gl=on\n" +
           kTail;
}

/* The nested desktop's one card, 2026-10-15 to 2026-10-22 */
static QString eraNested(const QString &machine = "q35,memory-backend=mem,dump-guest-core=off",
                         const QString &card = "virtio-vga-rutabaga", const QString &vga = "none",
                         const QString &display = "dbus,p2p=yes,gl=on")
{
    return QString(kHeader) +
           "-name Fedora,debug-threads=on\n"
           "#guest linux,desktop=kde\n"
           "\n"
           "# System\n"
           "-machine " + machine + "\n"
           "-accel kvm,honor-guest-pat=auto\n"
           "-cpu host,topoext=on\n"
           "-smp 8,sockets=1,cores=4,threads=2\n"
           "-global kvm-pit.lost_tick_policy=discard\n"
           "-object memory-backend-memfd,id=mem,size=8G,share=on\n"
           "\n"
           "# Display\n"
           "-vga " + vga + "\n"
           "-device " + card + ",x-drm=on,x-virgl2=on,cross-domain=on,hostmem=4G,ioeventfd=on,"
           "x-prefault-blobs=on,x-guest-dmabuf=on\n"
           "-device pvpanic\n"
           "-display " + display + "\n" +
           kTail;
}

/* A PC with vitrine's QEMU, which has all the template uses */
static Context pc(bool topoext = true)
{
    Context c;
    c.hostArch = "x86_64";
    c.hostTopoext = topoext;
    c.offers = CardSettings::Offers();
    c.vgaCardOffered = true;
    return c;
}

static QStringList idsOf(const Check &check)
{
    QStringList out;
    for (const Change &c : check.changes) {
        out << c.id;
    }
    return out;
}

/* @text with @from replaced by @to, which must be there once */
static QString replaced(const QString &text, const QString &from, const QString &to)
{
    [&]() { QCOMPARE(text.count(from), 1); }();
    return QString(text).replace(from, to);
}

class TestSettingsUpdates : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        /* settings and stack in here: vitrine's QEMU not built, none chosen */
        qputenv("XDG_DATA_HOME", m_tmp.filePath("data").toUtf8());
        qputenv("XDG_CONFIG_HOME", m_tmp.filePath("config").toUtf8());
        qputenv("XDG_CACHE_HOME", m_tmp.filePath("cache").toUtf8());
        Paths::setQemuBinary({});
    }

    void registry()
    {
        QCOMPARE(ids(), QStringList({"hpet-off", "hostmem-16g", "guest-kde", "honor-guest-pat",
                                     "frame-timing", "debug-threads", "vga-card", "native-context",
                                     "topoext", "dump-guest-core"}));
        for (const QString &id : ids()) {
            QVERIFY(!title(id).isEmpty());
        }
        QVERIFY(title("unknown").isEmpty());
        /* in running text: lower case but for acronyms; a sentence of its own starts with one */
        const QStringList all = {title("hpet-off"), title("hostmem-16g"), title("guest-kde"),
                                 title("native-context")};
        QCOMPARE(listed(all), "no HPET timer, a 16 GiB window for GPU memory, KDE Plasma as the "
                              "guest's desktop, DRM native context");
        QCOMPARE(listed(all.mid(0, 2), true), "No HPET timer, a 16 GiB window for GPU memory");
        QCOMPARE(listed({}), QString());
    }

    /* The first hour: a guest nobody said is Linux gets nothing */
    void eraOfTheImport()
    {
        QVERIFY(check(eraImport(), pc()).changes.isEmpty());

        Context c = pc();
        c.guestLinux = true;
        c.guestDesktop = "kde";
        const Check found = check(eraImport(), c);
        QVERIFY(found.problem.isEmpty());
        /* no topoext: -smp gives no threads */
        QCOMPARE(idsOf(found), QStringList({"hpet-off", "guest-kde", "frame-timing",
                                            "debug-threads", "native-context",
                                            "dump-guest-core"}));
        Record record;
        const QString out = apply(eraImport(), ids(), c, &record);
        QVERIFY(out.contains("\n-name Fedora,debug-threads=on\n#guest linux,desktop=kde\n"));
        QVERIFY(out.contains("\n-machine q35,memory-backend=mem,hpet=off,dump-guest-core=off\n"));
        QVERIFY(out.contains("\n-accel kvm,honor-guest-pat=auto\n"));
        QVERIFY(out.contains("\n-device virtio-vga-gl,hostmem=16G,blob=on,drm_native_context=on,"
                             "x-host-vblank=on,x-vblank-lead=3000,x-vblank-lead-auto=on\n"));
        /* nothing more to suggest, and nothing else changed */
        QVERIFY(check(out, c).changes.isEmpty());
        QVERIFY(out.contains("\n-drive file=disk.qcow2,format=qcow2,if=virtio,discard=unmap\n"));
        QVERIFY(out.contains("\n-display sdl,gl=on\n"));
        QCOMPARE(revert(out, record), eraImport());
    }

    /* The research launcher's machine of 2026-10-03: exactly its lines change */
    void eraOfTheResearchMachine()
    {
        const Check found = check(eraResearch(), pc());
        QCOMPARE(idsOf(found), QStringList({"hpet-off", "hostmem-16g", "vga-card", "topoext"}));
        /* each shows its lines */
        QCOMPARE(found.changes[0].before,
                 QStringList{"-machine q35,memory-backend=mem,dump-guest-core=off"});
        QCOMPARE(found.changes[0].after,
                 QStringList{"-machine q35,memory-backend=mem,dump-guest-core=off,hpet=off"});
        QCOMPARE(found.changes[2].after.first().section(',', 0, 0), "-device virtio-vga-gl");
        QCOMPARE(found.changes[3].after, QStringList{"-cpu host,topoext=on"});

        Record record;
        const QString out = apply(eraResearch(), idsOf(found), pc(), &record);
        QString expected = replaced(eraResearch(),
                                    "-machine q35,memory-backend=mem,dump-guest-core=off\n",
                                    "-machine q35,memory-backend=mem,dump-guest-core=off,hpet=off\n");
        expected = replaced(expected, "-cpu host\n", "-cpu host,topoext=on\n");
        expected = replaced(expected, "-device virtio-gpu-gl-pci,hostmem=4G,",
                            "-device virtio-vga-gl,hostmem=16G,");
        QCOMPARE(out, expected);
        QCOMPARE(record.changes.size(), 4);
        QVERIFY(check(out, pc()).changes.isEmpty());

        /* undone: as it was, byte for byte */
        QStringList failed;
        QCOMPARE(revert(out, record, &failed), eraResearch());
        QVERIFY(failed.isEmpty());

        /* a host without topoext (Intel), a QEMU without virtio-vga-gl */
        Context c = pc(false);
        c.vgaCardOffered = false;
        QCOMPARE(idsOf(check(eraResearch(), c)), QStringList({"hpet-off", "hostmem-16g"}));
    }

    /* 2026-10-05 to 2026-10-26, and today's template */
    void erasSinceOctober5()
    {
        QCOMPARE(idsOf(check(eraOctober(), pc())), QStringList({"hpet-off", "hostmem-16g"}));
        QCOMPARE(apply(eraOctober(), {"hostmem-16g"}, pc()),
                 eraOctober("q35,memory-backend=mem,dump-guest-core=off", "16G"));
        /* 2026-10-22 to 26 */
        const QString hpetOff = eraOctober("q35,memory-backend=mem,dump-guest-core=off,hpet=off");
        QCOMPARE(idsOf(check(hpetOff, pc())), QStringList({"hostmem-16g"}));
        QCOMPARE(apply(eraOctober(), ids(), pc()),
                 eraOctober("q35,memory-backend=mem,dump-guest-core=off,hpet=off", "16G"));
        /* the 4 GiB window in any spelling */
        QCOMPARE(idsOf(check(eraOctober("q35,memory-backend=mem,dump-guest-core=off,hpet=off",
                                        "4096M"),
                             pc())),
                 QStringList({"hostmem-16g"}));
    }

    void templateOfToday()
    {
        VmTemplate::Options o;
        o.name = "Fedora";
        o.desktop = "kde";
        o.memoryMiB = 8192;
        o.cpus = 8;
        o.threadsPerCore = 2;
        o.disk = "disk.qcow2";
        o.sshPort = 10022;
        o.passt = true;
        ArgsFile args = VmTemplate::build(o);
        Context c = pc(HostDevices::cpuHasFlag("topoext"));
        c.guestLinux = true;
        c.guestDesktop = "kde";

        const Check found = check(args.toText(), c);
        QVERIFY(found.problem.isEmpty());
        QCOMPARE(idsOf(found), QStringList());
        /* with the nested desktop too */
        NestedDesktop::set(args, true, std::nullopt);
        QCOMPARE(idsOf(check(args.toText(), c)), QStringList());
        /* and another desktop's guest */
        o.desktop = "gnome";
        c.guestDesktop = "gnome";
        QCOMPARE(idsOf(check(VmTemplate::build(o).toText(), c)), QStringList());
    }

    void nestedDesktop()
    {
        /* 2026-10-14: the card beside QEMU's VGA, moved to one card at start */
        const QString twoCards = eraNested("q35,memory-backend=mem,dump-guest-core=off",
                                           "virtio-gpu-rutabaga-pci", "std");
        QCOMPARE(idsOf(check(twoCards, pc())), QStringList({"hpet-off", "hostmem-16g"}));
        /* 2026-10-15 to 22, then to 26 */
        QCOMPARE(idsOf(check(eraNested(), pc())), QStringList({"hpet-off", "hostmem-16g"}));
        const QString hpetOff = eraNested("q35,memory-backend=mem,dump-guest-core=off,hpet=off");
        QCOMPARE(idsOf(check(hpetOff, pc())), QStringList({"hostmem-16g"}));
        QCOMPARE(apply(hpetOff, {"hostmem-16g"}, pc()),
                 replaced(hpetOff, "cross-domain=on,hostmem=4G,", "cross-domain=on,hostmem=16G,"));

        /* the guest's desktop is KDE: the nested desktop is; nothing of it acts in vitrine's window */
        QString unknown = replaced(hpetOff, "#guest linux,desktop=kde\n", "#guest linux\n");
        QCOMPARE(idsOf(check(unknown, pc())), QStringList({"hostmem-16g"}));
        /* in QEMU's own window it does */
        unknown = replaced(unknown, "-display dbus,p2p=yes,gl=on\n", "-display sdl\n");
        QCOMPARE(idsOf(check(unknown, pc())), QStringList({"hostmem-16g", "guest-kde"}));
        QVERIFY(apply(unknown, {"guest-kde"}, pc()).contains("\n#guest linux,desktop=kde\n"));
    }

    /* A launch script's command line, imported: no #guest, -enable-kvm, -M */
    void imported()
    {
        const QString script = "# imported from run-vm.sh\n"
                               "#qemu /opt/qemu/bin/qemu-system-x86_64\n"
                               "-enable-kvm\n"
                               "-M q35\n"
                               "-m 8G\n"
                               "-cpu host\n"
                               "-smp 8\n"
                               "-device virtio-vga-gl,blob=true,hostmem=4G,drm_native_context=on\n"
                               "-display sdl,gl=on\n";
        Context c = pc();
        /* its QEMU, as asked: the card's properties, kvm-accel's */
        c.offers->card = QStringList{"blob", "hostmem", "drm_native_context", "x-host-vblank",
                                     "x-vblank-lead", "x-vblank-lead-auto"};
        c.offers->accel = QStringList{"honor-guest-pat"};
        /* a guest nobody said is Linux: what any guest's card and KVM take */
        QCOMPARE(idsOf(check(script, c)), QStringList({"hostmem-16g", "honor-guest-pat"}));

        c.guestLinux = true;
        QCOMPARE(idsOf(check(script, c)), QStringList({"hpet-off", "hostmem-16g", "honor-guest-pat",
                                                       "frame-timing", "dump-guest-core"}));
        Record record;
        const QString out = apply(script, ids(), c, &record);
        QCOMPARE(out, QString("# imported from run-vm.sh\n"
                              "#qemu /opt/qemu/bin/qemu-system-x86_64\n"
                              "-M q35,hpet=off,dump-guest-core=off\n"
                              "-accel kvm,honor-guest-pat=auto\n"
                              "-m 8G\n"
                              "-cpu host\n"
                              "-smp 8\n"
                              "-device virtio-vga-gl,blob=true,hostmem=16G,drm_native_context=on,"
                              "x-host-vblank=on,x-vblank-lead=3000,x-vblank-lead-auto=on\n"
                              "-display sdl,gl=on\n"));
        QCOMPARE(revert(out, record), script);

        /* a QEMU without vitrine's properties: only what needs none */
        c.offers->card = QStringList{"blob", "hostmem", "drm_native_context"};
        c.offers->accel = QStringList();
        QCOMPARE(idsOf(check(script, c)),
                 QStringList({"hpet-off", "hostmem-16g", "dump-guest-core"}));
        /* one that did not answer: the same */
        c.offers.reset();
        QCOMPARE(idsOf(check(script, c)),
                 QStringList({"hpet-off", "hostmem-16g", "dump-guest-core"}));
    }

    /* What the user set, to any value, stays theirs */
    void usersOwn()
    {
        Context c = pc();
        c.guestLinux = true;

        QVERIFY(!idsOf(check(eraOctober("q35,hpet=on"), c)).contains("hpet-off"));
        QVERIFY(!idsOf(check(eraOctober("q35,nohpet"), c)).contains("hpet-off"));
        QVERIFY(!idsOf(check(eraOctober() + "-no-hpet\n", c)).contains("hpet-off"));
        QVERIFY(!idsOf(check(eraOctober("q35", "8G"), c)).contains("hostmem-16g"));
        QVERIFY(!idsOf(check(eraOctober("q35", "16G"), c)).contains("hostmem-16g"));
        QVERIFY(!idsOf(check(eraOctober("q35,dump-guest-core=on"), c)).contains("dump-guest-core"));
        /* Windows and other systems keep their HPET: only their card's window */
        QCOMPARE(idsOf(check(replaced(eraOctober(), "#guest linux,desktop=kde", "#guest windows"),
                             c)),
                 QStringList{"hostmem-16g"});
        QCOMPARE(idsOf(check(replaced(eraOctober(), "#guest linux,desktop=kde", "#guest other"), c)),
                 QStringList{"hostmem-16g"});

        const QString research = eraResearch();
        QVERIFY(!idsOf(check(replaced(research, "-cpu host\n", "-cpu host,-topoext\n"), c))
                     .contains("topoext"));
        QVERIFY(!idsOf(check(replaced(research, "-cpu host\n", "-cpu EPYC\n"), c)).contains("topoext"));
        QVERIFY(!idsOf(check(replaced(research, "Fedora,debug-threads=on", "Fedora,debug-threads=off"),
                             c))
                     .contains("debug-threads"));
        QCOMPARE(idsOf(check(replaced(research, "Fedora,debug-threads=on", "Fedora"), c)),
                 QStringList({"hpet-off", "hostmem-16g", "debug-threads", "vga-card", "topoext"}));
        QVERIFY(!idsOf(check(replaced(research, "honor-guest-pat=on", "honor-guest-pat=off"), c))
                     .contains("honor-guest-pat"));
        /* frame timing off on the Display page, or with -global */
        const QString off = replaced(research,
                                     "x-host-vblank=on,x-vblank-lead=3000,x-vblank-lead-auto=on",
                                     "x-host-vblank=off");
        QVERIFY(!idsOf(check(off, c)).contains("frame-timing"));
        const QString global = replaced(research,
                                        ",x-host-vblank=on,x-vblank-lead=3000,x-vblank-lead-auto=on",
                                        "") +
                               "-global virtio-gpu-gl-pci.x-host-vblank=off\n";
        QVERIFY(!idsOf(check(global, c)).contains("frame-timing"));
        /* native context off on the Display page: blob and hostmem stay */
        const QString noNative = replaced(research, ",drm_native_context=on", "");
        QVERIFY(!idsOf(check(noNative, c)).contains("native-context"));
        /* the guest's desktop said */
        c.guestDesktop = "gnome";
        QVERIFY(!idsOf(check(replaced(research, "#guest linux\n", "#guest linux,desktop=gnome\n"), c))
                     .contains("guest-kde"));
    }

    /* KDE, as the guest said, where the desktop decides something at start */
    void kdeGuest()
    {
        Context c = pc();
        const QString plain = replaced(eraOctober(), "#guest linux,desktop=kde", "#guest linux");
        QVERIFY(!idsOf(check(plain, c)).contains("guest-kde"));
        c.guestDesktop = "kde";
        QCOMPARE(idsOf(check(plain, c)), QStringList({"hpet-off", "hostmem-16g", "guest-kde"}));
        const QString out = apply(plain, {"guest-kde"}, c);
        QCOMPARE(out, eraOctober());
        /* another desktop's settings, the other keys of the line kept */
        const QString other = replaced(eraOctober(), "#guest linux,desktop=kde",
                                       "#guest linux,desktop=other,id=fedora44");
        QCOMPARE(apply(other, {"guest-kde"}, c),
                 replaced(eraOctober(), "#guest linux,desktop=kde",
                          "#guest linux,desktop=kde,id=fedora44"));
        /* no #guest: Linux, as the agent said */
        c.guestLinux = true;
        const QString none = replaced(eraOctober(), "#guest linux,desktop=kde\n", "");
        QCOMPARE(apply(none, {"guest-kde"}, c), eraOctober());
        /* a swap target of the user's, and no window of QEMU's: nothing it decides */
        const QString fixed = replaced(plain, "x-vblank-lead-auto=on\n",
                                       "x-vblank-lead-auto=on,x-vblank-swap-target=5000\n");
        QVERIFY(!idsOf(check(fixed, c)).contains("guest-kde"));
        /* a QEMU without the swap target: the same */
        c.offers->card = QStringList{"hostmem", "blob", "drm_native_context", "x-host-vblank",
                                     "x-vblank-lead", "x-vblank-lead-auto"};
        QVERIFY(!idsOf(check(plain, c)).contains("guest-kde"));
    }

    /* What vitrine cannot read safely gets no suggestion, and says why */
    void unsafe()
    {
        Context c = pc();
        c.guestLinux = true;
        const QStringList broken = {
            /* a shell command pasted with its line continuations */
            replaced(eraOctober(), "-display dbus,p2p=yes,gl=on\n",
                     "-display dbus,\\\n    p2p=yes,gl=on\n"),
            eraOctober() + "-readconfig more.cfg\n",
            eraOctober() + "-set device.gpu.hostmem=4G\n",
            eraOctober() + "-device {\"driver\":\"virtio-rng-pci\"}\n",
            replaced(eraOctober(), "-machine q35,memory-backend=mem,dump-guest-core=off",
                     "-machine {\"type\":\"q35\"}"),
        };
        for (const QString &text : broken) {
            const Check found = check(text, c);
            QVERIFY2(!found.problem.isEmpty(), qPrintable(text));
            QVERIFY(found.changes.isEmpty());
            QCOMPARE(apply(text, ids(), c), text);
        }
        QVERIFY(problem(broken[0]).contains("line 17"));
        QVERIFY(problem(eraOctober()).isEmpty());
        /* -blockdev in JSON, which no change reads, is fine */
        QVERIFY(problem(eraOctober() + "-blockdev {\"driver\":\"file\",\"filename\":\"x\"}\n")
                    .isEmpty());
    }

    /* Only the lines in question change: the others stay as written */
    void untouchedLines()
    {
        const QString text = "  # an indented comment   \n"
                             "-name\tFedora,debug-threads=on   \n"
                             "#guest linux\n"
                             "--machine q35,memory-backend=mem,dump-guest-core=off\n"
                             "\n"
                             "-object memory-backend-memfd,id=mem,size=8G,share=on\t\n"
                             "-device virtio-vga-gl,hostmem=4G,blob=on,drm_native_context=on,"
                             "x-host-vblank=on,x-vblank-lead=3000,x-vblank-lead-auto=on\n"
                             "\n"
                             "\n";
        const QString out = apply(text, {"hostmem-16g"}, pc());
        QCOMPARE(out, replaced(text, "hostmem=4G", "hostmem=16G"));
        /* the line edited is written as vitrine writes it: one dash */
        QCOMPARE(apply(text, {"hpet-off"}, pc()),
                 replaced(text, "--machine q35,memory-backend=mem,dump-guest-core=off\n",
                          "-machine q35,memory-backend=mem,dump-guest-core=off,hpet=off\n"));
        /* Windows' line ends */
        QString crlf = eraOctober();
        crlf.replace("\n", "\r\n");
        const QString crlfOut = apply(crlf, {"hpet-off", "hostmem-16g"}, pc());
        QCOMPARE(crlfOut.count("\r\n"), crlf.count("\r\n"));
        QCOMPARE(QString(crlfOut).remove('\r'),
                 eraOctober("q35,memory-backend=mem,dump-guest-core=off,hpet=off", "16G"));
        /* no final line end */
        QString last = eraOctober();
        last.chop(1);
        QVERIFY(!apply(last, {"hpet-off"}, pc()).endsWith('\n'));
    }

    /* A line vitrine adds, and what comes on it later */
    void addedLines()
    {
        const QString text = "-name Fedora\n"
                             "#guest linux\n"
                             "-device virtio-vga-gl,hostmem=16G\n";
        Record record;
        const QString out = apply(text, {"hpet-off", "dump-guest-core"}, pc(), &record);
        QCOMPARE(out, QString("-name Fedora\n"
                              "-machine hpet=off,dump-guest-core=off\n"
                              "#guest linux\n"
                              "-device virtio-vga-gl,hostmem=16G\n"));
        QCOMPARE(revert(out, record), text);
        /* the user's own key on that line stays, and so does the line */
        const QString edited = replaced(out, "dump-guest-core=off\n", "dump-guest-core=off,usb=on\n");
        QStringList failed;
        QCOMPARE(revert(edited, record, &failed), replaced(text, "#guest linux\n",
                                                            "-machine usb=on\n#guest linux\n"));
        QVERIFY(failed.isEmpty());

        /* -enable-kvm taken for -accel, and given back */
        const QString kvm = "-name Fedora\n#guest linux\n-enable-kvm\n-machine q35\n"
                            "-device virtio-vga-gl,drm_native_context=on,blob=on,hostmem=16G\n";
        Record pat;
        const QString withPat = apply(kvm, {"honor-guest-pat"}, pc(), &pat);
        QVERIFY(withPat.contains("\n-machine q35\n-accel kvm,honor-guest-pat=auto\n"));
        QVERIFY(!withPat.contains("enable-kvm"));
        QCOMPARE(revert(withPat, pat), kvm);
    }

    /* Undone after more edits: those stay, and what changed since is said */
    void undoAfterEdits()
    {
        Record record;
        const QString out = apply(eraResearch(), ids(), pc(), &record);
        /* the user's: more memory, another lead, and a window of 32 GiB */
        QString edited = replaced(out, "size=8G,share=on", "size=12G,share=on");
        edited = replaced(edited, "x-vblank-lead=3000", "x-vblank-lead=2000");
        edited = replaced(edited, "hostmem=16G", "hostmem=32G");
        QStringList failed;
        const QString back = revert(edited, record, &failed);
        QCOMPARE(failed, QStringList{"hostmem-16g"});
        QString expected = replaced(eraResearch(), "size=8G,share=on", "size=12G,share=on");
        expected = replaced(expected, "x-vblank-lead=3000", "x-vblank-lead=2000");
        expected = replaced(expected, "hostmem=4G", "hostmem=32G");
        QCOMPARE(back, expected);
        /* a key put back by hand already: nothing to say */
        failed.clear();
        const QString byHand = replaced(out, "hostmem=16G", "hostmem=4G");
        QCOMPARE(revert(byHand, record, &failed), eraResearch());
        QVERIFY(failed.isEmpty());
        /* the card moved back by hand: its keys still go back */
        failed.clear();
        const QString moved = replaced(out, "-device virtio-vga-gl,", "-device virtio-gpu-gl-pci,");
        QCOMPARE(revert(moved, record, &failed), eraResearch());
        QCOMPARE(failed, QStringList{"vga-card"});
    }

    void recordsKept()
    {
        Record record;
        apply(eraResearch(), ids(), pc(), &record);
        record.time = QDateTime(QDate(2026, 11, 12), QTime(15, 30));
        record.backup = "vm.args.2026-11-12-153000.bak";
        QCOMPARE(Record::fromJson(record.toJson()), record);

        const QString vm = "records-vm";
        QVERIFY(records(vm).isEmpty());
        setRecords(vm, {record, record});
        QCOMPARE(records(vm), QList<Record>({record, record}));
        setDeclined(vm, "hpet-off", true);
        setDeclined(vm, "topoext", true);
        setDeclined(vm, "hpet-off", false);
        QCOMPARE(declined(vm), QStringList{"topoext"});
        setGuestDesktop(vm, "kde");
        QCOMPARE(guestDesktop(vm), "kde");
        QVERIFY(!notNow(vm));
        setNotNow(vm);
        QVERIFY(notNow(vm));
        forget(vm);
        QVERIFY(records(vm).isEmpty());
        QVERIFY(declined(vm).isEmpty());
        QVERIFY(guestDesktop(vm).isEmpty());
        QVERIFY(!notNow(vm));
        /* never in the group VmStore purges */
        QVERIFY(!QSettings(Paths::settingsPath(), QSettings::IniFormat)
                     .childGroups()
                     .contains("cardupdate"));

        QTemporaryDir dir;
        const QDateTime time(QDate(2026, 11, 12), QTime(15, 30, 5));
        QCOMPARE(backupName(dir.path(), time), "vm.args.2026-11-12-153005.bak");
        QFile taken(dir.filePath("vm.args.2026-11-12-153005.bak"));
        QVERIFY(taken.open(QIODevice::WriteOnly));
        QCOMPARE(backupName(dir.path(), time), "vm.args.2026-11-12-153005-2.bak");
    }

    /* A VM of the store: found, declined, applied with a copy first, undone */
    void monitor()
    {
        if (Paths::hostArch() != "x86_64") {
            QSKIP("the changes for a PC");
        }
        QTemporaryDir vms;
        const QString folder = vms.filePath("fedora");
        QVERIFY(QDir().mkpath(folder));
        const QByteArray original = eraOctober().toUtf8();
        {
            QFile f(folder + "/vm.args");
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(original);
        }
        VmStore store(vms.path());
        Vm *vm = store.find("fedora");
        QVERIFY(vm);
        SettingsUpdatesMonitor *monitor = SettingsUpdatesMonitor::of(vm);
        QCOMPARE(SettingsUpdatesMonitor::of(vm), monitor);
        QSignalSpy changed(monitor, &SettingsUpdatesMonitor::changed);
        QVERIFY(monitor->problem().isEmpty());
        QVERIFY(monitor->blocked().isEmpty());
        QStringList offered;
        for (const Change &c : monitor->offered()) {
            offered << c.id;
        }
        QCOMPARE(offered, QStringList({"hpet-off", "hostmem-16g"}));

        /* not asked again: the other one alone */
        monitor->setDeclined("hostmem-16g", true);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(monitor->offered().size(), 1);
        QCOMPARE(monitor->declined().first().id, "hostmem-16g");
        QVERIFY(!monitor->notNow());
        monitor->setNotNow();
        QVERIFY(monitor->notNow());

        /* vm.args edited since the list was shown: refused, the list up to date again */
        const QList<Change> shown = monitor->offered();
        {
            QFile f(folder + "/vm.args");
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(eraOctober("q35,memory-backend=mem").toUtf8());
        }
        QString error;
        bool stale = false;
        QVERIFY(!monitor->apply(shown, &error, &stale));
        QVERIFY(stale);
        QVERIFY(!error.isEmpty());
        {
            QFile f(folder + "/vm.args");
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(original);
        }
        monitor->refresh();

        /* applied: a copy of vm.args first, its lines changed, a record */
        QVERIFY2(monitor->apply(monitor->offered(), &error, &stale), qPrintable(error));
        const QList<Record> kept = monitor->records();
        QCOMPARE(kept.size(), 1);
        QFile backup(QDir(folder).filePath(kept.first().backup));
        QVERIFY(backup.open(QIODevice::ReadOnly));
        QCOMPARE(backup.readAll(), original);
        QString text;
        QVERIFY(readText(vm->argsPath(), &text, &error));
        QCOMPARE(text, eraOctober("q35,memory-backend=mem,dump-guest-core=off,hpet=off"));
        QCOMPARE(vm->args().toText(), ArgsFile::parse(text).toText());
        QVERIFY(monitor->offered().isEmpty());
        QCOMPARE(monitor->declined().size(), 1);

        /* undone: vm.args as it was, the record gone */
        QStringList failed;
        QVERIFY2(monitor->undo(&failed, &error), qPrintable(error));
        QVERIFY(failed.isEmpty());
        QVERIFY(readText(vm->argsPath(), &text, &error));
        QCOMPARE(text.toUtf8(), original);
        QVERIFY(monitor->records().isEmpty());
        QCOMPARE(monitor->offered().size(), 1);
        QVERIFY(!monitor->undo(&failed, &error));

        /* not UTF-8: said, and nothing suggested */
        {
            QFile f(folder + "/vm.args");
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(original + "# \xff\xfe\n");
        }
        monitor->refresh();
        QVERIFY(!monitor->problem().isEmpty());
        QVERIFY(monitor->offered().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestSettingsUpdates)
#include "test_settingsupdates.moc"
