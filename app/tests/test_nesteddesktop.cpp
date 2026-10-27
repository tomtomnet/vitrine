// SPDX-License-Identifier: GPL-2.0-or-later
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include "core/hostgpus.h"
#include "core/nesteddesktop.h"
#include "core/vmhardware.h"
#include "core/vmrunner.h"
#include "fakegpus.h"

using NestedDesktop::Mode;

/* The display lines of a new Linux VM (VmTemplate), in vitrine's window */
static const char kVm[] = "-name Test VM,debug-threads=on\n"
                          "-machine q35,memory-backend=mem,dump-guest-core=off,hpet=off\n"
                          "-accel kvm,honor-guest-pat=auto\n"
                          "-smp 8,sockets=1,cores=4,threads=2\n"
                          "-object memory-backend-memfd,id=mem,size=8G,share=on\n"
                          "-device virtio-vga-gl,hostmem=16G,blob=on,drm_native_context=on\n"
                          "-display dbus,p2p=yes,gl=on\n"
                          "-device virtio-keyboard-pci\n";

/* With the nested desktop, as the Display page's checkbox writes it */
static const char kNested[] =
    "-name Test VM,debug-threads=on\n"
    "-machine q35,memory-backend=mem,dump-guest-core=off,hpet=off\n"
    "-accel kvm,honor-guest-pat=auto\n"
    "-smp 8,sockets=1,cores=4,threads=2\n"
    "-global kvm-pit.lost_tick_policy=discard\n"
    "-object memory-backend-memfd,id=mem,size=8G,share=on\n"
    "-vga none\n"
    "-device virtio-vga-rutabaga,x-drm=on,x-virgl2=on,cross-domain=on,hostmem=16G,"
    "ioeventfd=on,x-prefault-blobs=on,x-guest-dmabuf=on\n"
    "-device pvpanic\n"
    "-display dbus,p2p=yes,gl=on\n"
    "-device virtio-keyboard-pci\n";

/* As the checkbox wrote it until 2026-10-15: the rutabaga card beside QEMU's VGA, a 4 GiB window */
static const char kTwoCards[] =
    "-name Test VM,debug-threads=on\n"
    "-machine q35,memory-backend=mem,dump-guest-core=off,hpet=off\n"
    "-accel kvm,honor-guest-pat=auto\n"
    "-smp 8,sockets=1,cores=4,threads=2\n"
    "-global kvm-pit.lost_tick_policy=discard\n"
    "-object memory-backend-memfd,id=mem,size=8G,share=on\n"
    "-vga std\n"
    "-device virtio-gpu-rutabaga-pci,x-drm=on,x-virgl2=on,cross-domain=on,hostmem=4G,"
    "ioeventfd=on,x-prefault-blobs=on,x-guest-dmabuf=on\n"
    "-device pvpanic\n"
    "-display dbus,p2p=yes,gl=on\n"
    "-device virtio-keyboard-pci\n";

class TestNestedDesktop : public QObject
{
    Q_OBJECT

    QTemporaryDir tmp;

