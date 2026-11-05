// SPDX-License-Identifier: GPL-2.0-or-later
#include "hosttuningprefs.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>

#include "core/hostgpus.h"
#include "ui/hosttuningnotifier.h"
#include "ui/hosttuningtext.h"
#include "ui/widgets.h"

/*
 * @widget's window as high as its content wants at its width: the state
 * comes after the window took its size from "Checking…", and a top-level
 * window does not grow by itself for a wrapped text that got longer
 */
static void fitWindow(QWidget *widget)
{
    QWidget *window = widget->window();

    if (!window->isVisible()) {
        /* sized when shown */
        return;
    }
    /* the label's new size, through the layouts between it and the window */
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    if (window->layout()) {
        window->layout()->activate();
    }
    const int need = window->hasHeightForWidth() ? window->heightForWidth(window->width())
                                                 : window->sizeHint().height();
    if (need > window->height()) {
        window->resize(window->width(), need);
    }
}

HostTuningPrefs::HostTuningPrefs(Form *form, QWidget *parent)
    : QObject(parent), m_parent(parent),
      m_tune(new QCheckBox(tr("&Tune the host while VMs run"))), m_state(Widgets::note()), m_setUp(new QPushButton(tr("Set &Up…"))),
      m_floor(new QComboBox), m_mhz(new QSpinBox),
      m_cpuLatency(new QCheckBox(tr("&Keep the CPU ready to wake up while VMs run")))
{
    const QString floor = HostSettings::gpuFloor();
    auto *floorRow = new QHBoxLayout;

    m_tune->setObjectName("tuneHost");
    m_state->setObjectName("tuneHostState");
    m_setUp->setObjectName("tuneHostSetUp");
    m_floor->setObjectName("gpuFloor");
    m_mhz->setObjectName("gpuFloorMhz");
    m_cpuLatency->setObjectName("cpuLatency");
    m_cpuLatency->setChecked(HostSettings::cpuLatency());
    /* the user's choice, whatever the state: the line under it says that */
    m_tune->setChecked(HostSettings::enabled());
    m_setUp->setToolTip(tr("Adds you to the vitrine group: asks for an administrator's password"));
    m_floor->addItem(tr("Automatic"), "auto");
    m_floor->addItem(tr("Off"), "off");
    m_floor->addItem(tr("At least"), "mhz");
    m_floor->setCurrentIndex(floor == "auto" ? 0 : floor == "off" ? 1 : 2);
    m_floor->setToolTip(tr("Automatic: 1800 MHz on AMD APUs, none on other GPUs"));
    m_mhz->setRange(200, 4000);
    m_mhz->setSingleStep(100);
    m_mhz->setSuffix(tr(" MHz"));
    m_mhz->setValue(m_floor->currentIndex() == 2 ? floor.toInt() : 1800);
    floorRow->addWidget(m_floor);
    floorRow->addWidget(m_mhz);
    floorRow->addStretch();

    form->addRow(tr("Host:"), m_tune);
    form->addRow(QString(), m_state);
    form->addRow(QString(), m_setUp);
    form->addRow(QString(), Widgets::hint(HostTuningNotifier::summary()));
    form->addRow(Widgets::label(tr("&GPU clock floor:"), m_floor), floorRow);
    /* the dedicated GPUs, which a floor does nothing for: gpu-input-awake */
    for (const HostGpu &gpu : GpuInputAwake::served(HostGpus::list())) {
        m_dedicated << gpu.name();
    }
    if (!m_dedicated.isEmpty()) {
        auto *awakeRow = new QHBoxLayout;
        m_awake = Widgets::note();
        m_awake->setObjectName("gpuInputAwake");
        m_awakeInstall = new QPushButton(tr("&Install…"));
        m_awakeInstall->setObjectName("gpuInputAwakeInstall");
        m_awakeRemove = new QPushButton(tr("Re&move…"));
        m_awakeRemove->setObjectName("gpuInputAwakeRemove");
        awakeRow->addWidget(m_awakeInstall);
        awakeRow->addWidget(m_awakeRemove);
        awakeRow->addStretch();
        form->addRow(QString(), m_awake);
        form->addRow(QString(), awakeRow);
        connect(m_awakeInstall, &QPushButton::clicked, this, [this]() { runAwake(true); });
        connect(m_awakeRemove, &QPushButton::clicked, this, [this]() { runAwake(false); });
        refreshAwake();
    }
    form->addRow(tr("CPU:"), m_cpuLatency);
    form->addRow(QString(), Widgets::hint(HostTuningPrefsText::cpuHint()));

    connect(m_tune, &QCheckBox::toggled, this, &HostTuningPrefs::update);
    connect(m_floor, &QComboBox::currentIndexChanged, this, &HostTuningPrefs::update);
    connect(m_setUp, &QPushButton::clicked, this, [this]() {
        m_setUp->setEnabled(false);
        HostTuningNotifier::setUp(m_parent, [this]() {
            m_setUp->setEnabled(true);
            refresh();
        });
    });
    update();
}

