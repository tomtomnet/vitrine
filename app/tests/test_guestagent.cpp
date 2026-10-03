// SPDX-License-Identifier: GPL-2.0-or-later
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "core/guestagent.h"

/*
 * A stand-in for qemu-ga: answers guest-sync, and runs "programs" whose
 * behaviour the test sets: exit codes, output, how many polls they take
 */
class FakeAgent : public QObject
{
    Q_OBJECT

public:
    struct Program {
        int exitCode = 0;
        QByteArray out, err;
        int polls = 0;          // guest-exec-status calls before it exits
    };

    QLocalServer server;
    QLocalSocket *client = nullptr;
    QHash<QString, Program> programs;       // by the first argument
    QList<QStringList> commands;            // what ran
    QByteArray garbage;                     // sent before answering the sync

    explicit FakeAgent(const QString &path)
    {
        server.listen(path);
        connect(&server, &QLocalServer::newConnection, this, [this]() {
            client = server.nextPendingConnection();
            connect(client, &QLocalSocket::readyRead, this, &FakeAgent::read);
        });
    }

private:
    QByteArray buffer;
    QHash<qint64, std::pair<Program, int>> running;
    qint64 nextPid = 100;

    void reply(const QJsonObject &message, const QJsonValue &id, const QJsonValue &result)
    {
        QJsonObject r{{"return", result}};
        if (!id.isUndefined()) {
            r["id"] = id;
        }
        Q_UNUSED(message);
        client->write(QJsonDocument(r).toJson(QJsonDocument::Compact) + '\n');
    }

    void read()
    {
        buffer += client->readAll();
        qsizetype newline;
        while ((newline = buffer.indexOf('\n')) >= 0) {
            QByteArray line = buffer.left(newline);
            buffer.remove(0, newline + 1);
            line = line.mid(line.lastIndexOf('\xff') + 1);
            const QJsonObject m = QJsonDocument::fromJson(line).object();
            const QString command = m["execute"].toString();
            const QJsonObject args = m["arguments"].toObject();

            if (command == "guest-sync-delimited") {
                /* qemu-ga takes the 0xFF for a stray byte, and says so */
                client->write(garbage + "{\"error\": {\"class\": \"GenericError\", \"desc\": "
                                        "\"JSON parse error, stray '\\xff'\"}}\n\xff");
                reply(m, m["id"], args["id"]);
            } else if (command == "guest-exec") {
                QStringList argv{args["path"].toString()};
                for (const QJsonValue &a : args["arg"].toArray()) {
                    argv << a.toString();
                }
                commands << argv;
                const qint64 pid = nextPid++;
                running[pid] = {programs.value(argv.value(1)), 0};
                reply(m, m["id"], QJsonObject{{"pid", pid}});
            } else if (command == "guest-exec-status") {
                auto &[program, polls] = running[args["pid"].toInteger()];
                if (polls++ < program.polls) {
                    reply(m, m["id"], QJsonObject{{"exited", false}});
                } else {
                    reply(m, m["id"],
                          QJsonObject{{"exited", true}, {"exitcode", program.exitCode},
                                      {"out-data", QString::fromLatin1(program.out.toBase64())},
                                      {"err-data", QString::fromLatin1(program.err.toBase64())}});
                }
            }
        }
    }
};

class TestGuestAgent : public QObject
{
    Q_OBJECT

private slots:
    void exec()
    {
        QTemporaryDir tmp;
        FakeAgent agent(tmp.filePath("qga.sock"));
        GuestAgent client;
        QSignalSpy ready(&client, &GuestAgent::ready);

        /* a half-message from an earlier client, before the answer */
        agent.garbage = "{\"return\": 12";
        agent.programs["ok"] = {0, "mounted\n", {}, 3};
        agent.programs["bad"] = {32, {}, "mount: no such tag\n", 0};
        client.connectToSocket(tmp.filePath("qga.sock"));
        QVERIFY(ready.wait(5000));

        GuestAgent::ExecResult first, second;
        int done = 0;
        client.exec("/bin/sh", {"ok", "a b"}, [&](const GuestAgent::ExecResult &r) {
            first = r;
            done++;
        });
        client.exec("/bin/sh", {"bad"}, [&](const GuestAgent::ExecResult &r) {
            second = r;
            done++;
        });
        QTRY_COMPARE(done, 2);
        QCOMPARE(first.exitCode, 0);
        QCOMPARE(first.out, "mounted\n");
        QVERIFY(first.error.isEmpty());
        QCOMPARE(second.exitCode, 32);
        QCOMPARE(second.err, "mount: no such tag\n");
        QCOMPARE(agent.commands[0], QStringList({"/bin/sh", "ok", "a b"}));
    }

    void noAgent()
    {
        QTemporaryDir tmp;
        QLocalServer silent;                 /* the port, and no agent behind it */
        silent.listen(tmp.filePath("qga.sock"));
        GuestAgent client;
        QSignalSpy failed(&client, &GuestAgent::failed);

        client.connectToSocket(tmp.filePath("qga.sock"), 300);
        QVERIFY(failed.wait(3000));
        QVERIFY(failed[0][0].toString().contains("qemu-guest-agent"));
    }
};

QTEST_GUILESS_MAIN(TestGuestAgent)
#include "test_guestagent.moc"
