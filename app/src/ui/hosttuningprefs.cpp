// SPDX-License-Identifier: GPL-2.0-or-later
#include "hosttuningprefs.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>

#include <grp.h>

#include "core/hostsettings.h"
#include "ui/widgets.h"

HostTuningPrefs::HostTuningPrefs(Form *form, QWidget *parent)
    : QObject(parent), m_tune(new QCheckBox(tr("&Tune the host while VMs run"))),
      m_floor(new QComboBox), m_mhz(new QSpinBox), m_status(Widgets::hint())
{
    const QString floor = HostSettings::gpuFloor();
    const bool installed = HostSettings::helperInstalled();
    auto *floorRow = new QHBoxLayout;

    m_tune->setObjectName("tuneHost");
    m_floor->setObjectName("gpuFloor");
    m_mhz->setObjectName("gpuFloorMhz");
    /* on when the helper is there to do it */
    m_tune->setChecked(installed && HostSettings::enabled());
    m_tune->setEnabled(installed);
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
    form->addRow(QString(), m_status);
    form->addRow(Widgets::label(tr("&GPU clock floor:"), m_floor), floorRow);

    connect(m_tune, &QCheckBox::toggled, this, &HostTuningPrefs::update);
    connect(m_floor, &QComboBox::currentIndexChanged, this, &HostTuningPrefs::update);
    update();
}

void HostTuningPrefs::update()
{
    QString text = tr("Real-time QEMU threads, a shorter kernel fair-server period and a GPU "
                      "clock floor while VMs run, put back after the last one.");

    if (!HostSettings::helperInstalled()) {
        text += ' ' + tr("This needs vitrine-helper, which is not installed.");
    } else if (!HostSettings::inVitrineGroup()) {
        /* polkit asks no password of the group's members only */
        text += ' ' + tr("This needs membership of the vitrine group:") + "<br><code>" +
                (getgrnam("vitrine") ? QString() : "sudo groupadd --system vitrine; ") +
                "sudo usermod -aG vitrine $USER</code>";
    }
    m_status->setText(text);
    m_floor->setEnabled(m_tune->isChecked());
    m_mhz->setEnabled(m_tune->isChecked());
    m_mhz->setVisible(m_floor->currentData() == "mhz");
}

void HostTuningPrefs::save() const
{
    const QString floor = m_floor->currentData().toString();
    const bool wasEnabled = HostSettings::enabled();
    const QString wasFloor = HostSettings::gpuFloor();

    if (m_tune->isEnabled()) {
        HostSettings::setEnabled(m_tune->isChecked());
    }
    HostSettings::setGpuFloor(floor == "mhz" ? QString::number(m_mhz->value()) : floor);
    /* for the VMs running now, not only the next ones */
    HostSettings *running = HostSettings::instance();
    if (running && (HostSettings::enabled() != wasEnabled || HostSettings::gpuFloor() != wasFloor)) {
        running->preferencesChanged();
    }
}
