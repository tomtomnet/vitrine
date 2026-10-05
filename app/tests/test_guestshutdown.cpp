// SPDX-License-Identifier: GPL-2.0-or-later
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include "core/guestshutdown.h"

using Way = GuestShutdown::Way;

/*
 * A stand-in for qemu-ga behind QEMU's socket: answers guest-sync, then
 * says nothing to guest-shutdown (qemu-ga's success), refuses it, or
 * closes the connection (QEMU going away with the guest)
 */
class FakeQga : public QObject
{
public:
    enum class Shutdown { Silent, Refuse, Close };

    bool answersSync = true;
    Shutdown shutdown = Shutdown::Silent;
    int refuseAfterMs = 0;
    QList<QJsonObject> commands;    // what came, guest-sync aside
    int connections = 0;

    explicit FakeQga(const QString &path)
    {
        m_server.listen(path);
        connect(&m_server, &QLocalServer::newConnection, this, [this]() {
            QLocalSocket *peer = m_server.nextPendingConnection();
            connections++;
            connect(peer, &QLocalSocket::readyRead, this, [this, peer]() { read(peer); });
        });
    }

private:
    void read(QLocalSocket *peer)
    {
        m_buffer += peer->readAll();
        for (qsizetype nl; (nl = m_buffer.indexOf('\n')) >= 0;) {
            QByteArray line = m_buffer.left(nl);
            m_buffer.remove(0, nl + 1);
            line = line.mid(line.lastIndexOf('\xff') + 1);
            const QJsonObject m = QJsonDocument::fromJson(line).object();

            if (m["execute"] == "guest-sync-delimited") {
                if (answersSync) {
                    peer->write("\xff" + QJsonDocument(QJsonObject{{"return", m["arguments"]["id"]}})
                                             .toJson(QJsonDocument::Compact) + '\n');
                }
                continue;
            }
            commands << m;
            if (m["execute"] != "guest-shutdown") {
                continue;
            }
            if (shutdown == Shutdown::Refuse) {
                const QJsonObject error{{"class", "GenericError"},
                                        {"desc", "child process has failed to shutdown"}};
                const QByteArray line = QJsonDocument(QJsonObject{{"error", error}, {"id", m["id"]}})
                                            .toJson(QJsonDocument::Compact) + '\n';
                QTimer::singleShot(refuseAfterMs, peer, [peer, line]() { peer->write(line); });
            } else if (shutdown == Shutdown::Close) {
                peer->disconnectFromServer();
            }
        }
    }

    QLocalServer m_server;
    QByteArray m_buffer;
};

