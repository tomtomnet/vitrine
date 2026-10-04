// SPDX-License-Identifier: GPL-2.0-or-later
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include "core/vmhardware.h"

using namespace VmConfig;

/* The display lines of the user's launch script */
static const char kScript[] = "-accel kvm,honor-guest-pat=on\n"
                              "-machine q35,memory-backend=mem\n"
                              "-device virtio-gpu-gl,hostmem=8G,blob=on,drm_native_context=on\n"
                              "-vga none\n"
                              "-display sdl,gl=on\n";

/* What a new VM gets */
static const char kTemplate[] = "-machine q35,memory-backend=mem\n"
                                "-accel kvm\n"
                                "-device virtio-vga-gl\n"
                                "-display sdl,gl=on\n";

class TestVmHardware : public QObject
{
    Q_OBJECT

    static QString text(const ArgsFile &args) { return args.toText(); }

private slots:
    void accelerator()
    {
        ArgsFile a = ArgsFile::parse("-machine q35,accel=kvm\n-m 1G\n");

        QCOMPARE(accel(a), "kvm");
        setAccelProperty(a, "honor-guest-pat", "on");
        QCOMPARE(text(a), "-machine q35\n-accel kvm,honor-guest-pat=on\n-m 1G\n");
        QCOMPARE(accelProperty(a, "honor-guest-pat"), "on");
        setAccelProperty(a, "honor-guest-pat", {});
        QCOMPARE(text(a), "-machine q35\n-accel kvm\n-m 1G\n");

        a = ArgsFile::parse("-enable-kvm\n-machine q35\n");
        setAccelProperty(a, "honor-guest-pat", "on");
        QCOMPARE(text(a), "-machine q35\n-accel kvm,honor-guest-pat=on\n");

        a = ArgsFile::parse("-machine q35,accel=kvm:tcg\n");
        setAccelProperty(a, "honor-guest-pat", "on");
        QCOMPARE(text(a), "-machine q35\n-accel kvm,honor-guest-pat=on\n-accel tcg\n");

        /* TCG, QEMU's default: nothing to put it on */
        a = ArgsFile::parse("-machine q35\n");
        setAccelProperty(a, "honor-guest-pat", "on");
        QCOMPARE(text(a), "-machine q35\n");

        a = ArgsFile::parse("# top\n-m 1G\n");
        setMachineType(a, "q35");
        setAccel(a, "kvm");
        QCOMPARE(text(a), "# top\n-machine q35\n-accel kvm\n-m 1G\n");
        QCOMPARE(machineType(a), "q35");
    }

    void readGraphics()
    {
        Graphics g = graphics(ArgsFile::parse(kScript));
        QCOMPARE(g.kind, Graphics::Accelerated);
        QCOMPARE(g.device, "virtio-gpu-gl");
        QVERIFY(g.nativeContext);
        QVERIFY(!g.venus);
        QCOMPARE(g.hostmemMiB, 8192);
        QCOMPARE(g.display, "sdl");

        g = graphics(ArgsFile::parse(kTemplate));
        QCOMPARE(g.kind, Graphics::Accelerated);
        QVERIFY(!g.nativeContext);
        QCOMPARE(g.hostmemMiB, 0);

        const std::pair<const char *, Graphics::Kind> cases[] = {
            {"-device virtio-vga\n", Graphics::Virtio},
            {"-device virtio-gpu-pci\n-vga none\n", Graphics::Virtio},
            {"-device VGA\n", Graphics::Standard},
            {"-m 1G\n", Graphics::Standard},
            {"-vga std\n", Graphics::Standard},
            {"-vga virtio\n", Graphics::Virtio},
            {"-vga none\n", Graphics::None},
            {"-nodefaults\n", Graphics::None},
            {"-machine virt\n", Graphics::None},
            {"-vga qxl\n", Graphics::Custom},
            {"-device qxl-vga\n", Graphics::Custom},
            {"-device virtio-vga\n-device secondary-vga\n", Graphics::Custom},
            {"-vga std\n-device virtio-gpu-pci\n", Graphics::Custom},
            {"-nographic\n", Graphics::Custom},
        };
        for (const auto &[args, kind] : cases) {
            QVERIFY2(graphics(ArgsFile::parse(args)).kind == kind, args);
        }
    }

