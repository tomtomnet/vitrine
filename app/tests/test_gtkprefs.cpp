// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef Q_MOC_RUN
#include "gtk/gtkbase.h"
#endif

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <cerrno>
#include <memory>

#include "gtkdisplay.h"

#ifndef Q_MOC_RUN
#include "core/buildmanager.h"
#include "core/gpuinputawake.h"
#include "core/hostsettings.h"
#include "core/paths.h"
#include "core/udmabuf.h"
#include "core/vmrunner.h"
#include "core/vmstore.h"
#include "fakegpus.h"
#include "gtk/gtkloop.h"
#include "gtk/hostprefs.h"
#include "gtk/mainwindow.h"
#include "gtk/notices.h"
#include "gtk/preferencesdialog.h"
#include "gtk/richtext.h"
#include "gtk/rows.h"
#include "ui/hosttuningtext.h"
#include "ui/memorytext.h"
#include "ui/preferencestext.h"
#include "ui/udmabuftext.h"
#endif

/*
 * The GTK app's Preferences and the notices of the running VMs, on a
 * display no one sees: each change is saved at once, through the calls
 * the Qt dialog makes on OK; the host page's rows on a fake host (its
 * GPUs, a fake root for gpu-input-awake, stand-ins for pkexec, id and
 * systemctl), never this computer's, and no helper (nothing asks
 * polkit); the notices of a VM found running (the stand-in QEMU and its
 * monitor): untuned, short of memory, udmabuf's limits low.  The shared
 * rich text as Pango markup.
 */

static const char kFakePkexec[] = R"sh(#!/bin/bash
echo "pkexec $*" >> "${STATE:?}/calls"
[ "$1" = --disable-internal-agent ] && shift
if [ -e "$STATE/cancel" ]; then
	echo "Error executing command as another user: Not authorized"
	exit 127
fi
exec "$@"
)sh";

static const char kFakeSystemctl[] = R"sh(#!/bin/bash
echo "systemctl $*" >> "${STATE:?}/calls"
[ "$1" = show ] && [ -e "$STATE/show" ] && cat "$STATE/show"
exit 0
)sh";

static bool writeFile(const QString &path, const QByteArray &data, bool exec = false)
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

static QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

/* QEMU's end of QMP: running, and taking every other command */
class FakeQmp : public QObject
{
public:
    explicit FakeQmp(const QString &path)
    {
        QLocalServer::removeServer(path);
        m_server.listen(path);
        connect(&m_server, &QLocalServer::newConnection, this, [this]() {
            QLocalSocket *peer = m_server.nextPendingConnection();
            connect(peer, &QLocalSocket::readyRead, this, [this, peer]() {
                m_buffer += peer->readAll();
                qsizetype nl;
                while ((nl = m_buffer.indexOf('\n')) >= 0) {
                    const QJsonObject c = QJsonDocument::fromJson(m_buffer.left(nl)).object();
                    m_buffer.remove(0, nl + 1);
                    const QByteArray result = c["execute"] == "query-status"
                                                  ? R"({"running": true, "status": "running"})"
                                                  : "{}";
                    peer->write("{\"return\": " + result + ", \"id\": " +
                                QByteArray::number(c["id"].toInteger()) + "}\n");
                }
            });
            peer->write(R"({"QMP": {"version": {"qemu": {"major": 11}}, "capabilities": []}})"
                        "\n");
        });
    }

private:
    QLocalServer m_server;
    QByteArray m_buffer;
};

/* A VM found running: the stand-in QEMU of a VM of @args, and its monitor */
struct Running {
    QTemporaryDir vms;
    QProcess qemu;
    std::unique_ptr<FakeQmp> qmp;
    std::unique_ptr<VmStore> store;
    Vm *vm = nullptr;

