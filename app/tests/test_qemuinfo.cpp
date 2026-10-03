// SPDX-License-Identifier: GPL-2.0-or-later
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>

#include "core/qemuinfo.h"

/* Excerpts of the real output of QEMU 11.1.50 and its qemu-options.hx */
static const char kHelp[] = R"EXCERPT(QEMU emulator version 11.1.50
Copyright (c) 2003-2026 Fabrice Bellard and the QEMU Project developers
usage: qemu-system-x86_64 [options] [disk_image]

'disk_image' is a raw hard disk image for IDE hard disk 0

Standard options:
-h or -help     display this help and exit
-version        display version information and exit
-machine [type=]name[,prop=value[,...]]
                selects emulated machine ('-machine help' for list)
                property accel=accel1[:accel2[:...]] selects accelerator
                boot-certs.0.path=/path/directory,boot-certs.1.path=/path/file provides paths to a directory and/or a certificate file
                secure-boot=on|off enable/disable secure boot (default=off)
-M              as -machine
-cpu cpu        select CPU ('-cpu help' for list)
-accel [accel=]accelerator[,prop=value[,...]]
                select accelerator (kvm, xen, hvf, nitro, nvmm, whpx, mshv or tcg; use 'help' for a list)
                igd-passthru=on|off (enable Xen integrated Intel graphics passthrough, default=off)
                notify-vmexit=run|internal-error|disable,notify-window=n (enable notify VM exit and set notify window, x86 only)
                thread=single|multi (enable multi-threaded TCG)
                device=path (KVM device path, default /dev/kvm)
-smp [[cpus=]n][,maxcpus=maxcpus][,drawers=drawers][,books=books][,sockets=sockets]
               [,dies=dies][,clusters=clusters][,modules=modules][,cores=cores]
               [,threads=threads]
                set the number of initial CPUs to 'n' [default=1]
                maxcpus= maximum number of total CPUs, including
                offline CPUs for hotplug, etc
                drawers= number of drawers on the machine board
                books= number of books in one drawer
                sockets= number of sockets in one book
                dies= number of dies in one socket
                clusters= number of clusters in one die
                modules= number of modules in one cluster
                cores= number of cores in one module
                threads= number of threads in one core
Note: Different machines may have different subsets of the CPU topology
      parameters supported, so the actual meaning of the supported parameters
      will vary accordingly. For example, for a machine type that supports a
      three-level CPU hierarchy of sockets/cores/threads, the parameters will
      sequentially mean as below:
                sockets means the number of sockets on the machine board
                cores means the number of cores in one socket
                threads means the number of threads in one core
      For a particular machine type board, an expected CPU topology hierarchy
      can be defined through the supported sub-option. Unsupported parameters
      can also be provided in addition to the sub-option, but their values
      must be set as 1 in the purpose of correct parsing.
-numa node[,mem=size][,cpus=firstcpu[-lastcpu]][,nodeid=node][,initiator=node]
-numa node[,memdev=id][,cpus=firstcpu[-lastcpu]][,nodeid=node][,initiator=node]
-numa dist,src=source,dst=destination,val=distance
-numa cpu,node-id=node[,socket-id=x][,core-id=y][,thread-id=z]
-numa hmat-lb,initiator=node,target=node,hierarchy=memory|first-level|second-level|third-level,data-type=access-latency|read-latency|write-latency[,latency=lat][,bandwidth=bw]
-numa hmat-cache,node-id=node,size=size,level=level[,associativity=none|direct|complex][,policy=none|write-back|write-through][,line=size]
-add-fd fd=fd,set=set[,opaque=opaque]
                Add 'fd' to fd 'set'

USB convenience options:
-usb            enable on-board USB host controller (if not enabled by default)
-usbdevice name add the host or guest USB device 'name'

Display options:
-display sdl[,gl=on|core|es|off][,grab-mod=<mod>][,show-cursor=on|off]
            [,window-close=on|off]
-display gtk[,clipboard=on|off][,full-screen=on|off][,gl=on|off]
            [,grab-on-hover=on|off][,show-tabs=on|off][,show-cursor=on|off]
            [,window-close=on|off][,show-menubar=on|off][,zoom-to-fit=on|off]
