// Client side of QEMU's org.qemu.Display1 over a peer-to-peer GDBus
// connection. Calls are asynchronous; replies are dispatched by the GUI
// thread (Qt runs the GLib main context there).
#pragma once

#include <QObject>
#include <QString>
#include <cstdint>

typedef struct _GCancellable GCancellable;
typedef struct _GDBusConnection GDBusConnection;
class Stats;

class DBusDisplay : public QObject
{
    Q_OBJECT
public:
    explicit DBusDisplay(Stats *stats, QObject *parent = nullptr);
    ~DBusDisplay() override;

    bool connectPeer(int fd, QString *err);  // takes ownership of fd
    bool selectConsole(QString *err);        // first graphic console
    bool registerListener(int peerFd, QString *err);

    void applyUiInfo(uint32_t width, uint32_t height, uint32_t refreshMilliHz,
                     uint16_t widthMm, uint16_t heightMm);
    void keyPress(uint32_t qnum);
    void keyRelease(uint32_t qnum);
    void mouseAbs(uint32_t x, uint32_t y);
    void mouseRel(int dx, int dy);
    void mouseButton(uint32_t button, bool down);
    bool mouseIsAbsolute() const { return m_absolute; }
    QString consolePath() const { return m_path; }
    GDBusConnection *connection() const { return m_conn; }
    QString vmName() const { return m_name; }

Q_SIGNALS:
    void mouseModeChanged(bool absolute);
    void uiInfoApplied(const QString &result);

private:
    void call(const char *iface, const char *method, struct _GVariant *args,
              bool measure = false);
    void readMouseMode();
    static void propertiesChanged(GDBusConnection *, const char *, const char *,
                                  const char *, const char *, struct _GVariant *, void *);

    Stats *m_stats;
    GDBusConnection *m_conn = nullptr;
    // the calls still on their way when it goes: their completions, which
    // GLib runs later on the GUI thread, then touch neither it nor m_stats
    GCancellable *m_cancel = nullptr;
    QString m_path;
    QString m_name;
    bool m_absolute = true;
    unsigned m_signalId = 0;
};
