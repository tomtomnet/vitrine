// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef Q_MOC_RUN
#include "gtk/gtkbase.h"
#endif

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include "gtkdisplay.h"

#ifndef Q_MOC_RUN
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "gtk/gtkloop.h"
#include "gtk/mainwindow.h"
#endif

/*
 * The GTK app's window, on a display no one sees: the sidebar lists the
 * VMs by name with their state, follows the store as VMs come, go and are
 * renamed, and keeps the selection; the empty state shows without VMs;
 * the actions are those the selected VM's state allows, the ones whose
 * dialogs are not in the GTK app yet inactive.
 */
class TestGtkMainWindow : public QObject
{
    Q_OBJECT

    QProcess m_broadway;
    GTestDBus *m_bus = nullptr;
    GtkLoop *m_loop = nullptr;

    /* A VM folder @id holding a vm.args with -name @name */
    static bool addVm(const QString &dir, const QString &id, const QString &name)
    {
        QFile args(dir + "/" + id + "/vm.args");
        return QDir().mkpath(dir + "/" + id) && args.open(QIODevice::WriteOnly) &&
               args.write(("-name " + name + "\n-m 1G\n").toUtf8()) > 0;
    }

    /* The first widget of @type under @widget, depth first */
    static GtkWidget *find(GtkWidget *widget, GType type)
    {
        if (G_TYPE_CHECK_INSTANCE_TYPE(widget, type)) {
            return widget;
        }
        for (GtkWidget *child = gtk_widget_get_first_child(widget); child;
             child = gtk_widget_get_next_sibling(child)) {
            if (GtkWidget *found = find(child, type)) {
                return found;
            }
        }
        return nullptr;
    }

    /* The sidebar's items: title, then subtitle */
    static QList<QStringList> items(MainWindow &window)
    {
        auto *sidebar = ADW_SIDEBAR(find(GTK_WIDGET(window.window()), ADW_TYPE_SIDEBAR));
        GListModel *model = sidebar ? G_LIST_MODEL(adw_sidebar_get_items(sidebar)) : nullptr;
        QList<QStringList> list;

        for (guint i = 0; model && i < g_list_model_get_n_items(model); i++) {
            auto *item = ADW_SIDEBAR_ITEM(g_list_model_get_item(model, i));
            list << QStringList{QString::fromUtf8(adw_sidebar_item_get_title(item)),
                                QString::fromUtf8(adw_sidebar_item_get_subtitle(item))};
            g_object_unref(item);
        }
        if (model) {
            g_object_unref(model);
        }
        return list;
    }

    static QStringList titles(MainWindow &window)
    {
        QStringList list;
        for (const QStringList &item : items(window)) {
            list << item.first();
        }
        return list;
    }

    /* "vms" or "empty" */
    static QString shown(MainWindow &window)
    {
        GtkWidget *content = adw_application_window_get_content(ADW_APPLICATION_WINDOW(window.window()));
        return QString::fromUtf8(gtk_stack_get_visible_child_name(GTK_STACK(content)));
    }

    static bool enabled(MainWindow &window, const char *action)
    {
        return g_action_group_get_action_enabled(G_ACTION_GROUP(window.window()), action);
    }

private slots:
    void initTestCase()
    {
        QString error;

        QStandardPaths::setTestModeEnabled(true);
        m_bus = GtkDisplay::up(m_broadway);
        if (!m_bus) {
            QSKIP("gtk4-broadwayd is not installed: no display to test on but the desktop's");
        }
        m_loop = new GtkLoop("org.vitrine.Vitrine");
        QCOMPARE(m_loop->start(&error), GtkLoop::Start::Primary);
    }

    void cleanupTestCase()
    {
        delete m_loop;
        GtkDisplay::down(m_broadway, m_bus);
    }

    /* By name, whatever the case and the folders' names; the first selected */
    void sidebar()
    {
        QTemporaryDir dir;
        QVERIFY(addVm(dir.path(), "b", "Zeta") && addVm(dir.path(), "a", "alpha") &&
                addVm(dir.path(), "c", "Beta"));
        VmStore store(dir.path());
        MainWindow window(m_loop, &store);

        QCOMPARE(shown(window), "vms");
        QCOMPARE(items(window), QList<QStringList>({{"alpha", "Powered off"},
                                                    {"Beta", "Powered off"},
                                                    {"Zeta", "Powered off"}}));
        QVERIFY(window.current());
        QCOMPARE(window.current()->id(), "a");
        window.select("b");
        QCOMPARE(window.current()->id(), "b");
        window.select("nonexistent");
        QCOMPARE(window.current()->id(), "b");
    }

    void empty()
    {
        QTemporaryDir dir;
        VmStore store(dir.path());
        MainWindow window(m_loop, &store);

        QCOMPARE(shown(window), "empty");
        QVERIFY(!window.current());
        QVERIFY(!enabled(window, "start"));
        QVERIFY(!enabled(window, "remove"));
        /* then a VM comes: the list, the VM selected */
        QVERIFY(store.create("First"));
        QCOMPARE(shown(window), "vms");
        QCOMPARE(titles(window), QStringList({"First"}));
        QVERIFY(window.current());
        QVERIFY(enabled(window, "start"));
    }

    /* What a VM that is off allows; the dialogs to come inactive */
    void actions()
    {
        QTemporaryDir dir;
        QVERIFY(addVm(dir.path(), "off", "Off"));
        VmStore store(dir.path());
        MainWindow window(m_loop, &store);

        QCOMPARE(window.current()->runner()->state(), VmRunner::State::Stopped);
        for (const char *on : {"start", "remove", "show-log", "open-folder", "new-vm", "settings",
                               "command-line"}) {
            QVERIFY2(enabled(window, on), on);
        }
        for (const char *off : {"pause", "shut-down", "reset", "force-off", "import-vm", "clone"}) {
            QVERIFY2(!enabled(window, off), off);
        }
    }

    /* VMs added, renamed and removed outside the window: the list follows */
    void followsTheStore()
    {
        QTemporaryDir dir;
        QVERIFY(addVm(dir.path(), "m", "Middle") && addVm(dir.path(), "z", "Zulu"));
        VmStore store(dir.path());
        MainWindow window(m_loop, &store);

        window.select("z");
        Vm *added = store.create("Alpha");
        QVERIFY(added);
        QCOMPARE(titles(window), QStringList({"Alpha", "Middle", "Zulu"}));
        /* the selection stays on its VM as the list moves under it */
        QCOMPARE(window.current()->id(), "z");

        /* renamed: in its new place, still selected */
        Vm *zulu = store.find("z");
        ArgsFile args = zulu->args();
        const int name = args.indexOf("name");
        QVERIFY(name >= 0);
        args.setValueAt(name, "Aaron");
        QVERIFY(zulu->save(args));
        QTRY_COMPARE(titles(window), QStringList({"Aaron", "Alpha", "Middle"}));
        QCOMPARE(window.current()->id(), "z");

        /* its folder gone (the store's watcher sees it, no trash): gone, another one selected */
        QVERIFY(QDir(dir.filePath("z")).removeRecursively());
        QTRY_COMPARE(titles(window), QStringList({"Alpha", "Middle"}));
        QVERIFY(window.current());
        QVERIFY(window.current()->id() != "z");
    }
};

int main(int argc, char **argv)
{
    QTemporaryDir runtime(QDir::tempPath() + "/vitrine-gtkwin-XXXXXX");
    if (!GtkDisplay::prepare(runtime)) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    TestGtkMainWindow test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_gtkmainwindow.moc"
