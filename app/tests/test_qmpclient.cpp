// SPDX-License-Identifier: GPL-2.0-or-later
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "core/qmpclient.h"

/* The QEMU end of a QMP connection, answering as told */
class FakeQmp : public QObject
{
    Q_OBJECT

public:
    explicit FakeQmp(const QString &path)
    {
        server.listen(path);
        connect(&server, &QLocalServer::newConnection, this, [this]() {
            peer = server.nextPendingConnection();
            connect(peer, &QLocalSocket::readyRead, this, &FakeQmp::read);
            if (greet) {
                write(R"({"QMP": {"version": {"qemu": {"major": 11}}, "capabilities": ["oob"]}})"
                      "\n");
            }
        });
    }

    void write(const QByteArray &data)
    {
        peer->write(data);
        peer->flush();
    }
    void reply(const QJsonObject &command, const QByteArray &body)
    {
        write("{" + body + ", \"id\": " + QByteArray::number(command["id"].toInteger()) +
              "}\n");
    }

    QLocalServer server;
    QLocalSocket *peer = nullptr;
    QList<QJsonObject> commands;
    bool greet = true;
    bool autoCapabilities = true;

signals:
    void received(const QJsonObject &command);

private:
    void read()
    {
        buffer += peer->readAll();
        qsizetype newline;
        while ((newline = buffer.indexOf('\n')) >= 0) {
            const QJsonObject command = QJsonDocument::fromJson(buffer.left(newline)).object();
            buffer.remove(0, newline + 1);
            if (autoCapabilities && command["execute"] == "qmp_capabilities") {
                reply(command, R"("return": {})");
                continue;
            }
            commands << command;
            emit received(command);
        }
    }

    QByteArray buffer;
};

class TestQmpClient : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir tmp;
    QString socket;
    int count = 0;
    QString path() const { return socket; }

