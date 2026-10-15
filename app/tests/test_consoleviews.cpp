// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWidget>

#include "core/consoleviews.h"
#include "core/guesttools.h"
#include "core/nesteddesktop.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "nested/desktopview.h"
#include "ui/vmconsole.h"

using View = ConsoleViews::View;
Q_DECLARE_METATYPE(std::optional<ConsoleViews::View>)

/* QEMU's end of QMP, for a QEMU found running: answers every command,
   query-status with running; events on demand */
class FakeQmp : public QObject
{
public:
    explicit FakeQmp(const QString &path)
    {
        QLocalServer::removeServer(path);
        m_server.listen(path);
        connect(&m_server, &QLocalServer::newConnection, this, [this]() {
            QLocalSocket *peer = m_server.nextPendingConnection();
            m_peers << peer;
            connect(peer, &QLocalSocket::readyRead, this, [this, peer]() {
                m_buffer += peer->readAll();
                for (qsizetype nl; (nl = m_buffer.indexOf('\n')) >= 0;) {
                    const QJsonObject c = QJsonDocument::fromJson(m_buffer.left(nl)).object();
                    m_buffer.remove(0, nl + 1);
                    QJsonObject reply{{"return", QJsonObject()}, {"id", c["id"]}};
                    if (c["execute"] == "query-status") {
                        reply["return"] = QJsonObject{{"running", true}, {"status", "running"}};
                    }
                    peer->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
                }
            });
            peer->write(R"({"QMP": {"version": {"qemu": {"major": 11}}, "capabilities": []}})"
                        "\n");
        });
    }
    /* to every monitor connection */
    void event(const QString &name, const QJsonObject &data = {})
    {
        for (QLocalSocket *peer : std::as_const(m_peers)) {
            if (peer && peer->state() == QLocalSocket::ConnectedState) {
                peer->write(QJsonDocument(QJsonObject{{"event", name}, {"data", data}})
                                .toJson(QJsonDocument::Compact) + '\n');
            }
        }
    }

private:
    QLocalServer m_server;
    QList<QPointer<QLocalSocket>> m_peers;
    QByteArray m_buffer;
};

/* The guest's desktop as VmConsole shows it: its window comes and goes on demand */
class StandInDesktop : public DesktopView
{
    Q_OBJECT

public:
    StandInDesktop(const QString &socket, QObject *parent)
        : DesktopView(parent), m_socket(socket), m_widget(new QWidget)
    {
    }
    ~StandInDesktop() override { delete m_widget; }

    QString socket() const override { return m_socket; }
    QWidget *widget() const override { return m_widget; }
    bool isFullScreen() const override { return false; }
    void setFullScreen(bool) override {}
    void setInputEnabled(bool) override {}
    bool hasKeyboard() const override { return false; }
    void focus() override {}
    int connections() const override { return m_mapped ? 1 : 0; }
    bool isMapped() const override { return m_mapped; }

    /* The guest's nested KWin: its window came or went */
    void guestMaps(bool on)
    {
        m_mapped = on;
        Q_EMIT mappedChanged(on);
    }

private:
    QString m_socket;
    QWidget *m_widget;
    bool m_mapped = false;
};

/* XKB's keycodes of the keys 1, 2, 3 above the letters, and of A, F, KP_1, KP_3 */
static constexpr quint32 kKey1 = 10, kKey2 = 11, kKey3 = 12, kKeyA = 38, kKeyF = 41, kKp1 = 87,
                         kKp3 = 89;

static std::optional<View> viewOf(int key, Qt::KeyboardModifiers mods, quint32 scanCode,
                                  const QString &text = {})
{
    const QKeyEvent event(QEvent::KeyPress, key, mods, scanCode, 0, 0, text);
    return ConsoleViews::viewForKey(&event);
}

class TestConsoleViews : public QObject
{
    Q_OBJECT

    QTemporaryDir m_home;

private slots:
    /* the settings the guest tools' monitor keeps of a VM, and the rest, in here */
    void initTestCase()
    {
        QVERIFY(m_home.isValid());
        qputenv("XDG_DATA_HOME", m_home.filePath("data").toUtf8());
        qputenv("XDG_CONFIG_HOME", m_home.filePath("config").toUtf8());
        qputenv("XDG_CACHE_HOME", m_home.filePath("cache").toUtf8());
    }

