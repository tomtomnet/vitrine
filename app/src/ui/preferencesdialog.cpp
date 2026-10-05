// SPDX-License-Identifier: GPL-2.0-or-later
#include "preferencesdialog.h"

#include <QCheckBox>
#include <QSettings>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "core/paths.h"
#include "core/stackbuilder.h"
#include "ui/hosttuningprefs.h"
#include "ui/icons.h"
#include "ui/qemubuilddialog.h"
#include "ui/qemudocs.h"
#include "ui/widgets.h"

static QString autoVirtiofsd()
{
    if (QFileInfo("/usr/libexec/virtiofsd").isExecutable()) {
        return "/usr/libexec/virtiofsd";
    }
    return QStandardPaths::findExecutable("virtiofsd");
}

PreferencesDialog::PreferencesDialog(QWidget *parent)
    : QDialog(parent), m_stack(Widgets::note()), m_stackState(Widgets::hint()),
      m_build(new QPushButton),
      m_custom(new QCheckBox(tr("Use &another QEMU (advanced):"))), m_qemu(new QLineEdit),
      m_qemuStatus(Widgets::hint()), m_virtiofsd(new QLineEdit),
      m_virtiofsdStatus(Widgets::hint()),
      m_updates(new QCheckBox(tr("Check GitHub for &updates once a day"))),
      m_vmsDir(Paths::vmsDir()), m_vmsDirLabel(new QLabel), m_vmsDirState(Widgets::hint()),
      m_vmsDirDefault(new QPushButton(tr("&Default"))), m_timer(new QTimer(this))
{
    auto *layout = new QVBoxLayout(this);
    auto *form = Widgets::form();
    auto *stackRow = new QHBoxLayout;
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto *vmsRow = new QHBoxLayout;
    auto *vmsChange = new QPushButton(tr("C&hange…"));
    const QString custom = Paths::customQemuBinary();
    const QString virtiofsd = Paths::virtiofsd();

    setWindowTitle(tr("Preferences"));
    m_build->setObjectName("buildQemu");
    m_custom->setObjectName("customQemu");
    m_qemu->setObjectName("qemu");
    m_virtiofsd->setObjectName("virtiofsd");
    m_qemu->setPlaceholderText(tr("Path of qemu-system-x86_64"));
    m_virtiofsd->setPlaceholderText(autoVirtiofsd().isEmpty() ? tr("virtiofsd from PATH")
                                                              : autoVirtiofsd());
    /* VMs with a #qemu line of their own keep it, whatever is chosen here */
    m_custom->setToolTip(tr("For the VMs without a QEMU of their own; Vitrine's QEMU has what "
                            "its VMs need, another one may not."));
    m_custom->setChecked(!custom.isEmpty());
    m_qemu->setText(custom);
    /* empty when automatic */
    m_virtiofsd->setText(virtiofsd == autoVirtiofsd() ? QString() : virtiofsd);
    m_vmsDirLabel->setObjectName("vmsDir");
    m_vmsDirLabel->setOpenExternalLinks(true);
    m_vmsDirLabel->setWordWrap(true);
    vmsChange->setObjectName("vmsDirChange");
    m_vmsDirDefault->setObjectName("vmsDirDefault");
    vmsChange->setToolTip(tr("A folder holding VMs, each a folder with its vm.args: Vitrine "
                             "lists the VMs it finds there, and makes new ones there"));
    m_updates->setObjectName("checkUpdates");
    m_updates->setChecked(QSettings(Paths::settingsPath(), QSettings::IniFormat)
                              .value("updates/check", true).toBool());

    stackRow->addWidget(m_stack, 1);
    stackRow->addWidget(m_build, 0, Qt::AlignTop);
    form->addRow(tr("QEMU:"), stackRow);
    form->addRow(QString(), m_stackState);
    m_qemuRow = Widgets::browseRow(m_qemu, tr("QEMU Binary"));
    form->addRow(QString(), m_custom);
    form->addRow(QString(), m_qemuRow);
    form->addRow(QString(), m_qemuStatus);
    form->addRow(Widgets::label(tr("&virtiofsd:"), m_virtiofsd),
                 Widgets::browseRow(m_virtiofsd, tr("virtiofsd Binary")));
    form->addRow(QString(), m_virtiofsdStatus);
    vmsRow->addWidget(m_vmsDirLabel, 1);
    vmsRow->addWidget(vmsChange, 0, Qt::AlignTop);
    vmsRow->addWidget(m_vmsDirDefault, 0, Qt::AlignTop);
    form->addRow(tr("Virtual machines:"), vmsRow);
    form->addRow(QString(), m_vmsDirState);
    form->addRow(QString(), m_updates);
    form->addRow(QString(), Widgets::hint(tr("Of Vitrine itself: one request to GitHub.")));
    /* the host's settings while VMs run: HostSettings' entries */
    connect(this, &QDialog::accepted, new HostTuningPrefs(form, this), &HostTuningPrefs::save);
    layout->addLayout(form);
    layout->addStretch();
    layout->addWidget(buttons);

    m_timer->setSingleShot(true);
    m_timer->setInterval(400);
    connect(m_timer, &QTimer::timeout, this, &PreferencesDialog::checkQemu);
    connect(m_qemu, &QLineEdit::textChanged, m_timer, qOverload<>(&QTimer::start));
    connect(m_custom, &QCheckBox::toggled, this, [this](bool on) {
        m_qemuRow->setEnabled(on);
        checkQemu();
        if (on) {
            m_qemu->setFocus();
        }
    });
    connect(m_virtiofsd, &QLineEdit::textChanged, this, &PreferencesDialog::checkVirtiofsd);
    connect(m_build, &QPushButton::clicked, this, [this]() {
        /* the build goes on in the background when this window closes */
        QemuBuildDialog dialog(this);
        dialog.exec();
        updateStack();
    });
    connect(StackBuilder::instance(), &StackBuilder::finished, this,
            &PreferencesDialog::updateStack);
    connect(vmsChange, &QPushButton::clicked, this, [this]() {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Folder of the VMs"),
                                                              m_vmsDir);
        if (!dir.isEmpty()) {
            m_vmsDir = QDir(dir).absolutePath();
            updateVmsDir();
        }
    });
    connect(m_vmsDirDefault, &QPushButton::clicked, this, [this]() {
        m_vmsDir = Paths::defaultVmsDir();
        updateVmsDir();
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    m_qemuRow->setEnabled(m_custom->isChecked());
    updateStack();
    checkQemu();
    checkVirtiofsd();
    updateVmsDir();
    Widgets::resizeToWidth(this, 640);
}

void PreferencesDialog::updateStack()
{
    const StackBuilder::State state = StackBuilder::state();
    const StackBuilder::Build build = StackBuilder::current();
    const bool running = StackBuilder::instance()->isRunning();

    m_stack->setText(QemuBuildDialog::describe(build).toHtmlEscaped());
    m_stack->setToolTip(build.isValid() ? build.qemuBinary() : QString());
    m_stackState->setText(running ? tr("Building…")
                                  : QemuBuildDialog::explain(state, build).toHtmlEscaped());
    m_build->setText(running                                    ? tr("Show…")
                     : state == StackBuilder::State::Outdated  ? tr("Update…")
                     : state == StackBuilder::State::UpToDate ? tr("Details…")
                                                               : tr("Build…"));
    m_build->setEnabled(state != StackBuilder::State::NoSources || running);
}

void PreferencesDialog::checkQemu()
{
    const QString binary = m_qemu->text().trimmed();

    if (m_version) {
        m_version->disconnect(this);
        m_version->kill();
        m_version->deleteLater();
        m_version = nullptr;
    }
    m_qemuStatus->setVisible(m_custom->isChecked());
    if (!m_custom->isChecked()) {
        return;
    }
    if (binary.isEmpty()) {
        m_qemuStatus->setText(tr("Choose the QEMU binary, for example one you built."));
        return;
    }
    if (!QFileInfo(binary).isExecutable()) {
        m_qemuStatus->setText(tr("This file is not an executable."));
        return;
    }

    m_qemuStatus->setText(tr("Checking…"));
    m_version = new QProcess(this);
    m_version->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_version, &QProcess::finished, this, [this]() {
        const QString first = QString::fromLocal8Bit(m_version->readAll()).section('\n', 0, 0);
        m_qemuStatus->setText(first.contains("version") ? first.trimmed().toHtmlEscaped()
                                                        : tr("This does not look like QEMU."));
        m_version->deleteLater();
        m_version = nullptr;
    });
    connect(m_version, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_qemuStatus->setText(tr("Cannot run it: %1").arg(m_version->errorString()));
            m_version->deleteLater();
            m_version = nullptr;
        }
    });
    m_version->start(binary, {"-version"});
}