    explicit Running(const QByteArray &args)
    {
        writeFile(vms.path() + "/busy/vm.args", args);
        const QString run = Paths::vmRuntimeDir("busy");
        const QString qmpArg = QString("unix:%1/qmp.sock,server=on,wait=off").arg(run);
        qemu.start(FAKE_QEMU, {"-qmp", qmpArg});
        qemu.waitForStarted();
        /* the runner knows its QEMU by its command line: once exec is done */
        (void)QTest::qWaitFor([&]() {
            QFile f(QString("/proc/%1/cmdline").arg(qemu.processId()));
            return f.open(QIODevice::ReadOnly) && f.readAll().contains(qmpArg.toLocal8Bit());
        }, 20000);
        writeFile(run + "/qemu.pid", QByteArray::number(qemu.processId()) + "\n");
        qmp = std::make_unique<FakeQmp>(run + "/qmp.sock");
        store = std::make_unique<VmStore>(vms.path());
        vm = store->find("busy");
    }
    ~Running() { stop(); }
    void attach() { vm->runner()->attach(vm->args()); }
    void stop()
    {
        qmp.reset();
        if (qemu.state() != QProcess::NotRunning) {
            qemu.kill();
            qemu.waitForFinished();
        }
    }
};

class TestGtkPrefs : public QObject
{
    Q_OBJECT

    QProcess m_broadway;
    GTestDBus *m_bus = nullptr;
    GtkLoop *m_loop = nullptr;
    QTemporaryDir m_tmp;
    QString m_state, m_root;
    std::unique_ptr<FakeGpus> m_gpus;
    const QByteArray m_path = qgetenv("PATH");
    /* what was presented, in its order */
    QList<AdwDialog *> m_presented;

    static QString source() { return QStringLiteral(GPU_AWAKE_SOURCE_DIR); }

    Gtk4::Dialog::Presenter presenter()
    {
        return [this](AdwDialog *dialog) { m_presented << ADW_DIALOG(g_object_ref_sink(dialog)); };
    }

    /* The alert presented last with @heading, if any */
    AdwAlertDialog *alert(const QString &heading) const
    {
        for (qsizetype i = m_presented.size() - 1; i >= 0; i--) {
            if (ADW_IS_ALERT_DIALOG(m_presented[i]) &&
                QString::fromUtf8(adw_alert_dialog_get_heading(ADW_ALERT_DIALOG(m_presented[i]))) ==
                    heading) {
                return ADW_ALERT_DIALOG(m_presented[i]);
            }
        }
        return nullptr;
    }

    static QString body(AdwAlertDialog *alert)
    {
        return QString::fromUtf8(adw_alert_dialog_get_body(alert));
    }

    /* The widget named @name under @widget, depth first */
    static GtkWidget *named(GtkWidget *widget, const char *name)
    {
        if (g_strcmp0(gtk_widget_get_name(widget), name) == 0) {
            return widget;
        }
        for (GtkWidget *child = gtk_widget_get_first_child(widget); child;
             child = gtk_widget_get_next_sibling(child)) {
            if (GtkWidget *found = named(child, name)) {
                return found;
            }
        }
        return nullptr;
    }

    static QString subtitle(GtkWidget *row)
    {
        return QString::fromUtf8(adw_action_row_get_subtitle(ADW_ACTION_ROW(row)));
    }

    /* @markup as GTK's labels take it: Pango's, links apart (the label takes those) */
    static bool parses(const QString &markup, QString *error)
    {
        static const QRegularExpression link("</?a(?:\\s[^>]*)?>");
        GError *e = nullptr;
        QString text = markup;
        text.remove(link);
        const bool ok = pango_parse_markup(text.toUtf8().constData(), -1, 0, nullptr, nullptr,
                                           nullptr, &e);
        if (!ok) {
            *error = QString::fromUtf8(e->message) + ": " + text;
            g_error_free(e);
        }
        return ok;
    }

    static QSettings settings()
    {
        return QSettings(Paths::settingsPath(), QSettings::IniFormat);
    }

private slots:
    void initTestCase()
    {
        QString error;

        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(m_tmp.isValid());
        m_bus = GtkDisplay::up(m_broadway);
        if (!m_bus) {
            QSKIP("gtk4-broadwayd is not installed: no display to test on but the desktop's");
        }
        m_loop = new GtkLoop("org.vitrine.Vitrine");
        QCOMPARE(m_loop->start(&error), GtkLoop::Start::Primary);
        const QString bin = m_tmp.filePath("bin");
        QVERIFY(writeFile(bin + "/pkexec", kFakePkexec, true));
        QVERIFY(writeFile(bin + "/systemctl", kFakeSystemctl, true));
        QVERIFY(writeFile(bin + "/id", "#!/bin/bash\n[ \"$1\" = -u ] && { echo 0; exit 0; }\n"
                                       "exec /usr/bin/id \"$@\"\n", true));
        qputenv("PATH", bin.toLocal8Bit() + ':' + m_path);
        /* no helper: nothing asks polkit, nothing runs as root */
        qputenv("VITRINE_HELPER", m_tmp.filePath("no-helper").toLocal8Bit());
        qputenv("VITRINE_AWAKE_DIR", source().toLocal8Bit());
    }

