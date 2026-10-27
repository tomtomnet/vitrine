// SPDX-License-Identifier: GPL-2.0-or-later
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFocusEvent>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include "core/argsfile.h"
#include "core/hostgpus.h"
#include "core/paths.h"
#include "core/vmstore.h"
#include "ui/argseditor.h"
#include "ui/banner.h"
#include "ui/settingspages.h"
#include "ui/vmpane.h"
#include "fakegpus.h"

#include <sys/sysmacros.h>

/*
 * The pages as VmPane drives them, without a screen: load() when a page is
 * shown or the VM changed outside, save() when it is left or applied, and
 * isModified() for the Apply and Discard buttons and for whether an outside
 * change may reload the page
 */
class TestSettingsPages : public QObject
{
    Q_OBJECT

    /* a host without GPUs, not this computer's (fakegpus.h) */
    QTemporaryDir noGpus;

    static QString text(const ArgsFile &args) { return args.toText(); }

    /* The Display page's choice, as the user makes it: -display's value of an entry */
    static void choose(QComboBox *display, const QString &value)
    {
        QVERIFY(display->findData(value) >= 0);
        display->setCurrentIndex(display->findData(value));
    }
    static QString chosen(const QComboBox *display)
    {
        return display->currentIndex() < 0 ? QString() : display->currentData().toString();
    }
    /* Whether the user can choose the entry of @value, which the QEMU may lack */
    static bool choosable(const QComboBox *display, const QString &value)
    {
        const auto *model = qobject_cast<const QStandardItemModel *>(display->model());
        return model && model->item(display->findData(value))->isEnabled();
    }

private slots:
    void initTestCase()
    {
        /* settings of their own, and no QEMU to read the documentation of */
        QStandardPaths::setTestModeEnabled(true);
        Paths::setQemuBinary("/nonexistent/qemu-system-x86_64");
        FakeGpus(noGpus.path()).use();
    }

    void cleanupTestCase()
    {
        Paths::setQemuBinary({});
        HostGpus::setRoots("/sys", "/dev");
        HostGpus::setDesktopDevice(std::nullopt);
    }

    /* The list: vitrine's window first, then QEMU's GTK and SDL windows, and none */
    void displayPageList()
    {
        DisplayPage page;
        auto *display = page.findChild<QComboBox *>("display");
        auto *hint = page.findChild<QLabel *>("displayHint");
        QVERIFY(display && hint);
        QStringList values, texts;
        for (int i = 0; i < display->count(); i++) {
            values << display->itemData(i).toString();
            texts << display->itemText(i);
        }
        QCOMPARE(values, QStringList({"dbus", "gtk", "sdl", "none"}));
        QCOMPARE(texts, QStringList({"Vitrine's window (D-Bus)", "QEMU's GTK window",
                                     "QEMU's SDL window", "None"}));

        /* what each is, under the list */
        ArgsFile args = ArgsFile::parse("-display dbus,p2p=yes,gl=on\n");
        page.load(args);
        QVERIFY(hint->isVisibleTo(&page));
        QVERIFY(hint->text().contains("Vitrine's window"));
        choose(display, "gtk");
        QVERIFY(hint->text().contains("menus"));
        choose(display, "sdl");
        QVERIFY(hint->text().contains("stays open"));
        choose(display, "none");
        QCOMPARE(hint->text(), "The VM runs without a screen.");

        /* -nographic: nothing to choose, the Arguments page's */
        args = ArgsFile::parse("-nographic\n");
        page.load(args);
        QVERIFY(!display->isEnabled());
        QCOMPARE(display->currentIndex(), -1);
        QCOMPARE(display->placeholderText(), "No screen (-nographic)");
        QVERIFY(!hint->isVisibleTo(&page));
        QVERIFY(!page.isModified());
        page.save(args);
        QCOMPARE(text(args), "-nographic\n");
    }

    /* A VM whose screen is none of the list's has no choice, whatever was before */
    void displayPageClearsItsChoice()
    {
        DisplayPage page;
        auto *display = page.findChild<QComboBox *>("display");
        auto *custom = page.findChild<Banner *>("customDisplay");
        QVERIFY(display && custom);

        /* shown once for vitrine's window, then -display changed by hand */
        ArgsFile args = ArgsFile::parse("-display dbus,p2p=yes,gl=on\n");
        page.load(args);
        QCOMPARE(chosen(display), "dbus");
        QVERIFY(custom->isHidden());
        QVERIFY(!page.isModified());

        args = ArgsFile::parse("-display egl-headless\n");
        page.load(args);
        QCOMPARE(chosen(display), "none");
        QVERIFY(!page.isModified());
        page.save(args);
        QCOMPARE(text(args), "-display egl-headless\n");

        /* a choice, then Discard: the page loads the arguments again */
        choose(display, "sdl");
        QVERIFY(page.isModified());
        page.load(args);
        QCOMPARE(chosen(display), "none");
        QVERIFY(!page.isModified());

        choose(display, "sdl");
        choose(display, "dbus");
        page.save(args);
        QCOMPARE(text(args), "-display dbus,p2p=yes\n");
        QVERIFY(!page.isModified());

        /* VNC alone: QEMU opens no window, which SDL adds */
        args = ArgsFile::parse("-vnc :0\n");
        page.load(args);
        QCOMPARE(chosen(display), "");
        QCOMPARE(display->placeholderText(), "VNC or SPICE only");
        QVERIFY(!custom->isHidden());
        QVERIFY(!page.isModified());
        page.save(args);
        QCOMPARE(text(args), "-vnc :0\n");
        choose(display, "sdl");
        QVERIFY(page.isModified());
        page.save(args);
        QCOMPARE(text(args), "-vnc :0\n-display sdl\n");
        QCOMPARE(chosen(display), "sdl");

        /* QEMU's default window, and a display of QEMU's that vitrine does not show */
        for (const auto &[line, own] : {std::pair("-m 1G\n", "QEMU's default window"),
                                        std::pair("-display curses\n",
                                                  "Elsewhere (-display curses)")}) {
            args = ArgsFile::parse(line);
            page.load(args);
            QCOMPARE(chosen(display), "");
            QCOMPARE(display->placeholderText(), own);
            QVERIFY(!custom->isHidden());
            QVERIFY(!page.isModified());
        }
    }

