// SPDX-License-Identifier: GPL-2.0-or-later
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFocusEvent>
#include <QLabel>
#include <QListWidget>
#include <QRadioButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include "core/argsfile.h"
#include "core/paths.h"
#include "core/vmstore.h"
#include "ui/settingspages.h"
#include "ui/vmpane.h"

/*
 * The pages as VmPane drives them, without a screen: load() when a page is
 * shown or the VM changed outside, save() when it is left or applied, and
 * isModified() for the Apply and Discard buttons and for whether an outside
 * change may reload the page
 */
class TestSettingsPages : public QObject
{
    Q_OBJECT

    static QString text(const ArgsFile &args) { return args.toText(); }

private slots:
    void initTestCase()
    {
        /* settings of their own, and no QEMU to read the documentation of */
        QStandardPaths::setTestModeEnabled(true);
        Paths::setQemuBinary("/nonexistent/qemu-system-x86_64");
    }

    void cleanupTestCase()
    {
        Paths::setQemuBinary({});
    }

    /* A VM that shows nowhere has neither radio checked, whatever was before */
    void displayPageClearsItsChoice()
    {
        DisplayPage page;
        auto *embedded = page.findChild<QRadioButton *>("embedded");
        auto *ownWindow = page.findChild<QRadioButton *>("ownWindow");
        QVERIFY(embedded && ownWindow);

        /* shown once for vitrine's window, then -display changed by hand */
        ArgsFile args = ArgsFile::parse("-display dbus,p2p=yes,gl=on\n");
        page.load(args);
        QVERIFY(embedded->isChecked());
        QVERIFY(!page.isModified());

        args = ArgsFile::parse("-display egl-headless\n");
        page.load(args);
        QVERIFY(!embedded->isChecked());
        QVERIFY(!ownWindow->isChecked());
        QVERIFY(!page.isModified());
        page.save(args);
        QCOMPARE(text(args), "-display egl-headless\n");

        /* a click, then Discard: the page loads the arguments again */
        ownWindow->click();
        QVERIFY(ownWindow->isChecked());
        QVERIFY(page.isModified());
        page.load(args);
        QVERIFY(!ownWindow->isChecked());
        QVERIFY(!page.isModified());

        /* and the buttons stay exclusive for the user */
        ownWindow->click();
        embedded->click();
        QVERIFY(embedded->isChecked());
        QVERIFY(!ownWindow->isChecked());
        page.save(args);
        QCOMPARE(text(args), "-display dbus,p2p=yes\n");
        QVERIFY(!page.isModified());

        /* VNC alone: QEMU opens no window, which SDL adds */
        args = ArgsFile::parse("-vnc :0\n");
        page.load(args);
        QVERIFY(!embedded->isChecked());
        QVERIFY(!ownWindow->isChecked());
        ownWindow->click();
        page.save(args);
        QCOMPARE(text(args), "-vnc :0\n-display sdl\n");
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

    /* Keyboard focus crossing the lists of pages chooses none */
    void paneKeepsItsPageOnFocus()
    {
        QTemporaryDir dir;
        QFile args(dir.filePath("vm.args"));
        QVERIFY(args.open(QIODevice::WriteOnly) && args.write("-m 1G\n") > 0);
        args.close();
        Vm vm(dir.path());
        VmPane pane;
        auto *pages = pane.findChild<QListWidget *>("pages");
        auto *advanced = pane.findChild<QListWidget *>("advancedPages");
        QVERIFY(pages && advanced);

        pane.setVm(&vm);
        pane.setTab(VmPane::Settings);
        pane.setPage(VmPane::Machine);
        QCOMPARE(pane.page(), VmPane::Machine);
        /* Shift+Tab from the Advanced button into the simple pages */
        QFocusEvent backtab(QEvent::FocusIn, Qt::BacktabFocusReason);
        QCoreApplication::sendEvent(pages, &backtab);
        QCOMPARE(pane.page(), VmPane::Machine);

        /* Tab into the advanced ones from a simple page */
        pane.setPage(VmPane::Network);
        QCOMPARE(pane.page(), VmPane::Network);
        QFocusEvent tab(QEvent::FocusIn, Qt::TabFocusReason);
        QCoreApplication::sendEvent(advanced, &tab);
        QCOMPARE(pane.page(), VmPane::Network);

        /* the keys and the mouse still choose */
        QTest::keyClick(advanced, Qt::Key_Down);
        QCOMPARE(pane.page(), VmPane::Machine);
        pages->setCurrentRow(VmPane::Display);
        QCOMPARE(pane.page(), VmPane::Display);
    }
};

QTEST_MAIN(TestSettingsPages)
#include "test_settingspages.moc"
