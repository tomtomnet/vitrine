// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include "core/updatecheck.h"

/* What GitHub answers to a compare, cut down: the branch is 3 commits ahead */
static const char kAhead[] = "{\n"
    "    \"html_url\": \"https://github.com/tomtomnet/qemu-gui-manager/compare/abc...main\",\n"
    "    \"status\": \"ahead\", \"ahead_by\": 3, \"behind_by\": 0, \"total_commits\": 3,\n"
    "    \"commits\": [\n"
    "        {\"sha\": \"1\", \"commit\": {\"message\": \"First change\\n\\nWith a body\"}},\n"
    "        {\"sha\": \"2\", \"commit\": {\"message\": \"Second change\"}},\n"
    "        {\"sha\": \"3\", \"commit\": {\"message\": \"Third change\"}}\n"
    "    ]\n"
    "}";

static void write(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

class TestUpdateCheck : public QObject
{
    Q_OBJECT

private slots:
    void parse()
    {
        const UpdateCheck::Project project{"qemu-gui-manager", "tomtomnet/qemu-gui-manager",
                                           "main", "abc"};
        UpdateCheck::Result r = UpdateCheck::parse(project, kAhead);

        QCOMPARE(r.newCommits, 3);
        QCOMPARE(r.head, "3");
        QCOMPARE(r.subjects, QStringList({"Third change", "Second change", "First change"}));
        QVERIFY(r.url.endsWith("abc...main"));
        QVERIFY(r.error.isEmpty());

        r = UpdateCheck::parse(project, "{\"status\": \"identical\", \"ahead_by\": 0, \"commits\": []}");
        QCOMPARE(r.newCommits, 0);
        /* the running commit is newer: a build of one's own changes */
        r = UpdateCheck::parse(project, "{\"status\": \"behind\", \"ahead_by\": 0, \"behind_by\": 2}");
        QCOMPARE(r.newCommits, 0);
        r = UpdateCheck::parse(project, "{\"message\": \"API rate limit exceeded\"}");
        QCOMPARE(r.newCommits, -1);
        QCOMPARE(r.error, "API rate limit exceeded");
    }

    void json()
    {
        const UpdateCheck::Project project{"qemu-gui", "tomtomnet/qemu-gui", "master", "abc"};
        const UpdateCheck::Result r = UpdateCheck::parse(project, kAhead);
        const QList<UpdateCheck::Result> back = UpdateCheck::fromJson(UpdateCheck::toJson({r}));

        QCOMPARE(back.size(), 1);
        QCOMPARE(back[0].project.name, "qemu-gui");
        QCOMPARE(back[0].project.commit, "abc");
        QCOMPARE(back[0].newCommits, 3);
        QCOMPARE(back[0].subjects, r.subjects);
        QCOMPARE(back[0].url, r.url);
        QCOMPARE(back[0].head, "3");
    }

    void checkoutCommit()
    {
        QTemporaryDir tmp;
        const QString sha = "0123456789abcdef0123456789abcdef01234567";
        const QString other = "fedcba9876543210fedcba9876543210fedcba98";

        QCOMPARE(UpdateCheck::checkoutCommit(tmp.path()), QString());
        write(tmp.filePath(".git/HEAD"), "ref: refs/heads/master\n");
        write(tmp.filePath(".git/packed-refs"),
              "# pack-refs with: peeled fully-peeled sorted\n" + other.toLatin1() +
                  " refs/heads/other\n" + sha.toLatin1() + " refs/heads/master\n");
        QCOMPARE(UpdateCheck::checkoutCommit(tmp.path()), sha);
        write(tmp.filePath(".git/refs/heads/master"), other.toLatin1() + "\n");
        QCOMPARE(UpdateCheck::checkoutCommit(tmp.path()), other);
        /* detached */
        write(tmp.filePath(".git/HEAD"), sha.toLatin1() + "\n");
        QCOMPARE(UpdateCheck::checkoutCommit(tmp.path()), sha);
    }

    /* The requests, and the answers of a server standing in for GitHub */
    void check()
    {
        QTcpServer server;
        QStringList requests;

        QVERIFY(server.listen(QHostAddress::LocalHost));
        connect(&server, &QTcpServer::newConnection, &server, [&]() {
            QTcpSocket *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [&requests, socket]() {
                const QByteArray request = socket->readAll();
                const QByteArray line = request.left(request.indexOf('\r'));
                const bool known = line.contains("/qemu-gui-manager/");
                const QByteArray body = known ? QByteArray(kAhead)
                                              : QByteArray("{\"message\": \"Not Found\"}");

                requests << QString::fromLatin1(line);
                socket->write((known ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 404 Not Found\r\n") +
                              QByteArray("Content-Type: application/json\r\nContent-Length: ") +
                              QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" +
                              body);
                socket->disconnectFromHost();
            });
        });
        qputenv("QGM_GITHUB_API", QString("http://127.0.0.1:%1").arg(server.serverPort()).toLatin1());

        UpdateCheck check;
        QSignalSpy finished(&check, &UpdateCheck::finished);
        check.check({{"qemu-gui-manager", "tomtomnet/qemu-gui-manager", "main", "abc"},
                     {"qemu-gui", "tomtomnet/qemu-gui", "master", "def"},
                     {"unknown", "x/y", "main", {}}});
        QVERIFY(finished.wait(10000));
        qunsetenv("QGM_GITHUB_API");

        requests.sort();
        QCOMPARE(requests, QStringList({"GET /repos/tomtomnet/qemu-gui-manager/compare/abc...main HTTP/1.1",
                                        "GET /repos/tomtomnet/qemu-gui/compare/def...master HTTP/1.1"}));
        auto results = finished[0][0].value<QList<UpdateCheck::Result>>();
        QCOMPARE(results.size(), 2);
        std::sort(results.begin(), results.end(),
                  [](const auto &a, const auto &b) { return a.project.name > b.project.name; });
        QCOMPARE(results[0].project.name, "qemu-gui-manager");
        QCOMPARE(results[0].newCommits, 3);
        QCOMPARE(results[1].newCommits, -1);
        QVERIFY2(results[1].error.contains("def"), qPrintable(results[1].error));
    }
};

QTEST_GUILESS_MAIN(TestUpdateCheck)
#include "test_updatecheck.moc"