    void cleanupTestCase()
    {
        HostGpus::setRoots("/sys", "/dev");
        qputenv("PATH", m_path);
        delete m_loop;
        GtkDisplay::down(m_broadway, m_bus);
    }

    void init()
    {
        const QString name = QTest::currentTestFunction();
        m_state = m_tmp.filePath("state-" + name);
        m_root = m_tmp.filePath("root-" + name);
        QDir().mkpath(m_state);
        QDir().mkpath(m_root);
        qputenv("STATE", m_state.toLocal8Bit());
        qputenv("VITRINE_AWAKE_ROOT", m_root.toLocal8Bit());
        /* a host without GPUs, unless the test gives it some */
        m_gpus = std::make_unique<FakeGpus>(m_tmp.filePath("gpus-" + name));
        m_gpus->use();
        settings().remove("host");
        settings().remove("qemu");
        settings().remove("virtiofsd");
        settings().remove("vms");
        settings().remove("updates");
    }

    void cleanup()
    {
        for (AdwDialog *dialog : std::as_const(m_presented)) {
            g_object_unref(dialog);
        }
        m_presented.clear();
    }

    /* Qt's rich text as Pango markup, and every text both apps share parses so */
    void markup()
    {
        QCOMPARE(Gtk4::markup("<p>One.</p><p>Two <b>bold</b>.</p>"),
                 QString("One.\n\nTwo <b>bold</b>."));
        QCOMPARE(Gtk4::markup("Install it:<pre>a --b\nc</pre>Then go."),
                 QString("Install it:\n<tt>a --b\nc</tt>\nThen go."));
        QCOMPARE(Gtk4::markup("Run <code>x &amp; y</code><br>next"),
                 QString("Run <tt>x &amp; y</tt>\nnext"));
        QCOMPARE(Gtk4::markup("<nobr>journalctl -u x</nobr> says why."),
                 QString("journalctl -u x says why."));
        QCOMPARE(Gtk4::markup("See <a href=\"https://example.org/a#b\">docs</a>."),
                 QString("See <a href=\"https://example.org/a#b\">docs</a>."));
        QCOMPARE(Gtk4::markup("a &nbsp;b"), QString("a &#160;b"));
        /* plain words: what would be markup escaped */
        QCOMPARE(Gtk4::markup("1 < 2 & 3 > 2"), QString("1 &lt; 2 &amp; 3 &gt; 2"));

        using Problem = HostSettings::Problem;
        QStringList texts = {HostTuningText::summary(), HostTuningText::active().text,
                             HostTuningText::setUpFailed("it <failed> & stopped").text,
                             HostTuningPrefsText::cpuHint(), HostTuningPrefsText::active("Real-time"),
                             UdmabufText::persistentFix(), PreferencesText::virtiofsd({}),
                             PreferencesText::virtiofsd("/nonexistent"),
                             PreferencesText::virtiofsd("/bin/sh"),
                             MemoryText::text(4096, 1024, HostMemory::Info{16384, 2048, 0})};
        for (const Problem problem :
             {Problem::None, Problem::NotInstalled, Problem::NoPolkit, Problem::NoPolicy,
              Problem::NoGroup, Problem::NotMember, Problem::NotLocal, Problem::Failed}) {
            const HostSettings::Status status{problem, "the helper said <no> & left"};
            texts << HostTuningText::fix(status) << HostTuningText::inactive(2, status).text
                  << HostTuningText::offer(status).text << HostTuningText::setUpDone(status).text
                  << HostTuningPrefsText::notActive(status);
        }
        for (const auto installed : {GpuInputAwake::Installed::No, GpuInputAwake::Installed::Vitrine,
                                     GpuInputAwake::Installed::Other,
                                     GpuInputAwake::Installed::Partial}) {
            texts << HostTuningPrefsText::awake({"Intel Arc Pro B50"}, installed, "failed");
        }
        UdmabufWatch::Issue low;
        low.vmId = "f";
        low.vmName = "Fedora <KDE>";
        low.limitsLow = true;
        low.limits.listLimit = 1024;
        low.limits.sizeLimitMb = 64;
        Udmabuf::Limits now = low.limits;
        texts << UdmabufText::explanation({low}, now);
        now.deviceErrno = ENOENT;
        texts << UdmabufText::explanation({low}, now);
        for (const QString &text : std::as_const(texts)) {
            QString error;
            QVERIFY2(parses(Gtk4::markup(text), &error), qPrintable(error));
        }
        /* the commands as they are to be typed, the docs' link kept */
        const QString fix = Gtk4::markup(HostTuningText::fix({Problem::NotInstalled, {}}));
        QVERIFY(fix.contains("<tt>cmake -B build -DCMAKE_INSTALL_PREFIX=/usr\ncmake --build build\n"
                             "sudo cmake --install build</tt>"));
        QVERIFY(fix.contains("<a href=\"https://github.com/tomtomnet/vitrine/blob/main/docs/"
                             "host-tuning.md\">docs/host-tuning.md</a>"));
        QVERIFY(!fix.contains("nobr"));
    }

