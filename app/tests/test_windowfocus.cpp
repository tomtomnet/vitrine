// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * WindowFocus on a session bus of the test's own (a dbus-daemon it starts:
 * never the desktop's, whose KWin would get the script), against a
 * stand-in for KWin's scripting.  The script itself runs in KWin only.
 */
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

#include "ui/windowfocus.h"

/* KWin's /Scripting: what was loaded, and the calls */
class FakeScripting : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kwin.Scripting")

public:
    QStringList calls;
    QString script;
    bool loaded = false;

public slots:
    Q_SCRIPTABLE int loadScript(const QString &path, const QString &plugin)
    {
        QFile file(path);
        calls << "loadScript " + plugin;
        /* as KWin: one script of a name */
        if (loaded || !file.open(QIODevice::ReadOnly)) {
            return -1;
        }
        script = QString::fromUtf8(file.readAll());
        loaded = true;
        return 7;
    }
    Q_SCRIPTABLE bool unloadScript(const QString &plugin)
    {
        const bool was = loaded;
        calls << "unloadScript " + plugin;
        loaded = false;
        return was;
    }
    Q_SCRIPTABLE void start() { calls << "start"; }
};

/* The loaded script's object, /Scripting/Script7 */
class FakeScript : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kwin.Script")

public:
    int runs = 0;

public slots:
    Q_SCRIPTABLE void run() { runs++; }
};

/* A KWin on the test's bus */
struct FakeKWin
{
    FakeKWin(const QString &address, const QString &name, bool scriptObject = true)
        : bus(QDBusConnection::connectToBus(address, name)), name(name)
    {
        bus.registerObject("/Scripting", &scripting, QDBusConnection::ExportScriptableSlots);
        if (scriptObject) {
            bus.registerObject("/Scripting/Script7", &script, QDBusConnection::ExportScriptableSlots);
        }
        registered = bus.registerService("org.kde.KWin");
    }
    ~FakeKWin() { QDBusConnection::disconnectFromBus(name); }
    QDBusConnection bus;
    QString name;
    FakeScripting scripting;
    FakeScript script;
    bool registered = false;
};