-display egl-headless[,rendernode=<file>]
-display none
                select display backend type
                The default display is equivalent to
                "-display gtk"
-nographic      disable graphical output and redirect serial I/Os to console
-vga [std|cirrus|vmware|qxl|xenfb|tcx|cg3|virtio|none]
                select video card type
-full-screen    start in full screen


Network options:
-netdev passt,id=str[,path=file][,quiet=on|off][,vhost-user=on|off]
[,mtu=mtu][,address=addr][,netmask=mask][,mac=addr][,gateway=addr]
          [,interface=name][,outbound=address][,outbound-if4=name]
          [,outbound-if6=name][,dns=addr][,search=list][,fqdn=name]
          [,dhcp-dns=on|off][,dhcp-search=on|off][,map-host-loopback=addr]
          [,map-guest-addr=addr][,dns-forward=addr][,dns-host=addr]
          [,tcp=on|off][,udp=on|off][,icmp=on|off][,dhcp=on|off]
          [,ndp=on|off][,dhcpv6=on|off][,ra=on|off][,freebind=on|off]
          [,ipv4=on|off][,ipv6=on|off][,tcp-ports=spec][,udp-ports=spec]
          [,param=list]
                configure a passt network backend with ID 'str'
                if 'path' is not provided 'passt' will be started according to PATH
                by default, informational message of passt are not displayed (quiet=on)
                to display this message, use 'quiet=off'
                by default, passt will be started in socket-based mode, to enable vhost-mode,
                use 'vhost-user=on'

Boot Image or Kernel specific:
-bios file      set the filename for the BIOS
-pflash file    use 'file' as a parallel flash image
-kernel bzImage use 'bzImage' as kernel image
-shim shim.efi use 'shim.efi' to boot the kernel
-append cmdline use 'cmdline' as kernel command line
-initrd file    use 'file' as initial ram disk

During emulation, the following keys are useful:
ctrl-alt-f      toggle full screen
ctrl-alt-n      switch to virtual console 'n'
ctrl-alt-g      toggle mouse and keyboard grab

When using -nographic, press 'ctrl-a h' to get some help.

See <https://qemu.org/contribute/report-a-bug> for how to report bugs.
More information on the QEMU project at <https://qemu.org>.
)EXCERPT";

static const char kHx[] = R"EXCERPT(DEFHEADING(Standard options:)

DEF("help", 0, QEMU_OPTION_h,
    "-h or -help     display this help and exit\n", QEMU_ARCH_ALL)
SRST
``-h``
    Display help and exit
ERST

DEF("version", 0, QEMU_OPTION_version,
    "-version        display version information and exit\n", QEMU_ARCH_ALL)
SRST
``-version``
    Display version information and exit
ERST
DEF("M", HAS_ARG, QEMU_OPTION_M,
    "-M              as -machine\n", QEMU_ARCH_ALL)
SRST
``-M``
    as -machine.
ERST

DEF("cpu", HAS_ARG, QEMU_OPTION_cpu,
    "-cpu cpu        select CPU ('-cpu help' for list)\n", QEMU_ARCH_ALL)
SRST
``-cpu model``
    Select CPU model (``-cpu help`` for list and additional feature
    selection)
ERST
DEF("fda", HAS_ARG, QEMU_OPTION_fda,
    "-fda/-fdb file  use 'file' as floppy disk 0/1 image\n", QEMU_ARCH_ALL)
DEF("fdb", HAS_ARG, QEMU_OPTION_fdb, "", QEMU_ARCH_ALL)
SRST
``-fda file``
  \
``-fdb file``
    Use file as floppy disk 0/1 image (see the :ref:`disk images` chapter in
    the System Emulation Users Guide).
ERST

DEF("hda", HAS_ARG, QEMU_OPTION_hda,
    "-hda/-hdb file  use 'file' as hard disk 0/1 image\n", QEMU_ARCH_ALL)
DEF("hdb", HAS_ARG, QEMU_OPTION_hdb, "", QEMU_ARCH_ALL)
DEF("hdc", HAS_ARG, QEMU_OPTION_hdc,
    "-hdc/-hdd file  use 'file' as hard disk 2/3 image\n", QEMU_ARCH_ALL)