    /* A stand-in for a QEMU asked for its devices' properties: @properties for the nested card */
    QString qemuWith(const QString &name, const QStringList &properties)
    {
        const QString dir = tmp.filePath(name);
        const QString path = dir + "/qemu-system-x86_64";
        QStringList lines;

        QDir().mkpath(dir);
        for (const QString &p : properties) {
            lines << QString("  %1=<bool>    - on/off (default: off)").arg(p);
        }
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) {
            f.write("#!/bin/sh\ncase \"$2\" in\nvirtio-vga-rutabaga,help)\n");
            if (!properties.isEmpty()) {
                f.write("cat <<'EOF'\nvirtio-vga-rutabaga options:\n" +
                        lines.join('\n').toUtf8() + "\nEOF\n");
            } else {
                f.write("echo \"qemu-system-x86_64: -device $2: Device "
                        "'virtio-vga-rutabaga' not found\" >&2\n");
            }
            f.write(";;\nesac\nexit 0\n");
            f.close();
        }
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        return path;
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        /* a host without GPUs, not this computer's: command lines as with one */
        FakeGpus(tmp.filePath("no-gpus")).use();
    }

    void cleanupTestCase() { HostGpus::setRoots("/sys", "/dev"); }

    /*
     * The GPU the card renders on, as the Display page keeps it: the card's
     * x-render-node; turned off and on again, the card is a new one, without
     */
    void renderNode()
    {
        const QString chosen = "/dev/dri/by-path/pci-0000:03:00.0-render";
        ArgsFile a = ArgsFile::parse(kNested);
        auto card = [&a]() { return a.lines[a.indexOf("vga") + 1].value; };

        QCOMPARE(NestedDesktop::renderNode(a), "");
        NestedDesktop::setRenderNode(a, chosen);
        QCOMPARE(card(), "virtio-vga-rutabaga,x-drm=on,x-virgl2=on,cross-domain=on,hostmem=16G,"
                         "ioeventfd=on,x-prefault-blobs=on,x-guest-dmabuf=on,"
                         "x-render-node=/dev/dri/by-path/pci-0000:03:00.0-render");
        QCOMPARE(NestedDesktop::renderNode(a), chosen);
        /* another in its place */
        NestedDesktop::setRenderNode(a, "/dev/dri/renderD128");
        QCOMPARE(NestedDesktop::renderNode(a), "/dev/dri/renderD128");
        QCOMPARE(card().count("x-render-node"), 1);
        /* Automatic: none, the line as it was */
        NestedDesktop::setRenderNode(a, "");
        QCOMPARE(a.toText(), QString(kNested));
        NestedDesktop::setRenderNode(a, "");
        QCOMPARE(a.toText(), QString(kNested));
        /* the two cards of before have it on theirs */
        ArgsFile two = ArgsFile::parse(kTwoCards);
        NestedDesktop::setRenderNode(two, chosen);
        QCOMPARE(NestedDesktop::renderNode(two), chosen);
        QVERIFY(two.toText().contains("-device virtio-gpu-rutabaga-pci,") &&
                two.toText().contains("x-guest-dmabuf=on,x-render-node=" + chosen + "\n"));
        /* no rutabaga card: nothing to keep it on */
        ArgsFile vm = ArgsFile::parse(kVm);
        NestedDesktop::setRenderNode(vm, chosen);
        QCOMPARE(vm.toText(), QString(kVm));
        QCOMPARE(NestedDesktop::renderNode(vm), "");
        /* off and on again: a new card, Automatic */
        NestedDesktop::setRenderNode(a, chosen);
        NestedDesktop::set(a, false);
        QCOMPARE(NestedDesktop::renderNode(a), "");
        NestedDesktop::set(a, true);
        QCOMPARE(NestedDesktop::renderNode(a), "");
        QVERIFY(!a.toText().contains("x-render-node"));
        QVERIFY(NestedDesktop::isRutabagaCard("virtio-vga-rutabaga"));
        QVERIFY(NestedDesktop::isRutabagaCard("virtio-gpu-rutabaga-pci"));
        QVERIFY(!NestedDesktop::isRutabagaCard("virtio-vga-gl"));
    }

    /* The checkbox: the card and no VGA beside it, shared RAM, the PIT, pvpanic; and back */
    void turnOnAndOff()
    {
        ArgsFile a = ArgsFile::parse(kVm);

        QVERIFY(NestedDesktop::mode(a) == Mode::Off);
        NestedDesktop::set(a, true);
        QCOMPARE(a.toText(), QString(kNested));
        QVERIFY(VmConfig::graphics(a).kind == VmConfig::Graphics::Nested);
        QCOMPARE(VmConfig::graphics(a).hostmemMiB, 16384);
        QVERIFY(NestedDesktop::mode(a) == Mode::Integrated);
        /* on again: nothing more */
        NestedDesktop::set(a, true);
        QCOMPARE(a.toText(), QString(kNested));

        /* off: the template's 3D card in its place, no stray -vga, the rest kept */
        NestedDesktop::set(a, false);
        QString off = kNested;
        off.replace("-vga none\n", "");
        off.replace("-device virtio-vga-rutabaga,x-drm=on,x-virgl2=on,cross-domain=on,"
                    "hostmem=16G,ioeventfd=on,x-prefault-blobs=on,x-guest-dmabuf=on\n",
                    "-device virtio-vga-gl\n");
        QCOMPARE(a.toText(), off);
        QVERIFY(VmConfig::graphics(a).kind == VmConfig::Graphics::Accelerated);
        QVERIFY(NestedDesktop::mode(a) == Mode::Off);
        /* the two cards of before, off: the same, QEMU's VGA gone too */
        a = ArgsFile::parse(kTwoCards);
        NestedDesktop::set(a, false);
        QCOMPARE(a.toText(), off);

        /* no shared RAM yet, no -vga, no display: all of it comes */
        a = ArgsFile::parse("-machine q35\n-m 4G\n-device virtio-vga-gl\n");
        NestedDesktop::set(a, true);
        QCOMPARE(a.toText(), "-machine q35,memory-backend=mem,hpet=off\n"
                             "-global kvm-pit.lost_tick_policy=discard\n"
                             "-m 4G\n"
                             "-vga none\n"
                             "-device virtio-vga-rutabaga,x-drm=on,x-virgl2=on,cross-domain=on,"
                             "hostmem=16G,ioeventfd=on,x-prefault-blobs=on,x-guest-dmabuf=on\n"
                             "-device pvpanic\n"
                             "-object memory-backend-memfd,id=mem,size=4G,share=on\n");
        /* the user's PIT policy and pvpanic stay as they are */
        a = ArgsFile::parse("-global driver=kvm-pit,property=lost_tick_policy,value=delay\n"
                            "-device pvpanic,ioport=0x505\n-device virtio-vga\n"
                            "-object memory-backend-memfd,id=m,size=1G\n-machine memory-backend=m\n");
        NestedDesktop::set(a, true);
        QVERIFY(a.toText().startsWith(
            "-global driver=kvm-pit,property=lost_tick_policy,value=delay\n"
            "-device pvpanic,ioport=0x505\n-vga none\n-device virtio-vga-rutabaga,"));
        QCOMPARE(a.indexesOf("global").size(), 1);
        QCOMPARE(a.toText().count("pvpanic"), 1);

        /* without a window: none, which a 3D card turns into egl-headless */
        a = ArgsFile::parse("-device virtio-vga-gl\n-display egl-headless\n");
        NestedDesktop::set(a, true);
        QCOMPARE(a.lines[a.indexOf("display")].value, "none");
        NestedDesktop::set(a, false);
        QCOMPARE(a.lines[a.indexOf("display")].value, "egl-headless");
        /* a rutabaga card set up by hand stays */
        a = ArgsFile::parse("-vga none\n-device virtio-gpu-rutabaga-pci,cross-domain=on\n");
        NestedDesktop::set(a, false);
        QCOMPARE(a.toText(), "-vga none\n-device virtio-gpu-rutabaga-pci,cross-domain=on\n");
        a = ArgsFile::parse("-vga std\n-device virtio-vga-rutabaga,cross-domain=on\n");
        NestedDesktop::set(a, false);
        QCOMPARE(a.toText(), "-vga std\n-device virtio-vga-rutabaga,cross-domain=on\n");
    }

    /* On a PC no HPET, unless the VM says otherwise; and off, it stays */
    void hpet()
    {
        /* a VM of the template before 2026-10-22 */
        ArgsFile a = ArgsFile::parse(QString(kVm).remove(",hpet=off"));
        NestedDesktop::set(a, true);
        QCOMPARE(a.toText(), QString(kNested));
        NestedDesktop::set(a, false);
        QVERIFY(a.toText().startsWith("-name Test VM,debug-threads=on\n"
                                      "-machine q35,memory-backend=mem,dump-guest-core=off,"
                                      "hpet=off\n"));

        /* QEMU's PCs, its default machine too: on the last -machine, as QEMU merges them */
        for (const QString &vm : {"-machine pc-q35-10.1\n", "-machine pc\n", "-machine isapc\n",
                                  "-machine pc-i440fx-10.1\n-machine accel=kvm\n", ""}) {
            a = ArgsFile::parse(vm + "-device virtio-vga\n");
            NestedDesktop::set(a, true);
            const QList<int> machines = a.indexesOf("machine");
            QVERIFY2(!machines.isEmpty(), qPrintable(vm));
            QCOMPARE(a.valueAt(machines.last()).get("hpet"), "off");
            QCOMPARE(a.toText().count("hpet"), 1);
        }
        /* the VM's own say, and machines without an HPET: as they are */
        for (const QString &vm : {"-machine q35,hpet=on\n", "-machine q35,hpet\n",
                                  "-machine q35,hpet=off\n-machine accel=kvm\n",
                                  "-machine q35\n-no-hpet\n", "-machine microvm\n",
                                  "-machine virt\n"}) {
            a = ArgsFile::parse(vm + "-device virtio-vga\n");
            NestedDesktop::set(a, true);
            QCOMPARE(a.toText().count("hpet"), vm.count("hpet"));
            QCOMPARE(a.toText().count("hpet=off"), vm.count("hpet=off"));
        }
    }

    /*
     * The two cards vitrine wrote before, as VMs have them still: the one
     * card in their place, with the old card's properties, as the checkbox
     * writes it; nothing else moves
     */
    void moveToOneCard()
    {
        ArgsFile a = ArgsFile::parse(kTwoCards);
        /* the old card's 4 GiB window too: a VM keeps its line */
        const QString moved = QString(kNested).replace("hostmem=16G", "hostmem=4G");

        QVERIFY(NestedDesktop::moveToOneCard(a));
        QCOMPARE(a.toText(), moved);
        QCOMPARE(VmConfig::graphics(a).hostmemMiB, 4096);
        QVERIFY(!NestedDesktop::moveToOneCard(a));
        QCOMPARE(a.toText(), moved);

        /* the user's changes to the card stay; a VGA of its own goes */
        a = ArgsFile::parse("-machine q35\n-device VGA,id=vga0\n"
                            "-device virtio-gpu-rutabaga-pci,cross-domain=on,hostmem=8G,id=gpu,"
                            "x-prefault-blobs=off\n-display sdl\n");
        QVERIFY(NestedDesktop::moveToOneCard(a));
        QCOMPARE(a.toText(), "-machine q35\n-vga none\n"
                             "-device virtio-vga-rutabaga,cross-domain=on,hostmem=8G,id=gpu,"
                             "x-prefault-blobs=off\n-display sdl\n");
        QVERIFY(VmConfig::graphics(a).kind == VmConfig::Graphics::Nested);
        QVERIFY(NestedDesktop::mode(a) == Mode::OwnWindow);

        /* not the nested desktop, or set up by hand: as they are */
        for (const char *text : {kVm, "-vga none\n-device virtio-gpu-rutabaga-pci,cross-domain=on\n",
                                 "-vga std\n-device virtio-gpu-rutabaga-pci\n",
                                 "-vga std\n-device virtio-vga-rutabaga,cross-domain=on\n"}) {
            a = ArgsFile::parse(text);
            QVERIFY2(!NestedDesktop::moveToOneCard(a), text);
            QCOMPARE(a.toText(), QString(text));
        }
    }

    /* The D-Bus display's console of the VM's screen: the one card's, the virtio card's of two */
    void screenConsole()
    {
        QCOMPARE(NestedDesktop::screenConsole(ArgsFile::parse(kNested)), 0);
        QCOMPARE(NestedDesktop::screenConsole(ArgsFile::parse(kTwoCards)), 1);
        QCOMPARE(NestedDesktop::screenConsole(ArgsFile::parse(kVm)), 0);
        QCOMPARE(NestedDesktop::screenConsole(ArgsFile()), 0);
    }

    /* Only what the VM's QEMU has, cross-domain always */
    void cardProperties()
    {
        QCOMPARE(NestedDesktop::cardDevice(std::nullopt),
                 "virtio-vga-rutabaga,x-drm=on,x-virgl2=on,cross-domain=on,hostmem=16G,"
                 "ioeventfd=on,x-prefault-blobs=on,x-guest-dmabuf=on");
        /* QEMU's own rutabaga (Fedora's): no native context, prefault or guest dma-buf */
        QCOMPARE(NestedDesktop::cardDevice(QStringList{"cross-domain", "hostmem", "ioeventfd",
                                                       "wayland-socket-path"}),
                 "virtio-vga-rutabaga,cross-domain=on,hostmem=16G,ioeventfd=on");
        QCOMPARE(NestedDesktop::cardDevice(QStringList()),
                 "virtio-vga-rutabaga,cross-domain=on");
    }

    /* The display page's choices with the nested desktop on */
    void screens()
    {
        ArgsFile a = ArgsFile::parse(kNested);

        VmConfig::setScreen(a, VmConfig::Screen::OwnWindow, "sdl");
        QCOMPARE(a.indexOf("display"), 9);
        QCOMPARE(a.lines[9].value, "sdl");
        QVERIFY(VmConfig::graphics(a).kind == VmConfig::Graphics::Nested);
        QVERIFY(NestedDesktop::mode(a) == Mode::OwnWindow);
        VmConfig::setScreen(a, VmConfig::Screen::OwnWindow, "gtk");
        QCOMPARE(a.lines[9].value, "gtk");
        VmConfig::setScreen(a, VmConfig::Screen::None);
        QCOMPARE(a.lines[9].value, "none");
        QVERIFY(NestedDesktop::mode(a) == Mode::OwnWindow);
        VmConfig::setScreen(a, VmConfig::Screen::Embedded);
        QCOMPARE(a.lines[9].value, "dbus,p2p=yes,gl=on");
        QCOMPARE(a.toText(), QString(kNested));
        QVERIFY(NestedDesktop::mode(a) == Mode::Integrated);
    }

    void modes()
    {
        const std::pair<const char *, Mode> cases[] = {
            {kNested, Mode::Integrated},
            {kTwoCards, Mode::Integrated},
            {"-vga none\n-device virtio-vga-rutabaga,cross-domain=on\n-display sdl\n",
             Mode::OwnWindow},
            {"-device virtio-gpu-rutabaga-pci,cross-domain=on\n-display sdl\n", Mode::OwnWindow},
            {"-device virtio-gpu-rutabaga-pci,cross-domain=on\n-display gtk\n", Mode::OwnWindow},
            {"-device virtio-gpu-rutabaga-pci,cross-domain=on\n-display none\n", Mode::OwnWindow},
            /* a bare key is on for QEMU */
            {"-device virtio-gpu-rutabaga-pci,cross-domain\n-display dbus,p2p=yes\n",
             Mode::Integrated},
            /* any rutabaga card connects to the socket, set up by hand or not */
            {"-vga none\n-device virtio-gpu-rutabaga,cross-domain=on\n-display dbus,p2p=on\n",
             Mode::Integrated},
            {"-device virtio-gpu-rutabaga-pci,cross-domain=off\n-display sdl\n", Mode::Off},
            {"-device virtio-gpu-rutabaga-pci\n", Mode::Off},
            {kVm, Mode::Off},
        };
        for (const auto &[args, mode] : cases) {
            QVERIFY2(NestedDesktop::mode(ArgsFile::parse(args)) == mode, args);
        }
    }

    /* What the guest's session reads at boot: one word for the name */
    void bootWords()
    {
        const ArgsFile nested = ArgsFile::parse(kNested);

        QCOMPARE(NestedDesktop::bootWords(nested, "Test VM"),
                 QString("crosvm_desktop=1 crosvm_desktop_guest_dmabuf=1 crosvm_desktop_label=Test") +
                     QChar(0x00a0) + "VM");
        QCOMPARE(NestedDesktop::bootWords(
                     ArgsFile::parse("-device virtio-gpu-rutabaga-pci,cross-domain=on\n"), "f44"),
                 "crosvm_desktop=1 crosvm_desktop_label=f44");
        QCOMPARE(NestedDesktop::bootWords(
                     ArgsFile::parse("-device virtio-gpu-rutabaga-pci,cross-domain=on,"
                                     "x-guest-dmabuf=off\n"), {}),
                 "crosvm_desktop=1");
    }

    /* The socket and fw_cfg's words, unless the VM has them */
    void runArgs()
    {
        ArgsFile a = NestedDesktop::withRunArgs(ArgsFile::parse(kNested), "/run/x/nested.sock",
                                                "Test, VM");
        const int card = a.indexOfDevice([](const QString &d) { return d.contains("rutabaga"); });

        QVERIFY(a.lines[card].value.endsWith(",x-guest-dmabuf=on,wayland-socket-path=/run/x/nested.sock"));
        QCOMPARE(a.lines[card + 1].name, "fw_cfg");
        QCOMPARE(a.lines[card + 1].value,
                 QString("name=opt/org.drm-native-context/cmdline,string=crosvm_desktop=1 "
                         "crosvm_desktop_guest_dmabuf=1 crosvm_desktop_label=Test,,") +
                     QChar(0x00a0) + "VM");
        /* as QEMU will read it */
        QCOMPARE(a.argv().filter("wayland-socket-path").size(), 1);

        a = NestedDesktop::withRunArgs(
            ArgsFile::parse("-device virtio-gpu-rutabaga-pci,cross-domain=on,"
                            "wayland-socket-path=/mine\n"
                            "-fw_cfg name=opt/org.drm-native-context/cmdline,string=x\n"),
            "/run/x/nested.sock", "n");
        QCOMPARE(a.toText(), "-device virtio-gpu-rutabaga-pci,cross-domain=on,"
                             "wayland-socket-path=/mine\n"
                             "-fw_cfg name=opt/org.drm-native-context/cmdline,string=x\n");
        /* nothing for a VM without it */
        QCOMPARE(NestedDesktop::withRunArgs(ArgsFile::parse(kVm), "/s", "n").toText(),
                 QString(kVm));
    }

    void environment()
    {
        ArgsFile a = ArgsFile::parse(kNested);

        QCOMPARE(NestedDesktop::environment(a), QStringList());
        VmConfig::setScreen(a, VmConfig::Screen::OwnWindow, "sdl");
        QCOMPARE(NestedDesktop::environment(a), QStringList{"SDL_RENDER_DRIVER=vulkan"});
        QVERIFY(VmRunner::environment(a).contains("SDL_RENDER_DRIVER=vulkan"));
        QVERIFY(VmRunner::environment(a).contains("QEMU_SDL_ZERO_COPY=1"));
        VmConfig::setScreen(a, VmConfig::Screen::OwnWindow, "gtk");
        QCOMPARE(NestedDesktop::environment(a), QStringList());
        /* not without the nested desktop */
        QCOMPARE(NestedDesktop::environment(ArgsFile::parse("-display sdl,gl=on\n")),
                 QStringList());
    }

    void compositorSocket()
    {
        const QByteArray display = qgetenv("WAYLAND_DISPLAY");
        const QByteArray runtime = qgetenv("XDG_RUNTIME_DIR");

        qputenv("XDG_RUNTIME_DIR", "/run/user/7");
        qputenv("WAYLAND_DISPLAY", "wayland-1");
        QCOMPARE(NestedDesktop::compositorSocket(), "/run/user/7/wayland-1");
        qputenv("WAYLAND_DISPLAY", "/tmp/kwin-nested");
        QCOMPARE(NestedDesktop::compositorSocket(), "/tmp/kwin-nested");
        qunsetenv("WAYLAND_DISPLAY");
        QCOMPARE(NestedDesktop::compositorSocket(), "");
        qputenv("WAYLAND_DISPLAY", display);
        qputenv("XDG_RUNTIME_DIR", runtime);
    }

    /* Offered by a QEMU with the card and cross-domain; its properties only */
    void offer()
    {
        const QString research = qemuWith("research", {"cross-domain", "hostmem", "x-drm",
                                                       "x-virgl2", "wayland-socket-path"});
        const QString stock = qemuWith("stock", {"cross-domain", "hostmem", "ioeventfd"});
        const QString without = qemuWith("without", {});

        NestedDesktop::Offer o = NestedDesktop::offer(ArgsFile::parse("#qemu " + research + "\n"));
        QVERIFY(o.offered);
        QVERIFY(o.answered);
        QCOMPARE(NestedDesktop::cardDevice(o.properties),
                 "virtio-vga-rutabaga,x-drm=on,x-virgl2=on,cross-domain=on,hostmem=16G");
        o = NestedDesktop::offer(ArgsFile::parse("#qemu " + stock + "\n"));
        QVERIFY(o.offered);
        QVERIFY(!o.properties->contains("x-drm"));
        o = NestedDesktop::offer(ArgsFile::parse("#qemu " + without + "\n"));
        QVERIFY(!o.offered);
        QVERIFY(o.answered);
        /* one that does not answer offers nothing */
        o = NestedDesktop::offer(ArgsFile::parse("#qemu /nonexistent/qemu-system-x86_64\n"));
        QVERIFY(!o.offered);
        QVERIFY(!o.answered);
    }

    /* The command line of each display, with the nested desktop and without */
    void commandLines()
    {
        const QString id = QString("vitrine-nested-test-%1").arg(QCoreApplication::applicationPid());
        const VmRunner runner(id, tmp.path());
        const QString runDir = QFileInfo(runner.commandLine({}).last()).absolutePath();
        const QString qemu = "#qemu /nonexistent/qemu-system-x86_64\n";
        const QString monitor = "unix:" + runDir + "/display.sock,server=on,wait=off";
        struct Case {
            QString display;            // the -display line the page writes
            QString socket;             // the socket QEMU's cross-domain gets
            bool monitor;               // the display's QMP, which the view attaches through
        };
        const Case cases[] = {
            {"dbus,p2p=yes,gl=on", runDir + "/nested.sock", true},
            {"sdl", runDir + "/wayland.sock", false},
            {"gtk", runDir + "/wayland.sock", false},
            {"none", runDir + "/wayland.sock", false},
        };

        for (const Case &c : cases) {
            ArgsFile a = ArgsFile::parse(qemu + kNested);
            a.lines[a.indexOf("display")].value = c.display;
            const QStringList command = runner.commandLine(a);
            const QString card = command.filter("virtio-vga-rutabaga").value(0);

            QVERIFY2(card.endsWith(",wayland-socket-path=" + c.socket), qPrintable(card));
            QCOMPARE(command.at(command.indexOf(card) + 1), "-fw_cfg");
            QCOMPARE(command.at(command.indexOf(card) + 2),
                     QString("name=opt/org.drm-native-context/cmdline,string=crosvm_desktop=1 "
                             "crosvm_desktop_guest_dmabuf=1 crosvm_desktop_label=Test") +
                         QChar(0x00a0) + "VM");
            QCOMPARE(command.at(command.indexOf("-display") + 1), c.display);
            QCOMPARE(command.at(command.indexOf("-vga") + 1), "none");
            QVERIFY(command.filter("virtio-gpu-rutabaga-pci").isEmpty());
            QVERIFY(command.contains("pvpanic"));
            QVERIFY(command.contains("kvm-pit.lost_tick_policy=discard"));
            QCOMPARE(command.contains(monitor), c.monitor);
            /* in vitrine's window, paused until its screen is set */
            QCOMPARE(command.contains("-S"), c.monitor);
            QCOMPARE(VmRunner::environment(a).contains("SDL_RENDER_DRIVER=vulkan"),
                     c.display == "sdl");

            /* without the nested desktop: no socket, no words */
            ArgsFile plain = ArgsFile::parse(qemu + kVm);
            plain.lines[plain.indexOf("display")].value =
                c.display == "none" ? QString("egl-headless") : c.display;
            const QStringList without = runner.commandLine(plain);
            QVERIFY(without.filter("wayland-socket-path").isEmpty());
            QVERIFY(!without.contains("-fw_cfg"));
            QCOMPARE(without.contains(monitor), c.monitor);
            QVERIFY(!VmRunner::environment(plain).contains("SDL_RENDER_DRIVER=vulkan"));
        }
        /* not running: no socket for the console to serve */
        QCOMPARE(runner.nestedSocket(), "");
    }
};

QTEST_GUILESS_MAIN(TestNestedDesktop)
#include "test_nesteddesktop.moc"
