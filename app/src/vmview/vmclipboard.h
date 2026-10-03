// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>

class QMimeData;
typedef struct _GDBusConnection GDBusConnection;
typedef struct _GDBusMethodInvocation GDBusMethodInvocation;
typedef struct _GVariant GVariant;

/*
 * The names text goes by in a clipboard.  QEMU's clipboard (ui/clipboard.c)
 * carries text only, as UTF-8, and its D-Bus display takes and gives only
 * kTextMime; the others are what hosts and guests call the same text.
 */
namespace ClipboardMime {
inline constexpr char kText[] = "text/plain;charset=utf-8";
/* kText first, then the aliases of UTF-8 text */
QStringList textMimes();
bool isText(const QString &mime);
/* The first of @mimes that is UTF-8 text, or an empty string */
QString pickText(const QStringList &mimes);
/* The text @data holds, if any (empty: none, e.g. an image or files) */
QString textOf(const QMimeData *data);
}

/*
 * What goes where between the host clipboard and QEMU's, and the grab
 * serials: no D-Bus and no QClipboard, so that it can be tested alone.
 *
 * Serials (ui/dbus-display1.xml, org.qemu.Display1.Clipboard): each side
 * counts the grabs, from 0 when QEMU resets them (at Register, and when
 * the guest's agent connects).  The guest's spice-vdagentd takes a grab
 * from QEMU's side only with the serial it expects next, or the one before
 * (the host's grab wins a race); QEMU passes the serial of the D-Bus
 * client's grab on as it is.  So a grab of ours carries the serial after
 * the last grab seen from either side, and a guest grab with a serial
 * below that lost a race to ours and is dropped, as the guest drops it.
 *
 * Only the clipboard selection (Ctrl+C / Ctrl+V) is shared, not the
 * primary one (select / middle click).
 */
class ClipboardState
{
public:
    enum class HostAction { None, Grab, Release };

    /* QEMU counts from 0 again */
    void reset();
    /*
     * The host clipboard holds @text (empty: no text).  Grab: offer it to
     * the guest with *serial; Release: the guest no longer gets the text
     * offered before.  @again offers it even if unchanged: to a guest
     * agent that just connected.
     */
    HostAction hostChanged(const QString &text, bool again, uint32_t *serial);
    /* What the host clipboard holds, without telling the guest (before Register) */
    void hostSeen(const QString &text);
    /* The guest took the clipboard: whether to fetch its text */
    bool guestGrab(uint32_t serial, const QStringList &mimes);
    void guestRelease();
    /* The text of the guest's grab @serial: what goes to the host clipboard */
    std::optional<QString> guestText(uint32_t serial, const QString &text);
    /* What QEMU gets when the guest pastes the host's text (false: unhandled) */
    bool request(const QStringList &mimes, QString *mime, QByteArray *data) const;

    uint32_t nextSerial() const { return m_next; }
    /* The host clipboard's text, as far as we know */
    QString hostText() const { return m_hostText; }
    bool hostOwns() const { return m_owner == Owner::Host; }
    bool guestOwns() const { return m_owner == Owner::Guest; }
    /* The guest grab whose text is wanted */
    std::optional<uint32_t> guestSerial() const;

private:
    enum class Owner { None, Host, Guest };

    uint32_t m_next = 0;
    Owner m_owner = Owner::None;
    uint32_t m_guestSerial = 0;     // the guest's grab, while it owns
    bool m_guestText = false;       // and whether it has text
    QString m_hostText;             // in the host clipboard, as far as we know
    QString m_offered;              // offered to the guest by our grab
};

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
 * small.  On Wayland, the compositor takes a new clipboard only from the
 * client with the keyboard: text the guest copies while vitrine has not
 * got it waits until it has.
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
    void setHostClipboard(const QString &text);
    bool canSetHostClipboard() const;
    void applicationStateChanged();
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
    std::optional<QString> m_pending;   // guest text, waiting for the keyboard
    QPointer<QMimeData> m_ownData;      // what we put in the host clipboard
};
