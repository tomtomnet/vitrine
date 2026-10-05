// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The statistics of the status bar: what shows at first, the menu's
 * toggles and their persistence, what is read for what shows (and nothing
 * while the window is hidden), the labels of a VM found running (the
 * stand-in QEMU and a monitor answering as told), their layout in a
 * narrow window, the other items the menu hides, and the menu button and
 * context menu.  Offscreen, at the scales ctest gives.
 */
#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDir>
#include <QFile>
#include <QHelpEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMainWindow>
#include <QMenu>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QToolTip>

#include <memory>

#include "core/paths.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "scales.h"
#include "ui/statusstats.h"

using Sampler = VmStats::Sampler;

static bool writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

/* QEMU's end of QMP: running, its disk busier at each query-blockstats */
class FakeQmp : public QObject
{
public:
    explicit FakeQmp(const QString &path)
    {
        QLocalServer::removeServer(path);
        m_server.listen(path);
        connect(&m_server, &QLocalServer::newConnection, this, [this]() {
            QLocalSocket *peer = m_server.nextPendingConnection();
            connect(peer, &QLocalSocket::readyRead, this, [this, peer]() {
                m_buffer += peer->readAll();
                qsizetype nl;
                while ((nl = m_buffer.indexOf('\n')) >= 0) {
                    const QJsonObject c = QJsonDocument::fromJson(m_buffer.left(nl)).object();
                    m_buffer.remove(0, nl + 1);
                    peer->write("{\"return\": " + answer(c["execute"].toString()) +
                                ", \"id\": " + QByteArray::number(c["id"].toInteger()) + "}\n");
                }
            });
            peer->write(R"({"QMP": {"version": {"qemu": {"major": 11}}, "capabilities": []}})"
                        "\n");
        });
    }

    QHash<QString, int> asked;
    bool idle = false;      // the disk's counters stay

private:
    QByteArray answer(const QString &command)
    {
        int n = ++asked[command];
        if (command == "query-blockstats") {
            m_disk = idle ? m_disk : n;
            n = m_disk;
        }
        if (command == "query-status") {
            return R"({"running": true, "status": "running"})";
        }
        if (command == "query-blockstats") {
            /* and the empty drive the guest looked at once */
            return QString(R"([{"device": "disk0", "stats": {"rd_bytes": %1, "wr_bytes": %2,)"
                           R"( "rd_operations": %3, "wr_operations": %3}},)"
                           R"( {"device": "ide2-cd0", "stats": {"rd_bytes": 528, "wr_bytes": 0,)"
                           R"( "rd_operations": 1, "wr_operations": 0}}])")
                .arg(qint64(n) * 12 << 20)
                .arg(qint64(n) << 20)
                .arg(n)
                .toUtf8();
        }
        if (command == "query-commands") {
            return R"([{"name": "query-blockstats"}, {"name": "query-stats"}])";
        }
        return "{}";
    }

    QLocalServer m_server;
    QByteArray m_buffer;
    int m_disk = 0;
};

/* A VM found running: the stand-in QEMU (threads "CPU 0/KVM", "CPU 1/KVM")
   and its monitor */
struct Running {
    QTemporaryDir runtime{QDir::tempPath() + "/vt-XXXXXX"};
    QTemporaryDir vms;
    QProcess qemu;
    std::unique_ptr<FakeQmp> qmp;
    std::unique_ptr<VmStore> store;
    Vm *vm = nullptr;

    Running()
    {
        qputenv("XDG_RUNTIME_DIR", runtime.path().toLocal8Bit());
        QDir(vms.path()).mkpath("busy");
        writeFile(vms.path() + "/busy/vm.args", "-name Busy\n-m 1G\n-display none\n");
        const QString run = Paths::vmRuntimeDir("busy");
        qemu.start(FAKE_QEMU,
                   {"-qmp", QString("unix:%1/qmp.sock,server=on,wait=off").arg(run)});
        qemu.waitForStarted();
        writeFile(run + "/qemu.pid", QByteArray::number(qemu.processId()) + "\n");
        qmp = std::make_unique<FakeQmp>(run + "/qmp.sock");
        store = std::make_unique<VmStore>(vms.path());
        vm = store->find("busy");
        vm->runner()->attach(vm->args());
    }
    ~Running()
    {
        qmp.reset();
        if (qemu.state() != QProcess::NotRunning) {
            qemu.kill();
            qemu.waitForFinished();
        }
    }
};