    /* The Display page: the window and its options, the card and vitrine's settings of its 3D */
    void displayPageOptions()
    {
        const auto restore = qScopeGuard([]() {
            Paths::setQemuBinary("/nonexistent/qemu-system-x86_64");
        });
        /* vitrine's QEMU, not built: all it offers */
        Paths::setQemuBinary({});
        DisplayPage page;
        auto *screen = page.findChild<QComboBox *>("display");
        auto *card = page.findChild<QComboBox *>("card");
        auto *native = page.findChild<QCheckBox *>("nativeContext");
        auto *venus = page.findChild<QCheckBox *>("venus");
        auto *timing = page.findChild<QCheckBox *>("frameTiming");
        auto *cursor = page.findChild<QCheckBox *>("show-cursor");
        auto *zoom = page.findChild<QCheckBox *>("zoom-to-fit");
        auto *menubar = page.findChild<QCheckBox *>("show-menubar");
        auto *hover = page.findChild<QCheckBox *>("grab-on-hover");
        QVERIFY(screen && card && native && venus && timing && cursor && zoom && menubar && hover);
        const auto display = [](const ArgsFile &a) {
            return a.lines[a.indexesOf("display").last()].value;
        };
        const auto device = [](const ArgsFile &a) {
            return a.lines[a.indexOfDevice([](const QString &d) {
                                return d.startsWith("virtio") || d == "VGA";
                            })].value;
        };

        /* a new Linux VM's: vitrine's window, the 3D card with all of vitrine's settings */
        const QString full = "virtio-vga-gl,hostmem=16G,blob=on,drm_native_context=on,"
                             "x-host-vblank=on,x-vblank-lead=3000,x-vblank-lead-auto=on";
        ArgsFile args = ArgsFile::parse("-machine q35,memory-backend=mem\n"
                                        "-accel kvm,honor-guest-pat=auto\n"
                                        "-object memory-backend-memfd,id=mem,size=4G,share=on\n"
                                        "-device " + full + "\n"
                                        "-display dbus,p2p=yes,gl=on\n");
        const QString before = text(args);
        page.load(args);
        QCOMPARE(chosen(screen), "dbus");
        QCOMPARE(card->currentData().toString(), "virtio-vga-gl");
        QVERIFY(native->isChecked() && native->isEnabled());
        QVERIFY(timing->isChecked() && timing->isEnabled());
        /* vitrine's QEMU has no Venus */
        QVERIFY(!venus->isChecked() && !venus->isEnabled());
        QVERIFY(!cursor->isVisibleTo(&page));
        QVERIFY(!page.isModified());
        page.save(args);
        QCOMPARE(text(args), before);

        /* QEMU's GTK window, and its options */
        choose(screen, "gtk");
        QVERIFY(page.isModified());
        for (QCheckBox *box : {cursor, zoom, menubar, hover}) {
            QVERIFY(box->isVisibleTo(&page));
        }
        /* QEMU's own defaults */
        QVERIFY(!cursor->isChecked() && menubar->isChecked());
        cursor->setChecked(true);
        zoom->setChecked(true);
        menubar->setChecked(false);
        page.save(args);
        QCOMPARE(display(args), "gtk,gl=on,show-cursor=on,zoom-to-fit=on,show-menubar=off");
        QCOMPARE(chosen(screen), "gtk");
        QVERIFY(!page.isModified());

        /* SDL's: the mouse pointer, the others go */
        choose(screen, "sdl");
        QVERIFY(cursor->isVisibleTo(&page) && !zoom->isVisibleTo(&page));
        QVERIFY(cursor->isChecked());
        page.save(args);
        QCOMPARE(display(args), "sdl,gl=on,show-cursor=on");
        cursor->setChecked(false);
        page.save(args);
        QCOMPARE(display(args), "sdl,gl=on");

        /* the card without VGA: no VGA of QEMU's beside it, the properties as they were */
        card->setCurrentIndex(card->findData("virtio-gpu-gl-pci"));
        QVERIFY(page.isModified());
        page.save(args);
        QVERIFY2(text(args).contains("-device " + QString(full).replace("virtio-vga-gl",
                                                                        "virtio-gpu-gl-pci") +
                                     "\n-vga none\n"),
                 qPrintable(text(args)));

        /* vitrine's settings, one by one */
        timing->setChecked(false);
        page.save(args);
        QCOMPARE(device(args), "virtio-gpu-gl-pci,hostmem=16G,blob=on,drm_native_context=on,"
                               "x-host-vblank=off");
        QVERIFY(!timing->isChecked());
        native->setChecked(false);
        page.save(args);
        QCOMPARE(device(args), "virtio-gpu-gl-pci,hostmem=16G,blob=on,x-host-vblank=off");

        /* and back to the template's */
        card->setCurrentIndex(card->findData("virtio-vga-gl"));
        native->setChecked(true);
        timing->setChecked(true);
        page.save(args);
        QCOMPARE(device(args), full);
        QVERIFY(!page.isModified());

        /* none: a display with OpenGL for the 3D card, no window */
        choose(screen, "none");
        page.save(args);
        QCOMPARE(display(args), "egl-headless");
        QCOMPARE(chosen(screen), "none");
        /* a 2D card has none of the 3D settings, and QEMU's none display */
        card->setCurrentIndex(card->findData("virtio-vga"));
        QVERIFY(!native->isEnabled() && !venus->isEnabled() && !timing->isEnabled());
        page.save(args);
        QVERIFY(device(args).startsWith("virtio-vga,"));
        QVERIFY(!device(args).contains("drm_native_context"));
        QCOMPARE(display(args), "none");
        QCOMPARE(chosen(screen), "none");
        choose(screen, "dbus");
        page.save(args);
        QCOMPARE(display(args), "dbus,p2p=yes");
    }

