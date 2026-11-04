// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QWidget>

class Banner;

/*
 * Where a VM's guest tools are offered (Install Guest Tools): the medium of
 * the guest's Fedora release not built yet, being built, failed to build,
 * or out of date, with Build…, Show or Update…, which open the Builds
 * window on that medium.  Hidden while the medium is up to date.
 */
class GuestToolsBuildBanner : public QWidget
{
    Q_OBJECT

public:
    explicit GuestToolsBuildBanner(QWidget *parent = nullptr);

    /* The Fedora release of the medium: the VM's (GuestToolsMonitor::
       release()), GuestTools::newestRelease() until set */
    QString release() const { return m_release; }
    void setRelease(const QString &release);

signals:
    /* Another medium: a build made it, here or by hand */
    void mediumChanged();

private:
    void refresh();

    QString m_release;
    Banner *m_warning;
    Banner *m_note;
    int m_percent = -1;
};