    /* vitrine's window: the D-Bus display on a socket of its own, with OpenGL */
    void dbusDisplay()
    {
        ArgsFile a = ArgsFile::parse("-device virtio-vga-gl\n-display sdl,gl=on,grab-mod=rctrl\n");
        Graphics g = graphics(a);

        QVERIFY(screen(a) == Screen::OwnWindow);
        g.display = "dbus";
        setGraphics(a, g);
        QCOMPARE(text(a), "-device virtio-vga-gl\n-display dbus,gl=on,p2p=yes\n");
        QVERIFY(screen(a) == Screen::Embedded);
        QCOMPARE(graphics(a).display, "dbus");

        g = graphics(a);
        g.display = "sdl";
        setGraphics(a, g);
        QCOMPARE(text(a), "-device virtio-vga-gl\n-display sdl,gl=on\n");

        /* a new display line */
        a = ArgsFile::parse("-device virtio-vga-gl\n");
        g = graphics(a);
        g.display = "dbus";
        setGraphics(a, g);
        QCOMPARE(text(a), "-device virtio-vga-gl\n-display dbus,p2p=yes,gl=on\n");

        const std::pair<const char *, Screen> cases[] = {
            {"-m 1G\n", Screen::OwnWindow},
            {"-display gtk\n", Screen::OwnWindow},
            {"-display dbus,p2p=on\n", Screen::Embedded},
            {"-display dbus\n", Screen::None},
            {"-display none\n", Screen::None},
            {"-display egl-headless\n", Screen::None},
            {"-display sdl\n-nographic\n", Screen::None},
            /* p2p as QEMU reads booleans */
            {"-display dbus,p2p=true\n", Screen::Embedded},
            {"-display dbus,p2p=y\n", Screen::Embedded},
            {"-display dbus,p2p=n\n", Screen::None},
            /* QEMU opens no window of its own beside a remote display */
            {"-vnc :0\n", Screen::None},
            {"-spice port=5900\n", Screen::None},
            {"-display vnc=:0\n", Screen::None},
            {"-display default\n-vnc :0\n", Screen::None},
            {"-display sdl\n-vnc :0\n", Screen::OwnWindow},
            {"-display sdl\n-display vnc=:0\n", Screen::OwnWindow},
            {"-display default\n", Screen::OwnWindow},
            {"-daemonize\n", Screen::OwnWindow},
        };
        for (const auto &[args, expected] : cases) {
            QVERIFY2(screen(ArgsFile::parse(args)) == expected, args);
        }
    }

    /* The Display page: vitrine's window or SDL, the card as it is */
    void setScreens()
    {
        ArgsFile a = ArgsFile::parse("-vga none\n-device virtio-gpu-gl-pci,blob=on\n"
                                     "-display sdl,gl=on,window-close=off\n");

        setScreen(a, Screen::Embedded);
        QCOMPARE(text(a), "-vga none\n-device virtio-gpu-gl-pci,blob=on\n"
                          "-display dbus,gl=on,p2p=yes\n");
        QVERIFY(screen(a) == Screen::Embedded);
        setScreen(a, Screen::OwnWindow);
        QCOMPARE(text(a), "-vga none\n-device virtio-gpu-gl-pci,blob=on\n-display sdl,gl=on\n");

        /* OpenGL for the 3D card, even if the D-Bus display had none */
        a = ArgsFile::parse("-device virtio-vga-gl\n-display dbus,p2p=yes\n");
        setScreen(a, Screen::OwnWindow);
        QCOMPARE(text(a), "-device virtio-vga-gl\n-display sdl,gl=on\n");

        /* none for a 2D card */
        a = ArgsFile::parse("-device virtio-vga\n");
        setScreen(a, Screen::Embedded);
        QCOMPARE(text(a), "-device virtio-vga\n-display dbus,p2p=yes\n");

        /* a card set by hand stays */
        a = ArgsFile::parse("-device qxl-vga\n-device virtio-gpu-pci\n-display gtk\n");
        setScreen(a, Screen::Embedded);
        QCOMPARE(text(a), "-device qxl-vga\n-device virtio-gpu-pci\n-display dbus,p2p=yes\n");

        /* VNC alone: a window beside it; a -display vnc= stays as it is */
        a = ArgsFile::parse("-vnc :0\n");
        setScreen(a, Screen::OwnWindow);
        QCOMPARE(text(a), "-vnc :0\n-display sdl\n");
        QVERIFY(screen(a) == Screen::OwnWindow);
        a = ArgsFile::parse("-display sdl\n-display vnc=:0\n");
        setScreen(a, Screen::Embedded);
        QCOMPARE(text(a), "-display dbus,p2p=yes\n-display vnc=:0\n");

        /* a D-Bus display on a bus of its own: QEMU refuses addr= with p2p= */
        a = ArgsFile::parse("-display dbus,addr=unix:path=/run/user/1000/qemu.bus\n");
        QVERIFY(screen(a) == Screen::None);
        setScreen(a, Screen::Embedded);
        QCOMPARE(text(a), "-display dbus,p2p=yes\n");
        QVERIFY(screen(a) == Screen::Embedded);
        a = ArgsFile::parse("-device virtio-vga-gl\n-display dbus,addr=unix:path=/x,p2p=yes\n");
        QVERIFY(screen(a) == Screen::None);
        setScreen(a, Screen::Embedded);
        QCOMPARE(text(a), "-device virtio-vga-gl\n-display dbus,p2p=yes,gl=on\n");
    }

