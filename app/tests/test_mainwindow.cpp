// SPDX-License-Identifier: GPL-2.0-or-later
#include <QAbstractItemView>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QSplitter>
#include <QStatusBar>
#include <QStyleFactory>
#include <QTabBar>
#include <QTemporaryDir>
#include <QToolBar>
#include <QToolButton>
#include <QWidgetAction>

#include <algorithm>
#include <memory>

#include "core/paths.h"
#include "core/vmstore.h"
#include "scales.h"
#include "ui/mainwindow.h"
#include "ui/vmpane.h"

/*
 * The main window's rows: one bar under the title for the menus, the
 * actions and the VM's tabs, the menu bar shown on request; the list of
 * VMs hidden and shown, a drop-down of the VMs in the bar meanwhile; the
 * shortcuts of every menu with the menu bar hidden; and the bar at every
 * width the window can have, at every scale: nothing in an overflow menu,
 * the actions' text going in order.
 *
 * Only widgets' object names, actions' texts and Qt's API: the test
 * builds against older windows too, to show what they lacked.
 */

namespace {

const char kAlpha[] = "-name Alpha\n-machine q35\n-m 2G\n-display dbus,p2p=yes,gl=on\n";
const char kBravo[] = "-name Bravo\n-machine q35\n-m 1G\n-display sdl\n";
const char kLong[] = "-name A Virtual Machine With a Rather Long Name\n-machine q35\n-m 1G\n";

/* Text-less in this order when the bar is short of room */
const QStringList kTextOrder = {"Send Ctrl+Alt+Del", "Full Screen", "Force Off", "Shut Down",
                                "Pause", "New…", "Start"};

int s_ambiguous = 0;

QString plain(QString text)
{
    return text.remove('&');
}

QAction *action(const QWidget *window, const QString &text)
{
    for (QAction *a : window->findChildren<QAction *>()) {
        if (plain(a->text()) == text) {
            return a;
        }
    }
    return nullptr;
}

QToolBar *bar(const QWidget *window)
{
    return window->findChild<QToolBar *>("toolbar");
}

QMenuBar *menuBarOf(const QMainWindow *window)
{
    return qobject_cast<QMenuBar *>(window->menuWidget());
}

QListWidget *list(const QWidget *window)
{
    return window->findChild<QListWidget *>("vms");
}

QComboBox *chooser(const QWidget *window)
{
    return window->findChild<QComboBox *>("vmChooser");
}

QToolButton *menuButton(const QWidget *window)
{
    QToolBar *b = bar(window);
    return b ? b->findChild<QToolButton *>("menuButton") : nullptr;
}

/* The bar's buttons for the VM's tabs, in their order */
QList<QToolButton *> tabButtons(const QWidget *window)
{
    QToolBar *b = bar(window);
    QWidget *tabs = b ? b->findChild<QWidget *>("tabs") : nullptr;
    QList<QToolButton *> buttons;

    if (tabs) {
        for (QToolButton *button : tabs->findChildren<QToolButton *>()) {
            if (button->objectName() != "tabMenu") {
                buttons << button;
            }
        }
    }
    return buttons;
}

/* Every action of @menu and of its submenus, the submenus' own included */
void collect(const QMenu *menu, QList<QAction *> *actions)
{
    for (QAction *a : menu->actions()) {
        if (a->isSeparator()) {
            continue;
        }
        actions->append(a);
        if (a->menu()) {
            collect(a->menu(), actions);
        }
    }
}

QList<QAction *> menuBarActions(const QMainWindow *window)
{
    QList<QAction *> actions;

    if (const QMenuBar *menuBar = menuBarOf(window)) {
        for (QAction *a : menuBar->actions()) {
            actions << a;
            if (a->menu()) {
                collect(a->menu(), &actions);
            }
        }
    }
    return actions;
}

/* @window shown, laid out, and active: its shortcuts work */
void settle(QWidget *window)
{
    Scales::settle(window);
    window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(window));
}

/* The layouts' requests, some of which make more, and the rest */
void layOut()
{
    for (int i = 0; i < 4; i++) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        QCoreApplication::processEvents();
    }
}

/* The layouts' requests alone, without painting the window: for many sizes */
void layOutOnly()
{
    for (int i = 0; i < 4; i++) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    }
}

void useStyle(const QString &style, int points)
{
    QFont font = QApplication::font();

    QApplication::setStyle(style);
    font.setPointSize(points);
    QApplication::setFont(font);
}