    /* From power-on: the VM's screen, the guest's desktop while its window is there, and back */
    void automatic()
    {
        ConsoleViews views;
        QSignalSpy changed(&views, &ConsoleViews::viewChanged);

        views.powerOn();
        QCOMPARE(views.view(), View::Screen);
        QCOMPARE(changed.size(), 0);
        /* a login: the session's window */
        views.setDesktop(true);
        QCOMPARE(views.view(), View::Desktop);
        QVERIFY(views.hasDesktop());
        views.setDesktop(true);
        QCOMPARE(changed.size(), 1);
        /* a logout, or a shutdown from the session: its login screen, or its
           messages, on the VM's screen */
        views.setDesktop(false);
        QVERIFY(!views.hasDesktop());
        QCOMPARE(views.view(), View::Screen);
        QCOMPARE(changed.size(), 2);
        QCOMPARE(changed.last().first().value<View>(), View::Screen);
        /* a reboot under the session (the console serves the nested desktop
           anew): the screen, the window forgotten */
        views.setDesktop(true);
        views.powerOn();
        QCOMPARE(views.view(), View::Screen);
        QVERIFY(!views.hasDesktop());
        /* the old window's going, late: nothing moves */
        views.setDesktop(false);
        QCOMPARE(views.view(), View::Screen);
        QCOMPARE(changed.size(), 4);
    }

    /* The user's choice holds until the window comes or goes, or the guest restarts */
    void chosen()
    {
        ConsoleViews views;

        views.powerOn();
        /* no window yet: the view says the desktop is not running */
        views.choose(View::Desktop);
        QCOMPARE(views.view(), View::Desktop);
        views.setDesktop(true);
        QCOMPARE(views.view(), View::Desktop);
        /* the screen behind the session */
        views.choose(View::Screen);
        QCOMPARE(views.view(), View::Screen);
        views.setDesktop(false);
        QCOMPARE(views.view(), View::Screen);
        /* the window coming brings it, whatever was chosen */
        views.choose(View::Screen);
        views.setDesktop(true);
        QCOMPARE(views.view(), View::Desktop);
        views.choose(View::Screen);
        views.powerOn();
        QCOMPARE(views.view(), View::Screen);
        views.choose(View::Desktop);
        views.powerOn();
        QCOMPARE(views.view(), View::Screen);
    }

    /*
     * The console of a VM whose nested desktop shows here, its QEMU found
     * running (vitrine started again) with the guest tools' agent behind
     * the port: the VM's screen at first, wherever the guest is, the
     * session's window, the host's Reset under the session, a logout.  What
     * the agent says of the guest's screen, and QEMU's guest agent, move
     * nothing: there is one screen.
     */
    void consoleFollowsTheWindow()
    {
        QTemporaryDir tmp;
        const QString id = QString("vitrine-cv-%1").arg(QCoreApplication::applicationPid());
        const QString dir = tmp.filePath(id);
        const ArgsFile args = ArgsFile::parse("-machine q35\n-vga none\n"
                                              "-device virtio-vga-rutabaga,cross-domain=on\n"
                                              "-display dbus,p2p=yes,gl=on\n");
        QVERIFY(QDir().mkpath(dir));
        QFile file(dir + "/vm.args");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(args.toText().toUtf8());
        file.close();
        /* as vitrine started it: the display's monitor, the nested desktop's socket */
        const QStringList command = VmRunner(id, dir).commandLine(args);
        const QString runDir = QFileInfo(command.last()).absolutePath();
        QVERIFY(command.join(' ').contains("wayland-socket-path=" + runDir + "/nested.sock"));

        FakeQmp qmp(runDir + "/qmp.sock");
        QLocalServer agent;
        QLocalServer::removeServer(runDir + "/agent.sock");
        QVERIFY(agent.listen(runDir + "/agent.sock"));
        QPointer<QLocalSocket> peer;
        connect(&agent, &QLocalServer::newConnection, this,
                [&]() { peer = agent.nextPendingConnection(); });
        QProcess qemu;
        qemu.start(FAKE_QEMU, command.mid(1));
        QVERIFY(qemu.waitForStarted());
        QVERIFY(QTest::qWaitFor([&qemu]() {
            QFile f(QString("/proc/%1/cmdline").arg(qemu.processId()));
            return f.open(QIODevice::ReadOnly) && f.readAll().contains("-pidfile");
        }, 5000));
        QFile pid(runDir + "/qemu.pid");
        QVERIFY(pid.open(QIODevice::WriteOnly));
        pid.write(QByteArray::number(qemu.processId()) + '\n');
        pid.close();
        const auto end = qScopeGuard([&]() {
            qemu.kill();
            qemu.waitForFinished();
            VmConsole::setDesktopViewFactory({});
        });
        VmConsole::setDesktopViewFactory([](const QString &socket, QString *, QObject *parent) {
            return new StandInDesktop(socket, parent);
        });

        VmStore store(tmp.path());
        Vm *vm = store.find(id);
        QVERIFY(vm);
        VmConsole console(vm);
        vm->runner()->attach(vm->args());
        QTRY_COMPARE(vm->runner()->state(), VmRunner::State::Running);
        QVERIFY(console.hasViews());
        auto *desktop = qobject_cast<StandInDesktop *>(console.nestedView());
        QVERIFY(desktop);
        QCOMPARE(console.currentView(), View::Screen);
        /* the one card's console */
        QCOMPARE(NestedDesktop::screenConsole(vm->runner()->runArgs()), 0);
        QTRY_VERIFY(peer);
        auto say = [&peer](const QByteArray &line) { peer->write(line + '\n'); };
        GuestToolsMonitor *tools = GuestToolsMonitor::of(vm);

        /* the agent, its login screen: the screen shows it already */
        say(R"({"type":"hello","protocol":3,"agent":"0.1.0","status":)"
            R"({"tools":"0.1.0-22.fc44","installing":false,"screen":"login"})}");
        QTRY_VERIFY(tools->hasAgent());
        QCOMPARE(console.currentView(), View::Screen);
        /* a login: the session's window */
        say(R"({"type":"screen","screen":""})");
        desktop->guestMaps(true);
        QCOMPARE(console.currentView(), View::Desktop);
        /* the agent, and QEMU's, going and coming: nothing moves */
        qmp.event("VSERPORT_CHANGE", {{"id", "vitrine-ga-port"}, {"open", false}});
        qmp.event("VSERPORT_CHANGE", {{"id", GuestTools::kPortId}, {"open", false}});
        QTRY_VERIFY(!tools->hasAgent());
        QTest::qWait(100);
        QCOMPARE(console.currentView(), View::Desktop);

        /*
         * The host's Reset under the session: the screen, and the nested
         * desktop served anew - QEMU keeps the session's connection open
         * across that reset, whose window would count as shown still: the
         * next session's would change nothing
         */
        const QPointer<StandInDesktop> before = desktop;
        qmp.event("RESET");
        QTRY_COMPARE(console.currentView(), View::Screen);
        QTRY_VERIFY(!before);
        desktop = qobject_cast<StandInDesktop *>(console.nestedView());
        QVERIFY(desktop);
        QCOMPARE(desktop->socket(), runDir + "/nested.sock");
        /* the next boot's session, then a logout: its login screen */
        desktop->guestMaps(true);
        QCOMPARE(console.currentView(), View::Desktop);
        desktop->guestMaps(false);
        QCOMPARE(console.currentView(), View::Screen);
    }