    void readNetwork()
    {
        struct Case {
            const char *args;
            Network::Kind kind;
            const char *backend;
            const char *card;
            int ssh;
        };
        const Case cases[] = {
            {"-netdev user,id=n0,hostfwd=tcp:127.0.0.1:2222-:22\n-device virtio-net-pci,netdev=n0\n",
             Network::Nat, "user", "virtio-net-pci", 2222},
            {"-netdev user,id=n0,hostfwd=udp::2000-:22,hostfwd=::8080-:80,hostfwd=::2200-:22\n"
             "-device e1000e,netdev=n0\n",
             Network::Nat, "user", "e1000e", 2200},
            {"-netdev user,id=n0\n-device virtio-net-pci,netdev=n0\n", Network::Nat, "user",
             "virtio-net-pci", 0},
            {"-netdev passt,id=n0,vhost-user=on,tcp-ports=127.0.0.1/2232:22\n"
             "-device virtio-net-pci,netdev=n0\n",
             Network::Nat, "passt", "virtio-net-pci", 2232},
            {"-netdev passt,id=n0,tcp-ports=2022\n-device virtio-net-pci,netdev=n0\n",
             Network::Nat, "passt", "virtio-net-pci", 0},
            {"-netdev passt,id=n0,tcp-ports=22\n-device virtio-net-pci,netdev=n0\n",
             Network::Nat, "passt", "virtio-net-pci", 22},
            {"-nic user,model=virtio-net-pci,hostfwd=tcp::2022-:22\n", Network::Nat, "user",
             "virtio-net-pci", 2022},
            {"-m 1G\n", Network::Nat, "", "", 0},
            {"-nic none\n", Network::Off, "", "", 0},
            {"-nodefaults\n", Network::Off, "", "", 0},
            {"-netdev tap,id=n0\n-device virtio-net-pci,netdev=n0\n", Network::Custom, "", "", 0},
            {"-netdev {\"type\":\"passt\",\"id\":\"n0\"}\n-device virtio-net-pci,netdev=n0\n",
             Network::Custom, "", "", 0},
            {"-netdev user,id=a\n-netdev user,id=b\n", Network::Custom, "", "", 0},
            {"-netdev user,id=a\n", Network::Custom, "", "", 0},
            {"-netdev user,id=a\n-device e1000,netdev=a\n-device e1000,netdev=a\n",
             Network::Custom, "", "", 0},
            {"-net nic\n-net user\n", Network::Custom, "", "", 0},
        };
        for (const Case &c : cases) {
            const Network n = network(ArgsFile::parse(c.args));
            QVERIFY2(n.kind == c.kind, c.args);
            if (n.kind == Network::Custom) {
                QVERIFY2(!n.custom.isEmpty(), c.args);
                continue;
            }
            QCOMPARE(n.backend, QString(c.backend));
            QCOMPARE(n.card, QString(c.card));
            QCOMPARE(n.sshPort, c.ssh);
        }
    }

    /* An existing image's format from its first bytes, not its name */
    void imageFormats()
    {
        QTemporaryDir dir;
        const auto image = [&dir](const QString &name, const QByteArray &head) {
            QFile f(dir.filePath(name));
            if (!f.open(QIODevice::WriteOnly) || f.write(head + QByteArray(512, '\0')) < 0) {
                return QString();
            }
            return dir.filePath(name);
        };
        const QString cloud = image("jammy-server-cloudimg-amd64.img", QByteArray("QFI\xfb\0\0\0\3"));

        QCOMPARE(imageFormat(cloud), "qcow2");
        QCOMPARE(imageFormat(image("plain.img", {})), "raw");
        QCOMPARE(imageFormat(image("disk.vmdk", "KDMV")), "vmdk");
        QCOMPARE(imageFormat(image("disk", "vhdxfile")), "vhdx");
        QCOMPARE(imageFormat(image("disk.bin", QByteArray(0x40, 'x') + "\x7f\x10\xda\xbe")), "vdi");
        /* a raw disk's start is the guest's */
        QCOMPARE(imageFormat(image("guest.raw", "QFI\xfb")), "raw");
        /* by name: relative to the VM folder, or not there yet */
        QCOMPARE(imageFormat("disk.qcow2"), "qcow2");
        QCOMPARE(imageFormat("/nonexistent/disk.img"), "raw");

        ArgsFile a = ArgsFile::parse("-m 1G\n");
        addDisk(a, cloud, Disk::Virtio);
        QCOMPARE(text(a), "-m 1G\n-drive file=" + cloud + ",format=qcow2,if=virtio,discard=unmap\n");
    }

    /* Where the SSH forward listens */
    void sshAddresses()
    {
        const auto address = [](const char *args) {
            return network(ArgsFile::parse(args)).sshAddress;
        };

        QCOMPARE(address("-nic user,hostfwd=tcp::2222-:22\n"), "");
        QCOMPARE(address("-nic user,hostfwd=tcp:127.0.0.1:2222-:22\n"), "127.0.0.1");
        QCOMPARE(address("-nic user,hostfwd=tcp:192.168.1.5:2222-:22\n"), "192.168.1.5");
        QCOMPARE(address("-nic user,hostfwd=tcp:[::1]:2222-:22\n"), "[::1]");
        QCOMPARE(network(ArgsFile::parse("-nic user,hostfwd=tcp:[::1]:2222-:22\n")).sshPort, 2222);
        QCOMPARE(address("-nic passt,tcp-ports=10022:22\n"), "");
        QCOMPARE(address("-nic passt,tcp-ports=127.0.0.1/10022:22\n"), "127.0.0.1");
        QCOMPARE(address("-nic passt,tcp-ports=%eth0/10022:22\n"), "%eth0");

        QVERIFY(isLoopback("127.0.0.1"));
        QVERIFY(isLoopback("127.0.0.2%lo"));
        QVERIFY(isLoopback("[::1]"));
        QVERIFY(isLoopback("::1"));
        QVERIFY(!isLoopback(""));
        QVERIFY(!isLoopback("0.0.0.0"));
        QVERIFY(!isLoopback("192.168.1.5"));
        QVERIFY(!isLoopback("%eth0"));
    }