DEF("hdd", HAS_ARG, QEMU_OPTION_hdd, "", QEMU_ARCH_ALL)
SRST
``-hda file``
  \
``-hdb file``
  \
``-hdc file``
  \
``-hdd file``
    Use file as hard disk 0, 1, 2 or 3 image on the default bus of the
    emulated machine (this is for example the IDE bus on most x86 machines,
    but it can also be SCSI, virtio or something else on other target
    architectures). See also the :ref:`disk images` chapter in the System
    Emulation Users Guide.
ERST

DEF("machine", HAS_ARG, QEMU_OPTION_machine, \
    "-machine [type=]name[,prop=value[,...]]\n"
    "                selects emulated machine ('-machine help' for list)\n"
#ifdef CONFIG_POSIX
    "                aux-ram-share=on|off allocate (default: off)\n"
#endif
    "                secure-boot=on|off enable/disable secure boot (default=off)\n",
    QEMU_ARCH_ALL)
SRST
``-machine [type=]name[,prop=value[,...]]``
    Select the emulated machine by name.
ERST

DEF("not-in-this-binary", 0, QEMU_OPTION_x, "", QEMU_ARCH_ARM)
SRST
``-not-in-this-binary``
    Nothing.
ERST
)EXCERPT";

class TestQemuInfo : public QObject
{
    Q_OBJECT

    static const QemuOptionDoc *find(const QList<QemuOptionDoc> &options, const QString &name)
    {
        const QemuOptionDoc *found = nullptr;
        for (const QemuOptionDoc &o : options) {
            if (o.name == name) {
                if (found) {
                    return nullptr;     /* listed twice */
                }
                found = &o;
            }
        }
        return found;
    }

private slots:
    void parseHelp()
    {
        const QList<QemuOptionDoc> options = QemuInfo::parseHelp(kHelp);
        const QemuOptionDoc *o;

        QVERIFY((o = find(options, "h")));
        QCOMPARE(o->synopsis, "-h or -help");
        QCOMPARE(o->help, "display this help and exit");
        QCOMPARE(o->section, "Standard options");
        QVERIFY(!o->takesValue);
        QVERIFY((o = find(options, "help")));
        QVERIFY(!o->takesValue);

        QVERIFY((o = find(options, "machine")));
        QVERIFY(o->takesValue);
        QVERIFY(o->help.startsWith("selects emulated machine ('-machine help' for list)\n"));
        QVERIFY(o->help.endsWith("secure-boot=on|off enable/disable secure boot (default=off)"));
        QVERIFY((o = find(options, "M")));
        QCOMPARE(o->help, "as -machine");
        QVERIFY(o->takesValue);
        QVERIFY((o = find(options, "cpu")));
        QCOMPARE(o->synopsis, "-cpu cpu");

        /* a synopsis over three lines, then a note at column 0 */
        QVERIFY((o = find(options, "smp")));
        QCOMPARE(o->synopsis.count('\n'), 2);
        QVERIFY(o->synopsis.endsWith("\n               [,threads=threads]"));
        QVERIFY(o->help.startsWith("set the number of initial CPUs to 'n' [default=1]\n"));
        QVERIFY(o->help.contains("\nNote: Different machines may have different subsets"));
        QVERIFY(o->help.contains("\nparameters supported, so the actual meaning"));

        /* six forms of one option */
        QVERIFY((o = find(options, "numa")));
        QCOMPARE(o->synopsis.count("-numa "), 6);
        QCOMPARE(o->help, "");
        QVERIFY((o = find(options, "add-fd")));
        QCOMPARE(o->help, "Add 'fd' to fd 'set'");

        QVERIFY((o = find(options, "usb")));
        QVERIFY(!o->takesValue);
        QCOMPARE(o->section, "USB convenience options");
        QVERIFY((o = find(options, "usbdevice")));
        QCOMPARE(o->synopsis, "-usbdevice name");
        QCOMPARE(o->help, "add the host or guest USB device 'name'");

        QVERIFY((o = find(options, "display")));
        QCOMPARE(o->section, "Display options");
        QCOMPARE(o->synopsis.count("-display "), 4);
        QVERIFY(o->synopsis.contains("\n            [,window-close=on|off]\n-display gtk["));
        QVERIFY(o->help.startsWith("select display backend type\n"));

        /* a synopsis continued at column 0 */
        QVERIFY((o = find(options, "netdev")));
        QCOMPARE(o->section, "Network options");
        QVERIFY(o->synopsis.contains("\n[,mtu=mtu]"));
        QVERIFY(o->help.startsWith("configure a passt network backend with ID 'str'\n"));

        QVERIFY((o = find(options, "shim")));
        QCOMPARE(o->synopsis, "-shim shim.efi");
        QCOMPARE(o->help, "use 'shim.efi' to boot the kernel");
        QVERIFY((o = find(options, "kernel")));
        QCOMPARE(o->help, "use 'bzImage' as kernel image");
        QCOMPARE(o->section, "Boot Image or Kernel specific");

        /* nothing from the key bindings at the end */
        QCOMPARE(options.last().name, "initrd");
    }

