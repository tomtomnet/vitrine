// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QWidget>

class Banner;

/*
 * Where a VM's guest tools are offered (Install Guest Tools): the medium
 * not built yet, being built, failed to build, or out of date, with Build…,
 * Show or Update…, which open the Builds window on the medium.  Hidden
 * while the medium is up to date.
 */
class GuestToolsBuildBanner : public QWidget
{
    Q_OBJECT

public:
    explicit GuestToolsBuildBanner(QWidget *parent = nullptr);

signals:
    /* A build made a new medium */
    void mediumChanged();

private:
    void refresh();

    Banner *m_warning;
    Banner *m_note;
    int m_percent = -1;
};