    /* The ports to keep clear of for another VM's forward */
    void forwardedHostPorts()
    {
        const auto ports = [](const char *args) {
            return forwardedPorts(ArgsFile::parse(args));
        };

        QCOMPARE(ports("-m 1G\n"), QList<int>());
        /* the forward the Network page follows, and the others */
        QCOMPARE(ports("-netdev user,id=n0,hostfwd=tcp:127.0.0.1:2222-:22,hostfwd=::8080-:80,"
                       "hostfwd=udp::5353-:53,hostfwd=tcp:[::1]:2223-:22\n"
                       "-device e1000e,netdev=n0\n"),
                 QList<int>({2222, 8080, 2223}));
        /* two networks, which the page leaves to the Arguments page */
        QCOMPARE(ports("-netdev passt,id=n0,tcp-ports=127.0.0.1/10022:22\n"
                       "-device virtio-net-pci,netdev=n0\n"
                       "-netdev user,id=n1\n-device e1000e,netdev=n1\n"),
                 QList<int>({10022}));
        /* passt's lists and ranges, as QemuOpts escapes commas */
        QCOMPARE(ports("-nic passt,tcp-ports=%eth0/10030-10032:30-32,,~10031,,all,udp-ports=10040\n"),
                 QList<int>({10030, 10031, 10032, 10031}));
        /* in JSON, and the legacy -net */
        QCOMPARE(ports("-netdev {\"type\":\"passt\",\"id\":\"n0\","
                       "\"tcp-ports\":[{\"str\":\"127.0.0.1/10050:22\"}]}\n"
                       "-netdev {\"type\":\"user\",\"id\":\"n1\","
                       "\"hostfwd\":[{\"str\":\"tcp::10051-:80\"}]}\n"
                       "-net user,hostfwd=tcp::10052-:22\n"),
                 QList<int>({10050, 10051, 10052}));
    }

    void setNetworks()
    {
        const QString passt = "-m 1G\n"
                              "-netdev passt,id=net0,vhost-user=on,tcp-ports=127.0.0.1/2222:22\n"
                              "-device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56\n"
                              "-device virtio-rng-pci\n";
        ArgsFile a = ArgsFile::parse(passt);
        Network n = network(a);

        /* the forward only */
        n.sshPort = 2232;
        setNetwork(a, n);
        QCOMPARE(text(a), QString(passt).replace("/2222:", "/2232:"));
        n.sshPort = 0;
        setNetwork(a, n);
        QCOMPARE(text(a), QString(passt).replace(",tcp-ports=127.0.0.1/2222:22", ""));

        /* off and on again, in the same place */
        n.kind = Network::Off;
        setNetwork(a, n);
        QCOMPARE(text(a), "-m 1G\n-nic none\n-device virtio-rng-pci\n");
        QVERIFY(network(a).kind == Network::Off);
        n = network(a);
        n.kind = Network::Nat;
        n.backend = "passt";
        n.sshPort = 2222;
        setNetwork(a, n);
        /* no shared memory: no vhost-user */
        QCOMPARE(text(a), "-m 1G\n-netdev passt,id=net0,tcp-ports=127.0.0.1/2222:22\n"
                          "-device virtio-net-pci,netdev=net0\n-device virtio-rng-pci\n");
        QCOMPARE(network(a).sshPort, 2222);

        /* user: the other forwards stay */
        a = ArgsFile::parse("-netdev user,id=n0,hostfwd=tcp::2022-:22,hostfwd=tcp::8080-:80\n"
                            "-device e1000e,netdev=n0\n");
        n = network(a);
        n.sshPort = 2300;
        setNetwork(a, n);
        QCOMPARE(text(a), "-netdev user,id=n0,hostfwd=tcp::8080-:80,"
                          "hostfwd=tcp:127.0.0.1:2300-:22\n-device e1000e,netdev=n0\n");

        /* the card goes before its -netdev too */
        a = ArgsFile::parse("-device e1000e,netdev=n0\n-netdev user,id=n0\n-m 1G\n");
        n = network(a);
        n.kind = Network::Off;
        setNetwork(a, n);
        QCOMPARE(text(a), "-nic none\n-m 1G\n");

        /* with shared memory, passt maps it for a virtio-net card */
        const QString shared = "-machine q35,memory-backend=mem\n"
                               "-object memory-backend-memfd,id=mem,size=4G,share=on\n";
        a = ArgsFile::parse(shared + "-nic none\n");
        n = network(a);
        n.kind = Network::Nat;
        n.backend = "passt";
        setNetwork(a, n);
        QCOMPARE(text(a), shared + "-netdev passt,id=net0,vhost-user=on\n"
                                   "-device virtio-net-pci,netdev=net0\n");
        /* but not for Windows' e1000e: QEMU refuses vhost-user with other cards */
        a = ArgsFile::parse("#guest windows\n" + shared + "-nic none\n");
        setNetwork(a, n);
        QCOMPARE(text(a), "#guest windows\n" + shared +
                              "-netdev passt,id=net0\n-device e1000e,netdev=net0\n");
        a = ArgsFile::parse(shared + "-nic none\n");
        n.card = "e1000e";
        setNetwork(a, n);
        QCOMPARE(text(a), shared + "-netdev passt,id=net0\n-device e1000e,netdev=net0\n");
        n.card.clear();

        /* QEMU's default card: off, or a card of our own for a forward */
        a = ArgsFile::parse("-m 1G\n");
        n = network(a);
        setNetwork(a, n);
        QCOMPARE(text(a), "-m 1G\n");
        n.sshPort = 2222;
        setNetwork(a, n);
        QCOMPARE(text(a), "-m 1G\n-netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22\n"
                          "-device virtio-net-pci,netdev=net0\n");
        a = ArgsFile::parse("-m 1G\n");
        n.kind = Network::Off;
        setNetwork(a, n);
        QCOMPARE(text(a), "-m 1G\n-nic none\n");

        /* -nodefaults: at the end */
        a = ArgsFile::parse("-nodefaults\n-m 1G\n");
        n = network(a);
        n.kind = Network::Nat;
        setNetwork(a, n);
        QCOMPARE(text(a), "-nodefaults\n-m 1G\n-netdev user,id=net0\n"
                          "-device virtio-net-pci,netdev=net0\n");

        /* -nic user */
        a = ArgsFile::parse("-nic user,model=virtio-net-pci\n");
        n = network(a);
        n.sshPort = 2222;
        setNetwork(a, n);
        QCOMPARE(text(a), "-nic user,model=virtio-net-pci,hostfwd=tcp:127.0.0.1:2222-:22\n");

        /* set by hand: left alone */
        const QString tap = "-netdev tap,id=n0\n-device virtio-net-pci,netdev=n0\n";
        a = ArgsFile::parse(tap);
        n.kind = Network::Off;
        setNetwork(a, n);
        QCOMPARE(text(a), tap);
    }

