// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>

#include "clipboardstate.h"

class QMimeData;
typedef struct _GDBusConnection GDBusConnection;
typedef struct _GDBusMethodInvocation GDBusMethodInvocation;
typedef struct _GVariant GVariant;

/*
 * The host clipboard shared with the guest through QEMU's D-Bus display,
 * on the display's peer-to-peer connection (DBusDisplay::connection()).
 * QEMU's clipboard passes it on to its other peers: the qemu-vdagent
 * chardev, and spice-vdagent in the guest.
 *
 * The view registers with Clipboard.Register on QEMU's
 * /org/qemu/Display1/Clipboard, and serves the same interface at the same
 * path for QEMU to call: Grab / Release when the guest copies, Request when
 * it pastes the host's text.
 *
 * Host to guest: each change of the host clipboard (Wayland tells it when
 * vitrine has the keyboard) is offered with a Grab, and the text read then
 * answers QEMU's Request at once - QEMU waits for it with its main loop
 * stopped.
 *
 * Guest to host: the guest's text is fetched as soon as it copies (Request,
 * asynchronous) and put in the host clipboard, rather than offered there
 * and fetched only when a host application pastes.  Qt could do that
 * (QMimeData::retrieveData), but a paste would then wait on vitrine's GUI
 * thread for a D-Bus round trip through QEMU to the guest's agent (up to
 * QEMU's 5 s timeout), and Plasma's clipboard history reads every new
 * clipboard at once anyway; QEMU's clipboard only carries text, which is
 * small.
 *
 * On Wayland, a client sets the clipboard with the serial of its last input
 * (Qt: a key or button press, the pointer entering a window), and KWin
 * refuses it if the clipboard was set after that input
 * (SeatInterfacePrivate::updateSelection): a copy made in the guest right
 * after a host copy, with no input in vitrine since, e.g. one a script
 * made.  The host clipboard then comes back as it was: that text is not
 * offered to the guest, and the guest's is set again at vitrine's next
 * input.  A copy made with Ctrl+C or a click in vitrine's window has a
 * new serial.
 *
 * Calls run on the GUI thread (the default GLib main context).  The
 * object may go at any time: completions of calls still pending find it
 * gone (QPointer), and QEMU's calls stop with the object's registration.
 */
class VmClipboard : public QObject
{
    Q_OBJECT

public:
    explicit VmClipboard(GDBusConnection *connection, QObject *parent = nullptr);
    /* Unregisters */
    ~VmClipboard() override;

    /* How long after setting the host clipboard its old text coming back
       means the compositor refused it */
    static constexpr int kRefusalMs = 1000;

    /* Serves the interface and registers with QEMU (asynchronous) */
    bool start(QString *error);
    bool isRegistered() const { return m_registered; }
    const ClipboardState &state() const { return m_state; }

Q_SIGNALS:
    /* For tests: registered with QEMU, the guest's text in the host clipboard */
    void registered();
    void hostClipboardSet(const QString &text);

private:
    void callRegister(bool retry);
    void hostClipboardChanged();
    void announce(bool again);
    void fetch();
    void fetched(uint32_t serial, GVariant *reply);
    /* @text by value: it may be m_pending's, which this resets */
    void setHostClipboard(QString text);
    void retryPending();
    bool eventFilter(QObject *watched, QEvent *event) override;
    void call(const char *method, GVariant *args, const struct _GVariantType *replyType,
              void (*done)(VmClipboard *, GVariant *, const QString &error, uintptr_t tag),
              uintptr_t tag = 0);

    static void methodCall(GDBusConnection *, const char *, const char *, const char *,
                           const char *method, GVariant *params,
                           GDBusMethodInvocation *invocation, void *user);
    static GVariant *getProperty(GDBusConnection *, const char *, const char *, const char *,
                                 const char *property, struct _GError **, void *);

    GDBusConnection *m_conn = nullptr;
    unsigned m_objectId = 0;
    bool m_registered = false;
    ClipboardState m_state;
    bool m_fetching = false;            // QEMU takes one Request per selection
    bool m_fetchAgain = false;          // a newer guest grab came meanwhile
    QPointer<QMimeData> m_ownData;      // what we put in the host clipboard
    /* the guest's text last put there, and what the host clipboard held before */
    std::optional<QString> m_attempt;
    QString m_hostBefore;
    QElapsedTimer m_attemptTime;
    /* the guest's text the compositor refused, for vitrine's next input */
    std::optional<QString> m_pending;
};
