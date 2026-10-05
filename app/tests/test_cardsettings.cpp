// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include "core/cardsettings.h"
#include "core/paths.h"
#include "core/vmconfig.h"
#include "core/vmhardware.h"
#include "core/vmtemplate.h"

using namespace CardSettings;

/* The card of vitrine's QEMU, all of it, as the template writes it */
static const char kFullCard[] = "virtio-vga-gl,hostmem=4G,blob=on,drm_native_context=on,"
                                "x-host-vblank=on,x-vblank-lead=3000,x-vblank-lead-auto=on";

/* A Linux VM on a PC with @card, KVM and shared memory */
static ArgsFile vm(const QString &card, const QString &extra = {})
{
    return ArgsFile::parse("#guest linux\n"
                           "-machine q35,memory-backend=mem\n"
                           "-accel kvm\n"
                           "-object memory-backend-memfd,id=mem,size=4G,share=on\n"
                           "-device " + card + "\n"
                           "-display dbus,p2p=yes,gl=on\n" + extra);
}

/* The -device line of the card */
static QString cardOf(const ArgsFile &args)
{
    const int line = args.indexOfDevice([](const QString &driver) {
        return VmConfig::isAccelerated(driver);
    });
    return line < 0 ? QString() : args.lines[line].value;
}

/* What vitrine's QEMU has, all of it but Venus */
static const Offers kAll;

/* A QEMU of the VM's own with all of it, Venus too */
static Offers withVenus()
{
    Offers o;
    o.card = QStringList{"blob", "hostmem", "venus", "drm_native_context", "x-host-vblank",
                         "x-vblank-lead", "x-vblank-lead-auto"};
    o.accel = QStringList{"honor-guest-pat"};
    return o;
}