    void nativeContext()
    {
        ArgsFile a = ArgsFile::parse(kTemplate);
        Graphics g = graphics(a);

        g.nativeContext = true;
        setGraphics(a, g);
        QCOMPARE(text(a), "-machine q35,memory-backend=mem\n"
                          "-accel kvm,honor-guest-pat=auto\n"
                          "-device virtio-vga-gl,drm_native_context=on,blob=on,hostmem=4G\n"
                          "-display sdl,gl=on\n");

        /* off: blob and hostmem stay, 2D blobs use them too */
        a = ArgsFile::parse(kScript);
        g = graphics(a);
        g.nativeContext = false;
        setGraphics(a, g);
        QCOMPARE(text(a), QString(kScript).replace(",drm_native_context=on", ""));

        /* a bigger window */
        a = ArgsFile::parse(kScript);
        g = graphics(a);
        g.hostmemMiB = 16384;
        setGraphics(a, g);
        QVERIFY(text(a).contains("-device virtio-gpu-gl,hostmem=16G,blob=on,"
                                 "drm_native_context=on\n"));

        a = ArgsFile::parse(kTemplate);
        g = graphics(a);
        g.venus = true;
        setGraphics(a, g);
        QVERIFY(text(a).contains("-device virtio-vga-gl,venus=on,blob=on,hostmem=4G\n"));
        QVERIFY(text(a).contains("-accel kvm\n"));      /* only native context needs it */

        /* no KVM, no honor-guest-pat */
        a = ArgsFile::parse("-machine q35\n-device virtio-vga-gl\n");
        g = graphics(a);
        g.nativeContext = true;
        setGraphics(a, g);
        QCOMPARE(text(a), "-machine q35\n"
                          "-device virtio-vga-gl,drm_native_context=on,blob=on,hostmem=4G\n");
    }

    void changeCard()
    {
        /* 3D to 2D: the same card without OpenGL */
        ArgsFile a = ArgsFile::parse(kScript);
        Graphics g = graphics(a);
        g.kind = Graphics::Virtio;
        setGraphics(a, g);
        QCOMPARE(text(a), "-accel kvm,honor-guest-pat=on\n"
                          "-machine q35,memory-backend=mem\n"
                          "-device virtio-gpu,hostmem=8G,blob=on\n"
                          "-vga none\n"
                          "-display sdl\n");

        a = ArgsFile::parse("-device virtio-vga\n-display sdl\n");
        g = graphics(a);
        g.kind = Graphics::Accelerated;
        setGraphics(a, g);
        QCOMPARE(text(a), "-device virtio-vga-gl\n-display sdl,gl=on\n");

        /* from QEMU's default VGA */
        a = ArgsFile::parse("-machine q35\n-display sdl\n");
        g = graphics(a);
        g.kind = Graphics::Accelerated;
        setGraphics(a, g);
        QCOMPARE(text(a), "-machine q35\n-device virtio-vga-gl\n-display sdl,gl=on\n");

        /* a card without VGA: QEMU's own VGA goes */
        a = ArgsFile::parse("-machine q35\n-display sdl\n");
        g = graphics(a);
        g.kind = Graphics::Virtio;
        g.device = "virtio-gpu-pci";
        setGraphics(a, g);
        QCOMPARE(text(a), "-machine q35\n-device virtio-gpu-pci\n-vga none\n-display sdl\n");

        /* virtio to VGA: the virtio properties go */
        a = ArgsFile::parse("-device virtio-vga,max_outputs=2,id=gpu\n-display gtk,gl=on\n");
        g = graphics(a);
        g.kind = Graphics::Standard;
        setGraphics(a, g);
        QCOMPARE(text(a), "-device VGA,id=gpu\n-display gtk\n");

        a = ArgsFile::parse(kTemplate);
        g = graphics(a);
        g.kind = Graphics::None;
        setGraphics(a, g);
        QCOMPARE(text(a), "-machine q35,memory-backend=mem\n-accel kvm\n-vga none\n"
                          "-display sdl\n");
        QCOMPARE(graphics(a).kind, Graphics::None);

        /* ARM's virt: PCI cards, no VGA to turn off */
        a = ArgsFile::parse("-machine virt,gic-version=max\n-display sdl\n");
        g = graphics(a);
        QCOMPARE(g.kind, Graphics::None);
        g.kind = Graphics::Accelerated;
        setGraphics(a, g);
        QCOMPARE(text(a), "-machine virt,gic-version=max\n-device virtio-gpu-gl-pci\n"
                          "-display sdl,gl=on\n");
    }

