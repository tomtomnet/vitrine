// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Preferences > Host: gpu-input-awake on the GPU clock floor's row of a
 * dedicated GPU, Install… and Remove… run as the app runs them, the CPU
 * kept ready to wake up and the state line's words, on a fake
 * host (its GPUs, a fake root for /usr and /etc) with stand-ins for pkexec,
 * id and systemctl.  Nothing here touches this computer's /usr or /etc,
 * nor asks polkit.
 */
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <memory>

#include "core/gpuinputawake.h"
#include "core/hostgpus.h"
#include "core/hostsettings.h"
#include "fakegpus.h"
#include "ui/hosttuningprefs.h"
#include "ui/hosttuningtext.h"
#include "ui/widgets.h"

static const char kFakePkexec[] = R"sh(#!/bin/bash
echo "pkexec $*" >> "${STATE:?}/calls"
[ "$1" = --disable-internal-agent ] && shift
if [ -e "$STATE/cancel" ]; then
	echo "Error executing command as another user: Not authorized"
	echo
	echo "This incident has been reported."
	exit 127
fi
exec "$@"
)sh";

static const char kFakeSystemctl[] = R"sh(#!/bin/bash
echo "systemctl $*" >> "${STATE:?}/calls"
[ "$1" = show ] && [ -e "$STATE/show" ] && cat "$STATE/show"
exit 0
)sh";