    void mergeOptionsHx()
    {
        QList<QemuOptionDoc> options = QemuInfo::parseHelp(kHelp);
        options += QemuInfo::parseHelp("-fda/-fdb file  use 'file' as floppy disk 0/1 image\n"
                                       "-hda/-hdb file  use 'file' as hard disk 0/1 image\n"
                                       "-hdc/-hdd file  use 'file' as hard disk 2/3 image\n");
        QemuInfo::mergeOptionsHx(options, kHx);

        QVERIFY(find(options, "help")->details.contains("Display help and exit"));
        QCOMPARE(find(options, "h")->details, find(options, "help")->details);
        QVERIFY(!find(options, "version")->takesValue);
        QVERIFY(find(options, "M")->details.contains("as -machine."));
        QVERIFY(find(options, "cpu")->details.startsWith("``-cpu model``\n"));
        QVERIFY(find(options, "machine")->details.contains("Select the emulated machine"));
        QVERIFY(find(options, "machine")->takesValue);
        for (const char *name : {"hda", "hdb", "hdc", "hdd"}) {
            QVERIFY(find(options, name)->details.contains("hard disk 0, 1, 2 or 3"));
        }
        QVERIFY(find(options, "fdb")->details.contains("floppy disk 0/1"));
        QVERIFY(!find(options, "not-in-this-binary"));
    }

    void parseDeviceHelp()
    {
        const QList<QemuDeviceDoc> devices = QemuInfo::parseDeviceHelp(
            "Controller/Bridge/Hub devices:\n"
            "name \"cxl-downstream\", bus PCI, desc \"CXL Switch Downstream Port\"\n"
            "name \"i82801b11-bridge\", bus PCI\n"
            "\n"
            "Storage devices:\n"
            "name \"ich9-ahci\", bus PCI, alias \"ahci\"\n"
            "name \"x-test\", bus System, desc \"a, b\", no-user\n");
        QemuInfo info;

        QCOMPARE(devices.size(), 4);
        QCOMPARE(devices[0].name, "cxl-downstream");
        QCOMPARE(devices[0].bus, "PCI");
        QCOMPARE(devices[0].desc, "CXL Switch Downstream Port");
        QCOMPARE(devices[0].category, "Controller/Bridge/Hub devices");
        QCOMPARE(devices[1].desc, "");
        QCOMPARE(devices[2].aliases, QStringList{"ahci"});
        QCOMPARE(devices[2].category, "Storage devices");
        QCOMPARE(devices[3].desc, "a, b");
        QVERIFY(!devices[3].userCreatable);
        QVERIFY(devices[2].userCreatable);

        info.devices = devices;
        QCOMPARE(info.device("ahci")->name, "ich9-ahci");
        QVERIFY(!info.device("nope"));
    }

