// SPDX-License-Identifier: GPL-2.0-or-later
#include <QAbstractItemView>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QSplitter>
#include <QStatusBar>
#include <QStyleFactory>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QWidgetAction>

#include <algorithm>
#include <memory>

#include "core/paths.h"
#include "core/tasks.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "nested/desktopview.h"
#include "scales.h"
#include "ui/mainwindow.h"
#include "ui/vmconsole.h"
#include "ui/vmpane.h"

/*
 * The main window's rows: one bar under the title, with the menus and
 * the VM's tabs; the list of VMs collapsed to
 * a strip of icons and expanded, never hidden; the shortcuts of every menu,
 * F10 and Alt with a menu's letter; and the bar at every width the window
 * can have, at every scale: nothing in an overflow menu, the menus whole,
 * the tabs one button only when short of room.
 *
 * Only widgets' object names, actions' texts and Qt's API: the test
 * builds against older windows too, to show what they lacked.
 */

namespace {

const char kAlpha[] = "-name Alpha\n-machine q35\n-m 2G\n-display dbus,p2p=yes,gl=on\n";
const char kBravo[] = "-name Bravo\n-machine q35\n-m 1G\n-display sdl\n";
const char kLong[] = "-name A Virtual Machine With a Rather Long Name\n-machine q35\n-m 1G\n";

/* The VM's power actions, in the Machine menu alone */
const QStringList kPower = {"Start", "Pause", "Shut Down", "Reset", "Force Off"};
/* Actions that were in the bar, in the menus only now */
const QStringList kMenusOnly = {"New…", "Full Screen", "Send Ctrl+Alt+Del", "USB Devices"};

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

/* The window's menus, in the bar */
QMenuBar *menusOf(const QWidget *window)
{
    QToolBar *b = bar(window);
    return b ? b->findChild<QMenuBar *>() : nullptr;
}

QListWidget *list(const QWidget *window)
{
    return window->findChild<QListWidget *>("vms");
}

/* The side with the list, collapsed to its icons or not */
QWidget *sidebar(const QWidget *window)
{
    return window->findChild<QWidget *>("sidebar");
}

bool collapsed(const QWidget *window)
{
    const QWidget *side = sidebar(window);
    return side && side->property("collapsed").toBool();
}

/* What F9 and the bar's list button do, as View has it now */
QAction *collapseAction(const QWidget *window)
{
    for (QAction *a : window->findChildren<QAction *>()) {
        if (a->shortcut() == QKeySequence(Qt::Key_F9)) {
            return a;
        }
    }
    return nullptr;
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

/* Every action of the window's menus, the menus' own included */
QList<QAction *> menuActions(const QWidget *window)
{
    QList<QAction *> actions;

    if (const QMenuBar *menus = menusOf(window)) {
        for (QAction *a : menus->actions()) {
            actions << a;
            if (a->menu()) {
                collect(a->menu(), &actions);
            }
        }
    }
    return actions;
}

/* The menu of the bar named @title, without its mnemonic */
QMenu *menuOf(const QWidget *window, const QString &title)
{
    if (const QMenuBar *menus = menusOf(window)) {
        for (QAction *a : menus->actions()) {
            if (plain(a->text()) == title) {
                return a->menu();
            }
        }
    }
    return nullptr;
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

/* @action's name, Resume being Pause */
QString nameOf(const QAction *action)
{
    const QString name = plain(action->text());
    return name == "Resume" ? QString("Pause") : name;
}

/* Whether the bar of @window shows its tabs as one button */
bool compactTabs(const QWidget *window)
{
    QToolBar *b = bar(window);
    const auto *button = b ? b->findChild<QToolButton *>("tabMenu") : nullptr;
    return button && button->isVisible();
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
    /* the menus whole: none in the menu bar's own overflow menu */
    QMenuBar *menus = menusOf(window);
    if (!menus || !menus->isVisible()) {
        wrong << where + ": no menus";
    } else {
        if (const auto *more = menus->findChild<QToolButton *>("qt_menubar_ext_button");
            more && more->isVisible()) {
            wrong << where + ": the menu bar's overflow button shows";
        }
        for (QAction *a : menus->actions()) {
            const QRect r = menus->actionGeometry(a);
            if (!r.isValid() || !menus->rect().contains(r)) {
                wrong << QString("%1: menu '%2' cut").arg(where, plain(a->text()));
            }
        }
    }
    return wrong;
}

}

/* The guest's desktop in the console, as NestedView shows it, without a guest or Wayland */
class StandInDesktop : public DesktopView
{
    Q_OBJECT

public:
    StandInDesktop(const QString &socket, QObject *parent)
        : DesktopView(parent), m_socket(socket), m_widget(new QWidget)
    {
        m_widget->setFocusPolicy(Qt::StrongFocus);
    }
    ~StandInDesktop() override { delete m_widget; }

    QString socket() const override { return m_socket; }
    QWidget *widget() const override { return m_widget; }
    bool isFullScreen() const override { return m_fullScreen; }
    void setFullScreen(bool on) override
    {
        if (on != m_fullScreen) {
            m_fullScreen = on;
            Q_EMIT fullScreenChanged(on);
        }
    }
    void setInputEnabled(bool on) override { m_input = on; }
    bool inputEnabled() const { return m_input; }
    bool hasKeyboard() const override { return m_input && m_widget->hasFocus(); }
    void focus() override { m_widget->setFocus(); }
    int connections() const override { return m_connections; }
    bool isMapped() const override { return m_mapped; }

    /* The guest's proxy connects, its nested KWin's window comes */
    void guestConnects(int count)
    {
        m_connections = count;
        Q_EMIT connectionsChanged(count);
    }
    void guestMaps(bool on)
    {
        m_mapped = on;
        Q_EMIT mappedChanged(on);
    }

private:
    QString m_socket;
    QWidget *m_widget;
    bool m_fullScreen = false;
    bool m_input = true;
    int m_connections = 0;
    bool m_mapped = false;
};

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

    /*
     * One row above the VMs: the list's button, the menus, the VM's tabs at
     * the end; no menu bar above it, no separator in it, nor the actions
     * that are in the menus only (the power ones too, the user's choice)
     */
    void oneBar()
    {
        auto w = window();
        settle(w.get());
        QToolBar *b = bar(w.get());
        QVERIFY(b);

        QMenuBar *menus = menusOf(w.get());
        QVERIFY2(menus && menus->isVisible(), "no menus in the bar");
        QVERIFY2(!menus->isNativeMenuBar(), "the menus out of the window");
        QStringList titles;
        for (QAction *a : menus->actions()) {
            titles << plain(a->text());
        }
        QCOMPARE(titles, QStringList({"File", "View", "Machine", "Help"}));
        for (const QMenuBar *other : w->findChildren<QMenuBar *>()) {
            QVERIFY2(other == menus || !other->isVisible(), "a menu bar besides the bar's");
        }

        /* in order: what the bar shows, but the stretch; no separator */
        QStringList order;
        for (QAction *a : b->actions()) {
            QWidget *widget = b->widgetForAction(a);
            QVERIFY2(!a->isVisible() || !a->isSeparator(), "a separator in the bar");
            if (!a->isVisible() || !widget || a->isSeparator()) {
                continue;
            }
            if (widget == menus) {
                order << "menus";
            } else if (widget->objectName() == "tabs") {
                order << "tabs";
            } else if (a->objectName() == "listButton") {
                order << "list";
            } else if (qobject_cast<QToolButton *>(widget)) {
                order << nameOf(a);
            }
        }
        QCOMPARE(order, QStringList({"list", "menus", "tabs"}));
        const QList<QAction *> inMenus = menuActions(w.get());
        for (const QString &name : kMenusOnly + kPower) {
            QAction *a = action(w.get(), name);
            QVERIFY2(a, qPrintable(name + ": no such action"));
            QVERIFY2(inMenus.contains(a), qPrintable(name + " in no menu"));
            QVERIFY2(!b->widgetForAction(a), qPrintable(name + " in the bar"));
        }

        for (const QTabBar *tabs : w->findChildren<QTabBar *>()) {
            QVERIFY2(!tabs->isVisible(), "a row of tabs under the bar");
        }
        const QList<QToolButton *> tabs = tabButtons(w.get());
        QCOMPARE(tabs.size(), 5);
        QCOMPARE(plain(tabs[0]->text()), QString("Console"));
        /* one view only: no menu, no number */
        QVERIFY(!tabs[0]->menu());
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
        QList<QAction *> all = menuActions(w.get());
        for (const char *name : {"Console", "Details", "Settings", "Snapshots", "Logs"}) {
            QVERIFY2(std::any_of(all.begin(), all.end(),
                                 [&](QAction *a) {
                                     return a->isCheckable() && plain(a->text()) == name;
                                 }),
                     qPrintable(QString(name) + " not in a menu"));
        }
    }

    /* Show Menu Bar and Ctrl+M are gone: an older vitrine's setting for them changes nothing */
    void oldMenuBarSetting()
    {
        for (const bool shown : {true, false}) {
            QSettings(Paths::settingsPath(), QSettings::IniFormat)
                .setValue("mainwindow/menubar", shown);
            auto w = window();
            settle(w.get());
            QVERIFY(menusOf(w.get()) && menusOf(w.get())->isVisible());
            QVERIFY(!w->menuWidget() || !w->menuWidget()->isVisible());
            QVERIFY(!action(w.get(), "Show Menu Bar"));
            for (const QAction *a : w->findChildren<QAction *>()) {
                QVERIFY2(a->shortcut() != QKeySequence(Qt::CTRL | Qt::Key_M),
                         qPrintable(plain(a->text()) + " has Ctrl+M"));
            }
            w->close();
        }
    }

    /*
     * Every action of the bar is in a menu, but the button of the list,
     * which does what F9 does
     */
    void everyActionInAMenu()
    {
        auto w = window();
        settle(w.get());
        const QList<QAction *> menus = menuActions(w.get());
        QVERIFY(!menus.isEmpty());

        for (QAction *a : bar(w.get())->actions()) {
            if (a->isSeparator() || a->menu() || qobject_cast<QWidgetAction *>(a)) {
                continue;
            }
            if (a->objectName() == "listButton") {
                QVERIFY(menus.contains(collapseAction(w.get())));
                const bool was = collapsed(w.get());
                a->trigger();
                QCOMPARE(collapsed(w.get()), !was);
                a->trigger();
                QCOMPARE(collapsed(w.get()), was);
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

    /* Among the menus' titles and in each menu, each letter opens one item, not two by turns */
    void mnemonics()
    {
        auto w = window();
        settle(w.get());
        QMenuBar *menus = menusOf(w.get());
        QVERIFY(menus);
        QList<QList<QAction *>> lists = {menus->actions()};
        for (QAction *a : menuActions(w.get())) {
            if (a->menu()) {
                lists << a->menu()->actions();
            }
        }
        QVERIFY(lists.size() >= 5);
        for (const QList<QAction *> &list : std::as_const(lists)) {
            QHash<QKeySequence, QString> seen;
            for (QAction *a : list) {
                const QKeySequence key = QKeySequence::mnemonic(a->text());
                if (a->isSeparator() || !a->isVisible() || key.isEmpty()) {
                    continue;
                }
                QVERIFY2(!seen.contains(key), qPrintable(QString("%1 and %2 both %3")
                                                             .arg(seen.value(key), plain(a->text()),
                                                                  key.toString())));
                seen.insert(key, plain(a->text()));
            }
        }
    }

    /* Each shortcut of the menus triggers its action, once */
    void shortcuts()
    {
        auto w = window();
        settle(w.get());
        layOut();
        QListWidget *vms = list(w.get());
        int tried = 0;

        for (QAction *a : menuActions(w.get())) {
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
     * F9 and the bar's button collapse the list to a strip of the VMs'
     * icons and expand it back, as wide as it was, the actions saying
     * which they do; remembered; never hidden
     */
    void library()
    {
        int width = 0;
        int strip = 0;
        {
            auto w = window();
            settle(w.get());
            QListWidget *vms = list(w.get());
            QWidget *side = sidebar(w.get());
            QVERIFY(side);
            QVERIFY(vms->isVisible());
            QVERIFY(!collapsed(w.get()));
            auto *splitter = qobject_cast<QSplitter *>(side->parentWidget());
            QVERIFY(splitter);
            splitter->setSizes({333, w->width() - 333});
            layOut();
            width = side->width();
            QVERIFY(qAbs(width - 333) <= 1);
            QAction *f9 = collapseAction(w.get());
            QVERIFY(f9);
            QCOMPARE(plain(f9->text()), QString("Collapse the List of VMs"));

            QTest::keyClick(vms, Qt::Key_F9);
            layOut();
            QVERIFY(collapsed(w.get()));
            QVERIFY(vms->isVisible());
            strip = side->width();
            /* its icons, each as wide as the list's */
            QVERIFY2(strip >= vms->iconSize().width() && strip < 2 * vms->iconSize().width(),
                     qPrintable(QString("a strip %1 wide for icons %2 wide")
                                    .arg(strip).arg(vms->iconSize().width())));
            QCOMPARE(plain(f9->text()), QString("Expand the List of VMs"));
            /* the VMs as before: another one chosen there */
            w->select("bravo");
            QCOMPARE(w->current(), m_store->find("bravo"));
            QCOMPARE(vms->currentItem()->text(), QString("Bravo"));
            w->close();
        }
        {
            auto w = window();
            settle(w.get());
            QWidget *side = sidebar(w.get());
            QVERIFY(collapsed(w.get()));
            QCOMPARE(side->width(), strip);
            QCOMPARE(w->current(), m_store->find("bravo"));
            /* the bar's button */
            QAction *button = nullptr;
            for (QAction *a : bar(w.get())->actions()) {
                if (a->objectName() == "listButton") {
                    button = a;
                }
            }
            QVERIFY(button);
            QCOMPARE(plain(button->text()), QString("Expand the List of VMs"));
            button->trigger();
            layOut();
            QVERIFY(!collapsed(w.get()));
            QVERIFY2(qAbs(side->width() - width) <= 1,
                     qPrintable(QString("the list %1 wide, was %2").arg(side->width()).arg(width)));
            QCOMPARE(plain(button->text()), QString("Collapse the List of VMs"));
            w->close();
        }
        auto w = window();
        settle(w.get());
        QVERIFY(!collapsed(w.get()));
        QVERIFY(qAbs(sidebar(w.get())->width() - width) <= 1);
    }

    /* A VM whose disks are being copied for a clone: Start says so, and the VM has no error */
    void busyStart()
    {
        auto w = window();
        settle(w.get());
        Vm *bravo = m_store->find("bravo");
        w->select("bravo");
        QListWidget *vms = list(w.get());
        const QString before = vms->currentItem()->data(Qt::UserRole + 1).toString();

        bravo->runner()->setBusy("Bravo is being cloned: it can start once its disks are copied");
        action(w.get(), "Start")->trigger();
        QMessageBox *box = nullptr;
        QTRY_VERIFY((box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())));
        QVERIFY(box->text().contains("being cloned"));
        box->close();
        QCOMPARE(bravo->runner()->state(), VmRunner::State::Stopped);
        /* the line under its name as it was: no error of its own */
        QCOMPARE(vms->currentItem()->data(Qt::UserRole + 1).toString(), before);
        bravo->runner()->setBusy({});
    }

    /*
     * A VM with its nested desktop in the window: the console's views,
     * from the guest (its window comes and goes), the View menu and
     * Ctrl+Alt+1/2/3 by their keys' places on an AZERTY keyboard; and
     * closing the window, which logs the guest's session out, asks
     */
    void nestedDesktopViews()
    {
        VmConsole::setDesktopViewFactory([](const QString &socket, QString *, QObject *parent) {
            return new StandInDesktop(socket, parent);
        });
        const auto noStandIn = qScopeGuard([]() { VmConsole::setDesktopViewFactory({}); });
        auto w = window();
        settle(w.get());
        w->select("alpha");
        w->findChild<VmPane *>()->setTab(VmPane::Console);
        VmConsole *console = nullptr;
        for (VmConsole *each : w->findChildren<VmConsole *>()) {
            if (each->vm() == m_store->find("alpha")) {
                console = each;
            }
        }
        QVERIFY(console);
        QVERIFY(!console->hasViews());
        QString error;
        QVERIFY(console->serveNested(m_tmp.filePath("nested.sock"), &error));
        auto *desktop = qobject_cast<StandInDesktop *>(console->nestedView());
        QVERIFY(desktop);
        QVERIFY(console->hasViews());
        QCOMPARE(console->currentView(), ConsoleViews::View::Boot);
        /* served once a run */
        QVERIFY(console->serveNested(m_tmp.filePath("nested.sock"), &error));
        QCOMPARE(console->nestedView(), desktop);
        const auto viewAction = [&w](int n) -> QAction * {
            for (QAction *a : w->findChildren<QAction *>()) {
                if (a->text().endsWith(QString("\tCtrl+Alt+%1").arg(n))) {
                    return a;
                }
            }
            return nullptr;
        };
        QVERIFY(viewAction(1) && viewAction(2) && viewAction(3));
        QTRY_VERIFY(viewAction(1)->isEnabled() && viewAction(1)->isChecked());
        /* the views in the Console tab's menu, the tab's name with the one shown; no
           row of buttons over the screen (the user's idea) */
        QToolButton *consoleTab = tabButtons(w.get()).value(0);
        QVERIFY(consoleTab);
        QTRY_VERIFY(consoleTab->menu());
        QCOMPARE(consoleTab->popupMode(), QToolButton::MenuButtonPopup);
        QCOMPARE(consoleTab->menu()->actions().size(), 3);
        QCOMPARE(consoleTab->text(), QString("Console 1"));
        QVERIFY(!console->findChild<QWidget *>("viewSwitcher"));

        /* the nested KWin connected: its desktop, with the keys */
        desktop->guestConnects(1);
        desktop->guestMaps(true);
        QCOMPARE(console->currentView(), ConsoleViews::View::Desktop);
        QVERIFY(console->desktopShown());
        QCOMPARE(console->screenWidget(), desktop->widget());
        /* the VM does not run here: QEMU would drop the keys, the guest gets none */
        QVERIFY(!desktop->inputEnabled());
        QVERIFY(viewAction(3)->isChecked());
        QTRY_COMPARE(consoleTab->text(), QString("Console 3"));

        /* AZERTY: &, é and " on the keys of 1, 2 and 3 (XKB's keycodes 10, 11, 12) */
        const auto press = [&w](Qt::Key key, quint32 keycode, const QString &text) {
            QWidget *to = QApplication::focusWidget() ? QApplication::focusWidget() : w.get();
            QKeyEvent event(QEvent::KeyPress, key, Qt::ControlModifier | Qt::AltModifier, keycode,
                            0, 0, text);
            QApplication::sendEvent(to, &event);
        };
        press(Qt::Key_Ampersand, 10, "&");
        QCOMPARE(console->currentView(), ConsoleViews::View::Boot);
        QVERIFY(!desktop->inputEnabled());
        press(Qt::Key_Eacute, 11, QString::fromUtf8("é"));
        QCOMPARE(console->currentView(), ConsoleViews::View::Screen);
        press(Qt::Key_QuoteDbl, 12, "\"");
        QCOMPARE(console->currentView(), ConsoleViews::View::Desktop);
        /* the menu's */
        viewAction(1)->trigger();
        QCOMPARE(console->currentView(), ConsoleViews::View::Boot);
        viewAction(3)->trigger();
        QCOMPARE(console->currentView(), ConsoleViews::View::Desktop);
        /* full screen goes with the view shown */
        action(w.get(), "Full Screen")->trigger();
        QVERIFY(desktop->isFullScreen());
        QVERIFY(console->isFullScreen());
        action(w.get(), "Full Screen")->trigger();
        QVERIFY(!desktop->isFullScreen());
        /* the session ended, and no agent of the guest's tells of a login
           screen: the guest goes, its console on the boot's screen */
        desktop->guestMaps(false);
        QCOMPARE(console->currentView(), ConsoleViews::View::Boot);
        /* the host's Reset under the session: the boot, which the session's
           window going after the reset keeps */
        desktop->guestMaps(true);
        QCOMPARE(console->currentView(), ConsoleViews::View::Desktop);
        emit m_store->find("alpha")->runner()->guestReset();
        QCOMPARE(console->currentView(), ConsoleViews::View::Boot);
        desktop->guestMaps(false);
        QCOMPARE(console->currentView(), ConsoleViews::View::Boot);
        QTRY_COMPARE(consoleTab->text(), QString("Console 1"));

        /* closing asks, and says the guest's session ends with it */
        desktop->guestMaps(true);
        QString asked;
        QTimer::singleShot(100, w.get(), [&asked]() {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                asked = box->text() + ' ' + box->informativeText();
                box->reject();
            }
        });
        w->close();
        QVERIFY2(asked.contains("Closing Vitrine logs out the desktop session of Alpha"),
                 qPrintable(asked));
        QVERIFY(w->isVisible());
        /* its session over: nothing to say of it */
        desktop->guestMaps(false);
        desktop->guestConnects(0);
        QVERIFY(!console->hasNestedSession());
    }

    /* Closing with a task under way: asked, and not closed on Cancel */
    void closeWithTasks()
    {
        auto w = window();
        settle(w.get());
        QPointer<Task> task = TaskList::instance()->add(Task::Kind::Clone, "Clone of Alpha");
        QString asked;

        QTimer::singleShot(100, w.get(), [&asked]() {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                asked = box->text() + ' ' + box->informativeText();
                box->reject();
            }
        });
        w->close();
        QVERIFY2(asked.contains("Clone of Alpha"), qPrintable(asked));
        QVERIFY(w->isVisible());
        task->finish(Task::State::Done);
        task->dismiss();
        w->close();
        QVERIFY(!w->isVisible());
    }

    /*
     * F10 opens the first menu, Alt and a menu's letter that menu, its first
     * entry chosen as the keyboard goes; from the list and from a text field
     */
    void openMenu()
    {
        auto w = window();
        settle(w.get());
        QMenu *file = menuOf(w.get(), "File");
        QVERIFY2(file, "no File menu in the bar");
        auto *pane = w->findChild<VmPane *>();
        pane->setTab(VmPane::Settings);
        pane->setPage(VmPane::General);
        auto *name = w->findChild<QLineEdit *>("name");
        QVERIFY(name);
        QTRY_VERIFY(name->isVisible());
        const QString text = name->text();

        const QList<QWidget *> froms = {list(w.get()), name};
        for (QWidget *from : froms) {
            from->setFocus();
            QTRY_VERIFY(from->hasFocus());
            QTest::keyClick(from, Qt::Key_F10);
            QTRY_COMPARE(QApplication::activePopupWidget(), file);
            QVERIFY(file->activeAction());
            QCOMPARE(file->activeAction(), file->actions().value(0));
            file->close();
            QTRY_VERIFY(!QApplication::activePopupWidget());

            for (const char *title : {"File", "View", "Machine", "Help"}) {
                QMenu *menu = menuOf(w.get(), title);
                QVERIFY(menu);
                from->setFocus();
                QTest::keyClick(from, Qt::Key(QChar(title[0]).unicode()), Qt::AltModifier);
                QTRY_VERIFY2(QApplication::activePopupWidget() == menu,
                             qPrintable(QString("Alt+%1 from %2 opened %3")
                                            .arg(title[0])
                                            .arg(from->metaObject()->className())
                                            .arg(QApplication::activePopupWidget()
                                                     ? QApplication::activePopupWidget()
                                                           ->metaObject()->className()
                                                     : "nothing")));
                menu->close();
                QTRY_VERIFY(!QApplication::activePopupWidget());
            }
        }
        /* nothing typed into the field */
        QCOMPARE(name->text(), text);
    }

    /* No VM yet: the welcome page; the menus, nothing to start, no tab to choose; the list
       collapses all the same */
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
        for (const QString &name : kPower) {
            QVERIFY2(!action(&w, name)->isEnabled(), qPrintable(name + " enabled"));
        }
        QVERIFY(menusOf(&w) && menusOf(&w)->isVisible());
        QVERIFY(barProblems(&w, "no VMs").isEmpty());
        collapseAction(&w)->trigger();
        layOut();
        QVERIFY(collapsed(&w));
        QVERIFY(list(&w)->isVisible());
        QVERIFY(barProblems(&w, "no VMs, list collapsed").isEmpty());
        collapseAction(&w)->trigger();
        QVERIFY(!collapsed(&w));
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
     * At every width the window can have, the bar shows all its items in
     * its width, the menus whole, the tabs one button only when short of
     * room, Fusion and Breeze, the list expanded and collapsed, at a range
     * of scales
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
                auto w = window();
                settle(w.get());
                if (!libraryShown) {
                    collapseAction(w.get())->trigger();
                }
                layOut();
                const int least = w->minimumSizeHint().width();
                bool compact = false;
                for (int width = 1600; width >= least - 40; width -= 11) {
                    w->resize(width, 800);
                    layOutOnly();
                    const QString where = QString("%1 n=%2%3 %4 wide (least %5)")
                                              .arg(style).arg(n)
                                              .arg(libraryShown ? "" : " list collapsed")
                                              .arg(w->width()).arg(least);
                    wrong << barProblems(w.get(), where);
                    /* the tabs one button in a narrower window, never back */
                    if (compact && !compactTabs(w.get())) {
                        wrong << where + ": the tabs back to buttons, narrower";
                    }
                    compact = compactTabs(w.get());
                    if (width == 1600 && compact) {
                        wrong << where + ": the tabs one button in a wide window";
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
