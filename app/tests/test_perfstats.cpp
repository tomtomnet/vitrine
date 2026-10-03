// SPDX-License-Identifier: GPL-2.0-or-later
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

#include "core/perfstats.h"

using namespace PerfStats;

static QJsonObject json(const char *text)
{
    return QJsonDocument::fromJson(text).object();
}

static void write(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

/* x-query-display-stats of a VM with a text console and a graphic one */
static const char kDisplay[] = R"({
    "type": "sdl",
    "consoles": [
        {"console": 1, "width": 640, "height": 480, "refresh-rate": 240000,
         "method": "presentation", "flushes": 0, "presented": 0, "dropped": 0,
         "direct": 0, "redraws": 0,
         "frame-latency": {"samples": 0, "median": 0, "p99": 0, "max": 0},
         "qemu-latency": {"samples": 0, "median": 0, "p99": 0, "max": 0},
         "frame-interval": {"samples": 0, "median": 0, "p99": 0, "max": 0},
         "input-latency": {"samples": 0, "median": 0, "p99": 0, "max": 0}},
        {"console": 0, "width": 3840, "height": 2160, "refresh-rate": 239990,
         "method": "presentation", "flushes": 241.5, "presented": 239.6,
         "dropped": 1.9, "direct": 239.6, "redraws": 0,
         "frame-latency": {"samples": 240, "median": 6120, "p99": 7940, "max": 8210},
         "qemu-latency": {"samples": 241, "median": 210, "p99": 420, "max": 510},
         "frame-interval": {"samples": 239, "median": 4166, "p99": 4180, "max": 4200},
         "input-latency": {"samples": 3, "median": 11200, "p99": 14000, "max": 14000},
         "input-wait": {"samples": 120, "median": 2000, "p99": 4000, "max": 4000}}
    ]})";

class TestPerfStats : public QObject
{
    Q_OBJECT

private slots:
    void display()
    {
        Screen d;
        QString type;

        QVERIFY(parseDisplay(json(kDisplay), &d, &type));
        QCOMPARE(type, "sdl");
        /* the busy console, not the first */
        QCOMPARE(d.console, 0);
        QCOMPARE(d.width, 3840);
        QCOMPARE(d.refreshHz, 239.99);
        QCOMPARE(d.method, "presentation");
        QCOMPARE(d.presented, 239.6);
        QCOMPARE(d.frame.samples, 240);
        QCOMPARE(d.frame.median, 6.12);
        QCOMPARE(d.frame.p99, 7.94);
        QCOMPARE(d.qemu.median, 0.21);
        QCOMPARE(d.input.median, 11.2);
        QVERIFY(d.hasInputWait);
        QCOMPARE(d.inputWait.max, 4.0);

        /* no display measuring its frames */
        QVERIFY(!parseDisplay(json(R"({"type": "gtk", "consoles": []})"), &d, &type));
        QCOMPARE(type, "gtk");
    }

    void threads()
    {
        QTemporaryDir proc;
        write(proc.filePath("100/task/100/schedstat"), "5000000000 100000000 2000\n");
        write(proc.filePath("100/task/101/schedstat"), "7000000000 0 10\n");
        write(proc.filePath("100/task/102/schedstat"), "garbage\n");
        write(proc.filePath("100/task/stray/schedstat"), "1 2 3\n");

        const QHash<qint64, Thread> t = readThreads(100, proc.path());
        QCOMPARE(t.size(), 2);
        QCOMPARE(t.value(100).runNs, 5000000000);
        QCOMPARE(t.value(100).waitNs, 100000000);
        QCOMPARE(t.value(100).slices, 2000);
        QVERIFY(readThreads(0, proc.path()).isEmpty());
        QVERIFY(readThreads(200, proc.path()).isEmpty());
        /* the real thing */
        QVERIFY(readThreads(QCoreApplication::applicationPid()).size() >= 1);
    }

