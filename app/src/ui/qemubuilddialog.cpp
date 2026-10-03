// SPDX-License-Identifier: GPL-2.0-or-later
#include "qemubuilddialog.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include "core/paths.h"
#include "core/qemubuilder.h"
#include "ui/widgets.h"

/* What configure takes: the features of the branch built, and the targets */
static QString featuresUrl()
{
    return QString(QemuBuilder::defaultUrl()).chopped(4) + "/blob/" +
           QemuBuilder::defaultBranch() + "/meson_options.txt";
}

static const char kTargetsUrl[] = "https://www.qemu.org/docs/master/system/targets.html";

/* What a branch of the fork is: its README says */
static QString readmeUrl(const QString &branch)
{
    return QString(QemuBuilder::defaultUrl()).chopped(4) + "/blob/" + branch +
           "/.github/README.md";
}

QemuBuildDialog::QemuBuildDialog(QWidget *parent)
    : QDialog(parent), m_builder(new QemuBuilder(this))
{
    QSettings settings(Paths::settingsPath(), QSettings::IniFormat);
    auto *layout = new QVBoxLayout(this);
    auto *form = Widgets::form();
    auto *status = new QHBoxLayout;

    setWindowTitle(tr("Build QEMU"));

    auto *intro = Widgets::note(
        tr("Builds <a href=\"%1\">qemu-gui</a>, QEMU with its controls in the VM window, "
           "for this %2 computer, and makes it the QEMU of the VMs. Building again updates "
           "it first.")
            .arg(QString(QemuBuilder::defaultUrl()).chopped(4), Paths::hostArch()));
    intro->setToolTip(tr("Downloaded into %1").arg(QemuBuilder::defaultSourceDir()));
    layout->addWidget(intro);

    /* master, or an experiment of the fork's */
    m_branch = new QComboBox;
    /* the list comes later, and wider */
    m_branch->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_branchNote = Widgets::hint();
    form->addRow(tr("B&ranch:"), m_branch);
    form->addRow(QString(), m_branchNote);
    setBranches({}, settings.value("build/branch", QemuBuilder::defaultBranch()).toString());
    connect(m_branch, &QComboBox::currentIndexChanged, this, &QemuBuildDialog::updateBranchNote);
    listBranches();

    /*
     * The host's target alone, with the features configure finds: the
     * target list is what sizes a build, all of them being ten times more
     */
    m_preset = new QComboBox;
    m_preset->addItem(tr("%1, this computer").arg(Paths::hostArch()),
                      QemuBuilder::defaultConfigureArgs().join(' '));
    m_preset->addItem(tr("Custom configure options"));
    m_configure = new QLineEdit(settings.value("build/configure",
                                               QemuBuilder::defaultConfigureArgs().join(' '))
                                    .toString());
    form->addRow(tr("&Build for:"), m_preset);
    form->addRow(tr("configure &options:"), m_configure);
    form->addRow(QString(),
                 Widgets::hint(tr("Options of QEMU's <code>configure</code> script, like "
                                  "<nobr><code>--disable-gtk</code></nobr> or "
                                  "<nobr><code>--target-list=x86_64-softmmu,aarch64-softmmu"
                                  "</code></nobr>. The <a href=\"%1\">features</a> go with "
                                  "<code>--enable-</code> or <code>--disable-</code>, the "
                                  "<a href=\"%2\">targets</a> in <code>--target-list</code>.")
                                   .arg(featuresUrl(), QString(kTargetsUrl))));
    layout->addLayout(form);

    m_virgl = new QCheckBox(tr("&DRM native context: 3D acceleration by the host GPU's own "
                               "driver"));
    m_virgl->setChecked(settings.value("build/virgl").toBool());
    layout->addWidget(m_virgl);
    layout->addWidget(Widgets::hint(
        tr("Builds a virglrenderer with the renderers of every GPU that has one: Intel (Xe with "
           "a patch not upstream yet), AMD (with a patch of ours for smooth desktops), "
           "Qualcomm, Apple and Arm Mali, and Venus for Vulkan. This QEMU uses it rather than the system's, which may have none. The "
           "guest needs native context support in its Mesa.")));
    m_virglStatus = Widgets::hint();
    layout->addWidget(m_virglStatus);
    layout->addWidget(Widgets::note(
        tr("Building needs the build dependencies, on Fedora: <code>sudo dnf builddep qemu "
           "virglrenderer</code>, and an internet connection.")));

    /* the step and its progress on one line, the step alone after */
    m_step = new QLabel;
    m_step->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_progress = new QProgressBar;
    m_progress->setVisible(false);
    status->addWidget(m_step);
    status->addWidget(m_progress, 1);
    layout->addLayout(status);

    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(5000);
    m_log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_log->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(m_log, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    m_build = buttons->addButton(tr("Update and Build"), QDialogButtonBox::ActionRole);
    m_cancel = buttons->addButton(tr("Stop"), QDialogButtonBox::ActionRole);
    m_use = buttons->addButton(tr("Use for the VMs"), QDialogButtonBox::ActionRole);
    m_build->setDefault(true);
    layout->addWidget(buttons);

    /* the options saved are the host's, else custom ones */
    const int saved = m_preset->findData(m_configure->text().simplified());
    m_preset->setCurrentIndex(saved < 0 ? m_preset->count() - 1 : saved);
    connect(m_preset, &QComboBox::currentIndexChanged, this, [this](int i) {
        /* custom ones start from those shown */
        if (!m_preset->itemData(i).isNull()) {
            m_configure->setText(m_preset->itemData(i).toString());
        }
        updateState();
        if (m_configure->isEnabled()) {
            m_configure->setFocus();
        }
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(m_build, &QPushButton::clicked, this, &QemuBuildDialog::build);
    connect(m_cancel, &QPushButton::clicked, m_builder, &QemuBuilder::cancel);
    connect(m_use, &QPushButton::clicked, this, [this]() {
        const QString binary = QemuBuilder::binary(QemuBuilder::defaultSourceDir());
        Paths::setQemuBinary(binary);
        emit qemuChanged(binary);
        m_step->setText(tr("The VMs now use %1").arg(binary));
        updateState();
    });

    connect(m_builder, &QemuBuilder::stepStarted, m_step, &QLabel::setText);
    connect(m_builder, &QemuBuilder::output, this, [this](const QString &text) {
        m_log->moveCursor(QTextCursor::End);
        m_log->insertPlainText(QString(text).replace('\r', '\n'));
        m_log->moveCursor(QTextCursor::End);
    });
    connect(m_builder, &QemuBuilder::progress, this, [this](int done, int total) {
        m_progress->setMaximum(total);
        m_progress->setValue(done);
    });
    connect(m_builder, &QemuBuilder::finished, this, &QemuBuildDialog::finished);

    updateState();
    updateVirglStatus();
    resize(760, 620);
}

void QemuBuildDialog::listBranches()
{
    auto *git = new QProcess(this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

    env.insert("GIT_TERMINAL_PROMPT", "0");
    git->setProcessEnvironment(env);
    connect(git, &QProcess::finished, this, [this, git](int code) {
        const QStringList heads = QemuBuilder::parseHeads(git->readAllStandardOutput());
        git->deleteLater();
        /* offline: master and the chosen one stay */
        if (code == 0 && !heads.isEmpty()) {
            setBranches(heads, branch());
        }
    });
    connect(git, &QProcess::errorOccurred, this, [git](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            git->deleteLater();
        }
    });
    git->start("git", {"ls-remote", "--heads", QemuBuilder::defaultUrl()});
}

void QemuBuildDialog::setBranches(QStringList names, const QString &chosen)
{
    const QString master = QemuBuilder::defaultBranch();

    names.removeAll(master);
    names.sort();
    names.prepend(master);
    if (!chosen.isEmpty() && !names.contains(chosen)) {
        names << chosen;
    }
    m_branch->blockSignals(true);
    m_branch->clear();
    for (const QString &name : std::as_const(names)) {
        m_branch->addItem(name == master ? name : tr("%1 (experimental)").arg(name), name);
    }
    m_branch->setCurrentIndex(qMax(0, m_branch->findData(chosen)));
    m_branch->blockSignals(false);
    updateBranchNote();
}

QString QemuBuildDialog::branch() const
{
    return m_branch->currentData().toString();
}

void QemuBuildDialog::updateBranchNote()
{
    const bool experiment = branch() != QemuBuilder::defaultBranch();

    m_branchNote->setText(
        experiment ? tr("An experiment, not for everyday use yet: see <a href=\"%1\">what it "
                        "changes and needs</a>. Building master again goes back.")
                         .arg(readmeUrl(branch()))
                   : QString());
    m_branchNote->setVisible(experiment);
}

void QemuBuildDialog::updateVirglStatus()
{
    const QString lib =
        QemuBuilder::loadedVirgl(QemuBuilder::binary(QemuBuilder::defaultSourceDir()));

    m_virglStatus->setText(lib.isEmpty() ? QString() : tr("The QEMU built loads %1").arg(lib));
    m_virglStatus->setVisible(!lib.isEmpty());
}

void QemuBuildDialog::build()
{
    QSettings settings(Paths::settingsPath(), QSettings::IniFormat);
    QemuBuilder::Options options;

    settings.setValue("build/branch", branch());
    settings.setValue("build/configure", m_configure->text().simplified());
    settings.setValue("build/virgl", m_virgl->isChecked());

    options.sourceDir = QemuBuilder::defaultSourceDir();
    options.url = QemuBuilder::defaultUrl();
    options.branch = branch();
    options.configureArgs = QProcess::splitCommand(m_configure->text());
    if (m_virgl->isChecked()) {
        options.virgl = QemuBuilder::defaultVirgl();
    }

    m_log->clear();
    m_progress->setValue(0);
    m_progress->setMaximum(0);
    m_progress->setVisible(true);
    m_builder->start(options);
    updateState();
}

void QemuBuildDialog::finished(const QString &error)
{
    m_progress->setVisible(false);
    if (error.isEmpty()) {
        QProcess version;
        version.start(QemuBuilder::binary(QemuBuilder::defaultSourceDir()), {"--version"});
        version.waitForFinished(5000);
        m_step->setText(tr("Built: %1")
                            .arg(QString::fromLocal8Bit(version.readAll()).section('\n', 0, 0)));
        emit built();
    } else {
        m_step->setText(error + tr(", see the log below."));
    }
    updateVirglStatus();
    updateState();
}

void QemuBuildDialog::updateState()
{
    const bool running = m_builder->isRunning();
    const QString binary = QemuBuilder::binary(QemuBuilder::defaultSourceDir());

    m_branch->setEnabled(!running);
    m_preset->setEnabled(!running);
    m_virgl->setEnabled(!running);
    /* editable for custom options only */
    m_configure->setEnabled(!running && m_preset->currentData().isNull());
    m_build->setEnabled(!running);
    m_cancel->setVisible(running);
    m_use->setEnabled(!running && QFileInfo(binary).isExecutable() &&
                      Paths::qemuBinary() != binary);
}

void QemuBuildDialog::closeEvent(QCloseEvent *event)
{
    if (m_builder->isRunning()) {
        if (!Widgets::confirm(this, QMessageBox::Question, tr("Stop the build?"),
                              tr("The build stops where it is; building again goes on "
                                 "from there."),
                              tr("&Stop"))) {
            event->ignore();
            return;
        }
        m_builder->cancel();
    }
    event->accept();
}