    /* General: each change saved at once, as the Qt dialog's OK saves it */
    void general()
    {
        QTemporaryDir vms;
        VmStore store(vms.path());
        QStringList builds;
        auto *dialog = new PreferencesDialog(&store, presenter(),
                                             [&builds](const QString &component, const QString &release) {
            builds << component + '/' + release;
        });

        /* the update check */
        GtkWidget *updates = dialog->find("check-updates");
        QVERIFY(updates);
        QVERIFY(adw_switch_row_get_active(ADW_SWITCH_ROW(updates)));
        adw_switch_row_set_active(ADW_SWITCH_ROW(updates), false);
        QCOMPARE(settings().value("updates/check").toBool(), false);

        /* the build rows open the Builds window on what they show */
        g_signal_emit_by_name(dialog->find("qemu-build-button"), "clicked");
        g_signal_emit_by_name(dialog->find("guest-tools-button"), "clicked");
        QCOMPARE(builds, QStringList({"qemu/", "medium/" + BuildManager::instance()->guestRelease()}));

        /* another QEMU: switched on, a path applied and checked, switched off */
        const QString qemu = m_tmp.filePath("qemu-system-x86_64");
        QVERIFY(writeFile(qemu, "#!/bin/sh\necho 'QEMU emulator version 99.1.0 (test)'\n", true));
        GtkWidget *custom = dialog->find("custom-qemu");
        GtkWidget *path = dialog->find("custom-qemu-path");
        GtkWidget *status = dialog->find("custom-qemu-status");
        QVERIFY(custom && path && status);
        QVERIFY(!adw_expander_row_get_enable_expansion(ADW_EXPANDER_ROW(custom)));
        adw_expander_row_set_enable_expansion(ADW_EXPANDER_ROW(custom), true);
        QCOMPARE(Paths::customQemuBinary(), QString());
        QCOMPARE(subtitle(status), PreferencesText::qemuNone());
        Gtk4::setText(path, qemu);
        g_signal_emit_by_name(path, "apply");
        QCOMPARE(Paths::customQemuBinary(), qemu);
        QTRY_COMPARE(subtitle(status), QString("QEMU emulator version 99.1.0 (test)"));
        const QString notProgram = m_tmp.filePath("not-a-program");
        QVERIFY(writeFile(notProgram, "text"));
        Gtk4::setText(path, notProgram);
        g_signal_emit_by_name(path, "apply");
        QCOMPARE(Paths::customQemuBinary(), notProgram);
        QCOMPARE(subtitle(status), PreferencesText::notExecutable());
        adw_expander_row_set_enable_expansion(ADW_EXPANDER_ROW(custom), false);
        QCOMPARE(Paths::customQemuBinary(), QString());

        /* virtiofsd: checked as typed, saved when applied; empty for the one found */
        GtkWidget *virtiofsd = dialog->find("virtiofsd");
        GtkWidget *virtiofsdStatus = dialog->find("virtiofsd-status");
        QVERIFY(virtiofsd && virtiofsdStatus);
        Gtk4::setText(virtiofsd, "/nonexistent/virtiofsd");
        QCOMPARE(subtitle(virtiofsdStatus), PreferencesText::notExecutable());
        g_signal_emit_by_name(virtiofsd, "apply");
        QCOMPARE(Paths::virtiofsd(), QString("/nonexistent/virtiofsd"));
        Gtk4::setText(virtiofsd, QString());
        g_signal_emit_by_name(virtiofsd, "apply");
        QCOMPARE(Paths::virtiofsd(), PreferencesText::autoVirtiofsd());

        /* the folder of the VMs: the store lists the folder chosen, at once */
        QTemporaryDir other;
        QVERIFY(writeFile(other.path() + "/x/vm.args", "-name X\n") &&
                writeFile(other.path() + "/y/vm.args", "-name Y\n"));
        GtkWidget *state = dialog->find("vms-dir-state");
        GtkWidget *byDefault = dialog->find("vms-dir-default");
        QVERIFY(state && byDefault);
        QVERIFY(!gtk_widget_get_visible(state));
        dialog->setVmsDir(other.path());
        QCOMPARE(Paths::vmsDir(), QDir(other.path()).absolutePath());
        QCOMPARE(store.dir(), QDir(other.path()).absolutePath());
        QCOMPARE(store.vms().size(), 2);
        QVERIFY(gtk_widget_get_visible(state));
        QVERIFY(subtitle(state).startsWith("2 VM(s) found there."));
        QVERIFY(gtk_widget_get_sensitive(byDefault));
        g_signal_emit_by_name(byDefault, "clicked");
        QCOMPARE(Paths::vmsDir(), Paths::defaultVmsDir());
        QCOMPARE(store.dir(), QDir(Paths::defaultVmsDir()).absolutePath());
        QVERIFY(!gtk_widget_get_sensitive(byDefault));
        QVERIFY(m_presented.isEmpty());
        delete dialog;
    }