/* @action's name in kTextOrder, Resume being Pause */
QString orderName(const QAction *action)
{
    const QString name = plain(action->text());
    return name == "Resume" ? QString("Pause") : name;
}

/* The actions of kTextOrder that show their text in the bar */
QStringList withText(const QToolBar *bar)
{
    QStringList shown;

    for (QAction *a : bar->actions()) {
        auto *button = qobject_cast<QToolButton *>(bar->widgetForAction(a));
        if (a->isVisible() && button && button->toolButtonStyle() != Qt::ToolButtonIconOnly &&
            kTextOrder.contains(orderName(a))) {
            shown << orderName(a);
        }
    }
    return shown;
}

/* What is wrong with the bar of @window as it is laid out now */
QStringList barProblems(QMainWindow *window, const QString &where)
{
    QStringList wrong;
    QToolBar *b = bar(window);
    QRect previous;

    if (!b) {
        return {where + ": no bar"};
    }
    if (const auto *more = b->findChild<QToolButton *>("qt_toolbar_ext_button");
        more && more->isVisible()) {
        wrong << where + ": the bar's overflow button shows";
    }
    if (b->width() > window->width()) {
        wrong << QString("%1: the bar %2 wide in a window %3 wide")
                     .arg(where).arg(b->width()).arg(window->width());
    }
    for (QAction *a : b->actions()) {
        QWidget *w = b->widgetForAction(a);
        if (!a->isVisible() || !w) {
            continue;
        }
        if (!w->isVisible()) {
            wrong << QString("%1: '%2' hidden").arg(where, plain(a->text()));
            continue;
        }
        const QRect r = w->geometry();
        if (r.width() > 0 && !b->rect().contains(r)) {
            wrong << QString("%1: '%2' out of the bar").arg(where, plain(a->text()));
        }
        if (r.width() > 0 && previous.isValid() && r.left() < previous.right() + 1) {
            wrong << QString("%1: '%2' over the item before it").arg(where, plain(a->text()));
        }
        if (r.width() > 0) {
            previous = r;
        }
    }
    /* the actions without text first in kTextOrder, those with text after */
    const QStringList text = withText(b);
    bool seen = false;
    for (const QString &name : kTextOrder) {
        const bool has = text.contains(name);
        if (seen && !has) {
            wrong << QString("%1: '%2' without text after one with").arg(where, name);
        }
        seen = seen || has;
    }
    return wrong;
}

}

class TestMainWindow : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;
    std::unique_ptr<VmStore> m_store;

    bool write(const QString &path, const QByteArray &text)
    {
        QFile f(path);

        return QDir().mkpath(QFileInfo(path).path()) && f.open(QIODevice::WriteOnly) &&
               f.write(text) == text.size();
    }

    /* A window as main.cpp makes it, with the settings saved before */
    std::unique_ptr<MainWindow> window()
    {
        auto window = std::make_unique<MainWindow>(m_store.get());
        window->resize(1280, 800);
        return window;
    }