    void window()
    {
        ArgsFile a = ArgsFile::parse("-device virtio-vga-gl\n-display sdl,gl=on,grab-mod=rctrl\n");
        Graphics g = graphics(a);

        g.display = "gtk";
        setGraphics(a, g);
        QCOMPARE(text(a), "-device virtio-vga-gl\n-display gtk,gl=on\n");
        g.display = "none";
        setGraphics(a, g);
        QCOMPARE(text(a), "-device virtio-vga-gl\n-display none\n");

        a = ArgsFile::parse("-device virtio-vga-gl\n");
        g = graphics(a);
        g.display = "sdl";
        setGraphics(a, g);
        QCOMPARE(text(a), "-device virtio-vga-gl\n-display sdl,gl=on\n");

        /* a card set by hand stays as it is, the window may change */
        a = ArgsFile::parse("-device qxl-vga,vgamem_mb=64\n-display sdl\n");
        g = graphics(a);
        QCOMPARE(g.kind, Graphics::Custom);
        g.display = "gtk";
        setGraphics(a, g);
        QCOMPARE(text(a), "-device qxl-vga,vgamem_mb=64\n-display gtk\n");
    }

    void unchanged()
    {
        for (const char *args :
             {kScript, kTemplate, "-vga none\n-display none\n", "-device qxl-vga\n",
              "-machine virt\n-device virtio-gpu-pci\n-display gtk,gl=on\n",
              "-device virtio-gpu-gl-pci,venus=on,blob=on,hostmem=4G\n"}) {
            ArgsFile a = ArgsFile::parse(args);
            setGraphics(a, graphics(a));
            QCOMPARE(text(a), QString(args));
        }
    }

    void readDisks()
    {
        const ArgsFile a = ArgsFile::parse(
            "-drive if=pflash,format=raw,readonly=on,file=OVMF_CODE.fd\n"
            "-hda cachyos.qcow2\n"
            "-cdrom /isos/x.iso\n"
            "-drive file=disk.qcow2,format=qcow2,if=virtio,discard=unmap\n"
            "-drive file=win.iso,media=cdrom,readonly=on\n"
            "-drive file=d.qcow2,if=none,id=d0\n"
            "-device virtio-blk-pci,drive=d0,bootindex=2\n"
            "-blockdev driver=file,filename=/b.raw,node-name=f0\n");
        const QList<Disk> list = disks(a);

        QCOMPARE(list.size(), 6);
        QCOMPARE(list[0].file, "cachyos.qcow2");
        QCOMPARE(list[0].bus, Disk::Sata);
        QVERIFY(!list[0].cdrom);
        QVERIFY(list[1].cdrom);
        QCOMPARE(list[2].bus, Disk::Virtio);
        QCOMPARE(list[2].format, "qcow2");
        QVERIFY(list[3].cdrom);
        QCOMPARE(list[3].bus, Disk::Sata);
        QCOMPARE(list[4].deviceLine, 6);
        QCOMPARE(list[4].bootindex, 2);
        QCOMPARE(list[4].bus, Disk::Virtio);
        QVERIFY(!list[5].editable);
    }

    void editDisks()
    {
        ArgsFile a = ArgsFile::parse("-drive if=pflash,unit=0,file=CODE.fd\n"
                                     "-drive file=disk.qcow2,format=qcow2,if=virtio,discard=unmap\n"
                                     "-netdev user,id=net0\n");

        addDisk(a, "data,2.qcow2", Disk::Virtio);
        addDisk(a, "/imgs/win.img", Disk::Sata);
        addCdrom(a, {});
        QCOMPARE(text(a), "-drive if=pflash,unit=0,file=CODE.fd\n"
                          "-drive file=disk.qcow2,format=qcow2,if=virtio,discard=unmap\n"
                          "-drive file=data,,2.qcow2,format=qcow2,if=virtio,discard=unmap\n"
                          "-drive file=/imgs/win.img,format=raw,if=ide,discard=unmap\n"
                          "-drive media=cdrom,readonly=on\n"
                          "-netdev user,id=net0\n");

        QList<Disk> list = disks(a);
        QCOMPARE(list.size(), 4);
        setDisc(a, list[3], "/isos/fedora.iso");
        QVERIFY(text(a).contains("-drive media=cdrom,readonly=on,file=/isos/fedora.iso\n"));
        setDisc(a, disks(a)[3], {});
        QVERIFY(text(a).contains("-drive media=cdrom,readonly=on\n"));
        removeDisk(a, disks(a)[1]);
        QCOMPARE(disks(a).size(), 3);
        QVERIFY(!text(a).contains("data,,2"));

        /* -cdrom ejected is an empty drive in its place */
        a = ArgsFile::parse("-cdrom x.iso\n");
        setDisc(a, disks(a)[0], {});
        QCOMPARE(text(a), "-drive index=2,media=cdrom,readonly=on\n");
        QVERIFY(disks(a)[0].cdrom);

        /* the drive and its device go together */
        a = ArgsFile::parse("-drive file=d.qcow2,if=none,id=d0\n-m 1G\n"
                            "-device virtio-blk-pci,drive=d0\n");
        removeDisk(a, disks(a)[0]);
        QCOMPARE(text(a), "-m 1G\n");
    }