    void loads()
    {
        const QHash<qint64, Thread> before{{1, {1000000000, 10000000, 100}},
                                           {2, {0, 0, 0}},
                                           {3, {0, 0, 0}}};
        /* half a second: 1 ran 0.1 s and waited 2 ms over 50 runs; 3 ended */
        const QHash<qint64, Thread> after{{1, {1100000000, 12000000, 150}},
                                          {2, {500000000, 50000000, 10}},
                                          {4, {0, 0, 0}}};
        const qint64 half = 500000000;

        const Load main = load(before, after, {1}, half);
        QCOMPARE(main.threads, 1);
        QCOMPARE(main.cpu, 20.0);
        QCOMPARE(main.wait, 0.4);
        QCOMPARE(main.waitPerRunMs, 0.04);

        /* the busy vCPU, the gone one left out */
        const Load vcpus = load(before, after, {2, 3}, half);
        QCOMPARE(vcpus.threads, 1);
        QCOMPARE(vcpus.cpu, 100.0);
        QCOMPARE(vcpus.wait, 10.0);

        QCOMPARE(load(before, after, {1, 2}, half).wait, (0.4 + 10.0) / 2);
        QCOMPARE(load(before, after, {1}, 0).threads, 0);
    }

    void kvm()
    {
        const QJsonArray reply = QJsonDocument::fromJson(R"([
            {"provider": "kvm", "qom-path": "/machine/unattached/device[0]",
             "stats": [{"name": "exits", "value": 1000},
                       {"name": "halt_exits", "value": 300},
                       {"name": "halt_poll_success_hist", "value": [1, 2, 3]}]},
            {"provider": "kvm", "qom-path": "/machine/unattached/device[1]",
             "stats": [{"name": "exits", "value": 500}]},
            {"provider": "cryptodev", "stats": [{"name": "exits", "value": 7}]}
        ])").array();

        const QHash<QString, double> sums = sumKvmStats(reply);
        QCOMPARE(sums.value("exits"), 1500.0);
        QCOMPARE(sums.value("halt_exits"), 300.0);
        QVERIFY(!sums.contains("halt_poll_success_hist"));
    }

    void summaries()
    {
        Snapshot s;

        QCOMPARE(summary(s), QString());

        s.threads = true;
        s.mainLoop.wait = 0.43;
        QCOMPARE(summary(s), "main loop wait 0.4 %");

        s.display = true;
        QVERIFY(parseDisplay(json(kDisplay), &s.screen));
        QCOMPARE(summary(s), "240 fps · frame 6.1 ms · input 11 ms · main loop wait 0.4 %");

        /* X11: no presentation feedback */
        s.screen.method = "swap";
        s.screen.frame.median = 0.34;
        s.screen.input.samples = 0;
        s.mainLoop.wait = 12.4;
        QCOMPARE(summary(s), "240 fps · frame 0.34 ms (to swap) · main loop wait 12 %");

        s.screen.presented = 0;
        QCOMPARE(summary(s), "display idle · main loop wait 12 %");

        s.mainLoop.wait = 0.01;
        QCOMPARE(summary(s), "display idle · main loop wait < 0.1 %");

        QCOMPARE(formatMs(21.4), "21 ms");
        QCOMPARE(formatMs(6.15), "6.2 ms");
    }

    void detailsText()
    {
        Snapshot s;

        s.display = true;
        QVERIFY(parseDisplay(json(kDisplay), &s.screen));
        s.threads = true;
        s.mainLoop = {1, 12, 0.4, 0.02};
        s.vcpus = {4, 150, 1.2, 0};
        s.kvm = true;
        s.exits = 45000;
        s.haltPollSuccess = 0.85;

        const QString html = details(s);
        QVERIFY(html.contains("3840×2160"));
        QVERIFY(html.contains("Frame latency"));
        QVERIFY(html.contains("4 vCPU(s)"));
        QVERIFY(html.contains("45 k/s"));
        QVERIFY(html.contains("18 %"));         // 45 k exits at 4 µs
        QVERIFY(html.contains("85 % successful"));

        /* "< 0.1 %" is text, not a tag that would hide the rest */
        s.mainLoop.wait = 0.01;
        QVERIFY(details(s).contains("waited &lt; 0.1 % of the time"));

        Snapshot gtk;
        gtk.displayType = "gtk";
        QVERIFY(details(gtk).contains("does not measure its frames"));
    }
};

QTEST_GUILESS_MAIN(TestPerfStats)
#include "test_perfstats.moc"