    /* Host: tuning on and off, its state, the floor, the CPU; saved at once */
    void host()
    {
        HostSettings::setEnabled(false);
        auto prefs = std::make_unique<HostPrefs>(presenter());
        GtkWidget *page = GTK_WIDGET(prefs->page());
        GtkWidget *tune = named(page, "tune-host");
        GtkWidget *state = named(page, "tune-host-state");
        GtkWidget *setUp = named(page, "tune-host-set-up");
        GtkWidget *floor = named(page, "gpu-floor");
        GtkWidget *mhz = named(page, "gpu-floor-mhz");
        GtkWidget *cpu = named(page, "cpu-latency");
        QVERIFY(tune && state && setUp && floor && mhz && cpu);
        /* no dedicated GPU here: no gpu-input-awake */
        QVERIFY(!named(page, "gpu-input-awake"));

        /* off: nothing to warn about, the rest not for now */
        QVERIFY(!adw_switch_row_get_active(ADW_SWITCH_ROW(tune)));
        QVERIFY(!gtk_widget_get_visible(state));
        QVERIFY(!gtk_widget_get_sensitive(floor));
        QVERIFY(!gtk_widget_get_sensitive(cpu));

        /* on: why it is not active, and the fix (no helper here) */
        adw_switch_row_set_active(ADW_SWITCH_ROW(tune), true);
        QVERIFY(HostSettings::enabled());
        QVERIFY(gtk_widget_get_visible(state));
        QTRY_VERIFY(subtitle(state).startsWith("<b>Not active:</b>"));
        QVERIFY(subtitle(state).contains("<tt>cmake -B build"));
        QVERIFY(!gtk_widget_get_visible(setUp));
        QVERIFY(gtk_widget_get_sensitive(floor));
        QVERIFY(gtk_widget_get_sensitive(cpu));

        /* the floor: automatic, off, at least so many MHz */
        QCOMPARE(HostSettings::gpuFloor(), QString("auto"));
        QVERIFY(!gtk_widget_get_visible(mhz));
        adw_combo_row_set_selected(ADW_COMBO_ROW(floor), 1);
        QCOMPARE(HostSettings::gpuFloor(), QString("off"));
        adw_combo_row_set_selected(ADW_COMBO_ROW(floor), 2);
        QVERIFY(gtk_widget_get_visible(mhz));
        QCOMPARE(HostSettings::gpuFloor(), QString("1800"));
        adw_spin_row_set_value(ADW_SPIN_ROW(mhz), 2200);
        QCOMPARE(HostSettings::gpuFloor(), QString("2200"));
        prefs = std::make_unique<HostPrefs>(presenter());
        page = GTK_WIDGET(prefs->page());
        QCOMPARE(adw_combo_row_get_selected(ADW_COMBO_ROW(named(page, "gpu-floor"))), 2u);
        QCOMPARE(int(adw_spin_row_get_value(ADW_SPIN_ROW(named(page, "gpu-floor-mhz")))), 2200);
        adw_combo_row_set_selected(ADW_COMBO_ROW(named(page, "gpu-floor")), 0);
        QCOMPARE(HostSettings::gpuFloor(), QString("auto"));

        /* the CPU kept ready to wake up: on by default, said what it costs */
        cpu = named(page, "cpu-latency");
        QVERIFY(adw_switch_row_get_active(ADW_SWITCH_ROW(cpu)));
        QVERIFY(subtitle(cpu).contains("about 2 W at idle"));
        QVERIFY(subtitle(cpu).contains("ends when the last VM stops"));
        adw_switch_row_set_active(ADW_SWITCH_ROW(cpu), false);
        QVERIFY(!HostSettings::cpuLatency());

        /* off again */
        adw_switch_row_set_active(ADW_SWITCH_ROW(named(page, "tune-host")), false);
        QVERIFY(!HostSettings::enabled());
        QVERIFY(!gtk_widget_get_visible(named(page, "tune-host-state")));
        QVERIFY(m_presented.isEmpty());
    }

