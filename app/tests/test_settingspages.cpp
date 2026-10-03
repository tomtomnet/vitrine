// SPDX-License-Identifier: GPL-2.0-or-later
#include <QCheckBox>
#include <QComboBox>
#include <QRadioButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTest>

#include "core/argsfile.h"
#include "core/paths.h"
#include "ui/settingspages.h"

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
};

QTEST_MAIN(TestSettingsPages)
#include "test_settingspages.moc"