void HostTuningPrefs::update()
{
    const bool on = m_tune->isChecked();

    /* off: no warning, whatever the state */
    m_state->setVisible(on);
    if (on && !m_checked) {
        refresh();
    }
    m_setUp->setVisible(on && m_needsGroup);
    m_floor->setEnabled(on);
    m_mhz->setEnabled(on);
    m_cpuLatency->setEnabled(on);
    m_mhz->setVisible(m_floor->currentData() == "mhz");
}

void HostTuningPrefs::refresh()
{
    m_checked = true;
    m_state->setText(tr("Checking…"));
    m_needsGroup = false;
    m_setUp->hide();
    /* without interaction: pkcheck and the user database; through the
       app's HostSettings, so that the status bar's warning says the same
       (and VMs that ran untuned get tuned if it is active now) */
    auto show = [this](const HostSettings::Status &status) { showState(status); };
    if (HostSettings *host = HostSettings::instance()) {
        host->recheck(this, show);
    } else {
        HostSettings::check(this, show);
    }
}

void HostTuningPrefs::showState(const HostSettings::Status &status)
{
    if (status.active()) {
        /* what the helper said in this run, else what the kernel shows: no
           real-time threads without the fair server */
        const HostSettings *host = HostSettings::instance();
        const QString realtime = host ? host->realtimeOff() : HostSettings::realtimeLimit();
        m_state->setText(HostTuningPrefsText::active(realtime));
    } else {
        m_state->setText(HostTuningPrefsText::notActive(status));
    }
    m_needsGroup = status.needsGroup();
    m_setUp->setVisible(m_needsGroup && m_tune->isChecked());
    fitWindow(m_state);
}

void HostTuningPrefs::refreshAwake()
{
    if (!m_awake) {
        return;
    }
    m_installed = GpuInputAwake::installed(GpuInputAwake::sourceDir());
    m_awakeInstall->setEnabled(false);
    m_awakeRemove->setEnabled(false);
    if (m_installed == GpuInputAwake::Installed::No) {
        showAwake(QString());
        return;
    }
    GpuInputAwake::serviceState(this, [this](const QString &active, const QString &) {
        showAwake(active);
    });
}

void HostTuningPrefs::showAwake(const QString &active)
{
    using enum GpuInputAwake::Installed;
    const bool source = !GpuInputAwake::sourceDir().isEmpty();
    const bool busy = GpuInputAwake::running();

    m_awake->setText(HostTuningPrefsText::awake(m_dedicated, m_installed, active));
    /* both run install.sh of Vitrine's copy */
    const QString tip = source ? tr("Asks for an administrator's password")
                               : tr("This installation of Vitrine has no host/gpu-input-awake");
    m_awakeInstall->setVisible(m_installed != Vitrine);
    m_awakeInstall->setEnabled(source && !busy);
    m_awakeInstall->setToolTip(tip);
    m_awakeRemove->setVisible(m_installed != No);
    m_awakeRemove->setEnabled(source && !busy);
    m_awakeRemove->setToolTip(tip);
    fitWindow(m_awake);
}

void HostTuningPrefs::runAwake(bool install)
{
    const HostTuningText::Box confirm = HostTuningPrefsText::awakeConfirm(install, m_installed);
    const QString title = confirm.title;
    const QString text = confirm.text;

    if (!Widgets::confirm(m_parent, QMessageBox::Question, title, text,
                          install ? tr("&Install") : tr("&Remove"), QIcon())) {
        return;
    }
    m_awakeInstall->setEnabled(false);
    m_awakeRemove->setEnabled(false);
    /* before: the answer may come at once, and its state goes over this */
    m_awake->setText(HostTuningPrefsText::awakeBusy(install));
    const bool started = GpuInputAwake::run(this, install, [this, title](GpuInputAwake::Result result,
                                                                         const QString &error) {
        if (result == GpuInputAwake::Result::Failed) {
            Widgets::warn(m_parent, title, HostTuningPrefsText::awakeFailed(error));
        }
        refreshAwake();
    });
    if (!started) {
        /* one runs already (another window's) */
        refreshAwake();
    }
}

void HostTuningPrefs::save() const
{
    const QString floor = m_floor->currentData().toString();
    const bool wasEnabled = HostSettings::enabled();
    const QString wasFloor = HostSettings::gpuFloor();
    const bool wasCpuLatency = HostSettings::cpuLatency();

    HostSettings::setEnabled(m_tune->isChecked());
    HostSettings::setGpuFloor(floor == "mhz" ? QString::number(m_mhz->value()) : floor);
    HostSettings::setCpuLatency(m_cpuLatency->isChecked());
    /* for the VMs running now, not only the next ones */
    HostSettings *running = HostSettings::instance();
    if (running && (HostSettings::enabled() != wasEnabled || HostSettings::gpuFloor() != wasFloor ||
                    HostSettings::cpuLatency() != wasCpuLatency)) {
        running->preferencesChanged();
    }
}