    /* What the VM's QEMU lacks is not offered; a card set by hand stays */
    void displayPageOffers()
    {
        QTemporaryDir tmp;
        const QString qemu = tmp.filePath(Paths::qemuSystemName());
        QFile f(qemu);
        QVERIFY(f.open(QIODevice::WriteOnly));
        /* QEMU 10.2 without GTK: neither native context nor the host's vblank */
        f.write("#!/bin/sh\n"
                "case \"$1 $2\" in\n"
                "'-display help') printf 'Available display backend types:\\nnone\\nsdl\\n"
                "egl-headless\\ndbus\\n\\nSuboptions\\n' ;;\n"
                "'-device virtio-vga-gl,help') printf 'virtio-vga-gl options:\\n"
                "  blob=<bool>\\n  hostmem=<size>\\n  venus=<bool>\\n' ;;\n"
                "'-object kvm-accel,help') printf 'kvm-accel options:\\n"
                "  kernel-irqchip=<on|off|split>\\n' ;;\n"
                "esac\n");
        f.close();
        QVERIFY(f.setPermissions(QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        DisplayPage page;
        auto *display = page.findChild<QComboBox *>("display");
        auto *lacks = page.findChild<QLabel *>("displayLacks");
        auto *card = page.findChild<QComboBox *>("card");
        auto *native = page.findChild<QCheckBox *>("nativeContext");
        auto *venus = page.findChild<QCheckBox *>("venus");
        auto *timing = page.findChild<QCheckBox *>("frameTiming");
        QVERIFY(display && lacks && card && native && venus && timing);

        ArgsFile args = ArgsFile::parse("#qemu " + qemu + "\n-device virtio-vga-gl\n"
                                        "-display sdl,gl=on\n");
        page.load(args);
        /* GTK not to be chosen, and why */
        QVERIFY(!choosable(display, "gtk"));
        QVERIFY(choosable(display, "dbus") && choosable(display, "sdl") &&
                choosable(display, "none"));
        QVERIFY(lacks->isVisibleTo(&page));
        QCOMPARE(lacks->text(), "This VM's QEMU has no GTK display.");
        /* the keys go past it */
        choose(display, "dbus");
        QTest::keyClick(display, Qt::Key_Down);
        QCOMPARE(chosen(display), "sdl");
        QTest::keyClick(display, Qt::Key_Up);
        QCOMPARE(chosen(display), "dbus");
        choose(display, "sdl");
        QVERIFY(!page.isModified());
        QVERIFY(!native->isEnabled() && !timing->isEnabled());
        QVERIFY(venus->isEnabled());
        venus->setChecked(true);
        page.save(args);
        QCOMPARE(text(args), "#qemu " + qemu + "\n-device virtio-vga-gl,hostmem=16G,blob=on,"
                             "venus=on\n-display sdl,gl=on\n");

        /* a VM in the GTK window its QEMU lacks: shown as it is, until another is chosen */
        args = ArgsFile::parse("#qemu " + qemu + "\n-device virtio-vga-gl\n-display gtk,gl=on\n");
        page.load(args);
        QCOMPARE(chosen(display), "gtk");
        QVERIFY(choosable(display, "gtk"));
        QVERIFY(!page.isModified());
        choose(display, "sdl");
        QVERIFY(!choosable(display, "gtk"));

        /* a QEMU that does not say what it has: all of them */
        args = ArgsFile::parse("-device virtio-vga-gl\n-display gtk,gl=on\n");
        page.load(args);
        QVERIFY(lacks->isHidden());

        /* several cards: the Arguments page's */
        args = ArgsFile::parse("-device qxl-vga\n-device virtio-gpu-pci\n-display gtk\n");
        page.load(args);
        QVERIFY(!card->isEnabled());
        QVERIFY(!native->isEnabled());
        QVERIFY(!page.isModified());
        page.save(args);
        QCOMPARE(text(args), "-device qxl-vga\n-device virtio-gpu-pci\n-display gtk\n");
    }

    /*
     * The nested desktop's checkbox, beside the display list: its card in
     * place of the VM's, with what the VM's QEMU has, and back
     */
    void displayPageNested()
    {
        QTemporaryDir tmp;
        const auto qemuScript = [&tmp](const QString &name, bool rutabaga) {
            const QString dir = tmp.filePath(name);
            QDir().mkpath(dir);
            QFile f(dir + '/' + Paths::qemuSystemName());
            if (!f.open(QIODevice::WriteOnly)) {
                return QString();
            }
            f.write("#!/bin/sh\n"
                    "case \"$1 $2\" in\n"
                    "'-display help') printf 'Available display backend types:\\nnone\\ngtk\\n"
                    "sdl\\negl-headless\\ndbus\\n\\nSuboptions\\n' ;;\n"
                    "'-device virtio-vga-gl,help') printf 'virtio-vga-gl options:\\n"
                    "  blob=<bool>\\n  hostmem=<size>\\n  drm_native_context=<bool>\\n' ;;\n"
                    "'-object kvm-accel,help') printf 'kvm-accel options:\\n"
                    "  honor-guest-pat=<OnOffAuto>\\n' ;;\n");
            if (rutabaga) {
                f.write("'-device virtio-vga-rutabaga,help') printf 'virtio-vga-rutabaga "
                        "options:\\n  cross-domain=<bool>\\n  hostmem=<size>\\n  x-drm=<bool>\\n"
                        "  x-virgl2=<bool>\\n  wayland-socket-path=<str>\\n' ;;\n");
            }
            f.write("esac\n");
            f.close();
            f.setPermissions(QFileDevice::ReadOwner | QFileDevice::ExeOwner);
            return f.fileName();
        };
        const QString research = qemuScript("research", true);
        const QString older = qemuScript("older", false);
        DisplayPage page;
        auto *display = page.findChild<QComboBox *>("display");
        auto *nested = page.findChild<QCheckBox *>("nested");
        auto *hint = page.findChild<QLabel *>("nestedHint");
        auto *displayHint = page.findChild<QLabel *>("displayHint");
        auto *card = page.findChild<QComboBox *>("card");
        auto *native = page.findChild<QCheckBox *>("nativeContext");
        QVERIFY(display && nested && hint && displayHint && card && native);

        const QString vm = "-machine q35,memory-backend=mem\n"
                           "-object memory-backend-memfd,id=mem,size=4G,share=on\n"
                           "-device virtio-vga-gl,hostmem=4G,blob=on,drm_native_context=on\n"
                           "-display dbus,p2p=yes,gl=on\n";
        ArgsFile args = ArgsFile::parse("#qemu " + research + '\n' + vm);
        page.load(args);
        QVERIFY(nested->isEnabled());
        QVERIFY(!nested->isChecked());
        QVERIFY(hint->text().startsWith("The guest's KDE session through a KWin of its own"));
        QVERIFY(card->isEnabled());

        /* on: the card is the nested desktop's, its 3D its own */
        nested->setChecked(true);
        QVERIFY(page.isModified());
        QVERIFY(!card->isEnabled());
        QCOMPARE(card->currentData().toString(), "virtio-vga-rutabaga");
        QCOMPARE(card->currentText(), "Nested desktop's, with VGA (virtio-vga-rutabaga)");
        QVERIFY(!native->isEnabled());
        QVERIFY(displayHint->text().startsWith("Vitrine's window shows the guest's desktop "
                                               "beside the VM's screen"));
        QCOMPARE(hint->text(), "For Fedora KDE guests with the guest tools: a login at their "
                               "login screen starts the nested desktop.");
        page.save(args);
        QCOMPARE(text(args), "#qemu " + research + "\n"
                             "-machine q35,memory-backend=mem,hpet=off\n"
                             "-global kvm-pit.lost_tick_policy=discard\n"
                             "-object memory-backend-memfd,id=mem,size=4G,share=on\n"
                             "-vga none\n"
                             "-device virtio-vga-rutabaga,x-drm=on,x-virgl2=on,"
                             "cross-domain=on,hostmem=16G\n"
                             "-device pvpanic\n"
                             "-display dbus,p2p=yes,gl=on\n");
        QVERIFY(!page.isModified());
        QVERIFY(nested->isChecked());

        /* QEMU's SDL window: no OpenGL there with the nested desktop */
        choose(display, "sdl");
        QVERIFY(displayHint->text().startsWith("The guest's desktop opens as a window of its own"));
        page.save(args);
        QCOMPARE(args.lines[args.indexOf("display")].value, "sdl");
        choose(display, "dbus");
        page.save(args);
        QCOMPARE(args.lines[args.indexOf("display")].value, "dbus,p2p=yes,gl=on");

        /* off: the 3D card of new VMs back, with its native context */
        nested->setChecked(false);
        QVERIFY(card->isEnabled());
        QCOMPARE(card->currentData().toString(), "virtio-vga-gl");
        QCOMPARE(card->findData("virtio-vga-rutabaga"), -1);
        QVERIFY(native->isEnabled() && native->isChecked());
        page.save(args);
        QVERIFY(VmConfig::graphics(args).kind == VmConfig::Graphics::Accelerated);
        QVERIFY(VmConfig::graphics(args).nativeContext);
        /* no stray -vga: as the VM was */
        QCOMPARE(args.indexOf("vga"), -1);
        QCOMPARE(args.lines[args.indexOf("display")].value, "dbus,p2p=yes,gl=on");

        /* on, then off before Apply: nothing to save; native context shown as the card has it */
        page.load(args);
        native->setChecked(false);
        nested->setChecked(true);
        QVERIFY(native->isChecked() && !native->isEnabled());
        nested->setChecked(false);
        QVERIFY(!native->isChecked());
        native->setChecked(true);
        QVERIFY(!page.isModified());
        QCOMPARE(card->currentData().toString(), "virtio-vga-gl");

        /* a QEMU without it: not to be checked, and why */
        args = ArgsFile::parse("#qemu " + older + '\n' + vm);
        page.load(args);
        QVERIFY(!nested->isEnabled());
        QCOMPARE(hint->text(), "This VM's QEMU has no virtio-gpu-rutabaga with cross-domain.");
        /* but a VM that has it can turn it off */
        args = ArgsFile::parse("#qemu " + older + "\n-vga none\n"
                               "-device virtio-vga-rutabaga,cross-domain=on\n"
                               "-display sdl\n");
        page.load(args);
        QVERIFY(nested->isEnabled() && nested->isChecked());
        QVERIFY(native->isChecked());
        QVERIFY(!page.isModified());

        /*
         * The two cards of before (QEMU's VGA beside the rutabaga card), until
         * the VM's next start: the list's entry for the nested desktop, and
         * why the card is another; untouched, nothing written; off, the 3D
         * card in their place
         */
        auto *cardHint = page.findChild<QLabel *>("cardHint");
        QVERIFY(cardHint);
        const QString twoCards = "#qemu " + research + "\n-machine q35\n-vga std\n"
                                 "-device virtio-gpu-rutabaga-pci,cross-domain=on\n"
                                 "-display dbus,p2p=yes,gl=on\n";
        args = ArgsFile::parse(twoCards);
        page.load(args);
        QVERIFY(nested->isEnabled() && nested->isChecked());
        QCOMPARE(card->currentData().toString(), "virtio-vga-rutabaga");
        QCOMPARE(card->count(), 5);
        QVERIFY2(cardHint->text().contains("at its next start"), qPrintable(cardHint->text()));
        QVERIFY(!page.isModified());
        page.save(args);
        QCOMPARE(text(args), twoCards);
        nested->setChecked(false);
        QCOMPARE(card->findData("virtio-vga-rutabaga"), -1);
        QCOMPARE(card->count(), 4);
        page.save(args);
        QVERIFY(VmConfig::graphics(args).kind == VmConfig::Graphics::Accelerated);
        QCOMPARE(VmConfig::graphics(args).device, "virtio-vga-gl");
        QVERIFY(VmConfig::graphics(args).nativeContext);
        QCOMPARE(args.indexOf("vga"), -1);
        QVERIFY(!text(args).contains("rutabaga"));

        /* a rutabaga card set up by hand is the Arguments page's */
        for (const char *byHand : {"-vga none\n-device virtio-gpu-rutabaga-pci,cross-domain=on\n",
                                   "-vga std\n-device virtio-vga-rutabaga,cross-domain=on\n"}) {
            args = ArgsFile::parse("#qemu " + research + '\n' + byHand);
            page.load(args);
            QVERIFY2(!nested->isEnabled(), byHand);
            QVERIFY(hint->isHidden());
        }
    }

    /*
     * The host GPU the nested desktop renders on: a row under its checkbox
     * with several GPUs, or with a choice made; Automatic names what it
     * takes; a GPU chosen is kept on the card by its stable link
     */
    void displayPageGpu()
    {
        QTemporaryDir tmp;
        const auto qemuScript = [&tmp](const QString &name, bool renderNode) {
            const QString dir = tmp.filePath(name);
            QDir().mkpath(dir);
            QFile f(dir + '/' + Paths::qemuSystemName());
            if (!f.open(QIODevice::WriteOnly)) {
                return QString();
            }
            f.write("#!/bin/sh\n"
                    "case \"$1 $2\" in\n"
                    "'-display help') printf 'Available display backend types:\\nnone\\nsdl\\n"
                    "dbus\\n\\nSuboptions\\n' ;;\n"
                    "'-device virtio-vga-rutabaga,help') printf 'virtio-vga-rutabaga options:\\n"
                    "  cross-domain=<bool>\\n  hostmem=<size>\\n  x-drm=<bool>\\n"
                    "  x-virgl2=<bool>\\n");
            f.write(renderNode ? "  x-render-node=<str>\\n' ;;\n" : "' ;;\n");
            f.write("esac\n");
            f.close();
            f.setPermissions(QFileDevice::ReadOwner | QFileDevice::ExeOwner);
            return f.fileName();
        };
        const QString research = qemuScript("research", true);
        const QString older = qemuScript("older", false);
        const auto restore = qScopeGuard([this]() {
            FakeGpus(noGpus.path()).use();
            HostGpus::setDesktopDevice(std::nullopt);
        });
        const QString vm = "-machine q35,memory-backend=mem\n"
                           "-object memory-backend-memfd,id=mem,size=4G,share=on\n"
                           "-vga none\n"
                           "-device virtio-vga-rutabaga,x-drm=on,x-virgl2=on,cross-domain=on,"
                           "hostmem=16G\n"
                           "-device pvpanic\n"
                           "-display dbus,p2p=yes,gl=on\n";
        const auto chose = [&vm](const QString &qemu, const QString &path) {
            return ArgsFile::parse("#qemu " + qemu + '\n' +
                                   QString(vm).replace("hostmem=16G",
                                                       "hostmem=16G,x-render-node=" + path));
        };

        /* the user's desktop: the B50, the compositor's, and the AMD iGPU */
        FakeGpus desktop(tmp.filePath("desktop"));
        desktop.desktop();
        desktop.use();
        HostGpus::setDesktopDevice(makedev(226, 0));
        const QList<HostGpu> gpus = HostGpus::list();
        QCOMPARE(gpus.size(), 2);
        const QString b50 = gpus[0].name(), igpu = gpus[1].name();

        DisplayPage page;
        auto *nested = page.findChild<QCheckBox *>("nested");
        auto *gpu = page.findChild<QComboBox *>("gpu");
        auto *hint = page.findChild<QLabel *>("gpuHint");
        QVERIFY(nested && gpu && hint);
        const auto *model = qobject_cast<const QStandardItemModel *>(gpu->model());
        QVERIFY(model);

        ArgsFile args = ArgsFile::parse("#qemu " + research + '\n' + vm);
        page.load(args);
        QVERIFY(gpu->isVisibleTo(&page));
        QVERIFY(gpu->isEnabled());
        QCOMPARE(gpu->count(), 3);
        QCOMPARE(gpu->itemText(0), "Automatic: " + b50 + ", the desktop's");
        QCOMPARE(gpu->itemText(1), b50 + ", the desktop's");
        QCOMPARE(gpu->itemText(2), igpu);
        QCOMPARE(gpu->itemData(2, Qt::ToolTipRole).toString(),
                 "/dev/dri/renderD128, driver amdgpu, PCI 7a:00.0");
        QCOMPARE(gpu->currentIndex(), 0);
        QVERIFY(hint->isHidden());
        QVERIFY(!page.isModified());

        /* another GPU: why it may be slower; kept by its stable link */
        gpu->setCurrentIndex(2);
        QVERIFY(page.isModified());
        QVERIFY2(hint->text().startsWith("This computer's desktop runs on " + b50 +
                                         ": the guest's frames cross from one GPU to the other"),
                 qPrintable(hint->text()));
        QVERIFY(hint->text().contains("may show nothing"));
        page.save(args);
        QCOMPARE(text(args), text(chose(research, "/dev/dri/by-path/pci-0000:7a:00.0-render")));
        QCOMPARE(gpu->currentIndex(), 2);
        QVERIFY(!page.isModified());
        /* the desktop's: nothing to say */
        gpu->setCurrentIndex(1);
        QVERIFY(hint->isHidden());
        page.save(args);
        QCOMPARE(text(args), text(chose(research, "/dev/dri/by-path/pci-0000:03:00.0-render")));
        /* Automatic: none on the card, the VM as it was */
        gpu->setCurrentIndex(0);
        page.save(args);
        QCOMPARE(text(args), "#qemu " + research + '\n' + vm);

        /* the nested desktop off, no row; on again, nothing changed */
        nested->setChecked(false);
        QVERIFY(!gpu->isVisibleTo(&page));
        nested->setChecked(true);
        QVERIFY(gpu->isVisibleTo(&page));
        QVERIFY(!page.isModified());

        /* a choice written by hand: the GPU it names by any path, left as written */
        args = chose(research, "/dev/dri/renderD128");
        page.load(args);
        QCOMPARE(gpu->currentIndex(), 2);
        QVERIFY(!page.isModified());
        page.save(args);
        QCOMPARE(text(args), text(chose(research, "/dev/dri/renderD128")));

        /* a GPU taken out: said, until another is chosen */
        args = chose(research, "/dev/dri/by-path/pci-0000:05:00.0-render");
        page.load(args);
        QCOMPARE(gpu->count(), 4);
        QCOMPARE(gpu->currentText(), "Not on this computer: /dev/dri/by-path/pci-0000:05:00.0-render");
        QCOMPARE(hint->text(), "This GPU is not on this computer: the VM does not start until "
                               "another GPU, or Automatic, is chosen.");
        QVERIFY(!page.isModified());
        gpu->setCurrentIndex(0);
        QVERIFY(!model->item(3)->isEnabled());
        page.save(args);
        QCOMPARE(text(args), "#qemu " + research + '\n' + vm);
        QCOMPARE(gpu->count(), 3);
        args = chose(research, "/dev/dri/card0");
        page.load(args);
        QCOMPARE(gpu->currentText(), "Not a GPU: /dev/dri/card0");

        /* a QEMU without x-render-node: Automatic alone, and why */
        args = ArgsFile::parse("#qemu " + older + '\n' + vm);
        page.load(args);
        QVERIFY(gpu->isVisibleTo(&page));
        QVERIFY(!gpu->isEnabled());
        QCOMPARE(hint->text(), "This VM's QEMU cannot choose the GPU: an update of Vitrine's QEMU "
                               "brings it.");
        /* one chosen with another QEMU before: back to Automatic only */
        args = chose(older, "/dev/dri/by-path/pci-0000:7a:00.0-render");
        page.load(args);
        QVERIFY(gpu->isEnabled());
        QVERIFY(model->item(0)->isEnabled() && model->item(2)->isEnabled());
        QVERIFY(!model->item(1)->isEnabled());

        /* one GPU: no row, unless the VM chose one */
        FakeGpus laptop(tmp.filePath("laptop"));
        laptop.laptop();
        laptop.use();
        HostGpus::setDesktopDevice(std::nullopt);
        const QString radeon = HostGpus::list().value(0).name();
        args = ArgsFile::parse("#qemu " + research + '\n' + vm);
        page.load(args);
        QVERIFY(!gpu->isVisibleTo(&page));
        QVERIFY(hint->isHidden());
        args = chose(research, "/dev/dri/by-path/pci-0000:65:00.0-render");
        page.load(args);
        QVERIFY(gpu->isVisibleTo(&page));
        QCOMPARE(gpu->itemText(0), "Automatic: " + radeon + ", the desktop's");
        QCOMPARE(gpu->currentIndex(), 1);
        QVERIFY(hint->isHidden());
        QVERIFY(!page.isModified());
        gpu->setCurrentIndex(0);
        QVERIFY(hint->isHidden());

        /*
         * A desktop on a GPU without native context: not to be chosen, and
         * Automatic leaves it to the renderer, whose GPU it names
         */
        FakeGpus nvidia(tmp.filePath("nvidia"));
        nvidia.gpu("0000:01:00.0", "0x10de", "0x2684", "nvidia", true, 0, 128);
        nvidia.gpu("0000:00:02.0", "0x8086", "0xa780", "i915", false, 1, 129);
        nvidia.use();
        const QList<HostGpu> two = HostGpus::list();
        args = ArgsFile::parse("#qemu " + research + '\n' + vm);
        page.load(args);
        QCOMPARE(gpu->itemText(0), "Automatic: " + two[0].name());
        QCOMPARE(gpu->itemText(2), two[1].name() + ", the desktop's, without native context");
        QVERIFY(model->item(1)->isEnabled());
        QVERIFY(!model->item(2)->isEnabled());
        QVERIFY2(hint->text().contains("which has no native context in Vitrine's QEMU"),
                 qPrintable(hint->text()));
    }

    /* The count is the Hardware page's: edits elsewhere on the page keep -smp */
    void machinePageKeepsTheCount()
    {
        MachinePage page;
        auto *topology = page.findChild<QCheckBox *>("topology");
        auto *machine = page.findChild<QComboBox *>("machine");
        auto *model = page.findChild<QComboBox *>("cpuModel");
        auto *sockets = page.findChild<QSpinBox *>("sockets");
        auto *cores = page.findChild<QSpinBox *>("cores");
        auto *threads = page.findChild<QSpinBox *>("threads");
        QVERIFY(topology && machine && model && sockets && cores && threads);

        /* QEMU makes 2 sockets of it: the page shows what QEMU runs */
        ArgsFile args = ArgsFile::parse("-machine pc\n-cpu host\n-smp 8,cores=4\n");
        page.load(args);
        QVERIFY(topology->isChecked());
        QCOMPARE(sockets->value(), 2);
        QCOMPARE(cores->value(), 4);
        QCOMPARE(threads->value(), 1);
        QVERIFY(!page.isModified());

        machine->setCurrentText("q35");
        QVERIFY(page.isModified());
        page.save(args);
        QCOMPARE(text(args), "-machine q35\n-cpu host\n-smp 8,cores=4\n");
        QVERIFY(!page.isModified());

        /* the model alone */
        args = ArgsFile::parse("-cpu host\n-smp 8,threads=2\n");
        page.load(args);
        QCOMPARE(QString("%1x%2x%3").arg(sockets->value()).arg(cores->value()).arg(threads->value()),
                 "1x4x2");
        model->setCurrentText("qemu64");
        page.save(args);
        QCOMPARE(text(args), "-cpu qemu64\n-smp 8,threads=2\n");

        /* the topology edited: -smp from the numbers */
        args = ArgsFile::parse("-smp cpus=16,cores=4,threads=2\n");
        page.load(args);
        QCOMPARE(sockets->value(), 2);
        sockets->setValue(1);
        QVERIFY(page.isModified());
        page.save(args);
        QCOMPARE(text(args), "-smp cpus=8,cores=4,threads=2,sockets=1\n");

        /* and the box: on writes the numbers shown, off leaves the count */
        args = ArgsFile::parse("-smp 8\n");
        page.load(args);
        QVERIFY(!topology->isChecked());
        topology->setChecked(true);
        page.save(args);
        QCOMPARE(text(args), "-smp 8,sockets=1,cores=8,threads=1\n");
        page.load(args);
        topology->setChecked(false);
        page.save(args);
        QCOMPARE(text(args), "-smp 8\n");

        /* maxcpus= or dies=: the topology is the Arguments page's, the model still here */
        args = ArgsFile::parse("-cpu host\n-smp 8,sockets=1,dies=2,cores=4,threads=1\n");
        page.load(args);
        QVERIFY(!topology->isEnabled());
        QVERIFY(!sockets->isEnabled());
        QVERIFY(!page.isModified());
        model->setCurrentText("max");
        page.save(args);
        QCOMPARE(text(args), "-cpu max\n-smp 8,sockets=1,dies=2,cores=4,threads=1\n");
    }

    /* A new SSH forward takes none of the ports the VM forwards already */
    void networkPagePortIsFree()
    {
        NetworkPage page(nullptr);
        auto *ssh = page.findChild<QCheckBox *>("ssh");
        auto *port = page.findChild<QSpinBox *>("sshPort");
        QVERIFY(ssh && port);

        ArgsFile args = ArgsFile::parse("-nic user,model=virtio-net-pci,hostfwd=tcp::10022-:80\n");
        page.load(args);
        QVERIFY(!ssh->isChecked());
        ssh->setChecked(true);
        QVERIFY(port->value() > 10022);
        page.save(args);
        QCOMPARE(text(args), QString("-nic user,model=virtio-net-pci,hostfwd=tcp::10022-:80,"
                                     "hostfwd=tcp:127.0.0.1:%1-:22\n")
                                 .arg(port->value()));
    }

    /* The page says who reaches the guest's SSH, as the forward is written */
    void networkPageTellsWhoReachesSsh()
    {
        NetworkPage page(nullptr);
        auto *port = page.findChild<QSpinBox *>("sshPort");
        QLabel *info = nullptr;
        const auto load = [&page](const char *args) { page.load(ArgsFile::parse(args)); };

        load("-nic user,hostfwd=tcp:127.0.0.1:2222-:22\n");
        for (QLabel *label : page.findChildren<QLabel *>()) {
            if (label->text().contains("ssh -p")) {
                info = label;
            }
        }
        QVERIFY(info && port);
        QCOMPARE(info->text(), "From this computer only: ssh -p 2222 USER@127.0.0.1");
        load("-nic user,hostfwd=tcp::2222-:22\n");
        QVERIFY(info->text().contains("from other computers too"));
        load("-nic passt,tcp-ports=192.168.1.5/2222:22\n");
        QCOMPARE(info->text(),
                 "On 192.168.1.5 only, which other computers may reach: "
                 "ssh -p 2222 USER@192.168.1.5");
        /* another port is written on 127.0.0.1 */
        load("-nic user,hostfwd=tcp::2222-:22\n");
        port->setValue(2223);
        QCOMPARE(info->text(), "From this computer only: ssh -p 2223 USER@127.0.0.1");
    }

    void hardwarePageLeavesCustomCpus()
    {
        HardwarePage page;
        auto *cpus = page.findChild<QSpinBox *>("cpus");
        QVERIFY(cpus);

        ArgsFile args = ArgsFile::parse("-m 1G\n-smp 4,maxcpus=8,sockets=1,cores=8,threads=1\n");
        page.load(args);
        QCOMPARE(cpus->value(), 4);
        QVERIFY(!cpus->isEnabled());
        QVERIFY(!page.isModified());

        args = ArgsFile::parse("-m 1G\n-smp 4\n");
        page.load(args);
        QVERIFY(cpus->isEnabled());
        cpus->setValue(2);
        page.save(args);
        QCOMPARE(text(args), "-m 1G\n-smp 2\n");
    }

    /* Keyboard focus coming into the list of pages chooses none */
    void paneKeepsItsPageOnFocus()
    {
        QTemporaryDir dir;
        QFile args(dir.filePath("vm.args"));
        QVERIFY(args.open(QIODevice::WriteOnly) && args.write("-m 1G\n") > 0);
        args.close();
        Vm vm(dir.path());
        VmPane pane;
        auto *pages = pane.findChild<QListWidget *>("pages");
        QVERIFY(pages);

        pane.setVm(&vm);
        pane.setTab(VmPane::Settings);
        pane.setPage(VmPane::Machine);
        QCOMPARE(pane.page(), VmPane::Machine);
        /* Shift+Tab or Tab into the list, from the page or the tabs */
        QFocusEvent backtab(QEvent::FocusIn, Qt::BacktabFocusReason);
        QCoreApplication::sendEvent(pages, &backtab);
        QCOMPARE(pane.page(), VmPane::Machine);
        QFocusEvent tab(QEvent::FocusIn, Qt::TabFocusReason);
        QCoreApplication::sendEvent(pages, &tab);
        QCOMPARE(pane.page(), VmPane::Machine);

        /* the keys and the mouse still choose */
        QTest::keyClick(pages, Qt::Key_Down);
        QCOMPARE(pane.page(), VmPane::PciDevices);
        pages->setCurrentRow(VmPane::Display);
        QCOMPARE(pane.page(), VmPane::Display);
    }

    /*
     * Every page in one list, in the order of VmPane::Page, none folded
     * away under an Advanced button; the Boot page's content on the Machine
     * page, under a heading
     */
    void paneListsEveryPage()
    {
        QTemporaryDir dir;
        QFile args(dir.filePath("vm.args"));
        QVERIFY(args.open(QIODevice::WriteOnly) && args.write("-machine q35\n-m 1G\n") > 0);
        args.close();
        Vm vm(dir.path());
        VmPane pane;
        auto *pages = pane.findChild<QListWidget *>("pages");
        auto *stack = pane.findChild<QStackedWidget *>("pageStack");
        QVERIFY(pages && stack);

        pane.setVm(&vm);
        pane.setTab(VmPane::Settings);
        QStringList titles;
        for (int i = 0; i < pages->count(); i++) {
            titles << pages->item(i)->text();
            QVERIFY(!pages->item(i)->isHidden());
        }
        QCOMPARE(titles, QStringList({"General", "Hardware", "Display", "Storage",
                                      "Shared Folders", "USB Devices", "Network", "Machine",
                                      "PCI Devices", "Arguments"}));
        QCOMPARE(pane.findChildren<QListWidget *>("pages").size(), 1);
        QVERIFY(!pane.findChild<QWidget *>("advanced"));
        QVERIFY(!pane.findChild<QWidget *>("advancedPages"));
        QCOMPARE(stack->count(), pages->count());

        /* the pages by their names, which callers open */
        const QList<std::pair<VmPane::Page, QString>> named = {
            {VmPane::General, "General"},      {VmPane::UsbDevices, "USB Devices"},
            {VmPane::Network, "Network"},      {VmPane::Machine, "Machine"},
            {VmPane::PciDevices, "PCI Devices"}, {VmPane::Arguments, "Arguments"}};
        for (const auto &[page, title] : named) {
            pane.setPage(page);
            QCOMPARE(pane.page(), page);
            QCOMPARE(pages->currentItem()->text(), title);
            QCOMPARE(stack->currentIndex(), int(page));
        }

        /* the firmware and the boot menu under Boot, on the Machine page */
        QWidget *machine = stack->widget(VmPane::Machine);
        QVERIFY(machine->findChild<QComboBox *>("firmware"));
        QVERIFY(machine->findChild<QCheckBox *>("bootMenu"));
        QVERIFY(machine->findChild<QPushButton *>("resetVars"));
        QStringList labels;
        for (const QLabel *label : machine->findChildren<QLabel *>()) {
            labels << label->text();
        }
        QVERIFY2(labels.contains("Boot"), qPrintable(labels.join(" | ")));
        QVERIFY(labels.contains("F&irmware:"));
        int firmwarePages = 0;
        for (int i = 0; i < stack->count(); i++) {
            firmwarePages += stack->widget(i)->findChildren<QComboBox *>("firmware").size();
        }
        QCOMPARE(firmwarePages, 1);
    }

    /*
     * The page kept by name; an index an older vitrine kept, which counted
     * Boot between Machine and PCI Devices, on the page of the same name,
     * Boot on Machine
     */
    void savedPage_data()
    {
        QTest::addColumn<QVariant>("saved");
        QTest::addColumn<int>("page");

        for (int i = 0; i <= VmPane::Arguments; i++) {
            const auto page = VmPane::Page(i);
            QTest::addRow("%s", qPrintable(VmPane::pageKey(page))) << QVariant(VmPane::pageKey(page)) << i;
        }
        const QList<int> older = {VmPane::General,  VmPane::Hardware,      VmPane::Display,
                                  VmPane::Storage,  VmPane::SharedFolders, VmPane::UsbDevices,
                                  VmPane::Network,  VmPane::Machine,       VmPane::Machine,
                                  VmPane::PciDevices, VmPane::Arguments};
        for (int i = 0; i < older.size(); i++) {
            /* as QSettings reads an int back from the file: a string */
            QTest::addRow("older %d", i) << QVariant(QString::number(i)) << older[i];
        }
        QTest::newRow("older, an int") << QVariant(8) << int(VmPane::Machine);
        QTest::newRow("none") << QVariant() << int(VmPane::General);
        QTest::newRow("out of range") << QVariant("11") << int(VmPane::General);
        QTest::newRow("negative") << QVariant("-1") << int(VmPane::General);
        QTest::newRow("unknown name") << QVariant("boot") << int(VmPane::General);
    }

    void savedPage()
    {
        QFETCH(QVariant, saved);
        QFETCH(int, page);

        QCOMPARE(int(VmPane::savedPage(saved)), page);
    }

    /* Machine's Boot part: the firmware there is for the machine shown, the boot menu */
    void machinePageBoot()
    {
        using VmConfig::FirmwareKind;
        MachinePage page;
        auto *machine = page.findChild<QComboBox *>("machine");
        auto *firmware = page.findChild<QComboBox *>("firmware");
        auto *menu = page.findChild<QCheckBox *>("bootMenu");
        auto *reset = page.findChild<QPushButton *>("resetVars");
        QVERIFY(machine && firmware && menu && reset);
        auto items = [firmware]() {
            QStringList list;
            for (int i = 0; i < firmware->count(); i++) {
                list << firmware->itemText(i);
            }
            return list.join(", ");
        };

        ArgsFile args = ArgsFile::parse("-machine q35\n-m 1G\n");
        page.load(args);
        QCOMPARE(firmware->currentData().toInt(), int(FirmwareKind::Bios));
        QVERIFY(items().startsWith("BIOS"));
        QVERIFY(!menu->isChecked());
        /* no VM, no variables to reset */
        QVERIFY(reset->isHidden());
        QVERIFY(!page.isModified());

        menu->setChecked(true);
        QVERIFY(page.isModified());
        page.save(args);
        QVERIFY(VmConfig::bootMenu(args));
        QVERIFY2(text(args).contains("-boot menu=on\n"), qPrintable(text(args)));
        QVERIFY(!page.isModified());

        /* ARM's virt boots with UEFI only: the list follows the machine */
        firmware->setCurrentIndex(firmware->findData(int(FirmwareKind::Uefi)));
        QVERIFY(page.isModified());
        machine->setCurrentText("virt");
        QVERIFY2(!items().contains("BIOS"), qPrintable(items()));
        QCOMPARE(firmware->currentData().toInt(), int(FirmwareKind::Uefi));
        machine->setCurrentText("q35");
        QVERIFY(items().startsWith("BIOS"));
        QCOMPARE(firmware->currentData().toInt(), int(FirmwareKind::Uefi));

        /* loaded again (Discard): as the arguments say */
        page.load(args);
        QCOMPARE(firmware->currentData().toInt(), int(FirmwareKind::Bios));
        QVERIFY(menu->isChecked());
        QVERIFY(!page.isModified());
        menu->setChecked(false);
        page.save(args);
        QCOMPARE(text(args), "-machine q35\n-m 1G\n");
    }

    /* Ctrl+S in the Arguments page's editor applies the changes, as Apply does */
    void ctrlSApplies()
    {
        QTemporaryDir dir;
        QFile args(dir.filePath("vm.args"));
        QVERIFY(args.open(QIODevice::WriteOnly) && args.write("-m 1G\n") > 0);
        args.close();
        Vm vm(dir.path());
        VmPane pane;
        auto *apply = pane.findChild<QPushButton *>("apply");
        QVERIFY(apply);

        pane.setVm(&vm);
        pane.setTab(VmPane::Settings);
        pane.setPage(VmPane::Arguments);
        pane.show();
        /* shortcuts go to the active window */
        pane.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&pane));
        auto *editor = pane.findChild<ArgsEditor *>();
        QVERIFY(editor && editor->isVisible());
        editor->setFocus();
        QTRY_VERIFY(editor->hasFocus());

        /* nothing to apply: nothing happens */
        QTest::keyClick(editor, Qt::Key_S, Qt::ControlModifier);
        QVERIFY(!apply->isEnabled());

        editor->moveCursor(QTextCursor::End);
        QTest::keyClicks(editor, "-smp 2");
        QTRY_VERIFY(apply->isEnabled());
        QTest::keyClick(editor, Qt::Key_S, Qt::ControlModifier);
        QTRY_VERIFY(!pane.isModified());
        QVERIFY(!apply->isEnabled());
        QVERIFY(args.open(QIODevice::ReadOnly));
        const QByteArray saved = args.readAll();
        QVERIFY2(saved.contains("-smp 2"), saved.constData());
        /* the letter itself did not go into the text */
        QVERIFY(!editor->toPlainText().contains("-smp 2s"));

        /* with the keys outside the page (the tabs, the list of VMs after a
           click on them): still Apply, while the Settings tab shows */
        QTest::keyClicks(editor, " ");
        QTRY_VERIFY(apply->isEnabled());
        auto *tabs = pane.findChild<QTabBar *>();
        QVERIFY(tabs);
        tabs->setFocusPolicy(Qt::StrongFocus);
        tabs->setFocus();
        QTRY_VERIFY(tabs->hasFocus());
        QTest::keyClick(tabs, Qt::Key_S, Qt::ControlModifier);
        QTRY_VERIFY(!pane.isModified());

        /* on another tab, Ctrl+S is not the settings' */
        editor->setFocus();
        QTest::keyClicks(editor, "x");
        QTRY_VERIFY(apply->isEnabled());
        pane.setTab(VmPane::Details);
        QTest::keyClick(&pane, Qt::Key_S, Qt::ControlModifier);
        QVERIFY(pane.isModified());
    }

