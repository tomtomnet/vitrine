// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QElapsedTimer>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "vmview/clipboardstate.h"

typedef struct _GCancellable GCancellable;
typedef struct _GDBusConnection GDBusConnection;
typedef struct _GDBusMethodInvocation GDBusMethodInvocation;
typedef struct _GVariant GVariant;
typedef struct _GVariantType GVariantType;
typedef struct _GdkClipboard GdkClipboard;
typedef struct _GdkDisplay GdkDisplay;

/*
 * The host clipboard shared with the guest through QEMU's D-Bus display:
 * src/vmview's VmClipboard with GTK's clipboard instead of Qt's, the same
 * calls on the display's peer-to-peer connection and the same rules
 * (ClipboardState): the view registers with Clipboard.Register and serves
 * the interface for QEMU's Grab, Release and Request.
 *
 * GTK reads a clipboard only asynchronously: the host's text is read as it
 * changes (GTK hears of it when vitrine has the keyboard), and QEMU's
 * Request is answered at once from that - QEMU waits for it with its main
 * loop stopped.  The guest's text is fetched as soon as it copies, and put
 * in the host clipboard.  On Wayland a client sets the clipboard with the
 * serial of its last input, which KWin refuses when the clipboard was set
 * after that input: the host's text back within kRefusalMs means refused,
 * and the guest's text goes again at vitrine's next input (inputSeen()).
 *
 * On the GUI thread; the object may go at any time.
 */
class GtkVmClipboard
{
public:
    GtkVmClipboard(GDBusConnection *connection, GdkDisplay *display);
    /* Unregisters */
    ~GtkVmClipboard();
    GtkVmClipboard(const GtkVmClipboard &) = delete;
    GtkVmClipboard &operator=(const GtkVmClipboard &) = delete;

    static constexpr int kRefusalMs = 1000;

    /* Serves the interface and registers with QEMU (asynchronous) */
    bool start(QString *error);
    bool isRegistered() const { return m_registered; }
    const ClipboardState &state() const { return m_state; }
    /* The guest's text the compositor refused waits for vitrine's next input */
    bool waitsForInput() const { return m_pending.has_value(); }
    /* vitrine's input: a key or button pressed, the pointer in */
    void inputSeen();

    /* registered with QEMU; the guest's text put in the host clipboard */
    std::function<void()> registered;
    std::function<void(const QString &text)> hostClipboardSet;

private:
    using Done = void (*)(GtkVmClipboard *, GVariant *, const QString &error, uintptr_t tag);

    void callRegister(bool retry);
    void hostClipboardChanged();
    void readHost();
    void hostRead(uint64_t generation, const QString &text);
    void announce(bool again);
    void fetch();
    void fetched(uint32_t serial, GVariant *reply);
    void setHostClipboard(const QString &text);
    void call(const char *method, GVariant *args, const GVariantType *replyType, Done done,
              uintptr_t tag = 0);

    static void methodCall(GDBusConnection *, const char *, const char *, const char *,
                           const char *method, GVariant *params,
                           GDBusMethodInvocation *invocation, void *user);
    static GVariant *getProperty(GDBusConnection *, const char *, const char *, const char *,
                                 const char *property, struct _GError **, void *);

    GDBusConnection *m_conn = nullptr;
    GdkClipboard *m_clipboard = nullptr;
    GCancellable *m_cancel = nullptr;   // the host clipboard's reads
    unsigned long m_changedId = 0;
    unsigned m_objectId = 0;
    bool m_registered = false;
    ClipboardState m_state;
    bool m_fetching = false;            // QEMU takes one Request per selection
    bool m_fetchAgain = false;          // a newer guest grab came meanwhile
    uint64_t m_readGeneration = 0;      // the host clipboard's newest read
    /* the guest's text last put there, and what the host clipboard held before */
    std::optional<QString> m_attempt;
    QString m_hostBefore;
    QElapsedTimer m_attemptTime;
    /* the guest's text the compositor refused, for vitrine's next input */
    std::optional<QString> m_pending;
    /* for the completions that come after the object went */
    std::shared_ptr<bool> m_alive;
};
