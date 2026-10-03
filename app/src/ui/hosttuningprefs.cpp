// SPDX-License-Identifier: GPL-2.0-or-later
#include "hosttuningprefs.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QSpinBox>

#include "ui/hosttuningnotifier.h"
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
      m_floor(new QComboBox), m_mhz(new QSpinBox)
{
    const QString floor = HostSettings::gpuFloor();
    auto *floorRow = new QHBoxLayout;

    m_tune->setObjectName("tuneHost");
    m_state->setObjectName("tuneHostState");
    m_setUp->setObjectName("tuneHostSetUp");
    m_floor->setObjectName("gpuFloor");
    m_mhz->setObjectName("gpuFloorMhz");
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
    m_mhz->setVisible(m_floor->currentData() == "mhz");
}

void HostTuningPrefs::refresh()
{
    m_checked = true;
    m_state->setText(tr("Checking…"));
    m_needsGroup = false;
    m_setUp->hide();
    /* without interaction: pkcheck and the user database */
    HostSettings::check(this, [this](const HostSettings::Status &status) { showState(status); });
}

void HostTuningPrefs::showState(const HostSettings::Status &status)
{
    if (status.active()) {
        m_state->setText(tr("Active: vitrine-helper may tune the host without a password."));
    } else {
        m_state->setText("<b>" + tr("Not active:") + "</b> " +
                         tr("%1.").arg(status.why().toHtmlEscaped()) + ' ' +
                         HostTuningNotifier::fix(status));
    }
    m_needsGroup = status.needsGroup();
    m_setUp->setVisible(m_needsGroup && m_tune->isChecked());
    fitWindow(m_state);
}

void HostTuningPrefs::save() const
{
    const QString floor = m_floor->currentData().toString();
    const bool wasEnabled = HostSettings::enabled();
    const QString wasFloor = HostSettings::gpuFloor();

    HostSettings::setEnabled(m_tune->isChecked());
    HostSettings::setGpuFloor(floor == "mhz" ? QString::number(m_mhz->value()) : floor);
    /* for the VMs running now, not only the next ones */
    HostSettings *running = HostSettings::instance();
    if (running && (HostSettings::enabled() != wasEnabled || HostSettings::gpuFloor() != wasFloor)) {
        running->preferencesChanged();
    }
}