    /* An Arc Pro B50: gpu-input-awake not installed, Install… then Remove… as the app runs them */
    void awake()
    {
        m_gpus->gpu("0000:03:00.0", "0x8086", "0xe212", "xe", true, 0, 128);
        m_gpus->use();
        HostPrefs prefs(presenter());
        GtkWidget *page = GTK_WIDGET(prefs.page());
        GtkWidget *row = named(page, "gpu-input-awake");
        GtkWidget *install = named(page, "gpu-input-awake-install");
        GtkWidget *remove = named(page, "gpu-input-awake-remove");
        QVERIFY(row && install && remove);
        QTRY_VERIFY(subtitle(row).endsWith("It is not installed."));
        QVERIFY(subtitle(row).contains("a clock floor does nothing on a dedicated GPU"));
        QVERIFY(gtk_widget_get_visible(install) && gtk_widget_get_sensitive(install));
        QVERIFY(!gtk_widget_get_visible(remove));

        writeFile(m_state + "/show", "ActiveState=active\nUnitFileState=enabled\n");
        g_signal_emit_by_name(install, "clicked");
        AdwAlertDialog *confirm = alert("Install gpu-input-awake");
        QVERIFY(confirm);
        QVERIFY(body(confirm).contains("runs as root from now on"));
        QVERIFY(adw_alert_dialog_has_response(confirm, "go"));
        g_signal_emit_by_name(confirm, "response", "go");
        QTRY_VERIFY(subtitle(row).endsWith("It is installed and runs."));
        QCOMPARE(GpuInputAwake::installed(source(), m_root), GpuInputAwake::Installed::Vitrine);
        const QByteArray calls = readFile(m_state + "/calls");
        QVERIFY(calls.contains("pkexec --disable-internal-agent "));
        QVERIFY(calls.contains(QFile::encodeName(source()) + "/install.sh install"));
        QVERIFY(!gtk_widget_get_visible(install));
        QVERIFY(gtk_widget_get_visible(remove) && gtk_widget_get_sensitive(remove));

        /* dismissed: nothing happens */
        g_signal_emit_by_name(remove, "clicked");
        confirm = alert("Remove gpu-input-awake");
        QVERIFY(confirm);
        QVERIFY(body(confirm).contains("stops now and no longer starts at boot"));
        g_signal_emit_by_name(confirm, "response", "cancel");
        QCOMPARE(GpuInputAwake::installed(source(), m_root), GpuInputAwake::Installed::Vitrine);
        g_signal_emit_by_name(remove, "clicked");
        g_signal_emit_by_name(alert("Remove gpu-input-awake"), "response", "go");
        QTRY_VERIFY(subtitle(row).endsWith("It is not installed."));
        QCOMPARE(GpuInputAwake::installed(source(), m_root), GpuInputAwake::Installed::No);
        QVERIFY(gtk_widget_get_visible(install));
        QVERIFY(!gtk_widget_get_visible(remove));
    }