    void parsePropertyHelp()
    {
        const QList<QemuPropertyDoc> props = QemuInfo::parsePropertyHelp(
            "virtio-net-pci options:\n"
            "  acpi-index=<uint32>    -  (default: 0)\n"
            "  addr=<str>             - Slot and optional function number, example: 06.0 or 06 (default: -1)\n"
            "  aer=<bool>             - on/off (default: off)\n"
            "  bootindex=<int32>\n"
            "  iommufd=<link<iommufd>> - Set host IOMMUFD backend device\n");

        QCOMPARE(props.size(), 5);
        QCOMPARE(props[0].name, "acpi-index");
        QCOMPARE(props[0].type, "uint32");
        QCOMPARE(props[0].desc, "");
        QCOMPARE(props[0].defaultValue, "0");
        QCOMPARE(props[1].desc, "Slot and optional function number, example: 06.0 or 06");
        QCOMPARE(props[1].defaultValue, "-1");
        QCOMPARE(props[2].type, "bool");
        QCOMPARE(props[2].desc, "");
        QCOMPARE(props[2].defaultValue, "off");
        QCOMPARE(props[3].name, "bootindex");
        QCOMPARE(props[3].defaultValue, "");
        QCOMPARE(props[4].type, "link<iommufd>");
        QCOMPARE(props[4].desc, "Set host IOMMUFD backend device");
    }

    void parseListHelp()
    {
        QList<QemuNamedDoc> list = QemuInfo::parseListHelp(
            "Supported machines are:\n"
            "microvm              microvm (i386)\n"
            "pc                   Standard PC (i440FX + PIIX, 1996) (alias of pc-i440fx-11.2)\n"
            "pc-i440fx-11.2       Standard PC (i440FX + PIIX, 1996) (default)\n");
        QCOMPARE(list.size(), 3);
        QCOMPARE(list[1].name, "pc");
        QCOMPARE(list[1].desc, "Standard PC (i440FX + PIIX, 1996) (alias of pc-i440fx-11.2)");

        list = QemuInfo::parseListHelp(
            "Available CPUs:\n"
            "  486                   (alias configured by machine type)\n"
            "  486-v1                \n"
            "  host                  processor with all supported host features \n"
            "\n"
            "Recognized CPUID flags:\n"
            "  3dnow 3dnowext 3dnowprefetch abm ace2 ace2-en acpi adx aes amd-no-ssb\n");
        QCOMPARE(list.size(), 3);
        QCOMPARE(list[1].name, "486-v1");
        QCOMPARE(list[1].desc, "");
        QCOMPARE(list[2].desc, "processor with all supported host features");

        list = QemuInfo::parseListHelp(
            "Available display backend types:\n"
            "none\n"
            "sdl\n"
            "\n"
            "Some display backends support suboptions, which can be set with\n");
        QCOMPARE(list.size(), 2);
        QCOMPARE(list[1].name, "sdl");
    }

    void rstToHtml()
    {
        const QString html = QemuInfo::rstToHtml(R"RST(``-fda file``
  \
``-fdb file``
    Use file as floppy disk 0/1 image (see the :ref:`disk images` chapter in
    the System Emulation Users Guide), and :ref:`disk_005fimages`.

    Example::

        qemu-system-x86_64 -fda a.img

        -fdb b.img

    - first *item* with ``code<>``
      continued
    - second **bold**

    .. parsed-literal::

        |qemu_system| -machine q35 \\
            -m 1G

    .. warning::
        Deprecated \"soon\", on|off|auto.

    ==========  =====
    Col         Other
    ==========  =====
    a           b
    ==========  =====

    See `the docs <https://qemu.org/docs>`_ and :ref:`the guide <disk_005fimages>`.
)RST");

        QVERIFY2(html.contains("<p style=\"margin-left:0px\"><b><code>-fda file</code></b></p>"
                               "<p style=\"margin-left:0px\"><b><code>-fdb file</code></b></p>"
                               "<p style=\"margin-left:20px\">Use file as floppy disk 0/1 image "
                               "(see the disk images chapter in the System Emulation Users "
                               "Guide), and disk_images.</p>"), qPrintable(html));
        QVERIFY2(html.contains("<p style=\"margin-left:20px\">Example:</p>"
                               "<pre style=\"margin-left:40px\">qemu-system-x86_64 -fda a.img\n"
                               "\n-fdb b.img</pre>"), qPrintable(html));
        QVERIFY2(html.contains("<p style=\"margin-left:20px\">• first <i>item</i> with "
                               "<code>code&lt;&gt;</code> continued</p>"
                               "<p style=\"margin-left:20px\">• second <b>bold</b></p>"),
                 qPrintable(html));
        QVERIFY2(html.contains("<pre style=\"margin-left:40px\">qemu-system-x86_64 -machine q35 \\\n"
                               "    -m 1G</pre>"), qPrintable(html));
        QVERIFY2(html.contains("<p style=\"margin-left:40px\"><b>Warning:</b> Deprecated "
                               "&quot;soon&quot;, on|off|auto.</p>"), qPrintable(html));
        QVERIFY2(html.contains("<pre style=\"margin-left:20px\">==========  =====\n"
                               "Col         Other\n==========  =====\na           b\n"
                               "==========  =====</pre>"), qPrintable(html));
        QVERIFY2(html.contains("See <a href=\"https://qemu.org/docs\">the docs</a> and "
                               "the guide."), qPrintable(html));
    }