private slots:
    void initTestCase()
    {
        Scales::quieter();
        Scales::bigScreen();
        QVERIFY(m_tmp.isValid());
        QIcon::setThemeSearchPaths(QIcon::themeSearchPaths() << "/usr/share/icons");
        if (QDir("/usr/share/icons/breeze").exists()) {
            QIcon::setThemeName("breeze");
        }
        /* settings and stack in here, and no QEMU to read the documentation of */
        qputenv("XDG_DATA_HOME", m_tmp.filePath("data").toUtf8());
        qputenv("XDG_CONFIG_HOME", m_tmp.filePath("config").toUtf8());
        qputenv("XDG_CACHE_HOME", m_tmp.filePath("cache").toUtf8());
        Paths::setQemuBinary("/nonexistent/qemu-system-x86_64");
        QVERIFY(write(m_tmp.filePath("vms/alpha/vm.args"), kAlpha));
        QVERIFY(write(m_tmp.filePath("vms/bravo/vm.args"), kBravo));
        QVERIFY(write(m_tmp.filePath("vms/long/vm.args"), kLong));
        m_store = std::make_unique<VmStore>(m_tmp.filePath("vms"));
        QCOMPARE(m_store->vms().size(), 3);
        /* Qt's warning for a key two shortcuts of the window want */
        static QtMessageHandler previous = nullptr;
        previous = qInstallMessageHandler(
            [](QtMsgType type, const QMessageLogContext &context, const QString &message) {
                if (message.contains("Ambiguous shortcut")) {
                    s_ambiguous++;
                }
                if (previous) {
                    previous(type, context, message);
                }
            });
    }

    void init()
    {
        /* each test from the defaults: the window's settings are in there */
        QFile::remove(Paths::settingsPath());
        Paths::setQemuBinary("/nonexistent/qemu-system-x86_64");
        useStyle("Fusion", 9);
        s_ambiguous = 0;
    }

    void cleanupTestCase()
    {
        Paths::setQemuBinary({});
    }

    /* One row above the VMs: the bar, which has the tabs and the menus' button */
    void oneBar()
    {
        auto w = window();
        settle(w.get());

        QMenuBar *menuBar = menuBarOf(w.get());
        QVERIFY(menuBar);
        QVERIFY2(!menuBar->isVisible(), "the menu bar shows by default");
        QToolButton *menu = menuButton(w.get());
        QVERIFY2(menu && menu->isVisible(), "no menu button in the bar");
        QVERIFY(menu->menu() || menu->defaultAction()->menu());
        for (const QTabBar *tabs : w->findChildren<QTabBar *>()) {
            QVERIFY2(!tabs->isVisible(), "a row of tabs under the bar");
        }
        const QList<QToolButton *> tabs = tabButtons(w.get());
        QCOMPARE(tabs.size(), 5);
        QCOMPARE(plain(tabs[0]->text()), QString("Console"));
        QCOMPARE(plain(tabs[4]->text()), QString("Logs"));
        /* nothing between the bar and the window's content */
        QWidget *central = w->centralWidget();
        QCOMPARE(central->geometry().top(), bar(w.get())->geometry().bottom() + 1);
    }

    /* The tabs of the bar are the pane's: either way round */
    void tabs()
    {
        auto w = window();
        settle(w.get());
        auto *pane = w->findChild<VmPane *>();
        const QList<QToolButton *> tabs = tabButtons(w.get());
        QCOMPARE(tabs.size(), 5);

        tabs[VmPane::Logs]->click();
        QCOMPARE(pane->tab(), VmPane::Logs);
        QVERIFY(tabs[VmPane::Logs]->isChecked());
        pane->setTab(VmPane::Details);
        QVERIFY(tabs[VmPane::Details]->isChecked());
        QVERIFY(!tabs[VmPane::Logs]->isChecked());
        /* Machine > Show Log too */
        action(w.get(), "Show Log")->trigger();
        QVERIFY(tabs[VmPane::Logs]->isChecked());
        /* and View lists them */
        QList<QAction *> all = menuBarActions(w.get());
        for (const char *name : {"Console", "Details", "Settings", "Snapshots", "Logs"}) {
            QVERIFY2(std::any_of(all.begin(), all.end(),
                                 [&](QAction *a) {
                                     return a->isCheckable() && plain(a->text()) == name;
                                 }),
                     qPrintable(QString(name) + " not in a menu"));
        }
    }

    /* Ctrl+M, remembered; the button stands for the menu bar meanwhile */
    void menuBarToggle()
    {
        {
            auto w = window();
            settle(w.get());
            QAction *show = action(w.get(), "Show Menu Bar");
            QVERIFY(show);
            QVERIFY(!show->isChecked());
            QTest::keyClick(list(w.get()), Qt::Key_M, Qt::ControlModifier);
            QVERIFY(show->isChecked());
            QTRY_VERIFY(menuBarOf(w.get())->isVisible());
            QTRY_VERIFY(!menuButton(w.get())->isVisible());
            w->close();
        }
        {
            auto w = window();
            settle(w.get());
            QVERIFY(menuBarOf(w.get())->isVisible());
            QVERIFY(!menuButton(w.get())->isVisible());
            QTest::keyClick(list(w.get()), Qt::Key_M, Qt::ControlModifier);
            QTRY_VERIFY(!menuBarOf(w.get())->isVisible());
            QTRY_VERIFY(menuButton(w.get())->isVisible());
            w->close();
        }
        auto w = window();
        settle(w.get());
        QVERIFY(!menuBarOf(w.get())->isVisible());
    }

    /*
     * Every action is in a menu, which the menu button has too; every
     * action of the bar is in a menu, but the button of the list, which
     * does what Show Library does, and the menu button
     */
    void everyActionInAMenu()
    {
        auto w = window();
        settle(w.get());
        const QList<QAction *> menus = menuBarActions(w.get());
        QToolButton *button = menuButton(w.get());
        QVERIFY(button);
        QMenu *menu = button->menu() ? button->menu() : button->defaultAction()->menu();
        QVERIFY(menu);
        QList<QAction *> behind;
        collect(menu, &behind);

        for (QAction *a : menus) {
            QVERIFY2(behind.contains(a), qPrintable(plain(a->text()) + " not behind the button"));
        }
        QVERIFY(behind.contains(action(w.get(), "Show Menu Bar")));
        for (QAction *a : bar(w.get())->actions()) {
            if (a->isSeparator() || a->menu() || qobject_cast<QWidgetAction *>(a)) {
                continue;
            }
            if (a->objectName() == "listButton") {
                QAction *library = action(w.get(), "Show Library");
                const bool shown = library->isChecked();
                a->trigger();
                QCOMPARE(library->isChecked(), !shown);
                a->trigger();
                continue;
            }
            QVERIFY2(menus.contains(a), qPrintable(plain(a->text()) + " in no menu"));
        }
        /* the bar's own context menu cannot hide it: it has the menus */
        std::unique_ptr<QMenu> popup(w->createPopupMenu());
        if (popup) {
            for (QAction *a : popup->actions()) {
                QVERIFY2(a != bar(w.get())->toggleViewAction(), "the bar can be hidden");
            }
        }
    }

    /* With the menu bar hidden, each shortcut of the menus triggers its action, once */
    void shortcutsWithoutMenuBar()
    {
        auto w = window();
        settle(w.get());
        menuBarOf(w.get())->hide();
        layOut();
        QListWidget *vms = list(w.get());
        int tried = 0;

        for (QAction *a : menuBarActions(w.get())) {
            if (a->shortcut().isEmpty() || !a->isEnabled() || a->menu()) {
                continue;
            }
            /* what it does is not the point: dialogs would wait for an answer */
            const bool checked = a->isChecked();
            disconnect(a, &QAction::triggered, nullptr, nullptr);
            QSignalSpy spy(a, &QAction::triggered);
            vms->setFocus();
            QTest::keySequence(w.get(), a->shortcut());
            QVERIFY2(spy.count() == 1,
                     qPrintable(QString("%1 (%2): %3 times")
                                    .arg(plain(a->text()), a->shortcut().toString())
                                    .arg(spy.count())));
            if (a->isCheckable() && a->isChecked() != checked) {
                a->setChecked(checked);
            }
            menuBarOf(w.get())->hide();
            layOut();
            tried++;
        }
        QVERIFY(tried >= 10);
        QCOMPARE(s_ambiguous, 0);
    }

    /* Ctrl+S on the Settings tab applies, wherever the keys are; Settings elsewhere */
    void ctrlS()
    {
        auto w = window();
        settle(w.get());
        auto *pane = w->findChild<VmPane *>();
        QListWidget *vms = list(w.get());
        w->select("bravo");
        QTRY_COMPARE(pane->vm(), m_store->find("bravo"));

        /* Settings, from the Console tab */
        pane->setTab(VmPane::Console);
        vms->setFocus();
        QTest::keyClick(vms, Qt::Key_S, Qt::ControlModifier);
        QCOMPARE(pane->tab(), VmPane::Settings);

        pane->setPage(VmPane::General);
        auto *name = w->findChild<QLineEdit *>("name");
        QVERIFY(name);
        QTRY_VERIFY(name->isVisible());
        auto *apply = w->findChild<QPushButton *>("apply");
        QVERIFY(apply);
        name->setText("Bravo Two");
        /* once the pane saw the change */
        QTRY_VERIFY(apply->isEnabled());
        vms->setFocus();
        QTest::keyClick(vms, Qt::Key_S, Qt::ControlModifier);
        QTRY_VERIFY_WITH_TIMEOUT(!pane->isModified(), 2000);
        QCOMPARE(m_store->find("bravo")->name(), QString("Bravo Two"));
        QCOMPARE(s_ambiguous, 0);

        /* back as it was */
        name->setText("Bravo");
        QTRY_VERIFY(apply->isEnabled());
        QTest::keyClick(name, Qt::Key_S, Qt::ControlModifier);
        QTRY_VERIFY_WITH_TIMEOUT(!pane->isModified(), 2000);
        QCOMPARE(s_ambiguous, 0);
    }

    /*
     * F9 and the bar's button hide and show the list, remembered; the VMs
     * in the bar meanwhile; the list as wide as it was, however it went
     */
    void library()
    {
        int width = 0;
        /* KDE's style where it is installed: it does not widen a combo box's
           list to its items as Fusion does */
        if (QStyleFactory::keys().contains("Breeze", Qt::CaseInsensitive)) {
            useStyle("Breeze", 10);
        }
        {
            auto w = window();
            settle(w.get());
            QListWidget *vms = list(w.get());
            QVERIFY(vms->isVisible());
            QComboBox *vmChooser = chooser(w.get());
            QVERIFY2(vmChooser, "no VMs in the bar");
            QVERIFY(!vmChooser->isVisible());
            auto *splitter = qobject_cast<QSplitter *>(vms->parentWidget());
            QVERIFY(splitter);
            splitter->setSizes({333, w->width() - 333});
            layOut();
            width = vms->width();
            QVERIFY(qAbs(width - 333) <= 1);

            QTest::keyClick(vms, Qt::Key_F9);
            layOut();
            QVERIFY(!vms->isVisible());
            QVERIFY(vmChooser->isVisible());
            QCOMPARE(vmChooser->currentText(), w->current()->name());
            /* its list as wide as the items want, their names bold */
            vmChooser->showPopup();
            QTRY_VERIFY(vmChooser->view()->isVisible());
            const QString longest = "A Virtual Machine With a Rather Long Name";
            QFont bold = vmChooser->view()->font();
            bold.setBold(true);
            const int item =
                vmChooser->view()->sizeHintForIndex(
                    vmChooser->model()->index(vmChooser->findText(longest), 0)).width() +
                QFontMetrics(bold).horizontalAdvance(longest) -
                QFontMetrics(vmChooser->view()->font()).horizontalAdvance(longest);
            QVERIFY2(vmChooser->view()->viewport()->width() >= item,
                     qPrintable(QString("the list of VMs %1 wide for an item %2 wide")
                                    .arg(vmChooser->view()->viewport()->width())
                                    .arg(item)));
            vmChooser->hidePopup();
            QTRY_VERIFY(!vmChooser->view()->isVisible());
            /* another VM from the bar */
            const int bravo = vmChooser->findText("Bravo");
            QVERIFY(bravo >= 0);
            vmChooser->setCurrentIndex(bravo);
            emit vmChooser->activated(bravo);
            QTRY_COMPARE(w->current(), m_store->find("bravo"));
            w->close();
        }
        {
            auto w = window();
            settle(w.get());
            QVERIFY(!list(w.get())->isVisible());
            QVERIFY(chooser(w.get())->isVisible());
            QCOMPARE(w->current(), m_store->find("bravo"));
            QCOMPARE(chooser(w.get())->currentText(), QString("Bravo"));
            /* the bar's button */
            QAction *button = nullptr;
            for (QAction *a : bar(w.get())->actions()) {
                if (a->objectName() == "listButton") {
                    button = a;
                }
            }
            QVERIFY(button);
            button->trigger();
            layOut();
            QVERIFY(list(w.get())->isVisible());
            QVERIFY(!chooser(w.get())->isVisible());
            QVERIFY2(qAbs(list(w.get())->width() - width) <= 1,
                     qPrintable(QString("the list %1 wide, was %2")
                                    .arg(list(w.get())->width())
                                    .arg(width)));
            /* the list's selection is the bar's */
            w->select("alpha");
            QCOMPARE(chooser(w.get())->currentText(), QString("Alpha"));
            w->close();
        }
        auto w = window();
        settle(w.get());
        QVERIFY(list(w.get())->isVisible());
        QVERIFY(qAbs(list(w.get())->width() - width) <= 1);
    }

    /* F10: the menus under the bar's button, or the menu bar's first one */
    void openMenu()
    {
        auto w = window();
        settle(w.get());
        QToolButton *button = menuButton(w.get());
        QVERIFY(button);
        QMenu *menu = button->menu() ? button->menu() : button->defaultAction()->menu();

        QTest::keyClick(list(w.get()), Qt::Key_F10);
        QTRY_COMPARE(QApplication::activePopupWidget(), menu);
        menu->close();
        QTRY_VERIFY(!QApplication::activePopupWidget());

        action(w.get(), "Show Menu Bar")->trigger();
        QTRY_VERIFY(menuBarOf(w.get())->isVisible());
        QTest::keyClick(list(w.get()), Qt::Key_F10);
        QTRY_VERIFY(QApplication::activePopupWidget());
        QCOMPARE(menuBarOf(w.get())->activeAction(), menuBarOf(w.get())->actions().value(0));
        QApplication::activePopupWidget()->close();
    }

    /* No VM yet: the welcome page, and nothing in the bar to choose a tab or a VM */
    void noVms()
    {
        QTemporaryDir empty;
        VmStore store(empty.path());
        MainWindow w(&store);
        w.resize(1280, 800);
        settle(&w);
        for (QToolButton *tab : tabButtons(&w)) {
            QVERIFY(!tab->isEnabled());
        }
        action(&w, "Show Library")->trigger();
        layOut();
        QVERIFY(!chooser(&w)->isEnabled());
        QVERIFY(barProblems(&w, "no VMs").isEmpty());
        action(&w, "Show Library")->trigger();
    }

    /* An older vitrine's toolbar menu could hide the bar: it shows, with the menus */
    void barAlwaysShown()
    {
        {
            auto w = window();
            settle(w.get());
            bar(w.get())->hide();
            w->close();
        }
        auto w = window();
        settle(w.get());
        QVERIFY(bar(w.get())->isVisible());
    }

    /*
     * At every width the window can have, the bar shows all its items
     * in its width, the actions' text going in order, Fusion and Breeze,
     * the list shown and hidden, the menu bar hidden and shown, at a
     * range of scales
     */
    void fits_data()
    {
        QTest::addColumn<QString>("style");
        QTest::addColumn<int>("points");
        QTest::newRow("fusion") << "Fusion" << 9;
        if (QStyleFactory::keys().contains("Breeze", Qt::CaseInsensitive)) {
            QTest::newRow("breeze") << "Breeze" << 10;
        }
    }

    void fits()
    {
        QFETCH(QString, style);
        QFETCH(int, points);
        QList<int> scales = Scales::list();
        if (!qEnvironmentVariableIsSet("QT_SCALE_FACTOR") &&
            !qEnvironmentVariableIsSet("VITRINE_SCALES")) {
            /* 1, 1.25, 1.3, 1.5, 1.75, 2.35, 3: all of them would be slow */
            scales = {120, 150, 156, 180, 210, 282, 360};
        }
        useStyle(style, points);

        QStringList wrong;
        for (int n : std::as_const(scales)) {
            Scales::set(n);
            for (const bool libraryShown : {true, false}) {
                for (const bool menuBarShown : {false, true}) {
                    auto w = window();
                    settle(w.get());
                    if (!libraryShown) {
                        action(w.get(), "Show Library")->trigger();
                    }
                    if (menuBarShown) {
                        if (QAction *a = action(w.get(), "Show Menu Bar")) {
                            a->trigger();
                        } else {
                            menuBarOf(w.get())->show();
                        }
                    }
                    layOut();
                    const int least = w->minimumSizeHint().width();
                    int textless = -1;
                    for (int width = 1600; width >= least - 40; width -= 11) {
                        w->resize(width, 800);
                        layOutOnly();
                        const QString where = QString("%1 n=%2%3%4 %5 wide (least %6)")
                                                  .arg(style).arg(n)
                                                  .arg(libraryShown ? "" : " no list")
                                                  .arg(menuBarShown ? " menu bar" : "")
                                                  .arg(w->width()).arg(least);
                        wrong << barProblems(w.get(), where);
                        /* fewer texts in a narrower window, never more */
                        const int now = int(kTextOrder.size() - withText(bar(w.get())).size());
                        if (now < textless) {
                            wrong << where + ": an action got its text back, narrower";
                        }
                        textless = now;
                        if (width == 1600 && now > 0) {
                            wrong << where + ": actions without text in a wide window";
                        }
                    }
                }
            }
            Scales::set(120);
            QVERIFY2(QGuiApplication::allWindows().isEmpty(), "a window left over");
            if (wrong.size() > 20) {
                break;
            }
        }
        for (const QString &w : wrong.mid(0, 20)) {
            qWarning("%s", qPrintable(w));
        }
        QVERIFY2(wrong.isEmpty(), qPrintable(QString("%1 problems").arg(wrong.size())));
    }
};

QTEST_MAIN(TestMainWindow)
#include "test_mainwindow.moc"