    /*
     * A VM running untuned (tuning on, no helper) and short of memory: the
     * notices' button, their explanations; the group's offer; gone with
     * the run
     */
    void notices()
    {
        HostSettings::setEnabled(true);
        Running vm("-name Big\n-m 64T\n-display none\n");
        HostSettings host(vm.store.get());
        GtkWidget *window = gtk_window_new();
        {
            Notices notices(&host, vm.store.get(), GTK_WINDOW(window), presenter());
            QVERIFY(notices.shown().isEmpty());
            QVERIFY(!gtk_widget_get_visible(notices.button()));

            vm.attach();
            QTRY_VERIFY(notices.shown().contains(Notices::Kind::HostTuning));
            const HostSettings::Status notInstalled{HostSettings::Problem::NotInstalled, {}};
            QCOMPARE(notices.text(Notices::Kind::HostTuning),
                     QString("A running VM is not tuned: %1.").arg(notInstalled.why()));
            QTRY_VERIFY(notices.shown().contains(Notices::Kind::Memory));
            QVERIFY(gtk_widget_get_visible(notices.button()));
            const QString tip = QString::fromUtf8(gtk_widget_get_tooltip_text(notices.button()));
            QVERIFY(tip.contains("Host Tuning Inactive"));
            QVERIFY(tip.contains("Memory Is Short"));

            /* asked again, then said why, with the fix */
            notices.explain(Notices::Kind::HostTuning);
            QTRY_VERIFY(alert("Host Tuning Inactive"));
            AdwAlertDialog *inactive = alert("Host Tuning Inactive");
            QVERIFY(body(inactive).contains("Install it with Vitrine"));
            QVERIFY(adw_alert_dialog_has_response(inactive, "off"));
            QVERIFY(!adw_alert_dialog_has_response(inactive, "set-up"));
            QCOMPARE(QString::fromUtf8(adw_alert_dialog_get_default_response(inactive)), QString("close"));

            notices.explain(Notices::Kind::Memory);
            AdwAlertDialog *memory = alert("Memory Is Short");
            QVERIFY(memory);
            QVERIFY(body(memory).contains("Close programs or VMs"));

            /* the offer of the group, by itself at a start: Not Now the default */
            Q_EMIT host.groupSetupSuggested({HostSettings::Problem::NotMember, {}});
            AdwAlertDialog *offer = alert("Tune the Host for VMs?");
            QVERIFY(offer);
            QCOMPARE(QString::fromUtf8(adw_alert_dialog_get_default_response(offer)),
                     QString("not-now"));
            QVERIFY(adw_alert_dialog_has_response(offer, "set-up"));
            QVERIFY(body(offer).contains("You are not in it: Set Up adds you to it."));
            /* Turn Off Tuning: off, at once */
            g_signal_emit_by_name(offer, "response", "off");
            QVERIFY(!HostSettings::enabled());
            QTRY_VERIFY(!notices.shown().contains(Notices::Kind::HostTuning));

            /* the run ends: the notices go */
            vm.stop();
            QTRY_VERIFY(notices.shown().isEmpty());
            QVERIFY(!gtk_widget_get_visible(notices.button()));
        }
        gtk_window_destroy(GTK_WINDOW(window));
    }

