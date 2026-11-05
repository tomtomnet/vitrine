// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QCoreApplication>
#include <QString>

#include "core/guesttoolsbuilder.h"
#include "core/hostkde.h"
#include "core/stackbuilder.h"

/*
 * What the builds' states mean, in words, for both apps: their Builds
 * windows and the Preferences' rows of Vitrine's QEMU and the guest
 * tools.  The Qt app's BuildsWindow forwards to these.  Qt Core only.
 */
class BuildTexts
{
    /* the Qt app's BuildsWindow's words, in its translation context */
    Q_DECLARE_TR_FUNCTIONS(BuildsWindow)

public:
    /* "Vitrine's build of 3 October 2026, QEMU 11.1.50 + 7 patches" */
    static QString describe(const StackBuilder::Build &build);
    /* What the state of Vitrine's QEMU means for the user, in a sentence or two */
    static QString explain(StackBuilder::State state, const StackBuilder::Build &build);
    /* "Built on 5 October 2026: vitrine-guest-tools 0.1.0-14, Mesa 26.2.3, KWin 6.7.5",
       of the medium for Fedora @release */
    static QString describeGuestTools(const QString &release);
    static QString explain(GuestToolsBuilder::State state);
    /* "40 s", "18 min", "1 h 20 min": how long a build took */
    static QString duration(qint64 ms);
    /*
     * What a group of the Builds window makes, and of what: "host" (Vitrine's
     * QEMU), "system" (the host's desktop), else the guest tools of Fedora
     * @guestRelease; rich text
     */
    static QString groupAbout(const QString &group, const QString &guestRelease);
    /* What a host package is, and what the research's set changes and measured */
    static QString systemAbout(const HostKde::Target &target);
    /*
     * What the banner over the main window says of Vitrine's QEMU (rich
     * text), as @builder is: building (@percent of its step, -1 unknown), its
     * last build failed (@error, empty when it did not, or was stopped), not
     * built yet, out of date; nothing when it is up to date, or when the
     * preferences choose another QEMU.  @button: the banner's button (Show,
     * Show…, Build…, Update…); @note: a note rather than a warning
     */
    static QString stackNote(const StackBuilder *builder, int percent, const QString &error,
                             QString *button = nullptr, bool *note = nullptr);
};
