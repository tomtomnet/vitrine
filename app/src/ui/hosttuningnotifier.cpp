// SPDX-License-Identifier: GPL-2.0-or-later
#include "hosttuningnotifier.h"

#include <QEvent>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QToolButton>

#include "ui/hosttuningtext.h"
#include "ui/icons.h"
#include "ui/widgets.h"

HostTuningNotifier::HostTuningNotifier(HostSettings *host, QWidget *window)
    : QObject(window), m_host(host), m_window(window),
      /* tuning is on (the user's choice): it does not take effect */
      m_button(Widgets::statusButton(
          "hostTuning", Icons::themed({"dialog-warning"}, QStyle::SP_MessageBoxWarning),
          tr("Host tuning inactive")))
{
    connect(m_button, &QToolButton::clicked, this, &HostTuningNotifier::explain);
    connect(host, &HostSettings::untunedChanged, this, &HostTuningNotifier::update);
    connect(host, &HostSettings::groupSetupSuggested, this, &HostTuningNotifier::ask);
    /* back from a terminal where the helper was installed, say */
    window->installEventFilter(this);
    update();
}

bool HostTuningNotifier::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_window && event->type() == QEvent::WindowActivate && m_host->untuned()) {
        m_host->recheck();
    }
    return QObject::eventFilter(watched, event);
}

QString HostTuningNotifier::summary()
{
    return HostTuningText::summary();
}

QString HostTuningNotifier::fix(const HostSettings::Status &status)
{
    return HostTuningText::fix(status);
}

void HostTuningNotifier::update()
{
    const bool show = m_host->untuned();

    if (show) {
        m_button->setToolTip(HostTuningText::untuned(m_host->untunedCount(),
                                                     m_host->untunedStatus().why()) +
                             '\n' + tr("Click for what to do."));
    }
    m_button->setVisible(show);
}

void HostTuningNotifier::explain()
{
    if (!m_host->untuned()) {
        m_button->hide();
        return;
    }
    /* the state now, not the one of the VM start: the helper installed or
       the group joined since */
    m_button->setEnabled(false);
    m_host->recheck(this, [this](const HostSettings::Status &now) {
        m_button->setEnabled(true);
        if (now.active() || !m_host->untuned()) {
            /* the running VMs are being tuned: the warning goes with that */
            if (m_host->untuned()) {
                const HostTuningText::Box active = HostTuningText::active();
                auto *box = Widgets::messageBox(QMessageBox::Information, active.title, active.text,
                                                QMessageBox::Close, m_window);
                box->setAttribute(Qt::WA_DeleteOnClose);
                box->open();
            }
            return;
        }
        explainNow(m_host->untunedStatus());
    });
}

void HostTuningNotifier::explainNow(const HostSettings::Status &status)
{
    const HostTuningText::Box inactive = HostTuningText::inactive(m_host->untunedCount(), status);
    auto *box = Widgets::messageBox(QMessageBox::Warning, inactive.title, inactive.text,
                                    QMessageBox::Close, m_window);
    QPushButton *setUp = status.needsGroup() ? box->addButton(tr("&Set Up…"), QMessageBox::AcceptRole)
                                             : nullptr;
    QPushButton *off = box->addButton(tr("&Turn Off Tuning"), QMessageBox::DestructiveRole);
    off->setToolTip(tr("Preferences > Tune the host while VMs run"));
    /* opened by a click: Set Up may be the default; without it Close, not
       the first button, which would be Turn Off Tuning */
    if (setUp) {
        box->setDefaultButton(setUp);
    } else {
        box->setDefaultButton(QMessageBox::Close);
    }
    box->setAttribute(Qt::WA_DeleteOnClose);
    connect(box, &QMessageBox::finished, this, [this, box, setUp, off]() {
        if (setUp && box->clickedButton() == setUp) {
            HostTuningNotifier::setUp(m_window);
        } else if (box->clickedButton() == off) {
            turnOff();
        }
    });
    box->open();
}

void HostTuningNotifier::ask(const HostSettings::Status &status)
{
    const HostTuningText::Box offer = HostTuningText::offer(status);
    auto *box = Widgets::messageBox(QMessageBox::Question, offer.title, offer.text,
                                    QMessageBox::NoButton, m_window);
    QPushButton *setUp = box->addButton(tr("&Set Up…"), QMessageBox::AcceptRole);
    QPushButton *notNow = box->addButton(tr("&Not Now"), QMessageBox::RejectRole);
    QPushButton *off = box->addButton(tr("&Turn Off Tuning"), QMessageBox::DestructiveRole);
    off->setToolTip(tr("Preferences > Tune the host while VMs run"));
    /* it comes up by itself, at a VM start: an Enter meant for the guest
       (its boot menu, its login) must not start the password dialog */
    box->setDefaultButton(notNow);
    box->setEscapeButton(notNow);
    box->setAttribute(Qt::WA_DeleteOnClose);
    connect(box, &QMessageBox::finished, this, [this, box, setUp, off]() {
        if (box->clickedButton() == setUp) {
            HostTuningNotifier::setUp(m_window);
        } else if (box->clickedButton() == off) {
            turnOff();
        }
        /* Not Now: not asked again in this run (HostSettings), the warning stays */
    });
    box->open();
}

void HostTuningNotifier::setUp(QWidget *parent, const std::function<void()> &done)
{
    HostSettings *host = HostSettings::instance();
    QPointer<QWidget> guard(parent);

    if (!host) {
        if (done) {
            done();
        }
        return;
    }
    host->setUpGroup([guard, done](HostSettings::Setup result, const QString &error,
                                   const HostSettings::Status &now) {
        QMessageBox *box = nullptr;

        switch (result) {
        case HostSettings::Setup::Cancelled:
            break;
        case HostSettings::Setup::Failed: {
            const HostTuningText::Box failed = HostTuningText::setUpFailed(error);
            box = Widgets::messageBox(QMessageBox::Warning, failed.title, failed.text,
                                      QMessageBox::Close, guard);
            break;
        }
        case HostSettings::Setup::Done: {
            const HostTuningText::Box done = HostTuningText::setUpDone(now);
            /* not active, and not for want of the group: still inactive, a warning */
            box = Widgets::messageBox(now.active() || now.needsGroup() ? QMessageBox::Information
                                                                       : QMessageBox::Warning,
                                      done.title, done.text, QMessageBox::Close, guard);
            break;
        }
        }
        if (box) {
            box->setAttribute(Qt::WA_DeleteOnClose);
            box->open();
        }
        if (guard && done) {
            done();
        }
    });
}

void HostTuningNotifier::turnOff()
{
    HostSettings::setEnabled(false);
    if (HostSettings *host = HostSettings::instance()) {
        host->preferencesChanged();
    }
}
