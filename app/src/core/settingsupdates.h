// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDate>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <optional>

#include "core/argsfile.h"
#include "core/cardsettings.h"

class Vm;

/*
 * The settings vitrine gives the VMs it makes now, which a VM made by an
 * older vitrine lacks: vm.args keeps the defaults of the day it was
 * written (hostmem=4G before 2026-10-26, no hpet=off before 2026-10-22),
 * as template changes never reach existing VMs.  Each is found from what
 * vm.args says, and only suggested: the user picks those to take, vm.args
 * is backed up first, only the lines in question change, and an update
 * can be undone.
 *
 * A key the VM sets, to any value, is the user's: nothing is suggested
 * for it.  What VmRunner adds at every start (passt's path, the render
 * nodes, the swap targets...) is no setting of vm.args and is not here;
 * the guest type that decides the swap targets is.  A vm.args that cannot
 * be read safely (a line that is no option, settings in another file,
 * JSON) gets no suggestion at all, and says why.
 */
namespace SettingsUpdates {

/* What is known beyond vm.args, for the checks that need it */
struct Context {
    QString hostArch;           // Paths::hostArch(): x86_64, aarch64
    bool hostTopoext = false;   // the host's CPU has topoext (AMD)
    /*
     * What the VM's QEMU offers for its 3D card and KVM (CardSettings::
     * offers()); none when that QEMU did not answer: then nothing that
     * needs a property of it is suggested
     */
    std::optional<CardSettings::Offers> offers;
    /* That QEMU has virtio-vga-gl, the 3D card with VGA */
    bool vgaCardOffered = false;
    /* What runs in the guest, as its agents said: kde, gnome, or empty */
    QString guestDesktop;
    /* An agent said the guest runs Linux */
    bool guestLinux = false;
};

/* A setting the VM lacks, as found in its vm.args */
struct Change {
    QString id;                 // hpet-off, hostmem-16g...
    QString title;              // what it is
    QString reason;             // why, in a sentence or two
    QDate since;                // the day new VMs got it
    /* The lines of vm.args it changes, as they are and as they would be */
    QStringList before;
    QStringList after;
    bool operator==(const Change &other) const = default;
};

struct Check {
    /* Why vm.args cannot be checked safely; then there are no changes */
    QString problem;
    QList<Change> changes;
};

/* Every change there is, in the order they are offered and applied */
QStringList ids();
/* Its title, by its id; empty for an unknown one */
QString title(const QString &id);
/*
 * @titles as a list in running text: "no HPET timer, a 16 GiB window for
 * GPU memory", the first one's capital kept when @capitalized (a sentence
 * of its own), acronyms (KDE, DRM, GPU) kept as they are
 */
QString listed(const QStringList &titles, bool capitalized = false);

/* What @text, a vm.args, lacks */
Check check(const QString &text, const Context &context);
/* Why @text cannot be checked or edited safely, empty when it can */
QString problem(const QString &text);

/*
 * One edit of an update, recorded to undo it: a key of a line set, changed
 * or removed; a line's first value changed (a card's driver); a line added
 * or removed
 */
struct Edit {
    enum Kind { Key, Implied, Insert, Remove };
    Kind kind = Key;
    /* Key, Implied: the line edited, as found again */
    ArgsFile::Line::Kind lineKind = ArgsFile::Line::Option;
    QString name;               // the option or directive
    QString implied;            // Key: the line's first value, after the edit
    QString key;
    /* Key: the value before and after (none: absent); Implied: the values */
    std::optional<QString> before;
    std::optional<QString> after;
    /* Insert, Remove: the line, and the one before it (empty: none) */
    QString text;
    QString anchor;
    /* Where the line was: after the edit, but where Remove's was before */
    int index = -1;

    QJsonObject toJson() const;
    static Edit fromJson(const QJsonObject &json);
    bool operator==(const Edit &other) const = default;
};

/* An update applied: what each change edited, and where vm.args was saved before it */
struct Record {
    struct Applied {
        QString id;
        QList<Edit> edits;
        bool operator==(const Applied &other) const = default;
    };
    QDateTime time;
    QString backup;             // the file name of the copy, beside vm.args
    QList<Applied> changes;

    QJsonObject toJson() const;
    static Record fromJson(const QJsonObject &json);
    bool operator==(const Record &other) const = default;
};

/*
 * @text with the changes @ids applied, those that are found in it, in the
 * order of ids(); @record gets what each edited.  Only the lines they
 * change are written anew, the others stay as they are, byte for byte.
 */
QString apply(const QString &text, const QStringList &ids, const Context &context,
              Record *record = nullptr);
/*
 * @text with the edits of @record undone where they still are as the
 * update left them, so that later edits stay; @failed gets the ids of the
 * changes with lines changed since, left as they are
 */
QString revert(const QString &text, const Record &record, QStringList *failed = nullptr);

/* The changes of @vmId the user said not to ask about again */
QStringList declined(const QString &vmId);
void setDeclined(const QString &vmId, const QString &id, bool declined);
/* Not Now: none asked about for @vmId until vitrine quits */
bool notNow(const QString &vmId);
void setNotNow(const QString &vmId);
/* What its guest's agent said it runs, kept for while it is off */
QString guestDesktop(const QString &vmId);
void setGuestDesktop(const QString &vmId, const QString &desktop);
/* The updates applied to @vmId, the latest last */
QList<Record> records(const QString &vmId);
void setRecords(const QString &vmId, const QList<Record> &records);
/* All of it, for a VM removed */
void forget(const QString &vmId);

/* The text of vm.args at @path, strictly UTF-8; false with @error when not */
bool readText(const QString &path, QString *text, QString *error);
/* A name for a copy of vm.args in @dir: vm.args.YYYY-MM-DD-HHMMSS.bak */
QString backupName(const QString &dir, const QDateTime &time);

}

/*
 * The changes found for a VM, as its settings, its guest and its QEMU
 * change: one per VM, its child, made by of().  Applying and undoing go
 * through it, only while the VM is off.
 */
class SettingsUpdatesMonitor : public QObject
{
    Q_OBJECT

public:
    static SettingsUpdatesMonitor *of(Vm *vm);

    /* Found and not declined */
    QList<SettingsUpdates::Change> offered() const;
    /* Found and declined */
    QList<SettingsUpdates::Change> declined() const;
    /* Why its vm.args cannot be checked; empty when it can */
    QString problem() const { return m_check.problem; }
    QList<SettingsUpdates::Record> records() const;
    /* Why nothing can be applied or undone now (it runs, it is copied); empty when it can */
    QString blocked() const;

    bool notNow() const;
    void setNotNow();
    void setDeclined(const QString &id, bool declined);

    /*
     * Applies the changes @shown, as the user saw them: false with @error
     * when it cannot, or when vm.args changed since and they are no longer
     * what was shown (@stale: the list is up to date again then)
     */
    bool apply(const QList<SettingsUpdates::Change> &shown, QString *error,
               bool *stale = nullptr);
    /*
     * Undoes the latest update; @failed gets the titles of its changes whose
     * lines changed since and were left as they are
     */
    bool undo(QStringList *failed, QString *error);

    /* Checks again: vm.args, the guest, the VM's QEMU */
    void refresh();

signals:
    void changed();

private:
    explicit SettingsUpdatesMonitor(Vm *vm);
    SettingsUpdates::Context context(const ArgsFile &args) const;
    /* What the guest's agent says it runs, kept */
    void guestReported();

    Vm *m_vm;
    SettingsUpdates::Check m_check;
    QString m_state;
};