    /* Against a real binary: $QGM_TEST_QEMU, else qemu-system-x86_64 in PATH */
    void loader()
    {
        QString qemu = qEnvironmentVariable("QGM_TEST_QEMU");
        if (qemu.isEmpty()) {
            qemu = QStandardPaths::findExecutable("qemu-system-x86_64");
        }
        if (qemu.isEmpty()) {
            QSKIP("no QEMU binary, set QGM_TEST_QEMU");
        }
        QStandardPaths::setTestModeEnabled(true);

        QemuInfoLoader loader(qemu);
        QSignalSpy loaded(&loader, &QemuInfoLoader::loaded);
        loader.load();
        if (loaded.isEmpty()) {
            QVERIFY(loaded.wait(60000));
        }

        const QemuInfo &info = loader.info();
        QVERIFY(!info.version.isEmpty());
        QVERIFY(info.options.size() > 100);
        QVERIFY(find(info.options, "machine") && find(info.options, "machine")->takesValue);
        QVERIFY(find(info.options, "numa"));
        QVERIFY(find(info.options, "smp")->synopsis.count('\n') >= 1);
        QVERIFY(info.device("virtio-net-pci"));
        QVERIFY(info.device("virtio-net-pci")->category.startsWith("Network"));
        QVERIFY(!info.machines.isEmpty());
        QVERIFY(!info.cpus.isEmpty());
        QVERIFY(!info.objects.isEmpty());
        QVERIFY(!info.netdevs.isEmpty());
        QVERIFY(!info.chardevs.isEmpty());
        QVERIFY(!info.accels.isEmpty());

        if (!QemuInfoLoader::findOptionsHx(qemu).isEmpty()) {
            QStringList undocumented;
            for (const QemuOptionDoc &o : info.options) {
                if (o.details.isEmpty()) {
                    undocumented << o.name;
                }
            }
            qInfo("without qemu-options.hx details: %s", qPrintable(undocumented.join(' ')));
            QVERIFY(undocumented.size() < info.options.size() / 10);
        }

        /* all of them at once, for the search */
        QVERIFY(info.properties.size() > 100);
        QVERIFY(info.properties.contains("virtio-net-pci"));

        QSignalSpy props(&loader, &QemuInfoLoader::propertiesLoaded);
        loader.loadProperties("virtio-net-pci");
        if (props.isEmpty()) {
            QVERIFY(props.wait(30000));
        }
        bool hasMac = false;
        for (const QemuPropertyDoc &p : loader.info().properties.value("virtio-net-pci")) {
            hasMac |= p.name == "mac";
        }
        QVERIFY(hasMac);

        /* the second time from the cache, at once */
        QemuInfoLoader cached(qemu);
        QSignalSpy cachedLoaded(&cached, &QemuInfoLoader::loaded);
        cached.load();
        QCOMPARE(cachedLoaded.size(), 1);
        QCOMPARE(cached.info().options.size(), info.options.size());
        QCOMPARE(cached.info().properties.value("virtio-net-pci").size(),
                 loader.info().properties.value("virtio-net-pci").size());
        QCOMPARE(cached.info().options.first().details, info.options.first().details);
    }
};

QTEST_GUILESS_MAIN(TestQemuInfo)
#include "test_qemuinfo.moc"
