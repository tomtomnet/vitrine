// SPDX-License-Identifier: GPL-2.0-or-later
#include "updatenotifier.h"

#include <QDateTime>
#include <QVBoxLayout>
#include <QLabel>
#include <QDialogButtonBox>
#include <QDialog>
#include <QJsonDocument>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include <QToolButton>

#include "core/paths.h"
#include "core/qemubuilder.h"
#include "ui/icons.h"
#include "ui/widgets.h"

/* configured by CMake; not in the builds of the tests */
#if __has_include("buildinfo.h")
#include "buildinfo.h"
#else
#define QGM_COMMIT ""
#define QGM_SOURCE_DIR ""
#endif

static const qint64 kDaySecs = 24 * 3600;
/* Help > Check for Updates again soon after: the answer of a moment ago */
static const qint64 kAgainSecs = 5 * 60;

/* "1 new commit", "12 new commits": English has no translation file to pick */
static QString newCommits(int count)
{
    return count == 1 ? UpdateNotifier::tr("1 new commit")
                      : UpdateNotifier::tr("%1 new commits").arg(count);
}

/* "qemu-gui (zero-copy)" when built from an experiment of the fork */
static QString shownName(const UpdateCheck::Project &p)
{
    return p.name == "qemu-gui" && !p.branch.isEmpty() && p.branch != QemuBuilder::defaultBranch()
               ? QString("%1 (%2)").arg(p.name, p.branch)
               : p.name;
}

static QSettings settings()
{
    return QSettings(Paths::settingsPath(), QSettings::IniFormat);
}

UpdateNotifier::UpdateNotifier(QWidget *window)
    : QObject(window), m_window(window), m_check(new UpdateCheck(this)),
      m_button(new QToolButton), m_daily(new QTimer(this))
{
    const QList<UpdateCheck::Project> running = projects();
    const QSettings s = settings();

    m_button->setObjectName("updates");
    m_button->setAutoRaise(true);
    m_button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_button->setIcon(Icons::themed({"update-high", "system-software-update"},
                                    QStyle::SP_BrowserReload));
    m_button->setText(tr("Updates"));
    m_button->hide();
    connect(m_button, &QToolButton::clicked, this, &UpdateNotifier::showResults);
    connect(m_check, &UpdateCheck::finished, this, &UpdateNotifier::finished);

    /* the last answer, for what still runs: nothing asked of GitHub at start */
    for (const UpdateCheck::Result &r : UpdateCheck::fromJson(
             QJsonDocument::fromJson(s.value("updates/results").toByteArray()).array())) {
        for (const UpdateCheck::Project &p : running) {
            if (p.name == r.project.name && p.commit == r.project.commit) {
                m_results << r;
            }
        }
    }
    updateButton();

    /* a while after the start, then every hour: at most once a day, see maybeCheck() */
    m_daily->setInterval(3600 * 1000);
    connect(m_daily, &QTimer::timeout, this, &UpdateNotifier::maybeCheck);
    m_daily->start();
    QTimer::singleShot(15000, this, &UpdateNotifier::maybeCheck);
}

QList<UpdateCheck::Project> UpdateNotifier::projects()
{
    static const QRegularExpression sha("^[0-9a-f]{40}$");
    const QString source = QemuBuilder::defaultSourceDir();
    /* the commit last built, else the one checked out (before the stamp) */
    const QString built = QemuBuilder::builtCommit(source);
    const QString qemu = built.isEmpty() ? UpdateCheck::checkoutCommit(source) : built;
    QList<UpdateCheck::Project> list;

    /* built from a git checkout */
    if (sha.match(QGM_COMMIT).hasMatch()) {
        list << UpdateCheck::Project{"qemu-gui-manager", "tomtomnet/qemu-gui-manager", "main",
                                     QGM_COMMIT};
    }
    /* File > Build QEMU built it, from its branch */
    if (!qemu.isEmpty()) {
        const QString branch = QemuBuilder::builtBranch(source);
        list << UpdateCheck::Project{"qemu-gui", "tomtomnet/qemu-gui",
                                     branch.isEmpty() ? QemuBuilder::defaultBranch() : branch,
                                     qemu};
    }
    return list;
}

void UpdateNotifier::revalidate()
{
    const QList<UpdateCheck::Project> running = projects();
    QList<UpdateCheck::Result> kept;

    for (UpdateCheck::Result r : std::as_const(m_results)) {
        for (const UpdateCheck::Project &p : running) {
            if (p.name != r.project.name) {
                continue;
            }
            if (p.commit != r.project.commit && p.commit == r.head) {
                /* built the newest one GitHub told of */
                r.project.commit = p.commit;
                r.newCommits = 0;
                r.subjects.clear();
            }
            /* another commit, unknown until the next check */
            if (p.commit == r.project.commit) {
                kept << r;
            }
        }
    }
    m_results = kept;
    save();
    updateButton();
}

void UpdateNotifier::save()
{
    settings().setValue("updates/results",
                        QJsonDocument(UpdateCheck::toJson(m_results)).toJson(QJsonDocument::Compact));
}

