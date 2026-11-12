// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QCoreApplication>
#include <QString>
#include <QStringList>

#include "core/gpuinputawake.h"
#include "core/hostsettings.h"

/*
 * The words of host tuning, for both apps: the notice of VMs running
 * untuned, its explanation, the offer to set up the vitrine group and how
 * that went (the Qt app's HostTuningNotifier), and the Preferences' Host
 * rows (HostTuningPrefs).  Rich text as Qt's labels take it (<p>, <pre>,
 * <b>, <a>, <nobr>): the GTK app makes Pango markup of it.  Qt Core only.
 */
class HostTuningText
{
    /* the Qt app's HostTuningNotifier's words, in its translation context */
    Q_DECLARE_TR_FUNCTIONS(HostTuningNotifier)

public:
    /* A message box's title and rich text */
    struct Box {
        QString title;
        QString text;
    };

    /* What tuning does, one sentence */
    static QString summary();
    /* What to do about @status, rich text: commands, the docs */
    static QString fix(const HostSettings::Status &status);
    /* "A running VM is not tuned: why." */
    static QString untuned(int count, const QString &why);
    /* The running VMs are tuned now, asked again */
    static Box active();
    /* @count VMs run untuned for @status: why, what tuning does, the fix */
    static Box inactive(int count, const HostSettings::Status &status);
    /* The offer to set up the vitrine group, for @status */
    static Box offer(const HostSettings::Status &status);
    /* How setting up the group went: it failed for @error; done, the state @now after it */
    static Box setUpFailed(const QString &error);
    static Box setUpDone(const HostSettings::Status &now);
};

/*
 * The Preferences' Host rows' longer words: the state line, the CPU's
 * hint, gpu-input-awake's line and its confirmations (the Qt app's
 * HostTuningPrefs).  Rich text, as HostTuningText's.
 */
class HostTuningPrefsText
{
    Q_DECLARE_TR_FUNCTIONS(HostTuningPrefs)

public:
    /* Tuning is active; @realtime: why real-time threads are off, if they
       are; else which VMs get none on a computer of @cpus online CPUs */
    static QString active(const QString &realtime, int cpus = HostSettings::onlineCpus());
    /* Not active, why, and the fix */
    static QString notActive(const HostSettings::Status &status);
    /* What keeping the CPU ready to wake up does, and costs */
    static QString cpuHint();
    /* What the GPU clock floor's Automatic does, by GPU */
    static QString floorHint();
    /* "@gpus: a clock floor does nothing on a dedicated GPU… It is installed and runs." */
    static QString awake(const QStringList &gpus, GpuInputAwake::Installed installed,
                         const QString &active);
    /* Install or Remove: what it does, before it asks for the password */
    static HostTuningText::Box awakeConfirm(bool install, GpuInputAwake::Installed installed);
    /* While it runs */
    static QString awakeBusy(bool install);
    static QString awakeFailed(const QString &error);
};