    /* The boot order on the Storage page: checked devices, in order; Apply writes it */
    void storagePageBootOrder()
    {
        QTemporaryDir dir;
        StoragePage page(dir.path());
        auto *list = page.findChild<QListWidget *>("bootOrder");
        auto *up = page.findChild<QPushButton *>("bootUp");
        auto *down = page.findChild<QPushButton *>("bootDown");
        QVERIFY(list && up && down);
        auto rows = [list]() {
            QStringList rows;
            for (int i = 0; i < list->count(); i++) {
                const QListWidgetItem *item = list->item(i);
                rows << (!(item->flags() & Qt::ItemIsEnabled) ? "-"
                         : item->checkState() == Qt::Checked  ? "+"
                                                               : "o") +
                            item->text();
            }
            return rows.join(" | ");
        };

        /* no order of its own: the firmware's, all checked */
        ArgsFile args = ArgsFile::parse("-machine q35\n"
                                        "-drive file=x.iso,media=cdrom,readonly=on\n"
                                        "-drive file=disk.qcow2,format=qcow2,if=virtio\n"
                                        "-drive file=old.img,if=scsi\n"
                                        "-netdev user,id=net0\n"
                                        "-device virtio-net-pci,netdev=net0\n");
        page.load(args);
        QCOMPARE(rows(), "+Hard disk: disk.qcow2 | +CD/DVD: x.iso | "
                         "+Network (PXE): virtio-net-pci | -Hard disk: old.img");
        QVERIFY(!page.isModified());

        /* the CD/DVD drive first, the network never */
        list->setCurrentRow(1);
        QVERIFY(up->isEnabled());
        up->click();
        QCOMPARE(list->currentRow(), 0);
        list->item(2)->setCheckState(Qt::Unchecked);
        QCOMPARE(rows(), "+CD/DVD: x.iso | +Hard disk: disk.qcow2 | "
                         "oNetwork (PXE): virtio-net-pci | -Hard disk: old.img");
        /* the one set up by hand stays last */
        list->setCurrentRow(2);
        QVERIFY(!down->isEnabled());
        QVERIFY(page.isModified());
        page.save(args);
        QCOMPARE(text(args), "-machine q35\n"
                             "-boot strict=on\n"
                             "-drive file=x.iso,media=cdrom,readonly=on,if=none,id=cd0\n"
                             "-device ide-cd,drive=cd0,bootindex=1\n"
                             "-drive file=disk.qcow2,format=qcow2,if=none,id=disk0\n"
                             "-device virtio-blk-pci,drive=disk0,bootindex=2\n"
                             "-drive file=old.img,if=scsi\n"
                             "-netdev user,id=net0\n"
                             "-device virtio-net-pci,netdev=net0\n");
        /* read again as written */
        QVERIFY(!page.isModified());
        QCOMPARE(rows(), "+CD/DVD: x.iso | +Hard disk: disk.qcow2 | "
                         "oNetwork (PXE): virtio-net-pci | -Hard disk: old.img");

        /* one stays checked: the VM needs one to start from */
        list->item(1)->setCheckState(Qt::Unchecked);
        list->item(0)->setCheckState(Qt::Unchecked);
        QCOMPARE(list->item(0)->checkState(), Qt::Checked);
        list->item(1)->setCheckState(Qt::Checked);

        /* removed: out of the order */
        page.findChild<QTableWidget *>("disks")->setCurrentCell(1, 0);
        const auto buttons = page.findChildren<QPushButton *>();
        for (QPushButton *b : buttons) {
            if (b->text() == "&Remove") {
                b->click();
            }
        }
        QCOMPARE(rows(), "+CD/DVD: x.iso | oNetwork (PXE): virtio-net-pci | "
                         "-Hard disk: old.img");
        page.save(args);
        QCOMPARE(text(args), "-machine q35\n"
                             "-boot strict=on\n"
                             "-drive file=x.iso,media=cdrom,readonly=on,if=none,id=cd0\n"
                             "-device ide-cd,drive=cd0,bootindex=1\n"
                             "-drive file=old.img,if=scsi\n"
                             "-netdev user,id=net0\n"
                             "-device virtio-net-pci,netdev=net0\n");

        /* a drive added, with no disc: in an order set, last of those it
           can take, unchecked, as the VM would leave it */
        QTimer::singleShot(0, []() {
            if (QWidget *dialog = QApplication::activeModalWidget()) {
                dialog->close();
            }
        });
        for (QPushButton *b : page.findChildren<QPushButton *>()) {
            if (b->text() == "Add &CD/DVD Drive") {
                b->click();
            }
        }
        QCOMPARE(rows(), "+CD/DVD: x.iso | oNetwork (PXE): virtio-net-pci | "
                         "-Hard disk: old.img | oCD/DVD: empty");
        QVERIFY(page.isModified());
        page.save(args);
        QVERIFY2(text(args).contains("-drive media=cdrom,readonly=on\n"), qPrintable(text(args)));
        QCOMPARE(text(args).count("bootindex"), 1);
    }
};

QTEST_MAIN(TestSettingsPages)
#include "test_settingspages.moc"