    /* udmabuf's limits low for a native-context VM: the notice; tuning on,
       its explanation by itself, once */
    void udmabuf()
    {
        const QString sys = m_tmp.filePath("udmabuf-sys");
        const QString device = m_tmp.filePath("udmabuf-device");
        writeFile(sys + "/module/udmabuf/parameters/list_limit", "1024\n");
        writeFile(sys + "/module/udmabuf/parameters/size_limit_mb", "64\n");
        writeFile(device, QByteArray());
        GtkWidget *window = gtk_window_new();

        /* off by choice: the notice, nothing by itself; Turn On where a helper is */
        HostSettings::setEnabled(false);
        qputenv("VITRINE_HELPER", QCoreApplication::applicationFilePath().toLocal8Bit());
        {
            Running vm("-name Native\n-device virtio-vga-gl,blob=on,drm_native_context=on\n"
                       "-display dbus,p2p=yes\n");
            Notices notices(nullptr, vm.store.get(), GTK_WINDOW(window), presenter());
            notices.udmabufWatch()->setSysRoot(sys);
            notices.udmabufWatch()->setDevice(device);
            notices.udmabufWatch()->setPollInterval(100000);
            vm.attach();
            QTRY_VERIFY(notices.shown().contains(Notices::Kind::Udmabuf));
            QCOMPARE(notices.title(Notices::Kind::Udmabuf), QString("udmabuf Limits Too Low"));
            QVERIFY(notices.text(Notices::Kind::Udmabuf)
                        .startsWith("Native: Guest windows drawn by the CPU will be copied"));
            QTest::qWait(50);
            QVERIFY(m_presented.isEmpty());
            notices.explain(Notices::Kind::Udmabuf);
            AdwAlertDialog *explained = alert("udmabuf Limits Too Low");
            QVERIFY(explained);
            QVERIFY(body(explained).contains("grubby"));
            QVERIFY(adw_alert_dialog_has_response(explained, "turn-on"));
            QCOMPARE(QString::fromUtf8(adw_alert_dialog_get_default_response(explained)),
                     QString("close"));
        }
        qputenv("VITRINE_HELPER", m_tmp.filePath("no-helper").toLocal8Bit());
        cleanup();

        HostSettings::setEnabled(true);
        {
            Running vm("-name Native\n-device virtio-vga-gl,blob=on,drm_native_context=on\n"
                       "-display dbus,p2p=yes\n");
            Notices notices(nullptr, vm.store.get(), GTK_WINDOW(window), presenter());
            notices.udmabufWatch()->setSysRoot(sys);
            notices.udmabufWatch()->setDevice(device);
            notices.udmabufWatch()->setPollInterval(100000);
            vm.attach();
            QTRY_VERIFY(alert("udmabuf Limits Too Low"));
            QVERIFY(!adw_alert_dialog_has_response(alert("udmabuf Limits Too Low"), "turn-on"));
            QCOMPARE(m_presented.size(), 1);
        }
        gtk_window_destroy(GTK_WINDOW(window));
    }

    /* The window's Preferences action: active, one dialog at a time; its notices' button */
    void window()
    {
        QTemporaryDir dir;
        VmStore store(dir.path());
        MainWindow window(m_loop, &store);

        QVERIFY(g_action_group_get_action_enabled(G_ACTION_GROUP(m_loop->adw()), "preferences"));
        PreferencesDialog *shown = window.showPreferences();
        QVERIFY(shown);
        QCOMPARE(window.showPreferences(), shown);
        shown->setPage(PreferencesDialog::Host);
        QCOMPARE(QString::fromUtf8(adw_preferences_dialog_get_visible_page_name(
                     ADW_PREFERENCES_DIALOG(shown->dialog()))),
                 QString("host"));
        QVERIFY(window.notices());
        QVERIFY(!gtk_widget_get_visible(window.notices()->button()));
        /* closed: the next one is another */
        bool closed = false;
        Gtk4::connect<void()>(shown->dialog(), "closed", [&closed]() { closed = true; });
        shown->forceClose();
        QTRY_VERIFY(closed);
        PreferencesDialog *again = window.showPreferences();
        QVERIFY(again);
        closed = false;
        Gtk4::connect<void()>(again->dialog(), "closed", [&closed]() { closed = true; });
        again->forceClose();
        QTRY_VERIFY(closed);
    }
};

int main(int argc, char **argv)
{
    QTemporaryDir runtime(QDir::tempPath() + "/vitrine-gtkprefs-XXXXXX");
    if (!GtkDisplay::prepare(runtime)) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    TestGtkPrefs test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_gtkprefs.moc"