private slots:
    void init()
    {
        socket = tmp.filePath(QString("qmp%1.sock").arg(++count));
    }

    void greetingThenReady()
    {
        FakeQmp qemu(path());
        QmpClient client;
        QSignalSpy ready(&client, &QmpClient::ready);

        QVERIFY(!client.isReady());
        client.connectToSocket(path());
        QVERIFY(ready.wait());
        QVERIFY(client.isReady());
    }

    void queuedUntilReady()
    {
        FakeQmp qemu(path());
        QmpClient client;
        QSignalSpy received(&qemu, &FakeQmp::received);
        QJsonValue result;

        qemu.autoCapabilities = false;
        client.execute("query-status", {}, [&](const QJsonValue &r, const QString &error) {
            QVERIFY(error.isEmpty());
            result = r;
        });
        client.connectToSocket(path());
        QVERIFY(received.wait());
        /* nothing but the capabilities before they are accepted */
        QCOMPARE(qemu.commands.size(), 1);
        QCOMPARE(qemu.commands[0]["execute"].toString(), "qmp_capabilities");
        qemu.reply(qemu.commands[0], R"("return": {})");
        QVERIFY(received.wait());
        QCOMPARE(qemu.commands[1]["execute"].toString(), "query-status");
        qemu.reply(qemu.commands[1], R"("return": {"running": true, "status": "running"})");
        QTRY_VERIFY(result.isObject());
        QCOMPARE(result["status"].toString(), "running");
    }

    void repliesMatchedById()
    {
        FakeQmp qemu(path());
        QmpClient client;
        QSignalSpy received(&qemu, &FakeQmp::received);
        QStringList order;

        client.connectToSocket(path());
        client.execute("first", {{"x", 1}}, [&](const QJsonValue &r, const QString &) {
            order << "first:" + r.toString();
        });
        client.execute("second", {}, [&](const QJsonValue &r, const QString &) {
            order << "second:" + r.toString();
        });
        QTRY_COMPARE(qemu.commands.size(), 2);
        QCOMPARE(qemu.commands[0]["arguments"].toObject()["x"].toInt(), 1);
        QVERIFY(!qemu.commands[1].contains("arguments"));
        /* out of order, in one write */
        qemu.write(R"({"return": "b", "id": )" +
                   QByteArray::number(qemu.commands[1]["id"].toInteger()) + "}\n" +
                   R"({"return": "a", "id": )" +
                   QByteArray::number(qemu.commands[0]["id"].toInteger()) + "}\n");
        QTRY_COMPARE(order, QStringList({"second:b", "first:a"}));
    }

    void errorReply()
    {
        FakeQmp qemu(path());
        QmpClient client;
        QString error;

        client.connectToSocket(path());
        client.execute("cont", {}, [&](const QJsonValue &, const QString &e) { error = e; });
        QTRY_COMPARE(qemu.commands.size(), 1);
        qemu.reply(qemu.commands[0],
                   R"("error": {"class": "GenericError", "desc": "Resetting the VM failed"})");
        QTRY_COMPARE(error, "Resetting the VM failed");
    }

    void eventsAndSplitMessages()
    {
        FakeQmp qemu(path());
        QmpClient client;
        QSignalSpy ready(&client, &QmpClient::ready);
        QSignalSpy events(&client, &QmpClient::qmpEvent);

        client.connectToSocket(path());
        QVERIFY(ready.wait());
        qemu.write(R"({"event": "STOP", "data": {}, "timest)");
        QTest::qWait(50);
        QCOMPARE(events.size(), 0);
        qemu.write(R"(amp": {"seconds": 1, "microseconds": 2}})" "\n"
                   R"({"event": "SHUTDOWN", "data": {"guest": true, "reason": "guest-shutdown"}})"
                   "\n");
        QTRY_COMPARE(events.size(), 2);
        QCOMPARE(events[0][0].toString(), "STOP");
        QCOMPARE(events[1][0].toString(), "SHUTDOWN");
        QCOMPARE(events[1][1].toJsonObject()["reason"].toString(), "guest-shutdown");
    }

    void disconnectFailsPending()
    {
        FakeQmp qemu(path());
        QmpClient client;
        QSignalSpy disconnected(&client, &QmpClient::disconnected);
        QString error;
        bool called = false;

        client.connectToSocket(path());
        client.execute("quit", {}, [&](const QJsonValue &, const QString &e) {
            called = true;
            error = e;
        });
        QTRY_COMPARE(qemu.commands.size(), 1);
        qemu.peer->close();
        QVERIFY(disconnected.wait());
        QVERIFY(called);
        QVERIFY(!error.isEmpty());
        QVERIFY(!client.isReady());
    }

    void closedBeforeReadyIsAFailure()
    {
        FakeQmp qemu(path());
        QmpClient client;
        QSignalSpy failed(&client, &QmpClient::connectionFailed);
        QSignalSpy disconnected(&client, &QmpClient::disconnected);
        QSignalSpy ready(&client, &QmpClient::ready);
        bool called = false;

        qemu.greet = false;
        client.execute("query-status", {}, [&](const QJsonValue &, const QString &) {
            called = true;
        });
        client.connectToSocket(path());
        QTRY_VERIFY(qemu.peer);
        qemu.peer->close();
        QVERIFY(failed.wait());
        QCOMPARE(disconnected.size(), 0);
        /* still queued for the next connection */
        QVERIFY(!called);

        qemu.greet = true;
        client.connectToSocket(path());
        QVERIFY(ready.wait());
        QTRY_COMPARE(qemu.commands.size(), 1);
        QCOMPARE(qemu.commands[0]["execute"].toString(), "query-status");
    }

    void noServer()
    {
        QmpClient client;
        QSignalSpy failed(&client, &QmpClient::connectionFailed);

        client.connectToSocket(tmp.filePath("nothing.sock"));
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY(!failed[0][0].toString().isEmpty());
    }

    void disconnectFromSocketIsQuiet()
    {
        FakeQmp qemu(path());
        QmpClient client;
        QSignalSpy ready(&client, &QmpClient::ready);
        QSignalSpy disconnected(&client, &QmpClient::disconnected);
        QString error;

        client.connectToSocket(path());
        QVERIFY(ready.wait());
        client.execute("stop", {}, [&](const QJsonValue &, const QString &e) { error = e; });
        client.disconnectFromSocket();
        QVERIFY(!error.isEmpty());
        QTest::qWait(50);
        QCOMPARE(disconnected.size(), 0);
    }

    void deletedInCallback()
    {
        FakeQmp qemu(path());
        auto *client = new QmpClient;
        QPointer<QmpClient> guard(client);

        client->connectToSocket(path());
        client->execute("a", {}, [&](const QJsonValue &, const QString &) { delete client; });
        client->execute("b", {}, [&](const QJsonValue &, const QString &) {
            QFAIL("called after its client was deleted");
        });
        QTRY_COMPARE(qemu.commands.size(), 2);
        qemu.write(R"({"return": {}, "id": )" +
                   QByteArray::number(qemu.commands[0]["id"].toInteger()) + "}\n" +
                   R"({"return": {}, "id": )" +
                   QByteArray::number(qemu.commands[1]["id"].toInteger()) + "}\n");
        QTRY_VERIFY(!guard);
        QTest::qWait(50);
    }
};

QTEST_GUILESS_MAIN(TestQmpClient)
#include "test_qmpclient.moc"