void UpdateNotifier::maybeCheck()
{
    const QSettings s = settings();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QDateTime last = s.value("updates/last").toDateTime();
    const QDateTime notBefore = s.value("updates/notBefore").toDateTime();

    if (!s.value("updates/check", true).toBool() || m_check->isChecking()) {
        return;
    }
    /* a day since the last one (a clock put back does not stop it), and as GitHub asked */
    if ((last.isValid() && last <= now && last.secsTo(now) < kDaySecs) ||
        (notBefore.isValid() && now < notBefore)) {
        return;
    }
    start(false);
}

void UpdateNotifier::checkNow()
{
    const QSettings s = settings();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QDateTime last = s.value("updates/last").toDateTime();
    const QDateTime notBefore = s.value("updates/notBefore").toDateTime();

    if (m_check->isChecking()) {
        m_asked = true;
        return;
    }
    if (last.isValid() && last <= now && last.secsTo(now) < kAgainSecs && !m_results.isEmpty()) {
        showResults();
        return;
    }
    if (notBefore.isValid() && now < notBefore) {
        Widgets::inform(m_window, tr("Updates"),
                        tr("GitHub asked to wait before it is asked again, until %1.")
                            .arg(QLocale().toString(notBefore.toLocalTime().time(),
                                                    QLocale::ShortFormat)));
        return;
    }
    start(true);
}

void UpdateNotifier::start(bool asked)
{
    const QList<UpdateCheck::Project> list = projects();
    QSettings s = settings();

    /* now, whatever the answer: a failure too waits a day */
    s.setValue("updates/last", QDateTime::currentDateTimeUtc());
    m_asked = asked;
    if (list.isEmpty()) {
        if (asked) {
            Widgets::inform(
                m_window, tr("Updates"),
                tr("There is nothing to check: this qemu-gui-manager was not built from a git "
                   "checkout, and File > Build QEMU has not built a QEMU yet."));
        }
        return;
    }
    m_check->check(list);
}

void UpdateNotifier::finished(const QList<UpdateCheck::Result> &results)
{
    QSettings s = settings();

    m_results = results;
    save();
    for (const UpdateCheck::Result &r : results) {
        if (r.limitedUntil.isValid()) {
            s.setValue("updates/notBefore", r.limitedUntil);
        }
    }
    updateButton();
    if (m_asked) {
        m_asked = false;
        showResults();
    }
}

void UpdateNotifier::updateButton()
{
    QStringList news;

    for (const UpdateCheck::Result &r : std::as_const(m_results)) {
        if (r.newCommits > 0) {
            news << shownName(r.project) + ": " + newCommits(r.newCommits);
        }
    }
    m_button->setToolTip(news.join('\n'));
    m_button->setVisible(!news.isEmpty());
}

void UpdateNotifier::showResults()
{
    revalidate();

    QDialog dialog(m_window);
    auto *layout = new QVBoxLayout(&dialog);
    auto *label = new QLabel;
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    QPushButton *build = nullptr;
    QString text;

    for (const UpdateCheck::Result &r : std::as_const(m_results)) {
        const QString name = "<b>" + shownName(r.project).toHtmlEscaped() + "</b>";
        if (r.newCommits < 0) {
            text += "<p>" + tr("%1: could not check (%2).").arg(name, r.error.toHtmlEscaped()) +
                    "</p>";
            continue;
        }
        if (r.newCommits == 0) {
            text += "<p>" + tr("%1 is up to date.").arg(name) + "</p>";
            continue;
        }
        text += "<p>" + tr("%1 has %2, the latest:").arg(name, newCommits(r.newCommits)) +
                "</p><ul>";
        for (const QString &subject : r.subjects) {
            text += "<li>" + subject.toHtmlEscaped() + "</li>";
        }
        text += "</ul><p>";
        if (r.project.name == "qemu-gui") {
            text += tr("File > Build QEMU downloads and builds them.");
            if (!build) {
                build = buttons->addButton(tr("&Build QEMU…"), QDialogButtonBox::AcceptRole);
            }
        } else {
            text += tr("To update it: <code>git pull</code> in %1, then build and install it "
                       "as the first time.")
                        .arg(QString(QGM_SOURCE_DIR).isEmpty()
                                 ? tr("its folder")
                                 : "<code>" + QString(QGM_SOURCE_DIR).toHtmlEscaped() +
                                       "</code>");
        }
        if (!r.url.isEmpty()) {
            text += QString(" <a href=\"%1\">%2</a>").arg(r.url.toHtmlEscaped(),
                                                           tr("See the changes"));
        }
        text += "</p>";
    }

    dialog.setWindowTitle(tr("Updates"));
    label->setObjectName("updates");
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setOpenExternalLinks(true);
    label->setTextInteractionFlags(Qt::TextBrowserInteraction);
    label->setText(text.isEmpty() ? tr("Nothing checked yet.") : text);
    /* a width to read, not a column */
    label->setMinimumWidth(Widgets::em(label) * 26);
    layout->addWidget(label);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted && build) {
        emit buildQemuRequested();
    }
}
