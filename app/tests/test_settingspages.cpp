// SPDX-License-Identifier: GPL-2.0-or-later
#include <QRadioButton>
#include <QTest>

#include "core/argsfile.h"
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
    }
};

QTEST_MAIN(TestSettingsPages)
#include "test_settingspages.moc"