class TestWindowFocus : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QProcess m_daemon;
    QString m_address;
    int m_connections = 0;

    /* a connection of its own on the test's bus */
    QDBusConnection connection(const QString &what)
    {
        return QDBusConnection::connectToBus(m_address, QString("%1-%2").arg(what).arg(++m_connections));
    }
    /* @pid reported by a call of @from */
    static void report(QDBusConnection from, const QDBusConnection &to, const QString &pid)
    {
        QDBusMessage call = QDBusMessage::createMethodCall(to.baseService(), WindowFocus::objectPath(),
                                                           "org.vitrine.WindowFocus", "windowActivated");
        call.setArguments({pid});
        /* not waited for: the object answers in this thread's event loop */
        from.call(call, QDBus::NoBlock);
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(m_dir.isValid());
        if (QStandardPaths::findExecutable("dbus-daemon").isEmpty()) {
            QSKIP("no dbus-daemon");
        }
        m_daemon.start("dbus-daemon", {"--config-file=/usr/share/dbus-1/session.conf", "--nofork",
                                       "--nopidfile", "--print-address",
                                       "--address=unix:path=" + m_dir.path() + "/bus"});
        QVERIFY(m_daemon.waitForStarted());
        QVERIFY(m_daemon.waitForReadyRead(5000));
        m_address = QString::fromLatin1(m_daemon.readLine()).trimmed();
        QVERIFY(m_address.startsWith("unix:"));
    }
    void cleanupTestCase()
    {
        m_daemon.terminate();
        m_daemon.waitForFinished();
    }

    /* Loaded and run as it starts watching, what it reports said; gone after */
    void loadReportUnload()
    {
        FakeKWin kwin(m_address, "kwin-a");
        QVERIFY(kwin.registered);
        QDBusConnection bus = connection("vitrine");
        QDBusConnection other = connection("other");
        WindowFocus focus(bus);
        QSignalSpy activated(&focus, &WindowFocus::activated);

        /* nothing before it watches */
        QTest::qWait(100);
        QVERIFY(kwin.scripting.calls.isEmpty());
        QVERIFY(!focus.running());

        focus.setWatching(true);
        QTRY_VERIFY(focus.running());
        /* a stale one of the same name first, then this one */
        QCOMPARE(kwin.scripting.calls, QStringList({"unloadScript vitrine-window-focus",
                                                    "loadScript vitrine-window-focus"}));
        QCOMPARE(kwin.script.runs, 1);
        /* the script calls this connection's unique name */
        QVERIFY(kwin.scripting.script.contains(QString("callDBus(\"%1\", \"%2\"")
                                                   .arg(bus.baseService(), WindowFocus::objectPath())));
        QVERIFY(kwin.scripting.script.contains("workspace.windowActivated.connect(report)"));
        QCOMPARE(WindowFocus::script(":1.42"),
                 kwin.scripting.script.replace(bus.baseService(), ":1.42"));

        /* KWin's reports */
        report(kwin.bus, bus, "4242");
        QTRY_COMPARE(activated.size(), 1);
        QCOMPARE(activated.first().first().toLongLong(), 4242);
        report(kwin.bus, bus, "0");
        QTRY_COMPARE(activated.size(), 2);
        QCOMPARE(activated.last().first().toLongLong(), 0);
        /* anyone else's, and nonsense: nothing */
        report(other, bus, "1234");
        report(kwin.bus, bus, "abc");
        report(kwin.bus, bus, "-5");
        QTest::qWait(200);
        QCOMPARE(activated.size(), 2);

        /* stops: the script goes, reports count no more */
        focus.setWatching(false);
        QTRY_VERIFY(!kwin.scripting.loaded);
        QCOMPARE(kwin.scripting.calls.last(), QString("unloadScript vitrine-window-focus"));
        QVERIFY(!focus.running());
        report(kwin.bus, bus, "4242");
        QTest::qWait(200);
        QCOMPARE(activated.size(), 2);

        /* and again */
        focus.setWatching(true);
        QTRY_VERIFY(focus.running());
        QVERIFY(kwin.scripting.loaded);
        QCOMPARE(kwin.script.runs, 2);
        QDBusConnection::disconnectFromBus(bus.name());
        QDBusConnection::disconnectFromBus(other.name());
    }

    /* No KWin on the bus: nothing to load; one that comes gets the script */
    void kwinComesLater()
    {
        QDBusConnection bus = connection("vitrine");
        auto focus = std::make_unique<WindowFocus>(bus);
        focus->setWatching(true);
        QTest::qWait(100);
        QVERIFY(!focus->running());
        {
            FakeKWin kwin(m_address, "kwin-b");
            QTRY_VERIFY(focus->running());
            QVERIFY(kwin.scripting.loaded);
            /* KWin goes (a restart): not running; the next one gets it again */
        }
        QTRY_VERIFY(!focus->running());
        FakeKWin again(m_address, "kwin-c");
        QTRY_VERIFY(focus->running());
        QVERIFY(again.scripting.loaded);
        QSignalSpy activated(focus.get(), &WindowFocus::activated);
        report(again.bus, bus, "77");
        QTRY_COMPARE(activated.size(), 1);

        /* its end takes the script away */
        focus.reset();
        QTRY_VERIFY(!again.scripting.loaded);
        QDBusConnection::disconnectFromBus(bus.name());
    }

    /* A KWin whose script object is elsewhere (Plasma 5's /N): started
       through Scripting.start() */
    void runFallsBackToStart()
    {
        FakeKWin kwin(m_address, "kwin-d", false);
        QDBusConnection bus = connection("vitrine");
        WindowFocus focus(bus);
        focus.setWatching(true);
        QTRY_VERIFY(focus.running());
        QTRY_VERIFY(kwin.scripting.calls.contains("start"));
        QCOMPARE(kwin.script.runs, 0);
        focus.setWatching(false);
        QDBusConnection::disconnectFromBus(bus.name());
    }
};

QTEST_MAIN(TestWindowFocus)
#include "test_windowfocus.moc"
