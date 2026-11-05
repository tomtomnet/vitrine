// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QCoreApplication>
#include <QString>

#include "core/guesttoolsbuilder.h"
#include "core/stackbuilder.h"

/*
 * The Preferences' words, for both apps (the Qt app's PreferencesDialog):
 * what the QEMU, guest tools, virtiofsd and VM folder rows say of the
 * choices made there.  Rich text as Qt's labels take it.  Qt Core only.
 */
class PreferencesText
{
    /* the Qt app's PreferencesDialog's words, in its translation context */
    Q_DECLARE_TR_FUNCTIONS(PreferencesDialog)

public:
    /* virtiofsd where Fedora installs it, else from PATH; "" for none */
    static QString autoVirtiofsd();

    /* Another QEMU than Vitrine's: for which VMs */
    static QString customQemuTip();
    /* The folder of the VMs: what Vitrine does with it */
    static QString vmsDirTip();
    /* The update check: what it asks */
    static QString updatesHint();

    /* The button of Vitrine's QEMU, of the guest tools: Build…, Update…, Details…, Show… */
    static QString stackButton(bool running, StackBuilder::State state);
    static QString toolsButton(bool running, GuestToolsBuilder::State state);
    /* What builds now: "Building…", "Building: step 2 of 4."; @progress the builder's */
    static QString stackBuilding();
    static QString toolsBuilding(const QString &progress);

    /* Another QEMU: none chosen, not a program, being checked, what it said, why it did not run */
    static QString qemuNone();
    static QString notExecutable();
    static QString checking();
    static QString qemuVersion(const QString &output);
    static QString cannotRun(const QString &error);
    /* What shared folders use: virtiofsd @binary, "" for none found */
    static QString virtiofsd(const QString &binary);
    /* The folder @dir, another than the one used until now: what it holds, what changes */
    static QString vmsDirState(const QString &dir);
};