    void armDisks()
    {
        /* virt has no IDE: the CD goes on virtio-scsi */
        ArgsFile a = ArgsFile::parse("-machine virt\n"
                                     "-drive file=disk.qcow2,format=qcow2,if=virtio\n");
        addCdrom(a, "/isos/fedora.iso");
        addCdrom(a, {});
        QCOMPARE(text(a), "-machine virt\n"
                          "-drive file=disk.qcow2,format=qcow2,if=virtio\n"
                          "-device virtio-scsi-pci,id=scsi0\n"
                          "-drive file=/isos/fedora.iso,media=cdrom,readonly=on,if=none,id=cd0\n"
                          "-device scsi-cd,drive=cd0,bus=scsi0.0\n"
                          "-drive media=cdrom,readonly=on,if=none,id=cd1\n"
                          "-device scsi-cd,drive=cd1,bus=scsi0.0\n");
        const QList<Disk> list = disks(a);
        QCOMPARE(list.size(), 3);
        QCOMPARE(list[1].bus, Disk::Scsi);
        QVERIFY(list[1].cdrom);
        QCOMPARE(list[1].file, "/isos/fedora.iso");
    }

    void firmware()
    {
        QTemporaryDir none;
        const QStringList dirs{none.path()};

        QCOMPARE(firmwareKind(ArgsFile::parse("-m 1G\n"), dirs), FirmwareKind::Bios);
        QCOMPARE(firmwareKind(ArgsFile::parse("-bios my.fd\n"), dirs), FirmwareKind::Custom);
        QCOMPARE(firmwareKind(ArgsFile::parse("-machine q35,pflash0=code\n"), dirs),
                 FirmwareKind::Custom);
        QCOMPARE(firmwareKind(ArgsFile::parse(
                                  "-drive if=pflash,format=qcow2,unit=0,readonly=on,"
                                  "file=OVMF_CODE_4M.qcow2\n"
                                  "-drive if=pflash,format=qcow2,unit=1,file=OVMF_VARS_4M.qcow2\n"),
                              dirs),
                 FirmwareKind::Uefi);
        QCOMPARE(firmwareKind(ArgsFile::parse("-drive if=pflash,unit=0,readonly=on,"
                                              "file=OVMF_CODE_4M.secboot.qcow2\n"),
                              dirs),
                 FirmwareKind::UefiSecureBoot);

        /* by the descriptors, when they know the file */
        QFile json(none.filePath("50-custom.json"));
        QVERIFY(json.open(QIODevice::WriteOnly));
        json.write(R"({"interface-types": ["uefi"], "features": ["secure-boot"],
            "mapping": {"device": "flash", "mode": "split",
                        "executable": {"filename": "/fw/MY_CODE.fd", "format": "raw"},
                        "nvram-template": {"filename": "/fw/MY_VARS.fd", "format": "raw"}},
            "targets": [{"architecture": "x86_64", "machines": ["pc-q35-*"]}]})");
        json.close();
        QCOMPARE(firmwareKind(ArgsFile::parse("-drive if=pflash,unit=0,readonly=on,"
                                              "file=MY_CODE.fd\n"),
                              dirs),
                 FirmwareKind::UefiSecureBoot);