void PreferencesDialog::checkVirtiofsd()
{
    const QString binary = m_virtiofsd->text().trimmed().isEmpty()
                               ? autoVirtiofsd()
                               : m_virtiofsd->text().trimmed();

    if (binary.isEmpty()) {
        m_virtiofsdStatus->setText(
            tr("Not found: shared folders need it. Install it with "
               "<code>sudo dnf install virtiofsd</code>."));
    } else if (!QFileInfo(binary).isExecutable()) {
        m_virtiofsdStatus->setText(tr("This file is not an executable."));
    } else {
        m_virtiofsdStatus->setText(tr("Shared folders use %1.").arg(binary.toHtmlEscaped()));
    }
}

void PreferencesDialog::accept()
{
    QSettings(Paths::settingsPath(), QSettings::IniFormat)
        .setValue("updates/check", m_updates->isChecked());
    Paths::setQemuBinary(m_custom->isChecked() ? m_qemu->text().trimmed() : QString());
    Paths::setVirtiofsd(m_virtiofsd->text().trimmed());
    Paths::setVmsDir(m_vmsDir);
    QemuDocs::reloadPreferred();
    QDialog::accept();
}

void PreferencesDialog::updateVmsDir()
{
    const QDir dir(m_vmsDir);
    int count = 0;

    m_vmsDirLabel->setText(QString("<a href=\"%1\">%2</a>")
                               .arg(QUrl::fromLocalFile(m_vmsDir).toString(),
                                    QDir::toNativeSeparators(m_vmsDir).toHtmlEscaped()));
    m_vmsDirDefault->setEnabled(QDir(Paths::defaultVmsDir()).absolutePath() !=
                                dir.absolutePath());
    for (const QString &id : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (QFileInfo::exists(dir.filePath(id) + "/vm.args")) {
            count++;
        }
    }
    if (m_vmsDir == Paths::vmsDir()) {
        m_vmsDirState->clear();
        m_vmsDirState->hide();
        return;
    }
    if (!QFileInfo(m_vmsDir).isWritable() && dir.exists()) {
        m_vmsDirState->setText(tr("%n VM(s) found there. The folder is read-only: new VMs "
                                  "cannot be made there, and VMs may not start.", nullptr,
                                  count));
    } else {
        m_vmsDirState->setText(tr("%n VM(s) found there. The VMs of the folder used until "
                                  "now stay where they are; running ones stay listed until "
                                  "they stop.", nullptr, count));
    }
    m_vmsDirState->show();
}