/* A window with the status bar as MainWindow sets it up: an optional label,
   the statistics, a notice, the menu button */
struct Window {
    QMainWindow window;
    QLabel *keyboard = new QLabel("Keys go to the VM · Ctrl+Alt+G grabs");
    StatusStats *stats = new StatusStats;
    QToolButton *notice = new QToolButton;

    Window()
    {
        keyboard->setObjectName("input");
        keyboard->hide();
        notice->setText("Notice");
        window.statusBar()->addPermanentWidget(stats->optional("keyboard", "Keyboard", keyboard));
        window.statusBar()->addPermanentWidget(stats);
        window.statusBar()->addPermanentWidget(notice);
        window.statusBar()->addPermanentWidget(stats->menuButton());
        window.resize(1400, 400);
    }
};

static QSettings settings()
{
    return QSettings(Paths::settingsPath(), QSettings::IniFormat);
}

/* The windows shown but @windows: none may have come up */
static int otherWindows(const QList<const QWidget *> &windows)
{
    int n = 0;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        n += !windows.contains(w) && w->isVisible() && !qobject_cast<QMenu *>(w) &&
             !w->inherits("QTipLabel");
    }
    return n;
}

class TestStatusStats : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        Scales::quieter();
        Scales::bigScreen();
    }
    void init() { settings().remove("statusbar"); }

    void defaults()
    {
        StatusStats stats;
        const QList<QAction *> actions = stats.menu()->actions();

        QVERIFY(stats.isShown(StatusStats::Cpu));
        QVERIFY(stats.isShown(StatusStats::Memory));
        QVERIFY(!stats.isShown(StatusStats::Disk));
        QVERIFY(!stats.isShown(StatusStats::Network));
        QVERIFY(!stats.isShown(StatusStats::Gpu));
        QVERIFY(stats.isShown(StatusStats::Frames));
        QVERIFY(stats.isShown(StatusStats::MainLoop));
        QStringList names;
        for (QAction *a : actions) {
            if (a->isCheckable()) {
                names << a->text();
                QVERIFY(!a->toolTip().isEmpty());
            }
        }
        QCOMPARE(names, QStringList({"CPU", "Memory", "Disk", "Network", "GPU",
                                     "Frame Rate and Latency", "Main Loop Wait"}));
        QVERIFY(stats.menu()->toolTipsVisible());
        /* nothing to show without a VM */
        QVERIFY(stats.isHidden());
        QCOMPARE(stats.sources(), Sampler::Sources());
    }

    void persistence()
    {
        {
            StatusStats stats;
            stats.action(StatusStats::Disk)->trigger();
            stats.action(StatusStats::Cpu)->trigger();
            stats.setShown(StatusStats::Gpu, true);
            QVERIFY(stats.isShown(StatusStats::Disk));
            QVERIFY(!stats.isShown(StatusStats::Cpu));
        }
        QCOMPARE(settings().value("statusbar/disk").toBool(), true);
        QCOMPARE(settings().value("statusbar/cpu").toBool(), false);
        QCOMPARE(settings().value("statusbar/gpu").toBool(), true);
        QVERIFY(!settings().contains("statusbar/network"));

        StatusStats again;
        QVERIFY(again.isShown(StatusStats::Disk));
        QVERIFY(!again.isShown(StatusStats::Cpu));
        QVERIFY(again.isShown(StatusStats::Gpu));
        QVERIFY(again.isShown(StatusStats::Memory));    // never changed: its default
        QVERIFY(!again.isShown(StatusStats::Network));
        QVERIFY(again.action(StatusStats::Disk)->isChecked());
    }

    /* What is read: what shows, its details while their tooltip shows, nothing hidden */
    void sources()
    {
        Running vm;
        QTRY_COMPARE(vm.vm->runner()->state(), VmRunner::State::Running);
        Window w;
        w.stats->setVm(vm.vm);
        QCOMPARE(w.stats->sources(), Sampler::Sources());   // the window is hidden
        Scales::settle(&w.window);
        QTRY_COMPARE(w.stats->sources(), Sampler::Cpu | Sampler::Memory | Sampler::Display);

        w.stats->setShown(StatusStats::Disk, true);
        w.stats->setShown(StatusStats::Gpu, true);
        w.stats->setShown(StatusStats::Network, true);
        QCOMPARE(w.stats->sources(), Sampler::Cpu | Sampler::Memory | Sampler::Display |
                                         Sampler::Disk | Sampler::Gpu | Sampler::Network);
        /* the main loop's wait needs QEMU's threads too */
        w.stats->setShown(StatusStats::Cpu, false);
        QVERIFY(w.stats->sources() & Sampler::Cpu);
        w.stats->setShown(StatusStats::MainLoop, false);
        QVERIFY(!(w.stats->sources() & Sampler::Cpu));
        w.stats->setShown(StatusStats::Frames, false);
        QVERIFY(!(w.stats->sources() & Sampler::Display));

        /* the memory's tooltip: PSS too, while it shows */
        QTRY_VERIFY(w.stats->label(StatusStats::Memory)->isVisible());
        QLabel *memory = w.stats->label(StatusStats::Memory);
        QHelpEvent tip(QEvent::ToolTip, QPoint(2, 2), memory->mapToGlobal(QPoint(2, 2)));
        QApplication::sendEvent(memory, &tip);
        QVERIFY(w.stats->sources() & Sampler::Pss);
        QToolTip::hideText();

        w.window.hide();
        QCOMPARE(w.stats->sources(), Sampler::Sources());
        w.window.show();
        QVERIFY(w.stats->sources() & Sampler::Disk);
        w.stats->setVm(nullptr);
        QVERIFY(w.stats->isHidden());
    }

    /* The labels of a running VM, laid out at every scale ctest gives */
    void labels()
    {
        Running vm;
        QTRY_COMPARE(vm.vm->runner()->state(), VmRunner::State::Running);
        const QList<int> scales = qEnvironmentVariableIsSet("QT_SCALE_FACTOR")
                                      ? Scales::list()
                                      : QList<int>{120, 150, 180};

        for (const int n : scales) {
            Scales::set(n);
            {
                Window w;
                StatusStats *s = w.stats;
                s->setShown(StatusStats::Disk, true);
                s->setVm(vm.vm);
                Scales::settle(&w.window);
                QTRY_VERIFY(s->isVisible());
                QTRY_VERIFY(s->label(StatusStats::Cpu)->isVisible());
                QTRY_VERIFY(s->label(StatusStats::Disk)->isVisible());
                QVERIFY(s->label(StatusStats::Memory)->isVisible());
                QVERIFY(s->label(StatusStats::Cpu)->text().startsWith("CPU "));
                QVERIFY(s->label(StatusStats::Cpu)->text().endsWith(" %"));
                QVERIFY(s->label(StatusStats::Memory)->text().startsWith("RAM "));
                QVERIFY(s->label(StatusStats::Disk)->text().contains(QRegularExpression(
                    "^Disk R [0-9.]+ [KMG]?i?B/s · W [0-9.]+ [KMG]?i?B/s$")));
                QVERIFY2(!s->label(StatusStats::Gpu)->isVisible(), "the stand-in has no GPU");
                QVERIFY2(!s->label(StatusStats::MainLoop)->text().isEmpty(),
                         qPrintable(QString("n=%1").arg(n)));

                /* side by side in order, within the bar, apart */
                int right = -1;
                for (int i = 0; i < StatusStats::kStats; i++) {
                    QLabel *l = s->label(StatusStats::Stat(i));
                    if (!l->isVisible()) {
                        continue;
                    }
                    QVERIFY(l->x() > right);
                    QVERIFY(l->width() >= l->sizeHint().width());
                    QVERIFY(l->geometry().bottom() < s->height());
                    QVERIFY(s->contentsRect().contains(l->geometry()));
                    right = l->geometry().right();
                }
                /* no wider than the window lets them be: the last ones left out */
                const int wide = s->width();
                /* the room left of them gone, and two thirds of theirs */
                w.window.resize(w.window.width() - s->x() - wide * 2 / 3, 400);
                Scales::settle(&w.window);
                QTRY_VERIFY(s->width() < wide);
                QVERIFY(s->label(StatusStats::Cpu)->isVisible());
                QVERIFY(!s->label(StatusStats::MainLoop)->isVisible());
                for (int i = 0; i < StatusStats::kStats; i++) {
                    QLabel *l = s->label(StatusStats::Stat(i));
                    QVERIFY(!l->isVisible() || s->contentsRect().contains(l->geometry()));
                }
                /* the statistics never make the window wider */
                QCOMPARE(s->minimumSizeHint().width(), 0);
                w.window.resize(1400, 400);
                Scales::settle(&w.window);
                QTRY_VERIFY(s->label(StatusStats::MainLoop)->isVisible());

                /* each tooltip on the lines it has: laid out word-wrapped, as QToolTip
                   does, no taller than without wrapping */
                for (int i = 0; i < StatusStats::kStats; i++) {
                    QLabel *l = s->label(StatusStats::Stat(i));
                    if (!l->isVisible()) {
                        continue;
                    }
                    const QPoint at = l->rect().center();
                    QHelpEvent tip(QEvent::ToolTip, at, l->mapToGlobal(at));
                    QApplication::sendEvent(l, &tip);
                    QTRY_VERIFY(QToolTip::isVisible());
                    if (l == s->label(StatusStats::Disk)) {
                        QVERIFY(QToolTip::text().contains("disk0"));
                        QVERIFY(!QToolTip::text().contains("ide2-cd0"));
                    }
                    QLabel wrapped, unwrapped;
                    wrapped.setWordWrap(true);
                    wrapped.setText(QToolTip::text());
                    unwrapped.setText(QToolTip::text());
                    QVERIFY2(wrapped.sizeHint().height() <= unwrapped.sizeHint().height(),
                             qPrintable(QString("%1 at n=%2: %3 > %4")
                                            .arg(l->objectName()).arg(n)
                                            .arg(wrapped.sizeHint().height())
                                            .arg(unwrapped.sizeHint().height())));
                    QToolTip::hideText();
                }

                /* no label narrower than it was, so that what follows stays put */
                const int widest = s->label(StatusStats::Disk)->width();
                QTest::qWait(1200);
                QVERIFY(s->label(StatusStats::Disk)->width() >= widest);
                QCOMPARE(otherWindows({&w.window}), 0);
            }
        }
        Scales::set(120);
    }

    /* A statistic narrower for a while gets a narrower place, at once wider */
    void shrink()
    {
        Running vm;
        QTRY_COMPARE(vm.vm->runner()->state(), VmRunner::State::Running);
        StatusStats::setShrinkDelay(1500);
        Window w;
        w.stats->setShown(StatusStats::Disk, true);
        w.stats->setVm(vm.vm);
        Scales::settle(&w.window);
        QLabel *disk = w.stats->label(StatusStats::Disk);
        QTRY_VERIFY(disk->isVisible() && disk->text().contains("MiB/s"));
        const int wide = disk->width();
        const int memory = w.stats->label(StatusStats::Memory)->x();

        vm.qmp->idle = true;
        QTRY_COMPARE(disk->text(), "Disk R 0 B/s · W 0 B/s");
        /* its place kept for now, what comes before it where it was */
        QCOMPARE(disk->width(), wide);
        QTRY_VERIFY_WITH_TIMEOUT(disk->width() < wide, 8000);
        QCOMPARE(disk->width(), disk->sizeHint().width());
        QCOMPARE(w.stats->label(StatusStats::Memory)->x(), memory);
        /* busy again: as wide as it needs at once */
        vm.qmp->idle = false;
        QTRY_VERIFY(disk->text().contains("MiB/s"));
        QVERIFY(disk->width() >= disk->sizeHint().width());
        StatusStats::setShrinkDelay(10000);
    }

    /* An item of the status bar the menu hides */
    void optionalItems()
    {
        Window w;
        QWidget *holder = w.keyboard->parentWidget();
        QAction *action = nullptr;
        for (QAction *a : w.stats->menu()->actions()) {
            action = a->text() == "Keyboard" ? a : action;
        }
        QVERIFY(action && action->isCheckable() && action->isChecked());
        Scales::settle(&w.window);
        QCOMPARE(otherWindows({&w.window}), 0);
        /* hidden by its owner: so is its place */
        QVERIFY(!holder->isVisible());
        w.keyboard->show();
        QVERIFY(holder->isVisible());
        QVERIFY(w.keyboard->isVisible());
        /* hidden from the menu, whatever its owner does */
        action->trigger();
        QVERIFY(!holder->isVisible());
        QCOMPARE(settings().value("statusbar/keyboard").toBool(), false);
        w.keyboard->hide();
        w.keyboard->show();
        QVERIFY(!holder->isVisible());
        action->trigger();
        QVERIFY(holder->isVisible());
        w.keyboard->hide();
        QVERIFY(!holder->isVisible());

        /* remembered: hidden from the start, never a window of its own */
        action->trigger();
        Window again;
        QWidget *other = again.keyboard->parentWidget();
        again.keyboard->show();
        Scales::settle(&again.window);
        QVERIFY(!other->isVisible());
        QCOMPARE(otherWindows({&w.window, &again.window}), 0);
    }

    void menuButton()
    {
        Window w;
        QToolButton *button = w.stats->menuButton();
        const int small = button->style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, button);

        Scales::settle(&w.window);
        /* a notice of the status bar as the others: its last item */
        QVERIFY(button->isVisible());
        QCOMPARE(button->parentWidget(), w.window.statusBar());
        QCOMPARE(button->iconSize(), QSize(small, small));
        QVERIFY(button->autoRaise());
        QVERIFY(!button->icon().isNull());
        QCOMPARE(button->toolButtonStyle(), Qt::ToolButtonIconOnly);
        QCOMPARE(button->menu(), w.stats->menu());
        QCOMPARE(button->popupMode(), QToolButton::InstantPopup);
        QVERIFY(button->geometry().right() >= w.notice->geometry().right());
        QCOMPARE(otherWindows({&w.window}), 0);

        /* a right click on the status bar: the same menu */
        QStatusBar *bar = w.window.statusBar();
        QContextMenuEvent click(QContextMenuEvent::Mouse, QPoint(5, 5),
                                bar->mapToGlobal(QPoint(5, 5)));
        QApplication::sendEvent(bar, &click);
        QTRY_VERIFY(w.stats->menu()->isVisible());
        w.stats->menu()->hide();
    }

    /* The notes of what the VM cannot show, in the menu */
    void notes()
    {
        Running vm;
        QTRY_COMPARE(vm.vm->runner()->state(), VmRunner::State::Running);
        Window w;
        w.stats->setShown(StatusStats::Gpu, true);
        w.stats->setShown(StatusStats::Network, true);
        w.stats->setVm(vm.vm);
        Scales::settle(&w.window);
        /* read once at least: the stand-in has no GPU, and no guest tools */
        QTRY_VERIFY(w.stats->label(StatusStats::Cpu)->isVisible());
        QTest::qWait(1200);
        QMetaObject::invokeMethod(w.stats->menu(), "aboutToShow");
        QCOMPARE(w.stats->action(StatusStats::Network)->text(),
                 "Network (needs the guest tools)");
        QCOMPARE(w.stats->action(StatusStats::Gpu)->text(), "GPU (QEMU does not use it)");
        QCOMPARE(w.stats->action(StatusStats::Disk)->text(), "Disk");
        QVERIFY(!w.stats->label(StatusStats::Network)->isVisible());
        QVERIFY(!w.stats->label(StatusStats::Gpu)->isVisible());
        /* a VM that does not run: no notes */
        w.stats->setVm(nullptr);
        QMetaObject::invokeMethod(w.stats->menu(), "aboutToShow");
        QCOMPARE(w.stats->action(StatusStats::Network)->text(), "Network");
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    TestStatusStats test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}
#include "test_statusstats.moc"