    /* Ctrl+Alt+1/2 by the key's place: AZERTY's & and é */
    void keys_data()
    {
        const Qt::KeyboardModifiers ctrlAlt = Qt::ControlModifier | Qt::AltModifier;

        QTest::addColumn<int>("key");
        QTest::addColumn<Qt::KeyboardModifiers>("modifiers");
        QTest::addColumn<quint32>("scanCode");
        QTest::addColumn<QString>("text");
        QTest::addColumn<std::optional<View>>("view");
        QTest::newRow("AZERTY &") << int(Qt::Key_Ampersand) << ctrlAlt << kKey1 << "&"
                                  << std::optional(View::Screen);
        QTest::newRow("AZERTY é") << int(Qt::Key_Eacute) << ctrlAlt << kKey2 << "é"
                                  << std::optional(View::Desktop);
        /* no third view */
        QTest::newRow("AZERTY \"") << int(Qt::Key_QuoteDbl) << ctrlAlt << kKey3 << "\""
                                   << std::optional<View>();
        QTest::newRow("QWERTY 2") << int(Qt::Key_2) << ctrlAlt << kKey2 << "2"
                                  << std::optional(View::Desktop);
        QTest::newRow("keypad 1") << int(Qt::Key_1) << (ctrlAlt | Qt::KeypadModifier) << kKp1
                                  << "1" << std::optional(View::Screen);
        QTest::newRow("keypad 3") << int(Qt::Key_3) << (ctrlAlt | Qt::KeypadModifier) << kKp3
                                  << "3" << std::optional<View>();
        /* a 1 elsewhere than the 1's place: not it */
        QTest::newRow("1 on A's place") << int(Qt::Key_1) << ctrlAlt << kKeyA << "1"
                                        << std::optional<View>();
        /* no place given (some platforms): Qt's key */
        QTest::newRow("no scan code") << int(Qt::Key_2) << ctrlAlt << quint32(0) << "2"
                                      << std::optional(View::Desktop);
        QTest::newRow("no scan code, 3") << int(Qt::Key_3) << ctrlAlt << quint32(0) << "3"
                                         << std::optional<View>();
        QTest::newRow("Ctrl alone") << int(Qt::Key_Ampersand) << Qt::KeyboardModifiers(
                                           Qt::ControlModifier) << kKey1 << "&"
                                    << std::optional<View>();
        QTest::newRow("Ctrl+Alt+Shift") << int(Qt::Key_1) << (ctrlAlt | Qt::ShiftModifier)
                                        << kKey1 << "1" << std::optional<View>();
        QTest::newRow("Ctrl+Alt+F") << int(Qt::Key_F) << ctrlAlt << kKeyF << "f"
                                    << std::optional<View>();
    }

    void keys()
    {
        QFETCH(int, key);
        QFETCH(Qt::KeyboardModifiers, modifiers);
        QFETCH(quint32, scanCode);
        QFETCH(QString, text);
        QFETCH(std::optional<View>, view);

        QCOMPARE(viewOf(key, modifiers, scanCode, text), view);
    }
};

QTEST_MAIN(TestConsoleViews)
#include "test_consoleviews.moc"
