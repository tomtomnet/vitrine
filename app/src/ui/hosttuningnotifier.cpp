// SPDX-License-Identifier: GPL-2.0-or-later
#include "hosttuningnotifier.h"

#include <QEvent>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QToolButton>

#include "ui/icons.h"
#include "ui/widgets.h"

static const char kDocs[] = "https://github.com/tomtomnet/vitrine/blob/main/docs/host-tuning.md";

/* The commands that install the helper where polkit reads its files */
static QString installCommands()
{
    return "<pre>cmake -B build -DCMAKE_INSTALL_PREFIX=/usr\ncmake --build build\n"
           "sudo cmake --install build</pre>";
}

static QString docsLink()
{
    return HostTuningNotifier::tr("See <a href=\"%1\">docs/host-tuning.md</a>.").arg(kDocs);
}

/* "A running VM is not tuned: why." */
static QString untunedText(int count, const QString &why)
{
    return count == 1 ? HostTuningNotifier::tr("A running VM is not tuned: %1.").arg(why)
                      : HostTuningNotifier::tr("%1 running VMs are not tuned: %2.")
                            .arg(QString::number(count), why);
}

HostTuningNotifier::HostTuningNotifier(HostSettings *host, QWidget *window)
    : QObject(window), m_host(host), m_window(window), m_button(new QToolButton)
{
    m_button->setObjectName("hostTuning");
    m_button->setAutoRaise(true);
    m_button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_button->setIcon(Icons::themed({"dialog-warning"}, QStyle::SP_MessageBoxWarning));
    /* tuning is on (the user's choice): it does not take effect */
    m_button->setText(tr("Host tuning inactive"));
    m_button->hide();
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
    return tr("Real-time QEMU threads, a shorter kernel fair-server period, a GPU clock floor "
              "and, for native-context VMs, higher udmabuf limits while VMs run, put back after "
              "the last one.");
}

QString HostTuningNotifier::fix(const HostSettings::Status &status)
{
    using Problem = HostSettings::Problem;

    switch (status.problem) {
    case Problem::None:
        return QString();
    case Problem::NotInstalled:
        return tr("Install it with Vitrine, from Vitrine's source folder, configured for the "
                  "prefix <nobr>/usr</nobr>:") + installCommands() +
               tr("Then, unless you are in the vitrine group, Vitrine offers to set it up "
                  "(an administrator's password, once).") + ' ' + docsLink();
    case Problem::NoPolicy:
        return tr("polkit reads actions from /usr/share/polkit-1 only: configure Vitrine for "
                  "the prefix <nobr>/usr</nobr> and install it again, from its source folder:") +
               installCommands() + docsLink();
    case Problem::NoPolkit:
        return tr("Install polkit.");
    case Problem::NoGroup:
        return tr("Members of the vitrine group may tune the host without a password. Set Up "
                  "creates the group and adds you to it: it asks for an administrator's "
                  "password once.");
    case Problem::NotMember:
        return tr("Members of the vitrine group may tune the host without a password. Set Up "
                  "adds you to it: it asks for an administrator's password once.");
    case Problem::NotLocal:
        return tr("Start Vitrine from the desktop in front of you, not over ssh or a remote "
                  "desktop. The group's rule, 49-vitrine.rules, must be in "
                  "/usr/share/polkit-1/rules.d or /etc/polkit-1/rules.d.") + ' ' + docsLink();
    case Problem::Failed:
        break;
    }
    return docsLink();
}

void HostTuningNotifier::update()
{
    const bool show = m_host->untuned();

    if (show) {
        m_button->setToolTip(untunedText(m_host->untunedCount(), m_host->untunedStatus().why()) +
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
                auto *box = Widgets::messageBox(
                    QMessageBox::Information, tr("Host Tuning Active"),
                    tr("Vitrine may tune the host now: it tunes the running VMs."),
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
    auto *box = Widgets::messageBox(
        QMessageBox::Warning, tr("Host Tuning Inactive"),
        "<p>" + untunedText(m_host->untunedCount(), status.why().toHtmlEscaped()) + "</p><p>" +
            summary() + "</p><p>" + fix(status) + "</p>",
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
    const QString group =
        status.problem == HostSettings::Problem::NoGroup
            ? tr("There is no vitrine group yet: Set Up creates it and adds you to it.")
            : tr("You are not in it: Set Up adds you to it.");
    auto *box = Widgets::messageBox(
        QMessageBox::Question, tr("Tune the Host for VMs?"),
        "<p>" + tr("Vitrine can tune the host while VMs run: real-time QEMU threads, a shorter "
                   "kernel fair-server period, a GPU clock floor and higher udmabuf limits, "
                   "without which native-context VMs copy the windows of Qt Widgets and GTK "
                   "apps. All of it is put back after the last VM.") + "</p><p>" +
            tr("vitrine-helper does it as root, without a password for members of the vitrine "
               "group.") + ' ' + group + ' ' +
            tr("It asks for an administrator's password once.") + "</p><p>" +
            tr("Until then, the VMs run untuned.") + "</p>",
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
        case HostSettings::Setup::Failed:
            box = Widgets::messageBox(QMessageBox::Warning, tr("Host Tuning Not Set Up"),
                                      tr("The vitrine group could not be set up: %1.").arg(error),
                                      QMessageBox::Close, guard);
            break;
        case HostSettings::Setup::Done:
            if (now.active()) {
                box = Widgets::messageBox(
                    QMessageBox::Information, tr("Host Tuning On"),
                    tr("You are in the vitrine group: Vitrine tunes the host while VMs run, "
                       "from now on and for those running now."),
                    QMessageBox::Close, guard);
            } else if (now.needsGroup()) {
                /* polkit and vitrine read the user database: a cache in the
                   way (nscd, sssd) lets go at the next login */
                box = Widgets::messageBox(
                    QMessageBox::Information, tr("Log Out and Back In"),
                    tr("You are in the vitrine group now, but the system does not show it yet. "
                       "Log out and back in, then start the VMs again."),
                    QMessageBox::Close, guard);
            } else {
                box = Widgets::messageBox(
                    QMessageBox::Warning, tr("Host Tuning Still Inactive"),
                    "<p>" + tr("You are in the vitrine group now, but host tuning is still "
                               "inactive: %1.").arg(now.why().toHtmlEscaped()) +
                        "</p><p>" + fix(now) + "</p>",
                    QMessageBox::Close, guard);
            }
            break;
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
