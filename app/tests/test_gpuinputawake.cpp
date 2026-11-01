// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * gpu-input-awake: which GPUs it is for, what is installed, and
 * host/gpu-input-awake/install.sh on a fake root, against stand-ins for id
 * and systemctl.  Nothing here touches this computer's /usr or /etc.
 */
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>

#include "core/gpuinputawake.h"
#include "core/hostgpus.h"
#include "fakegpus.h"

using namespace GpuInputAwake;

/* systemctl: each call in $STATE/calls; $STATE/fail makes it fail; show
   prints $STATE/show */
static const char kFakeSystemctl[] = R"sh(#!/bin/bash
echo "$*" >> "${STATE:?}/calls"
[ -e "$STATE/fail" ] && { echo "Failed: $1" >&2; exit 1; }
[ "$1" = show ] && [ -e "$STATE/show" ] && cat "$STATE/show"
exit 0
)sh";

class TestGpuInputAwake : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_tmp;
    QString m_bin, m_state, m_root;
    const QByteArray m_path = qgetenv("PATH");

    static QString source() { return QStringLiteral(GPU_AWAKE_SOURCE_DIR); }
    static bool write(const QString &path, const QByteArray &data)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
    }
    static QByteArray read(const QString &path)
    {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }
    static int mode(const QString &path)
    {
        struct stat st;
        return ::stat(QFile::encodeName(path).constData(), &st) == 0 ? int(st.st_mode & 07777) : -1;
    }
    QStringList calls() const
    {
        return QString::fromUtf8(read(m_state + "/calls")).split('\n', Qt::SkipEmptyParts);
    }
    /* install.sh with the stand-ins, on the fake root, as "root" */
    QString script(const QString &action, int *status, bool root = true)
    {
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("PATH", m_bin + ":/usr/bin:/bin");
        env.insert("STATE", m_state);
        env.insert("FAKE_UID", root ? "0" : "1000");
        env.insert("VITRINE_AWAKE_ROOT", m_root);
        p.setProcessEnvironment(env);
        p.setProcessChannelMode(QProcess::MergedChannels);
        p.start("/bin/bash", {source() + "/install.sh", action});
        p.waitForFinished(30000);
        *status = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
        return QString::fromUtf8(p.readAll());
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(m_tmp.isValid());
        m_bin = m_tmp.filePath("bin");
        QVERIFY(write(m_bin + "/id", "#!/bin/bash\n[ \"$1\" = -u ] && { echo \"${FAKE_UID:-0}\"; exit 0; }\n"
                                     "exec /usr/bin/id \"$@\"\n"));
        QVERIFY(write(m_bin + "/systemctl", kFakeSystemctl));
        for (const char *name : {"id", "systemctl"}) {
            QFile::setPermissions(m_bin + '/' + name, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        }
    }

    void init()
    {
        qputenv("PATH", m_path);
        qunsetenv("VITRINE_AWAKE_DIR");
        m_state = m_tmp.filePath(QString("state-") + QTest::currentTestFunction());
        m_root = m_tmp.filePath(QString("root-") + QTest::currentTestFunction());
        QDir(m_state).removeRecursively();
        QDir(m_root).removeRecursively();
        QDir().mkpath(m_state);
        QDir().mkpath(m_root);
    }

    /* xe and AMD's discrete GPUs: the wake; AMD's APUs: the floor; others: neither */
    void kinds()
    {
        FakeGpus fake(m_tmp.filePath("gpus"));
        fake.gpu("0000:03:00.0", "0x8086", "0xe212", "xe", true, 0, 128);
        fake.gpu("0000:04:00.0", "0x1002", "0x73bf", "amdgpu", false, 1, 129);
        fake.gpu("0000:c5:00.0", "0x1002", "0x1900", "amdgpu", false, 2, 130);
        fake.gpu("0000:06:00.0", "0x1002", "0x67df", "amdgpu", false, 3, 131);
        fake.gpu("0000:07:00.0", "0x8086", "0x46a6", "i915", false, 4, 132);
        fake.gpu("0000:08:00.0", "0x10de", "0x2d04", "nvidia", false, 5, 133);
        /* gpu_metrics headers: v1 (discrete), v2 (an APU); none for 06:00.0 */
        FakeGpus::write(fake.sys + "/devices/pci0000:00/0000:04:00.0/gpu_metrics",
                        QByteArray("\x78\x00\x01\x01", 4));
        FakeGpus::write(fake.sys + "/devices/pci0000:00/0000:c5:00.0/gpu_metrics",
                        QByteArray("\x78\x00\x02\x01", 4));
        fake.use();
        const QList<HostGpu> gpus = HostGpus::list();
        QCOMPARE(gpus.size(), 6);
        QStringList kinds;
        for (const HostGpu &gpu : gpus) {
            const Kind k = kind(gpu, fake.sys);
            kinds << gpu.shortAddress() + ' ' +
                         (k == Kind::InputWake ? "wake" : k == Kind::ClockFloor ? "floor" : "none");
        }
        QCOMPARE(kinds, QStringList({"03:00.0 wake", "04:00.0 wake", "06:00.0 wake", "07:00.0 none",
                                     "08:00.0 none", "c5:00.0 floor"}));
        QStringList servedOnes;
        for (const HostGpu &gpu : served(gpus, fake.sys)) {
            servedOnes << gpu.shortAddress();
        }
        QCOMPARE(servedOnes, QStringList({"03:00.0", "04:00.0", "06:00.0"}));
        HostGpus::setRoots("/sys", "/dev");
    }

    /* The pinned copy is the one its README names */
    void pinned()
    {
        const QString readme = QString::fromUtf8(read(source() + "/README.md"));
        int rows = 0;
        for (const QString &file : {"gpu-input-awake", "gpu-input-awake.service"}) {
            const QRegularExpressionMatch m =
                QRegularExpression(QString("\\| `([0-9a-f]{64})` \\| `%1` \\|").arg(QRegularExpression::escape(file)))
                    .match(readme);
            QVERIFY2(m.hasMatch(), qPrintable(file));
            QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(read(source() + '/' + file),
                                                                  QCryptographicHash::Sha256).toHex()),
                     m.captured(1));
            rows++;
        }
        QCOMPARE(rows, 2);
        QCOMPARE(mode(source() + "/gpu-input-awake"), 0755);
        QCOMPARE(mode(source() + "/gpu-input-awake.service"), 0644);
        QCOMPARE(mode(source() + "/install.sh"), 0755);
        /* the unit runs the script where install.sh puts it */
        QVERIFY(read(source() + "/gpu-input-awake.service")
                    .contains("ExecStart=/usr/bin/python3 -B /usr/local/sbin/gpu-input-awake "));
        qputenv("VITRINE_AWAKE_DIR", source().toLocal8Bit());
        QCOMPARE(sourceDir(), QFileInfo(source()).canonicalFilePath());
        qputenv("VITRINE_AWAKE_DIR", m_tmp.filePath("nothing").toLocal8Bit());
        QCOMPARE(sourceDir(), QString());
    }

    /* What is installed: Vitrine's copy, another, part of one, none */
    void installedStates()
    {
        QCOMPARE(installed(source(), m_root), Installed::No);
        QVERIFY(write(scriptPath(m_root), read(source() + "/gpu-input-awake")));
        QCOMPARE(installed(source(), m_root), Installed::Partial);
        QVERIFY(write(unitPath(m_root), read(source() + "/gpu-input-awake.service")));
        QCOMPARE(installed(source(), m_root), Installed::Vitrine);
        /* the research's own, another version */
        QVERIFY(write(unitPath(m_root), read(source() + "/gpu-input-awake.service").replace("--hold 0.02", "--hold 3")));
        QCOMPARE(installed(source(), m_root), Installed::Other);
        QCOMPARE(installed(QString(), m_root), Installed::Other);
        QFile::remove(scriptPath(m_root));
        QCOMPARE(installed(source(), m_root), Installed::Partial);
        QCOMPARE(scriptPath(), QString("/usr/local/sbin/gpu-input-awake"));
        QCOMPARE(unitPath(), QString("/etc/systemd/system/gpu-input-awake.service"));
    }

    /* install, then remove, as the app runs them (without pkexec) */
    void installThenRemove()
    {
        int status = -1;
        QString out = script("install", &status);
        QVERIFY2(status == 0, qPrintable(out));
        QVERIFY(out.contains("vitrine-awake: done"));
        QCOMPARE(read(scriptPath(m_root)), read(source() + "/gpu-input-awake"));
        QCOMPARE(read(unitPath(m_root)), read(source() + "/gpu-input-awake.service"));
        QCOMPARE(mode(scriptPath(m_root)), 0755);
        QCOMPARE(mode(unitPath(m_root)), 0644);
        QCOMPARE(calls(), QStringList({"daemon-reload", "enable gpu-input-awake.service",
                                       "restart gpu-input-awake.service"}));
        QCOMPARE(installed(source(), m_root), Installed::Vitrine);
        QString error;
        QCOMPARE(result(status, out, &error), Result::Done);

        /* again over another version: replaced */
        QVERIFY(write(scriptPath(m_root), "#!/bin/sh\n"));
        out = script("install", &status);
        QCOMPARE(status, 0);
        QCOMPARE(installed(source(), m_root), Installed::Vitrine);

        QFile::remove(m_state + "/calls");
        out = script("remove", &status);
        QVERIFY2(status == 0, qPrintable(out));
        QVERIFY(out.contains("vitrine-awake: done"));
        QVERIFY(!QFileInfo::exists(scriptPath(m_root)));
        QVERIFY(!QFileInfo::exists(unitPath(m_root)));
        QCOMPARE(calls(), QStringList({"disable --now gpu-input-awake.service", "daemon-reload",
                                       "reset-failed gpu-input-awake.service"}));
        QCOMPARE(installed(source(), m_root), Installed::No);
        /* nothing left to remove: done all the same */
        out = script("remove", &status);
        QCOMPARE(status, 0);
    }

    /* A unit removed by hand, enabled before: its link goes too */
    void removeLeftovers()
    {
        const QString wants = m_root + "/etc/systemd/system/multi-user.target.wants/gpu-input-awake.service";
        QVERIFY(write(scriptPath(m_root), "#!/bin/sh\n"));
        QDir().mkpath(QFileInfo(wants).absolutePath());
        QVERIFY(QFile::link(unitPath(), wants));
        int status = -1;
        const QString out = script("remove", &status);
        QVERIFY2(status == 0, qPrintable(out));
        QVERIFY(!QFileInfo(wants).isSymLink());
        QVERIFY(!QFileInfo::exists(scriptPath(m_root)));
        QVERIFY(calls().contains("stop gpu-input-awake.service"));
        QVERIFY(!calls().join('\n').contains("disable"));
    }

    /* Not root, systemctl failing: said, as the app reads it */
    void scriptFailures()
    {
        int status = -1;
        QString out = script("install", &status, false);
        QCOMPARE(status, 1);
        QVERIFY(out.contains("vitrine-awake: error: run it as root (pkexec)"));
        QVERIFY(!QFileInfo::exists(scriptPath(m_root)));
        QString error;
        QCOMPARE(result(status, out, &error), Result::Failed);
        QCOMPARE(error, QString("run it as root (pkexec)"));

        QVERIFY(write(m_state + "/fail", ""));
        out = script("install", &status);
        QCOMPARE(status, 1);
        QCOMPARE(result(status, out, &error), Result::Failed);
        QCOMPARE(error, QString("systemctl daemon-reload failed"));

        out = script("frobnicate", &status);
        QCOMPARE(status, 2);
    }

    /* pkexec's ends */
    void results_data()
    {
        QTest::addColumn<int>("code");
        QTest::addColumn<QString>("output");
        QTest::addColumn<int>("expected");
        QTest::addColumn<QString>("error");
        QTest::newRow("done") << 0 << "vitrine-awake: step: x\nvitrine-awake: done\n" << int(Result::Done) << "";
        QTest::newRow("dismissed") << 126 << "Error executing command as another user: Request dismissed\n"
                                   << int(Result::Cancelled) << "";
        QTest::newRow("kde cancel") << 127 << "Error executing command as another user: Not authorized\n\n"
                                            "This incident has been reported.\n"
                                    << int(Result::Cancelled) << "";
        QTest::newRow("no agent") << 127 << "Error executing command as another user: No authentication agent found.\n"
                                  << int(Result::Failed) << "No authentication agent found";
        QTest::newRow("crashed") << -1 << "" << int(Result::Failed) << "the installer stopped unexpectedly";
        QTest::newRow("no done") << 0 << "" << int(Result::Failed) << "the installer failed (status 0)";
    }
    void results()
    {
        QFETCH(int, code);
        QFETCH(QString, output);
        QFETCH(int, expected);
        QFETCH(QString, error);
        QString why;
        QCOMPARE(int(result(code, output, &why)), expected);
        QCOMPARE(why, error);
    }

    void commandLine()
    {
        const QStringList install = command("/usr/share/vitrine/host/gpu-input-awake", true);
        QVERIFY(install.first().endsWith("pkexec"));
        QCOMPARE(install.mid(1, 1), QStringList({"--disable-internal-agent"}));
        QVERIFY(install.at(2).endsWith("bash"));
        QCOMPARE(install.mid(3), QStringList({"/usr/share/vitrine/host/gpu-input-awake/install.sh", "install"}));
        QCOMPARE(command("/x", false).last(), QString("remove"));
    }

    /* One at a time, the answer in the context's thread */
    void runOne()
    {
        QString error = "none";
        int calls = 0;
        Result got = Result::Failed;
        QVERIFY(run(this, true, [&](Result r, const QString &why) { got = r; error = why; calls++; },
                    {"/bin/sh", "-c", "sleep 0.3; echo 'vitrine-awake: done'"}));
        QVERIFY(running());
        QVERIFY(!run(this, false, [&](Result, const QString &) { calls++; }, {"/bin/true"}));
        QTRY_COMPARE(calls, 1);
        QCOMPARE(got, Result::Done);
        QCOMPARE(error, QString());
        QVERIFY(!running());
        QVERIFY(run(this, true, [&](Result r, const QString &why) { got = r; error = why; calls++; },
                    {m_tmp.filePath("no-such-program")}));
        QTRY_COMPARE(calls, 2);
        QCOMPARE(got, Result::Failed);
        QCOMPARE(error, QString("pkexec is not installed"));
    }

    /* systemctl's view of the unit */
    void service()
    {
        QVERIFY(write(m_state + "/show", "ActiveState=active\nUnitFileState=enabled\n"));
        qputenv("STATE", m_state.toLocal8Bit());
        qputenv("PATH", m_bin.toLocal8Bit() + ':' + m_path);
        QString active = "?", enabled = "?";
        bool answered = false;
        serviceState(this, [&](const QString &a, const QString &e) { active = a; enabled = e; answered = true; });
        QTRY_VERIFY(answered);
        QCOMPARE(active, QString("active"));
        QCOMPARE(enabled, QString("enabled"));
        QCOMPARE(calls(), QStringList({"show --property=ActiveState,UnitFileState gpu-input-awake.service"}));
        /* no systemctl at all */
        qputenv("PATH", m_tmp.filePath("empty").toLocal8Bit());
        answered = false;
        serviceState(this, [&](const QString &a, const QString &e) { active = a; enabled = e; answered = true; });
        QTRY_VERIFY(answered);
        QCOMPARE(active, QString());
        QCOMPARE(enabled, QString());
        qputenv("PATH", m_path);
    }
};

QTEST_GUILESS_MAIN(TestGpuInputAwake)
#include "test_gpuinputawake.moc"