        ArgsFile a = ArgsFile::parse("-machine q35,smm=on\n"
                                     "-drive if=pflash,unit=0,readonly=on,file=C.fd\n"
                                     "-drive if=pflash,unit=1,file=V.fd\n"
                                     "-global driver=cfi.pflash01,property=secure,value=on\n"
                                     "-m 1G\n");
        useBios(a);
        QCOMPARE(text(a), "-machine q35,smm=on\n-m 1G\n");
        QCOMPARE(firmwareKind(a, dirs), FirmwareKind::Bios);
    }

    void bootMenuOption()
    {
        ArgsFile a = ArgsFile::parse("-machine q35\n-accel kvm\n-m 1G\n");

        QVERIFY(!bootMenu(a));
        setBootMenu(a, true);
        QCOMPARE(text(a), "-machine q35\n-accel kvm\n-boot menu=on\n-m 1G\n");
        QVERIFY(bootMenu(a));
        setBootMenu(a, false);
        QCOMPARE(text(a), "-machine q35\n-accel kvm\n-m 1G\n");

        a = ArgsFile::parse("-boot order=d,menu=on\n");
        setBootMenu(a, false);
        QCOMPARE(text(a), "-boot order=d\n");
    }

    /* The devices the VM starts from, in their order */
    void bootOrder()
    {
        using E = BootEntry;
        auto entry = [](E::Kind kind, int index, bool on, bool editable = true) {
            E e;
            e.kind = kind;
            e.index = index;
            e.on = on;
            e.editable = editable;
            return e;
        };
        ArgsFile a = ArgsFile::parse("-machine q35\n"
                                     "-drive file=x.iso,media=cdrom,readonly=on\n"
                                     "-drive file=disk.qcow2,format=qcow2,if=virtio,discard=unmap\n"
                                     "-netdev user,id=net0\n"
                                     "-device virtio-net-pci,netdev=net0\n");
        const QString plain = text(a);

        /* none of its own: the firmware's, disks, CD/DVD drives, network */
        QVERIFY(!hasBootOrder(a));
        QCOMPARE(VmConfig::bootOrder(a),
                 QList<E>({entry(E::HardDisk, 1, true), entry(E::Cdrom, 0, true),
                           entry(E::Network, 0, true)}));
        /* as it is: nothing to write */
        setBootOrder(a, VmConfig::bootOrder(a));
        QVERIFY(!text(a).contains("strict"));

        /* the CD/DVD drive first, then the disk, never the network */
        a = ArgsFile::parse(plain);
        setBootOrder(a, {entry(E::Cdrom, 0, true), entry(E::HardDisk, 1, true),
                         entry(E::Network, 0, false)});
        QCOMPARE(text(a), "-machine q35\n"
                          "-boot strict=on\n"
                          "-drive file=x.iso,media=cdrom,readonly=on,if=none,id=cd0\n"
                          "-device ide-cd,drive=cd0,bootindex=1\n"
                          "-drive file=disk.qcow2,format=qcow2,discard=unmap,if=none,id=disk0\n"
                          "-device virtio-blk-pci,drive=disk0,bootindex=2\n"
                          "-netdev user,id=net0\n"
                          "-device virtio-net-pci,netdev=net0\n");
        QVERIFY(hasBootOrder(a));
        QCOMPARE(VmConfig::bootOrder(a),
                 QList<E>({entry(E::Cdrom, 0, true), entry(E::HardDisk, 1, true),
                           entry(E::Network, 0, false)}));
        /* read back and written again: the same */
        const QString ordered = text(a);
        setBootOrder(a, VmConfig::bootOrder(a));
        QCOMPARE(text(a), ordered);

        /* the network first, the disk off */
        setBootOrder(a, {entry(E::Network, 0, true), entry(E::Cdrom, 0, true),
                         entry(E::HardDisk, 1, false)});
        QVERIFY(text(a).contains("-device virtio-net-pci,netdev=net0,bootindex=1\n"));
        QVERIFY(text(a).contains("-device ide-cd,drive=cd0,bootindex=2\n"));
        QVERIFY(text(a).contains("-device virtio-blk-pci,drive=disk0\n"));
        QVERIFY(text(a).contains("-boot strict=on\n"));
        QCOMPARE(VmConfig::bootOrder(a),
                 QList<E>({entry(E::Network, 0, true), entry(E::Cdrom, 0, true),
                           entry(E::HardDisk, 1, false)}));

        /* all on: in that order, nothing left out, not strict */
        setBootOrder(a, {entry(E::HardDisk, 1, true), entry(E::Cdrom, 0, true),
                         entry(E::Network, 0, true)});
        QVERIFY(!text(a).contains("-boot"));
        QVERIFY(text(a).contains("-device virtio-blk-pci,drive=disk0,bootindex=1\n"));
        QVERIFY(text(a).contains("-device virtio-net-pci,netdev=net0,bootindex=3\n"));

        /* none on: the firmware's own again */
        setBootOrder(a, {entry(E::HardDisk, 1, false), entry(E::Cdrom, 0, false),
                         entry(E::Network, 0, false)});
        QVERIFY(!text(a).contains("bootindex"));
        QVERIFY(!text(a).contains("-boot"));
        QVERIFY(!hasBootOrder(a));

        /* -hda and -cdrom, and -boot order=, which OVMF does not follow */
        a = ArgsFile::parse("-hda win.qcow2\n-cdrom setup.iso\n-boot order=dc,menu=on\n");
        QVERIFY(hasBootOrder(a));
        QCOMPARE(VmConfig::bootOrder(a),
                 QList<E>({entry(E::Cdrom, 1, true), entry(E::HardDisk, 0, true)}));
        setBootOrder(a, {entry(E::HardDisk, 0, true), entry(E::Cdrom, 1, false)});
        QCOMPARE(text(a), "-drive file=win.qcow2,if=none,id=disk0\n"
                          "-device ide-hd,drive=disk0,bootindex=1\n"
                          "-cdrom setup.iso\n"
                          "-boot menu=on,strict=on\n");
        /* -boot d, the old way: its kinds only */
        a = ArgsFile::parse("-hda win.qcow2\n-cdrom setup.iso\n-boot d\n");
        QCOMPARE(VmConfig::bootOrder(a),
                 QList<E>({entry(E::Cdrom, 1, true), entry(E::HardDisk, 0, false)}));

        /* a -blockdev with its -device, and the bootindex of another device kept */
        a = ArgsFile::parse("-blockdev driver=file,filename=a.qcow2,node-name=f0\n"
                            "-blockdev driver=qcow2,file=f0,node-name=d0\n"
                            "-device virtio-blk-pci,drive=d0,bootindex=3\n"
                            "-drive file=old.img,if=scsi\n"
                            "-device usb-host,vendorid=0x1234,productid=0x5678,bootindex=1\n");
        QCOMPARE(VmConfig::bootOrder(a),
                 QList<E>({entry(E::HardDisk, 0, true), entry(E::HardDisk, 1, false, false)}));
        setBootOrder(a, VmConfig::bootOrder(a));
        QCOMPARE(text(a), "-boot strict=on\n"
                          "-blockdev driver=file,filename=a.qcow2,node-name=f0\n"
                          "-blockdev driver=qcow2,file=f0,node-name=d0\n"
                          "-device virtio-blk-pci,drive=d0,bootindex=2\n"
                          "-drive file=old.img,if=scsi\n"
                          "-device usb-host,vendorid=0x1234,productid=0x5678,bootindex=1\n");
    }
};

QTEST_APPLESS_MAIN(TestVmHardware)
#include "test_vmhardware.moc"