class TestHostTuningPrefs : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_tmp;
    QString m_state, m_root;
    std::unique_ptr<FakeGpus> m_gpus;
    const QByteArray m_path = qgetenv("PATH");

    static QString source() { return QStringLiteral(GPU_AWAKE_SOURCE_DIR); }
    static bool write(const QString &path, const QByteArray &data, bool exec = false)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        const bool ok = f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
        f.close();
        if (exec) {
            QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        }
        return ok;
    }
    static QByteArray read(const QString &path)
    {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }

    /* The Preferences' host rows, in a window of their own */
    struct Prefs
    {
        Prefs() : window(new QWidget), form(Widgets::form())
        {
            window->setLayout(form);
            prefs = new HostTuningPrefs(form, window.get());
            window->show();
        }
        QLabel *awake() const { return window->findChild<QLabel *>("gpuInputAwake"); }
        QPushButton *install() const { return window->findChild<QPushButton *>("gpuInputAwakeInstall"); }
        QPushButton *remove() const { return window->findChild<QPushButton *>("gpuInputAwakeRemove"); }
        std::unique_ptr<QWidget> window;
        Form *form;
        HostTuningPrefs *prefs;
    };

    /*
     * The confirmation's @action clicked once it shows; its text in
     * @shown.  Polled: the box comes in the click's own event loop.
     */
    static void confirm(const QString &action, QString *shown)
    {
        auto *timer = new QTimer;
        timer->setInterval(20);
        QObject::connect(timer, &QTimer::timeout, timer, [timer, action, shown]() {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box) {
                return;
            }
            *shown = box->text();
            for (QAbstractButton *button : box->buttons()) {
                if (button->text() == action) {
                    timer->deleteLater();
                    button->click();
                    return;
                }
            }
        });
        timer->start();
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(m_tmp.isValid());
        const QString bin = m_tmp.filePath("bin");
        QVERIFY(write(bin + "/pkexec", kFakePkexec, true));
        QVERIFY(write(bin + "/systemctl", kFakeSystemctl, true));
        QVERIFY(write(bin + "/id", "#!/bin/bash\n[ \"$1\" = -u ] && { echo 0; exit 0; }\n"
                                   "exec /usr/bin/id \"$@\"\n", true));
        qputenv("PATH", bin.toLocal8Bit() + ':' + m_path);
        /* no helper: nothing asks polkit */
        qputenv("VITRINE_HELPER", m_tmp.filePath("no-helper").toLocal8Bit());
        qputenv("VITRINE_AWAKE_DIR", source().toLocal8Bit());
    }
    void cleanupTestCase()
    {
        HostGpus::setRoots("/sys", "/dev");
        qputenv("PATH", m_path);
    }

    void init()
    {
        m_state = m_tmp.filePath(QString("state-") + QTest::currentTestFunction());
        m_root = m_tmp.filePath(QString("root-") + QTest::currentTestFunction());
        QDir().mkpath(m_state);
        QDir().mkpath(m_root);
        qputenv("STATE", m_state.toLocal8Bit());
        qputenv("VITRINE_AWAKE_ROOT", m_root.toLocal8Bit());
        m_gpus = std::make_unique<FakeGpus>(m_tmp.filePath(QString("gpus-") + QTest::currentTestFunction()));
    }

    /* An Arc Pro B50: not installed, Install… then Remove…, as the app runs them */
    void installAndRemove()
    {
        m_gpus->gpu("0000:03:00.0", "0x8086", "0xe212", "xe", true, 0, 128);
        m_gpus->use();
        Prefs p;
        QVERIFY(p.awake());
        QTRY_VERIFY(p.awake()->text().endsWith("It is not installed."));
        QVERIFY(p.awake()->text().contains("a clock floor does nothing on a dedicated GPU"));
        QVERIFY(p.install()->isVisible() && p.install()->isEnabled());
        QVERIFY(!p.remove()->isVisible());

        QString shown;
        write(m_state + "/show", "ActiveState=active\nUnitFileState=enabled\n");
        confirm("&Install", &shown);
        p.install()->click();
        QVERIFY(shown.contains("runs as root from now on"));
        QVERIFY(!shown.contains("not Vitrine's version"));
        QTRY_VERIFY(p.awake()->text().endsWith("It is installed and runs."));
        QCOMPARE(GpuInputAwake::installed(source(), m_root), GpuInputAwake::Installed::Vitrine);
        const QByteArray calls = read(m_state + "/calls");
        QVERIFY(calls.contains("pkexec --disable-internal-agent "));
        QVERIFY(calls.contains(QFile::encodeName(source()) + "/install.sh install"));
        QVERIFY(!p.install()->isVisible());
        QVERIFY(p.remove()->isVisible() && p.remove()->isEnabled());

        confirm("&Remove", &shown);
        p.remove()->click();
        QVERIFY(shown.contains("stops now and no longer starts at boot"));
        QTRY_VERIFY(p.awake()->text().endsWith("It is not installed."));
        QCOMPARE(GpuInputAwake::installed(source(), m_root), GpuInputAwake::Installed::No);
        QVERIFY(p.install()->isVisible());
        QVERIFY(!p.remove()->isVisible());
    }

    /* Another copy (the research's own): said, and what the buttons do to it */
    void otherCopy()
    {
        m_gpus->gpu("0000:03:00.0", "0x8086", "0xe212", "xe", true, 0, 128);
        m_gpus->use();
        write(GpuInputAwake::scriptPath(m_root), read(source() + "/gpu-input-awake"));
        write(GpuInputAwake::unitPath(m_root), "[Service]\nExecStart=/usr/local/sbin/gpu-input-awake --hold 3\n");
        Prefs p;
        QTRY_VERIFY(p.awake()->text().endsWith("It is installed, not Vitrine's version."));
        QVERIFY(p.install()->isVisible() && p.remove()->isVisible());

        /* dismissed at the password: nothing changes, no error */
        write(m_state + "/cancel", "");
        QString shown;
        confirm("&Install", &shown);
        p.install()->click();
        QVERIFY(shown.contains("It replaces the copy installed now, which is not Vitrine's version."));
        QTRY_VERIFY(p.install()->isEnabled());
        QVERIFY(!QApplication::activeModalWidget());
        QCOMPARE(GpuInputAwake::installed(source(), m_root), GpuInputAwake::Installed::Other);

        QFile::remove(m_state + "/cancel");
        confirm("&Remove", &shown);
        p.remove()->click();
        QVERIFY(shown.contains("This removes the copy installed now, which is not Vitrine's version"));
        QTRY_VERIFY(p.awake()->text().endsWith("It is not installed."));
        QCOMPARE(GpuInputAwake::installed(source(), m_root), GpuInputAwake::Installed::No);
    }

    /* The CPU kept ready to wake up: on by default, said what it does and
       costs; only with tuning on; saved */
    void cpuLatency()
    {
        m_gpus->use();
        Prefs p;
        auto *box = p.window->findChild<QCheckBox *>("cpuLatency");
        auto *tune = p.window->findChild<QCheckBox *>("tuneHost");
        QVERIFY(box && tune);
        QCOMPARE(box->text(), QString("&Keep the CPU ready to wake up while VMs run"));
        QVERIFY(box->isChecked());
        QVERIFY(box->isEnabled());
        QString hint;
        for (QLabel *label : p.window->findChildren<QLabel *>()) {
            if (label->text().contains("idle state")) {
                hint = label->text();
            }
        }
        QVERIFY(hint.contains("lightest idle state"));
        QVERIFY(hint.contains("about 2 W at idle"));
        QVERIFY(hint.contains("ends when the last VM stops"));
        tune->setChecked(false);
        QVERIFY(!box->isEnabled());
        tune->setChecked(true);
        QVERIFY(box->isEnabled());

        box->setChecked(false);
        p.prefs->save();
        QVERIFY(!HostSettings::cpuLatency());
        {
            Prefs again;
            QVERIFY(!again.window->findChild<QCheckBox *>("cpuLatency")->isChecked());
        }
        box->setChecked(true);
        p.prefs->save();
        QVERIFY(HostSettings::cpuLatency());
    }

    /* An AMD APU: the floor, no gpu-input-awake row; a discrete AMD card: the row */
    void apuOnly()
    {
        m_gpus->gpu("0000:c5:00.0", "0x1002", "0x1900", "amdgpu", true, 0, 128);
        FakeGpus::write(m_gpus->sys + "/devices/pci0000:00/0000:c5:00.0/gpu_metrics",
                        QByteArray("\x78\x00\x02\x01", 4));
        m_gpus->use();
        {
            Prefs p;
            QVERIFY(!p.awake());
            QVERIFY(!p.install());
        }
        m_gpus->gpu("0000:03:00.0", "0x1002", "0x73bf", "amdgpu", false, 1, 129);
        FakeGpus::write(m_gpus->sys + "/devices/pci0000:00/0000:03:00.0/gpu_metrics",
                        QByteArray("\x78\x00\x01\x01", 4));
        Prefs p;
        QVERIFY(p.awake());
        QTRY_VERIFY(p.awake()->text().endsWith("It is not installed."));
    }

    /* The state line: which VMs get no real-time threads, or why none do */
    void activeState()
    {
        const QString active = "Active: vitrine-helper may tune the host without a password. ";
        QCOMPARE(HostTuningPrefsText::active({}, 16),
                 active + "VMs with more than 8 processors, half of this computer's 16, run "
                          "without real-time threads.");
        QCOMPARE(HostTuningPrefsText::active({}, 3),
                 active + "VMs with more than 1 processor, half of this computer's 3, run without "
                          "real-time threads.");
        QCOMPARE(HostTuningPrefsText::active({}, 1),
                 active + "With one CPU, VMs run without real-time threads.");
        const QString off = "Real-time QEMU threads are off: the kernel's lockdown (Secure Boot) "
                            "blocks the fair server.";
        QCOMPARE(HostTuningPrefsText::active(off, 16), active + off);
    }
};

QTEST_MAIN(TestHostTuningPrefs)
#include "test_hosttuningprefs.moc"
