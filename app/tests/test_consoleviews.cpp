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

/* XKB's keycodes of the keys 1, 2, 3 above the letters, and of A, F, KP_1 */
static constexpr quint32 kKey1 = 10, kKey2 = 11, kKey3 = 12, kKeyA = 38, kKeyF = 41, kKp1 = 87;

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
    /* the tests' wait for an agent that went; the wiring test's, through
       sockets and the event loop */
    static constexpr int kWaitMs = 200;
    static constexpr int kWiringWaitMs = 1500;

private slots:
    /* the settings the guest tools' monitor keeps of a VM, and the rest, in here */
    void initTestCase()
    {
        QVERIFY(m_home.isValid());
        qputenv("XDG_DATA_HOME", m_home.filePath("data").toUtf8());
        qputenv("XDG_CONFIG_HOME", m_home.filePath("config").toUtf8());
        qputenv("XDG_CACHE_HOME", m_home.filePath("cache").toUtf8());
        /* the wait for an agent that went, shortened (a second or two in use) */
        ConsoleViews::setAgentWait(kWaitMs);
    }

    void cleanupTestCase()
    {
        ConsoleViews::setAgentWait(0);
    }

    /* From power-on: the boot, the guest's login screen, its desktop, and back */
    void automatic()
    {
        ConsoleViews views;
        QSignalSpy changed(&views, &ConsoleViews::viewChanged);

        views.powerOn();
        QCOMPARE(views.view(), View::Boot);
        QCOMPARE(changed.size(), 0);
        /* the guest tools' agent starts early in the boot: its system runs,
           its login screen is not there yet */
        views.setGuest(true, false);
        QCOMPARE(views.view(), View::Boot);
        QCOMPARE(changed.size(), 0);
        /* the login screen, on the guest's own screen */
        views.setGuest(true, true);
        QCOMPARE(views.view(), View::Screen);
        QCOMPARE(changed.size(), 1);
        views.setGuest(true, true);
        QCOMPARE(changed.size(), 1);
        /* the user logs in: the login screen goes before the session's window
           comes, which the view waits for on the login screen */
        views.setGuest(true, false);
        QCOMPARE(views.view(), View::Screen);
        views.setDesktop(true);
        QCOMPARE(views.view(), View::Desktop);
        QVERIFY(views.hasDesktop());
        views.setDesktop(true);
        QCOMPARE(changed.size(), 2);
        /* a logout: the window goes, the login screen comes back a moment
           later; the boot's screen does not show in between */
        views.setDesktop(false);
        QVERIFY(!views.hasDesktop());
        QCOMPARE(views.view(), View::Desktop);
        views.setGuest(true, true);
        QCOMPARE(views.view(), View::Screen);
        QCOMPARE(changed.size(), 3);
        /* a reboot (the console serves the nested desktop anew): the boot
           again, then the login screen when it comes back */
        views.powerOn();
        QCOMPARE(views.view(), View::Boot);
        views.setGuest(true, false);
        QCOMPARE(views.view(), View::Boot);
        views.setGuest(true, true);
        QCOMPARE(views.view(), View::Screen);
        QCOMPARE(changed.size(), 5);
        QCOMPARE(changed.last().first().value<View>(), View::Screen);
        /* the next run starts at the boot, its desktop not there */
        views.setDesktop(true);
        views.powerOn();
        QCOMPARE(views.view(), View::Boot);
        QVERIFY(!views.hasDesktop());
    }

    /*
     * The guest tools' install at boot: the agent of the medium answers as
     * the install starts, minutes before any login screen, and goes with
     * the restart that ends it.  The install writes on the guest's console
     * and its boot splash: the boot's screen stays (the user's report)
     */
    void installAtBoot()
    {
        ConsoleViews views;

        views.powerOn();
        views.setGuest(true, false);
        QCOMPARE(views.view(), View::Boot);
        /* done: its agent goes, then the guest restarts */
        views.setGuest(false, false);
        QCOMPARE(views.view(), View::Boot);
        views.powerOn();
        QCOMPARE(views.view(), View::Boot);
        /* the agent of the tools now in, then the login screen */
        views.setGuest(true, false);
        QCOMPARE(views.view(), View::Boot);
        views.setGuest(true, true);
        QCOMPARE(views.view(), View::Screen);
    }

    /*
     * A shutdown or a restart from the guest's session: the window goes,
     * no login screen comes, the agent goes as the guest shuts down; the
     * guest's console, where it writes as it goes, is on the boot's screen
     */
    void shutDownFromSession()
    {
        ConsoleViews views;

        views.powerOn();
        views.setGuest(true, true);
        views.setDesktop(true);
        views.setGuest(true, false);
        QCOMPARE(views.view(), View::Desktop);
        /* the window first, then the agent, which might be restarting
           itself: Boot once it has been gone for the wait */
        views.setDesktop(false);
        QCOMPARE(views.view(), View::Desktop);
        views.setGuest(false, false);
        QCOMPARE(views.view(), View::Desktop);
        QTRY_COMPARE(views.view(), View::Boot);

        /* the agent first, while the window shows: then the window */
        views.powerOn();
        views.setGuest(true, true);
        views.setDesktop(true);
        views.setGuest(true, false);
        views.setGuest(false, false);
        QCOMPARE(views.view(), View::Desktop);
        views.setDesktop(false);
        QCOMPARE(views.view(), View::Boot);
    }

    /* Shut down from the login screen (its own button, or the host's Shut Down) */
    void shutDownFromLoginScreen()
    {
        ConsoleViews views;

        views.powerOn();
        views.setGuest(true, true);
        QCOMPARE(views.view(), View::Screen);
        /* the login screen goes first: nothing to look at there, but a session
           may come next (a login), so the view stays until the agent goes,
           and the wait for it after */
        views.setGuest(true, false);
        QCOMPARE(views.view(), View::Screen);
        views.setGuest(false, false);
        QCOMPARE(views.view(), View::Screen);
        QTRY_COMPARE(views.view(), View::Boot);
    }

    /*
     * The agent restarts itself at the login screen: to take over a new
     * version of itself after a tools update it ran, by systemd, or after a
     * crash.  Gone for a moment, back with the same login screen: the view
     * does not move, nor after the wait (the Boot of 0.3-1 s it flashed)
     */
    void agentRestarts()
    {
        ConsoleViews views;

        views.powerOn();
        views.setGuest(true, true);
        QCOMPARE(views.view(), View::Screen);
        QSignalSpy changed(&views, &ConsoleViews::viewChanged);
        views.setGuest(false, false);
        QCOMPARE(views.view(), View::Screen);
        views.setGuest(true, true);
        QCOMPARE(views.view(), View::Screen);
        QTest::qWait(2 * kWaitMs);
        QCOMPARE(views.view(), View::Screen);
        QCOMPARE(changed.size(), 0);
        /* the hello first, its screen a moment later: the same */
        views.setGuest(false, false);
        views.setGuest(true, false);
        views.setGuest(true, true);
        QTest::qWait(2 * kWaitMs);
        QCOMPARE(views.view(), View::Screen);
        QCOMPARE(changed.size(), 0);

        /* the boot's screen chosen at the login screen: it holds across the
           restart, as the login screen it comes back with is not new */
        views.choose(View::Boot);
        views.setGuest(false, false);
        views.setGuest(true, true);
        QTest::qWait(2 * kWaitMs);
        QCOMPARE(views.view(), View::Boot);
        QCOMPARE(changed.size(), 1);

        /* during the boot, before any login screen: Boot, and Screen as the
           login screen comes */
        views.powerOn();
        views.setGuest(true, false);
        views.setGuest(false, false);
        views.setGuest(true, false);
        QTest::qWait(2 * kWaitMs);
        QCOMPARE(views.view(), View::Boot);
        views.setGuest(true, true);
        QCOMPARE(views.view(), View::Screen);

        /* back within the wait with something its screen did not show
           before: shown */
        views.choose(View::Boot);
        views.setGuest(true, false);
        views.setGuest(false, false);
        views.setGuest(true, true);
        QCOMPARE(views.view(), View::Screen);
    }

    /*
     * The agent gone for good while the login screen shows (a shutdown, a
     * reboot that takes its time): Screen through the wait, then Boot; an
     * agent that comes back later (a crash's restart, 5 s on) with the
     * login screen: Screen again
     */
    void agentGoneForGood()
    {
        ConsoleViews views;
        QSignalSpy changed(&views, &ConsoleViews::viewChanged);

        views.powerOn();
        views.setGuest(true, true);
        QCOMPARE(changed.size(), 1);
        views.setGuest(false, false);
        /* what the old agent's monitor says again meanwhile changes nothing */
        views.setGuest(false, false);
        QCOMPARE(views.view(), View::Screen);
        QTRY_COMPARE(views.view(), View::Boot);
        QCOMPARE(changed.size(), 2);
        views.setGuest(true, true);
        QCOMPARE(views.view(), View::Screen);
        QCOMPARE(changed.size(), 3);
    }

    /*
     * The guest's system goes down: QEMU's guest agent closes its port too,
     * which the guest tools' agent restarting itself does not do.  Boot at
     * once, its shutdown messages there, in either order
     */
    void systemGoesDown()
    {
        ConsoleViews views;

        /* a reboot from the login screen: the tools' agent, then qemu-ga */
        views.powerOn();
        views.setQemuAgent(true);
        views.setGuest(true, true);
        views.setGuest(false, false);
        QCOMPARE(views.view(), View::Screen);
        views.setQemuAgent(false);
        QCOMPARE(views.view(), View::Boot);
        QSignalSpy changed(&views, &ConsoleViews::viewChanged);
        QTest::qWait(2 * kWaitMs);
        QCOMPARE(changed.size(), 0);

        /* qemu-ga first */
        views.powerOn();
        views.setQemuAgent(true);
        views.setGuest(true, true);
        views.setQemuAgent(false);
        QCOMPARE(views.view(), View::Screen);
        views.setGuest(false, false);
        QCOMPARE(views.view(), View::Boot);

        /* from the session, its window gone first: as soon as both agents are */
        views.powerOn();
        views.setQemuAgent(true);
        views.setGuest(true, true);
        views.setDesktop(true);
        views.setGuest(true, false);
        views.setDesktop(false);
        views.setGuest(false, false);
        QCOMPARE(views.view(), View::Desktop);
        views.setQemuAgent(false);
        QCOMPARE(views.view(), View::Boot);

        /* qemu-ga restarting alone (an update of it) moves nothing, and an
           agent's restart after it waits as any */
        views.powerOn();
        views.setQemuAgent(true);
        views.setGuest(true, true);
        views.setQemuAgent(false);
        views.setQemuAgent(true);
        QCOMPARE(views.view(), View::Screen);
        views.setGuest(false, false);
        QCOMPARE(views.view(), View::Screen);
        views.setGuest(true, true);
        QTest::qWait(2 * kWaitMs);
        QCOMPARE(views.view(), View::Screen);

        /* the user's choice during the wait holds through qemu-ga's going,
           until the reset */
        views.setGuest(false, false);
        views.choose(View::Screen);
        views.setQemuAgent(false);
        QCOMPARE(views.view(), View::Screen);
        views.powerOn();
        QCOMPARE(views.view(), View::Boot);

        /* the next boot knows nothing of the last one's qemu-ga: its own
           agent's restart waits, qemu-ga not up yet */
        views.setGuest(true, true);
        views.setGuest(false, false);
        QCOMPARE(views.view(), View::Screen);
        views.setGuest(true, true);
        QTest::qWait(2 * kWaitMs);
        QCOMPARE(views.view(), View::Screen);
    }

    /* The reset of a reboot during the wait: Boot at once, and no late change after it */
    void powerOnDuringTheWait()
    {
        ConsoleViews views;

        views.powerOn();
        views.setGuest(true, true);
        views.setGuest(false, false);
        QCOMPARE(views.view(), View::Screen);
        QSignalSpy changed(&views, &ConsoleViews::viewChanged);
        views.powerOn();
        QCOMPARE(views.view(), View::Boot);
        QCOMPARE(changed.size(), 1);
        /* the next boot's agent, and the user's look at its screen */
        views.setGuest(true, false);
        views.choose(View::Screen);
        QTest::qWait(2 * kWaitMs);
        QCOMPARE(views.view(), View::Screen);
        QCOMPARE(changed.size(), 2);
        /* the next boot's agent going: its own wait */
        views.setGuest(false, false);
        QTRY_COMPARE(views.view(), View::Boot);
    }

    /* The desktop coming, or the user's choice, during the wait: no Boot after it */
    void desktopOrChoiceDuringTheWait()
    {
        ConsoleViews views;

        views.powerOn();
        views.setGuest(true, true);
        views.setGuest(true, false);
        views.setGuest(false, false);
        views.setDesktop(true);
        QCOMPARE(views.view(), View::Desktop);
        QTest::qWait(2 * kWaitMs);
        QCOMPARE(views.view(), View::Desktop);
        /* gone for good: as any window that goes after its agent */
        views.setDesktop(false);
        QCOMPARE(views.view(), View::Boot);

        /* a window that came and goes within the wait: Boot after it, unless
           the agent comes back with the login screen */
        views.powerOn();
        views.setGuest(true, true);
        views.setGuest(false, false);
        views.setDesktop(true);
        views.setDesktop(false);
        QCOMPARE(views.view(), View::Desktop);
        QTRY_COMPARE(views.view(), View::Boot);
        views.powerOn();
        views.setGuest(true, true);
        views.setGuest(false, false);
        views.setDesktop(true);
        views.setDesktop(false);
        views.setGuest(true, true);
        QCOMPARE(views.view(), View::Screen);

        /* the user's choice */
        for (const View chosen : {View::Desktop, View::Screen, View::Boot}) {
            views.powerOn();
            views.setGuest(true, true);
            views.setGuest(false, false);
            QSignalSpy changed(&views, &ConsoleViews::viewChanged);
            views.choose(chosen);
            QTest::qWait(2 * kWaitMs);
            QCOMPARE(views.view(), chosen);
            QCOMPARE(changed.size(), chosen == View::Screen ? 0 : 1);
        }
    }

    /*
     * The host's Reset while the session shows: QEMU tells the reset, and
     * the session's window may go after it, as the guest's proxy
     * disconnects.  That window going must not bring the guest's screen
     * during the firmware (the user's report: Screen during a reboot).  (The
     * console serves the nested desktop anew at a reset, which drops that
     * window's view; the views hold without it too.)
     */
    void resetUnderSession()
    {
        ConsoleViews views;

        views.powerOn();
        views.setGuest(true, true);
        views.setDesktop(true);
        views.setGuest(true, false);
        views.powerOn();
        QCOMPARE(views.view(), View::Boot);
        QVERIFY(!views.hasDesktop());
        views.setDesktop(false);
        QCOMPARE(views.view(), View::Boot);
        /* what the old boot's monitor says late: its agent gone */
        views.setGuest(false, false);
        QCOMPARE(views.view(), View::Boot);
        /* the next boot as any */
        views.setGuest(true, false);
        QCOMPARE(views.view(), View::Boot);
        views.setGuest(true, true);
        QCOMPARE(views.view(), View::Screen);
    }

    /*
     * The guest's console on its own screen (its framebuffer console on the
     * virtio card, as the agent says): Screen, the shutdown's messages
     * there too while the agent can tell
     */
    void consoleOnGuestScreen()
    {
        ConsoleViews views;

        views.powerOn();
        views.setGuest(true, true);
        QCOMPARE(views.view(), View::Screen);
        views.setDesktop(true);
        views.setDesktop(false);
        QCOMPARE(views.view(), View::Screen);
    }

    /*
     * A QEMU found running (vitrine started again): the boot until the
     * guest's agent says what its screen shows, whatever the guest does -
     * its firmware, the tools' install, its login screen
     */
    void foundRunning()
    {
        ConsoleViews views;

        views.powerOn();
        QCOMPARE(views.view(), View::Boot);
        views.setGuest(true, false);
        QCOMPARE(views.view(), View::Boot);

        views.powerOn();
        views.setGuest(true, true);
        QCOMPARE(views.view(), View::Screen);
        /* without the agent: no screen to look at, whatever was said */
        views.powerOn();
        views.setGuest(false, true);
        QCOMPARE(views.view(), View::Boot);
    }

    /* The user's choice holds until the next change that moves the view */
    void chosen()
    {
        ConsoleViews views;

        views.powerOn();
        views.choose(View::Desktop);
        QCOMPARE(views.view(), View::Desktop);
        /* the agent answers: nothing to move to */
        views.setGuest(true, false);
        QCOMPARE(views.view(), View::Desktop);
        /* the login screen comes: shown */
        views.setGuest(true, true);
        QCOMPARE(views.view(), View::Screen);
        /* the boot, to look at it while the login screen shows: until it changes */
        views.choose(View::Boot);
        views.setGuest(true, true);
        QCOMPARE(views.view(), View::Boot);
        views.setDesktop(true);
        QCOMPARE(views.view(), View::Desktop);
        /* the boot while the session runs: what the guest's screen does meanwhile
           moves nothing, until the session ends */
        views.choose(View::Boot);
        views.setGuest(true, false);
        views.setGuest(true, true);
        QCOMPARE(views.view(), View::Boot);
        views.setDesktop(false);
        QCOMPARE(views.view(), View::Screen);
        views.choose(View::Desktop);
        QCOMPARE(views.view(), View::Desktop);
        /* a reboot */
        views.powerOn();
        QCOMPARE(views.view(), View::Boot);
    }

    /*
     * The console of a VM whose nested desktop shows here, its QEMU found
     * running (vitrine started again) with the guest tools' agent behind
     * the port: the boot's screen at first, wherever the guest is, then
     * what the agent says - through the tools' install at boot, the
     * restart that ends it, the login screen, the session, the host's Reset
     * under the session, a shutdown from the login screen
     */
    void consoleFollowsTheAgent()
    {
        QTemporaryDir tmp;
        const QString id = QString("vitrine-cv-%1").arg(QCoreApplication::applicationPid());
        const QString dir = tmp.filePath(id);
        const ArgsFile args = ArgsFile::parse("-machine q35\n-vga std\n"
                                              "-device virtio-gpu-rutabaga-pci,cross-domain=on\n"
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
        /* wherever the guest is: the boot's screen until its agent says */
        QCOMPARE(console.currentView(), View::Boot);
        QTRY_VERIFY(peer);
        auto say = [&peer](const QByteArray &line) { peer->write(line + '\n'); };
        auto hello = [&say](const QByteArray &status) {
            say(R"({"type":"hello","protocol":3,"agent":"0.1.0","status":)" + status + "}");
        };
        GuestToolsMonitor *tools = GuestToolsMonitor::of(vm);

        /* the tools' install at boot: qemu-ga answers, then the medium's agent */
        emit vm->runner()->guestOsInfo(QJsonObject{{"id", "fedora"}, {"version-id", "44"}});
        hello(R"({"tools":null,"installing":true,"bootstrap":"running","screen":""})");
        QTRY_VERIFY(tools->hasAgent());
        QTest::qWait(50);
        QCOMPARE(console.currentView(), View::Boot);
        /* done: its agent goes, the guest restarts */
        qmp.event("VSERPORT_CHANGE", {{"id", GuestTools::kPortId}, {"open", false}});
        QTRY_VERIFY(!tools->hasAgent());
        qmp.event("RESET");
        QTest::qWait(50);
        QCOMPARE(console.currentView(), View::Boot);

        /* the tools' agent early in the next boot, then the login screen */
        hello(R"({"tools":"0.1.0-20.fc44","installing":false,"screen":""})");
        QTRY_VERIFY(tools->hasAgent());
        QTest::qWait(50);
        QCOMPARE(console.currentView(), View::Boot);
        say(R"({"type":"screen","screen":"login"})");
        QTRY_COMPARE(console.currentView(), View::Screen);

        /* a login: the login screen goes, the session's window comes */
        say(R"({"type":"screen","screen":""})");
        QTRY_VERIFY(!tools->screenShown());
        QCOMPARE(console.currentView(), View::Screen);
        desktop->guestMaps(true);
        QCOMPARE(console.currentView(), View::Desktop);

        /*
         * The host's Reset under the session: the boot, and the nested
         * desktop served anew - QEMU keeps the session's connection open
         * across that reset, whose window would count as shown still: the
         * next session's would change nothing
         */
        const QPointer<StandInDesktop> before = desktop;
        qmp.event("RESET");
        QTRY_COMPARE(console.currentView(), View::Boot);
        QTRY_VERIFY(!before);
        desktop = qobject_cast<StandInDesktop *>(console.nestedView());
        QVERIFY(desktop);
        QCOMPARE(desktop->socket(), runDir + "/nested.sock");

        /* the next boot's login screen, a login: the new session shows */
        hello(R"({"tools":"0.1.0-20.fc44","installing":false,"screen":"login"})");
        QTRY_COMPARE(console.currentView(), View::Screen);
        say(R"({"type":"screen","screen":""})");
        QTRY_VERIFY(!tools->screenShown());
        QCOMPARE(console.currentView(), View::Screen);
        desktop->guestMaps(true);
        QCOMPARE(console.currentView(), View::Desktop);
        /* a logout: the window goes, the view waits for what the guest shows */
        desktop->guestMaps(false);
        QCOMPARE(console.currentView(), View::Desktop);
        say(R"({"type":"screen","screen":"login"})");
        QTRY_COMPARE(console.currentView(), View::Screen);

        /*
         * The agent restarts itself at the login screen (it took over a new
         * version of itself, or crashed): its port closes, and its hello
         * comes again with the login screen, within the wait - the view stays
         */
        ConsoleViews::setAgentWait(kWiringWaitMs);
        const auto shortWait = qScopeGuard([]() { ConsoleViews::setAgentWait(kWaitMs); });
        qmp.event("VSERPORT_CHANGE", {{"id", GuestTools::kPortId}, {"open", false}});
        QTRY_VERIFY(!tools->hasAgent());
        QCOMPARE(console.currentView(), View::Screen);
        hello(R"({"tools":"0.1.0-20.fc44","installing":false,"screen":"login"})");
        QTRY_VERIFY(tools->hasAgent());
        QTest::qWait(kWiringWaitMs + 200);
        QCOMPARE(console.currentView(), View::Screen);

        /* a reboot from the login screen: its agent goes, then QEMU's guest
           agent, as the system goes down: the boot's screen at once, before
           the wait's end, with the shutdown's messages */
        qmp.event("VSERPORT_CHANGE", {{"id", "vitrine-ga-port"}, {"open", true}});
        qmp.event("VSERPORT_CHANGE", {{"id", GuestTools::kPortId}, {"open", false}});
        QTRY_VERIFY(!tools->hasAgent());
        QCOMPARE(console.currentView(), View::Screen);
        qmp.event("VSERPORT_CHANGE", {{"id", "vitrine-ga-port"}, {"open", false}});
        QTRY_COMPARE_WITH_TIMEOUT(console.currentView(), View::Boot, kWiringWaitMs / 2);
        qmp.event("RESET");
        QTest::qWait(50);
        QCOMPARE(console.currentView(), View::Boot);
        hello(R"({"tools":"0.1.0-20.fc44","installing":false,"screen":"login"})");
        QTRY_COMPARE(console.currentView(), View::Screen);

        /* the same without QEMU's guest agent: the reset brings the boot's
           screen, before the wait's end */
        qmp.event("VSERPORT_CHANGE", {{"id", GuestTools::kPortId}, {"open", false}});
        QTRY_VERIFY(!tools->hasAgent());
        QCOMPARE(console.currentView(), View::Screen);
        qmp.event("RESET");
        QTRY_COMPARE_WITH_TIMEOUT(console.currentView(), View::Boot, kWiringWaitMs / 2);
        hello(R"({"tools":"0.1.0-20.fc44","installing":false,"screen":"login"})");
        QTRY_COMPARE(console.currentView(), View::Screen);

        /* a shutdown from the login screen: the boot's screen once the agent
           has been gone for the wait */
        say(R"({"type":"screen","screen":""})");
        QTRY_VERIFY(!tools->screenShown());
        QCOMPARE(console.currentView(), View::Screen);
        qmp.event("VSERPORT_CHANGE", {{"id", GuestTools::kPortId}, {"open", false}});
        QTRY_VERIFY(!tools->hasAgent());
        QCOMPARE(console.currentView(), View::Screen);
        QTRY_COMPARE(console.currentView(), View::Boot);
    }

    void qemuConsoles()
    {
        QCOMPARE(ConsoleViews::qemuConsole(View::Boot), 0);
        QCOMPARE(ConsoleViews::qemuConsole(View::Screen), 1);
        /* the D-Bus display stays on the guest's screen behind the desktop */
        QCOMPARE(ConsoleViews::qemuConsole(View::Desktop), 1);
    }

    /* Ctrl+Alt+1/2/3 by the key's place: AZERTY's &, é and " */
    void keys_data()
    {
        const Qt::KeyboardModifiers ctrlAlt = Qt::ControlModifier | Qt::AltModifier;

        QTest::addColumn<int>("key");
        QTest::addColumn<Qt::KeyboardModifiers>("modifiers");
        QTest::addColumn<quint32>("scanCode");
        QTest::addColumn<QString>("text");
        QTest::addColumn<std::optional<View>>("view");
        QTest::newRow("AZERTY &") << int(Qt::Key_Ampersand) << ctrlAlt << kKey1 << "&"
                                  << std::optional(View::Boot);
        QTest::newRow("AZERTY é") << int(Qt::Key_Eacute) << ctrlAlt << kKey2 << "é"
                                  << std::optional(View::Screen);
        QTest::newRow("AZERTY \"") << int(Qt::Key_QuoteDbl) << ctrlAlt << kKey3 << "\""
                                   << std::optional(View::Desktop);
        QTest::newRow("QWERTY 2") << int(Qt::Key_2) << ctrlAlt << kKey2 << "2"
                                  << std::optional(View::Screen);
        QTest::newRow("keypad 1") << int(Qt::Key_1) << (ctrlAlt | Qt::KeypadModifier) << kKp1
                                  << "1" << std::optional(View::Boot);
        /* a 1 elsewhere than the 1's place: not it */
        QTest::newRow("1 on A's place") << int(Qt::Key_1) << ctrlAlt << kKeyA << "1"
                                        << std::optional<View>();
        /* no place given (some platforms): Qt's key */
        QTest::newRow("no scan code") << int(Qt::Key_3) << ctrlAlt << quint32(0) << "3"
                                      << std::optional(View::Desktop);
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