class TestGuestShutdown : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;

    struct Run {
        GuestShutdown shutdown;
        QList<Way> ways;
        int asked = 0;
        int pressed = 0;
        bool running = true;
        std::function<void(bool)> answer;      // the tools agent's, until it answers

        Run()
        {
            QObject::connect(&shutdown, &GuestShutdown::wayChanged,
                             [this](Way way) { ways << way; });
            shutdown.setPowerButton([this]() { pressed++; });
            shutdown.setRunning([this]() { return running; });
        }
        /* a guest tools' agent that takes the request (@takes), answering later */
        void toolsAgent(bool present)
        {
            shutdown.setToolsAgent([this, present](const std::function<void(bool)> &a) {
                asked++;
                if (present) {
                    answer = a;
                }
                return present;
            });
        }
        void answerTools(bool took)
        {
            if (const auto a = std::exchange(answer, {})) {
                a(took);
            }
        }
    };

    QString socket() const { return m_tmp.filePath("qga.sock"); }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        GuestShutdown::setGuestAgentTimeouts(300, 300);
    }

    /* The guest tools' agent first: once it took the request, nothing else */
    void toolsAgentTakes()
    {
        FakeQga qga(socket());
        Run run;
        run.toolsAgent(true);
        run.shutdown.setGuestAgent(socket());

        run.shutdown.start();
        QCOMPARE(run.asked, 1);
        QCOMPARE(run.shutdown.way(), Way::ToolsAgent);
        QVERIFY(run.shutdown.isBusy());
        run.answerTools(true);
        QVERIFY(!run.shutdown.isBusy());
        QTest::qWait(400);
        QCOMPARE(run.ways, QList<Way>{Way::ToolsAgent});
        QCOMPARE(qga.connections, 0);
        QCOMPARE(run.pressed, 0);
    }

    /* It refuses or does not answer (its monitor answers false then):
       qemu-ga's guest-shutdown, which shuts the guest down without a word */
    void thenQemuGuestAgent()
    {
        FakeQga qga(socket());
        Run run;
        run.toolsAgent(true);
        run.shutdown.setGuestAgent(socket());

        run.shutdown.start();
        run.answerTools(false);
        QCOMPARE(run.shutdown.way(), Way::GuestAgent);
        QTRY_COMPARE(qga.commands.size(), 1);
        QCOMPARE(qga.commands[0]["execute"].toString(), "guest-shutdown");
        QCOMPARE(qga.commands[0]["arguments"].toObject()["mode"].toString(), "powerdown");
        QVERIFY(run.shutdown.isBusy());
        /* no refusal in time: done */
        QTRY_VERIFY(!run.shutdown.isBusy());
        QTest::qWait(100);
        QCOMPARE(run.ways, QList<Way>({Way::ToolsAgent, Way::GuestAgent}));
        QCOMPARE(run.pressed, 0);
    }

    /* No tools agent: qemu-ga at once */
    void noToolsAgent()
    {
        FakeQga qga(socket());
        Run run;
        run.toolsAgent(false);
        run.shutdown.setGuestAgent(socket());

        run.shutdown.start();
        QCOMPARE(run.asked, 1);
        QTRY_COMPARE(qga.commands.size(), 1);
        QTRY_VERIFY(!run.shutdown.isBusy());
        QCOMPARE(run.ways, QList<Way>{Way::GuestAgent});
        QCOMPARE(run.pressed, 0);
    }

    /* The whole order: the tools agent declines, qemu-ga refuses, the button */
    void powerButtonLast()
    {
        FakeQga qga(socket());
        Run run;
        run.toolsAgent(true);
        run.shutdown.setGuestAgent(socket());
        qga.shutdown = FakeQga::Shutdown::Refuse;

        run.shutdown.start();
        run.answerTools(false);
        QTRY_COMPARE(run.pressed, 1);
        QCOMPARE(run.ways, QList<Way>({Way::ToolsAgent, Way::GuestAgent, Way::PowerButton}));
        QCOMPARE(run.shutdown.way(), Way::PowerButton);
        QVERIFY(!run.shutdown.isBusy());
        QCOMPARE(qga.commands.size(), 1);
    }

    /* A qemu-ga that does not answer (the port open but no agent behind it) */
    void guestAgentSilent()
    {
        FakeQga qga(socket());
        Run run;
        run.toolsAgent(false);
        run.shutdown.setGuestAgent(socket());
        qga.answersSync = false;

        run.shutdown.start();
        QCOMPARE(run.shutdown.way(), Way::GuestAgent);
        QTRY_COMPARE(run.pressed, 1);
        QCOMPARE(qga.commands.size(), 0);
        QCOMPARE(run.ways, QList<Way>({Way::GuestAgent, Way::PowerButton}));
    }

    /* The port closed in the guest (no qemu-ga there): the button, no wait */
    void portClosed()
    {
        FakeQga qga(socket());
        Run run;
        int checked = 0;
        run.toolsAgent(false);
        run.shutdown.setGuestAgent(socket(), [&checked](const std::function<void(bool)> &answer) {
            checked++;
            QTimer::singleShot(0, [answer]() { answer(false); });
        });

        run.shutdown.start();
        QTRY_COMPARE(run.pressed, 1);
        QCOMPARE(checked, 1);
        QCOMPARE(qga.connections, 0);
        QCOMPARE(run.ways, QList<Way>{Way::PowerButton});

        /* open: tried */
        run.shutdown.setGuestAgent(socket(), [](const std::function<void(bool)> &answer) {
            answer(true);
        });
        run.shutdown.start();
        QTRY_COMPARE(qga.commands.size(), 1);
        QTRY_VERIFY(!run.shutdown.isBusy());
        QCOMPARE(run.pressed, 1);
    }

    /* A run without qemu-ga's port, or without anything: the button */
    void noGuestAgent()
    {
        Run run;
        run.shutdown.start();
        QCOMPARE(run.pressed, 1);
        QCOMPARE(run.ways, QList<Way>{Way::PowerButton});
        QVERIFY(!run.shutdown.isBusy());
        /* asked again: again, and told again, for the window to say it again */
        run.shutdown.start();
        QCOMPARE(run.pressed, 2);
        QCOMPARE(run.ways, QList<Way>({Way::PowerButton, Way::PowerButton}));
    }

    /* Not twice while a request is on its way; again once it took a way */
    void notTwice()
    {
        Run run;
        run.toolsAgent(true);

        run.shutdown.start();
        run.shutdown.start();
        QCOMPARE(run.asked, 1);
        run.answerTools(true);
        run.shutdown.start();
        QCOMPARE(run.asked, 2);
        QCOMPARE(run.ways, QList<Way>({Way::ToolsAgent, Way::ToolsAgent}));
    }

    /* A refusal that takes long, shutdown(8) failing late: still the button */
    void lateRefusal()
    {
        FakeQga qga(socket());
        Run run;
        run.toolsAgent(false);
        run.shutdown.setGuestAgent(socket());
        qga.shutdown = FakeQga::Shutdown::Refuse;
        qga.refuseAfterMs = 150;

        run.shutdown.start();
        QTRY_COMPARE(run.pressed, 1);
        QCOMPARE(run.ways, QList<Way>({Way::GuestAgent, Way::PowerButton}));
    }

    /* The guest shut down, or the run ended: late answers do nothing */
    void reset()
    {
        FakeQga qga(socket());
        Run run;
        run.toolsAgent(true);
        run.shutdown.setGuestAgent(socket());

        run.shutdown.start();
        const auto late = run.answer;
        run.shutdown.reset();
        QCOMPARE(run.shutdown.way(), Way::None);
        QVERIFY(!run.shutdown.isBusy());
        late(false);
        QTest::qWait(400);
        QCOMPARE(qga.connections, 0);
        QCOMPARE(run.pressed, 0);
        QCOMPARE(run.ways, QList<Way>({Way::ToolsAgent, Way::None}));

        /* while qemu-ga is asked */
        run.ways.clear();
        run.shutdown.start();
        run.answerTools(false);
        QTRY_COMPARE(qga.connections, 1);
        run.shutdown.reset();
        QTest::qWait(400);
        QCOMPARE(run.pressed, 0);
        QCOMPARE(run.ways, QList<Way>({Way::ToolsAgent, Way::GuestAgent, Way::None}));
    }

    /* QEMU closing qemu-ga's socket after guest-shutdown: it goes, with the
       guest; and a guest gone before the button: no button */
    void guestGoes()
    {
        FakeQga qga(socket());
        Run run;
        run.toolsAgent(false);
        run.shutdown.setGuestAgent(socket());
        qga.shutdown = FakeQga::Shutdown::Close;

        run.shutdown.start();
        QTRY_VERIFY(!run.shutdown.isBusy());
        QCOMPARE(run.pressed, 0);
        QCOMPARE(run.ways, QList<Way>{Way::GuestAgent});

        qga.shutdown = FakeQga::Shutdown::Refuse;
        run.running = false;
        run.shutdown.start();
        QTRY_VERIFY(!run.shutdown.isBusy());
        QCOMPARE(run.pressed, 0);
        QCOMPARE(run.ways, QList<Way>({Way::GuestAgent, Way::GuestAgent}));
    }

    /* Destroyed with a request on its way, the answers coming after */
    void destroyedOnTheWay()
    {
        FakeQga qga(socket());
        int pressed = 0;
        std::function<void(bool)> answer;
        {
            GuestShutdown shutdown;
            shutdown.setToolsAgent([&answer](const std::function<void(bool)> &a) {
                answer = a;
                return true;
            });
            shutdown.setGuestAgent(socket());
            shutdown.setPowerButton([&pressed]() { pressed++; });
            shutdown.start();
        }
        QVERIFY(answer);
        answer(false);
        QTest::qWait(100);
        QCOMPARE(pressed, 0);
        QCOMPARE(qga.connections, 0);
    }
};

QTEST_GUILESS_MAIN(TestGuestShutdown)
#include "test_guestshutdown.moc"