class TestCardSettings : public QObject
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

    /*
     * A QEMU that answers for @card and kvm-accel with @properties and
     * @accel, and has the displays @displays
     */
    QString fakeQemu(const QString &name, const QString &card, const QStringList &properties,
                     const QStringList &accel,
                     const QStringList &displays = {"none", "sdl", "egl-headless", "dbus"})
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
        body += "  ;;\n'-display help')\n  echo 'Available display backend types:'\n";
        for (const QString &d : displays) {
            body += "  echo '" + d.toUtf8() + "'\n";
        }
        body += "  echo\n  echo 'Some display backends support suboptions'\n";
        body += "  ;;\nesac";
        return script(path, body) ? path : QString();
    }

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

    /* The template's card has them all on */
    void templateCard()
    {
        QString card = VmTemplate::kCard;
        for (const auto &[key, value] : VmTemplate::cardProperties()) {
            card += QString(",%1=%2").arg(key, value);
        }
        QCOMPARE(card, kFullCard);
        const ArgsFile args = vm(kFullCard);
        QVERIFY(hasCard(args));
        QVERIFY(isOn(args, Feature::NativeContext));
        QVERIFY(!isOn(args, Feature::Venus));
        QVERIFY(isOn(args, Feature::FrameTiming));
    }

    /* Each turned on from a bare card, in the template's order, and off again */
    void onAndOff()
    {
        ArgsFile args = vm("virtio-vga-gl");

        for (Feature f : {Feature::NativeContext, Feature::Venus, Feature::FrameTiming}) {
            QVERIFY(!isOn(args, f));
        }
        set(args, Feature::NativeContext, true, kAll);
        QCOMPARE(cardOf(args), "virtio-vga-gl,hostmem=4G,blob=on,drm_native_context=on");
        /* KVM honors the guest's caching of the GPU's memory */
        QCOMPARE(VmConfig::accelProperty(args, "honor-guest-pat"), "auto");
        set(args, Feature::FrameTiming, true, kAll);
        QCOMPARE(cardOf(args), kFullCard);
        QVERIFY(isOn(args, Feature::NativeContext));
        QVERIFY(isOn(args, Feature::FrameTiming));

        /* Venus: not in vitrine's QEMU, whose virglrenderer has none */
        QVERIFY(!isOffered(kAll, Feature::Venus));
        set(args, Feature::Venus, true, kAll);
        QCOMPARE(cardOf(args), kFullCard);
        set(args, Feature::Venus, true, withVenus());
        QCOMPARE(cardOf(args), QString(kFullCard) + ",venus=on");
        QVERIFY(isOn(args, Feature::Venus));
        set(args, Feature::Venus, false, withVenus());
        QCOMPARE(cardOf(args), kFullCard);

        /* off: blob and hostmem stay, Venus and 2D resources use them */
        set(args, Feature::NativeContext, false, kAll);
        QCOMPARE(cardOf(args), "virtio-vga-gl,hostmem=4G,blob=on,x-host-vblank=on,"
                               "x-vblank-lead=3000,x-vblank-lead-auto=on");
        QVERIFY(!isOn(args, Feature::NativeContext));
        /* QEMU's default is on: off says so, the lead goes */
        set(args, Feature::FrameTiming, false, kAll);
        QCOMPARE(cardOf(args), "virtio-vga-gl,hostmem=4G,blob=on,x-host-vblank=off");
        QVERIFY(!isOn(args, Feature::FrameTiming));
        /* and on again: where the template has them */
        set(args, Feature::FrameTiming, true, kAll);
        set(args, Feature::NativeContext, true, kAll);
        QCOMPARE(cardOf(args), kFullCard);
        /* nothing else changed */
        QCOMPARE(args.toText(), vm(kFullCard).toText().replace("-accel kvm\n",
                                                                "-accel kvm,honor-guest-pat=auto\n"));
    }

    /* What the user set by hand stays, where it leaves the feature on */
    void handSettings()
    {
        /* a hostmem of its own, blob turned off by hand: native context needs it */
        ArgsFile args = vm("virtio-gpu-gl-pci,blob=off,hostmem=8G,id=gpu");
        set(args, Feature::NativeContext, true, kAll);
        QCOMPARE(cardOf(args), "virtio-gpu-gl-pci,blob=on,hostmem=8G,drm_native_context=on,id=gpu");

        /* a bare key, and its no- form */
        args = vm("virtio-vga-gl,nodrm_native_context,blob");
        QVERIFY(!isOn(args, Feature::NativeContext));
        set(args, Feature::NativeContext, true, kAll);
        QCOMPARE(cardOf(args), "virtio-vga-gl,drm_native_context=on,hostmem=4G,blob");
        args = vm("virtio-vga-gl,venus");
        QVERIFY(isOn(args, Feature::Venus));
        set(args, Feature::Venus, false, withVenus());
        QCOMPARE(cardOf(args), "virtio-vga-gl");

        /* the host's vblank off by hand: on, the lead added */
        args = vm("virtio-vga-gl,x-host-vblank=off,x-vblank-lead=2000");
        QVERIFY(!isOn(args, Feature::FrameTiming));
        set(args, Feature::FrameTiming, true, kAll);
        /* a lead of its own stays fixed: no auto, which would move it */
        QCOMPARE(cardOf(args), "virtio-vga-gl,x-host-vblank=on,x-vblank-lead=2000");

        /* with -global: the card's line wins over it */
        args = vm("virtio-vga-gl", "-global virtio-gpu-base.x-host-vblank=off\n");
        QVERIFY(!isOn(args, Feature::FrameTiming));
        set(args, Feature::FrameTiming, true, kAll);
        QCOMPARE(cardOf(args), "virtio-vga-gl,x-host-vblank=on,x-vblank-lead=3000,"
                               "x-vblank-lead-auto=on");
        QVERIFY(isOn(args, Feature::FrameTiming));
        args = vm("virtio-vga-gl", "-global driver=virtio-vga-gl,property=x-host-vblank,"
                                   "value=on\n");
        QVERIFY(isOn(args, Feature::FrameTiming));

        /* the accelerator's own word on the guest's caching stays */
        args = ArgsFile::parse("-accel kvm,honor-guest-pat=off\n-m 4G\n-device virtio-vga-gl\n");
        set(args, Feature::NativeContext, true, kAll);
        QCOMPARE(VmConfig::accelProperty(args, "honor-guest-pat"), "off");
        /* and TCG has none */
        args = ArgsFile::parse("-m 4G\n-device virtio-vga-gl\n");
        set(args, Feature::NativeContext, true, kAll);
        QVERIFY(args.indexOf("accel") < 0);
    }

    /* Native context's resources in guest memory need it in a shared memfd */
    void sharedMemory()
    {
        ArgsFile args = ArgsFile::parse("-machine q35\n-accel kvm\n-m 4G\n-device virtio-vga-gl\n");
        set(args, Feature::NativeContext, true, kAll);
        QVERIFY(VmConfig::hasSharedMemory(args));
        QCOMPARE(VmConfig::memoryMiB(args), 4096);

        /* huge pages, or NUMA nodes, stay as written */
        args = ArgsFile::parse("-machine q35,memory-backend=mem\n-accel kvm\n"
                               "-object memory-backend-file,id=mem,size=4G,"
                               "mem-path=/dev/hugepages\n-device virtio-vga-gl\n");
        const QString before = args.toText();
        set(args, Feature::NativeContext, true, kAll);
        QVERIFY(args.toText().contains("mem-path=/dev/hugepages\n"));
        QVERIFY(!VmConfig::hasSharedMemory(args));
        QVERIFY(before != args.toText());

        /* Venus and the frame timing leave memory alone */
        args = ArgsFile::parse("-machine q35\n-accel kvm\n-m 4G\n-device virtio-vga-gl\n");
        set(args, Feature::Venus, true, withVenus());
        set(args, Feature::FrameTiming, true, kAll);
        QVERIFY(!VmConfig::hasSharedMemory(args));
        QVERIFY(isOn(args, Feature::Venus));
    }

    /* Only for a VM with one card with OpenGL */
    void cards()
    {
        for (const char *card : {"virtio-vga-gl", "virtio-gpu-gl-pci", "virtio-gpu-gl"}) {
            QVERIFY2(hasCard(vm(card)), card);
        }
        for (const char *args : {"-device virtio-vga\n", "-device VGA\n", "-m 1G\n",
                                 "-device virtio-vga-gl\n-device virtio-gpu-gl-pci\n",
                                 "-device virtio-vga-gl\n-nographic\n"}) {
            ArgsFile a = ArgsFile::parse(args);
            QVERIFY2(!hasCard(a), args);
            QVERIFY(!isOn(a, Feature::NativeContext));
            set(a, Feature::NativeContext, true, kAll);
            QCOMPARE(a.toText(), QString(args));
        }
    }

    /* Only what the VM's QEMU has: it refuses properties it does not know */
    void lacking()
    {
        Offers fedora;
        fedora.card = QStringList{"blob", "hostmem", "venus"};
        fedora.accel = QStringList{"kernel-irqchip"};
        ArgsFile args = vm("virtio-vga-gl");

        QVERIFY(!isOffered(fedora, Feature::NativeContext));
        QVERIFY(isOffered(fedora, Feature::Venus));
        QVERIFY(!isOffered(fedora, Feature::FrameTiming));
        for (Feature f : {Feature::NativeContext, Feature::FrameTiming}) {
            set(args, f, true, fedora);
            set(args, f, false, fedora);
        }
        QCOMPARE(args.toText(), vm("virtio-vga-gl").toText());
        set(args, Feature::Venus, true, fedora);
        QCOMPARE(cardOf(args), "virtio-vga-gl,hostmem=4G,blob=on,venus=on");

        /* native context in a QEMU without honor-guest-pat, or without the lead's auto mode */
        Offers older;
        older.card = QStringList{"blob", "hostmem", "drm_native_context", "x-host-vblank",
                                 "x-vblank-lead"};
        older.accel = QStringList{"kernel-irqchip"};
        args = vm("virtio-vga-gl");
        set(args, Feature::NativeContext, true, older);
        set(args, Feature::FrameTiming, true, older);
        QCOMPARE(cardOf(args), "virtio-vga-gl,hostmem=4G,blob=on,drm_native_context=on,"
                               "x-host-vblank=on,x-vblank-lead=3000");
        QVERIFY(args.toText().contains("-accel kvm\n"));
    }

    /* What a QEMU offers: vitrine's all, another as it says, once */
    void offersOfQemus()
    {
        const auto restore = qScopeGuard([]() { Paths::setQemuBinary({}); });
        const QStringList fork = {"blob", "hostmem", "venus", "drm_native_context",
                                  "x-host-vblank", "x-vblank-lead", "x-vblank-lead-auto"};
        const QString research = fakeQemu("research", "virtio-vga-gl", fork, {"honor-guest-pat"});
        const QString fedora =
            fakeQemu("fedora", "virtio-vga-gl", {"blob", "hostmem", "venus"}, {"kernel-irqchip"},
                     {"none", "gtk", "sdl", "egl-headless", "curses"});
        const QString broken = m_tmp.filePath("broken/" + Paths::qemuSystemName());
        QVERIFY(!research.isEmpty() && !fedora.isEmpty());
        QVERIFY(script(broken, "exit 1"));
        const ArgsFile plain = vm("virtio-vga-gl");
        const auto with = [&plain](const QString &qemu) {
            ArgsFile a = plain;
            VmConfig::setQemuBinary(a, qemu);
            return a;
        };

        /* vitrine's, not built: all of it */
        QVERIFY(Paths::stackQemu().isEmpty());
        std::optional<Offers> o = offers(plain);
        QVERIFY(o && !o->card && !o->accel && !o->displays);

        /* the #qemu line's, for the VM's card or another */
        o = offers(with(research));
        QVERIFY(o);
        QCOMPARE(*o->card, fork);
        QCOMPARE(*o->accel, QStringList{"honor-guest-pat"});
        QCOMPARE(*o->displays, QStringList({"none", "sdl", "egl-headless", "dbus"}));
        o = offers(with(fedora));
        QVERIFY(o && !isOffered(*o, Feature::NativeContext) && isOffered(*o, Feature::Venus));
        QVERIFY(o->displays->contains("gtk") && !o->displays->contains("dbus"));
        o = offers(with(research), "virtio-gpu-gl-pci");
        QVERIFY(o && o->card->isEmpty() && !isOffered(*o, Feature::Venus));
        /* a VM without a 3D card: its displays still */
        o = offers(ArgsFile::parse("#qemu " + research + "\n-device virtio-vga\n"));
        QVERIFY(o && o->card->isEmpty() && o->displays->contains("dbus"));
        /* one that does not answer, or is not there: nothing */
        QVERIFY(!offers(with(broken)));
        QVERIFY(!offers(with(m_tmp.filePath("nowhere/qemu"))));

        /* the preferences', the #qemu line first */
        Paths::setQemuBinary(fedora);
        QVERIFY(!isOffered(*offers(plain), Feature::FrameTiming));
        QVERIFY(isOffered(*offers(with(research)), Feature::FrameTiming));

        /* vitrine's, built: the displays that build has */
        Paths::setQemuBinary({});
        const QString stack = Paths::stackDir();
        const auto unlink = qScopeGuard([stack]() { QFile::remove(stack + "/current"); });
        QVERIFY(stack.startsWith(m_tmp.path()));
        QVERIFY(QDir().mkpath(stack + "/0123456789abcdef/bin"));
        QVERIFY(QFile::copy(research, stack + "/0123456789abcdef/bin/" + Paths::qemuSystemName()));
        QVERIFY(QFile::setPermissions(stack + "/0123456789abcdef/bin/" + Paths::qemuSystemName(),
                                      QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        QVERIFY(QFile::link("0123456789abcdef", stack + "/current"));
        QVERIFY(!Paths::stackQemu().isEmpty());
        o = offers(plain);
        QVERIFY(o && !o->card && !o->accel);
        QCOMPARE(*o->displays, QStringList({"none", "sdl", "egl-headless", "dbus"}));
        QVERIFY(!isOffered(*o, Feature::Venus));
        /* chosen by its path: what it says, but its venus property, which is no Venus */
        Paths::setQemuBinary(Paths::stackQemu());
        o = offers(plain);
        QVERIFY(o && o->card && o->card->contains("drm_native_context"));
        QVERIFY(!isOffered(*o, Feature::Venus));
        /* not answering: taken for vitrine's */
        QVERIFY(QFile::remove(stack + "/0123456789abcdef/bin/" + Paths::qemuSystemName()));
        QVERIFY(script(stack + "/0123456789abcdef/bin/" + Paths::qemuSystemName(), "exit 1"));
        Paths::setQemuBinary(Paths::stackQemu());
        o = offers(plain);
        QVERIFY(o && !o->card && !o->displays);
    }

    /* A QEMU is asked once until it changes, one that fails a minute later */
    void askedOnce()
    {
        const auto restore = qScopeGuard([]() { Paths::setQemuBinary({}); });
        const QString count = m_tmp.filePath("counted/count");
        const QString failing = m_tmp.filePath("counted/" + Paths::qemuSystemName());
        const auto calls = [&count]() {
            QFile f(count);
            return f.open(QIODevice::ReadOnly) ? f.readAll().count('\n') : 0;
        };

        QVERIFY(script(failing, "echo >> '" + count.toUtf8() + "'\nexit 1"));
        Paths::setQemuBinary(failing);
        QVERIFY(!offers(vm("virtio-vga-gl")));
        QCOMPARE(calls(), 1);
        QVERIFY(!offers(vm("virtio-vga-gl", "-m 2G\n")));
        QCOMPARE(calls(), 1);
        /* another card is asked about */
        QVERIFY(!offers(vm("virtio-gpu-gl-pci")));
        QCOMPARE(calls(), 2);

        /* a new binary there is asked */
        QVERIFY(QFile::remove(failing));
        QCOMPARE(fakeQemu("counted", "virtio-vga-gl", {"blob", "drm_native_context"},
                          {"honor-guest-pat"}),
                 failing);
        QVERIFY(isOffered(*offers(vm("virtio-vga-gl")), Feature::NativeContext));
    }
};

QTEST_GUILESS_MAIN(TestCardSettings)
#include "test_cardsettings.moc"
