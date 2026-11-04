// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QSize>
#include <QMetaObject>
#include <QString>

#include <gtkmm/eventcontrollerscroll.h>
#include <gtkmm/widget.h>
#include <sigc++/signal.h>

#include <cstdint>
#include <memory>
#include <set>

#include "displayinput.h"
#include "vmview/common.h"

class DBusDisplay;
class DisplaySurface;
class PointerConstraints;
class QImage;
class QWindow;
class Renderer;
typedef struct _GdkFrameClock GdkFrameClock;
typedef struct _GdkTexture GdkTexture;

/*
 * The VM's screen in a GTK window: the GTK counterpart of src/vmview's
 * DisplayWindow and of VmView's container.  The frames are on a surface of
 * their own over the widget (DisplaySurface), which the widget keeps at its
 * allocation as GTK lays the window out - its position goes with GTK's
 * commit of that layout - and shows only while the widget is mapped (a
 * page of a GtkStack not shown is not).  The widget draws black under it.
 *
 * The keyboard and the pointer come to the widget, as to any other of the
 * window, through GTK's event controllers, and go to the guest
 * (DisplayInput): every key while it has the focus and input is on, the
 * Ctrl+Alt+G and Ctrl+Alt+F hotkeys aside.
 */
class DisplayWidget : public Gtk::Widget, private DisplayInput::Sink,
                      private DisplayInput::Compositor
{
public:
    DisplayWidget(DBusDisplay *display, DisplaySurface *surface, const Options &opts);
    ~DisplayWidget() override;

    void setRenderer(Renderer *renderer) { m_renderer = renderer; }
    /* What the renderer reads of its window: the full-screen state */
    QWindow *rendererWindow() const { return m_window.get(); }
    void setGuestSize(uint32_t width, uint32_t height);
    void setGuestCursor(const QImage &image, int hotX, int hotY);
    void setGuestCursorVisible(bool visible);
    /* In buffer pixels: what the guest is told to be */
    QSize physicalSize() const;
    uint32_t refreshMilliHz() const;
    void sendUiInfo();

    bool grabbed() const { return m_input.grabbed(); }
    QString grabState() const { return m_input.grabState(); }
    void setGrab(bool on);
    /* The keyboard is the guest's: the widget has the focus in the active
       window, and input is on (the controller tells) */
    void setHostActive(bool active);
    /* Off: no keys taken (they go to the window's shortcuts) */
    void setInputEnabled(bool on) { m_inputEnabled = on; }
    bool isExposed() const { return m_exposed; }
    /* The window's own full-screen state, for the renderer */
    void setFullScreenWindow(bool on);

    /* grabbed() or grabState() changed */
    sigc::signal<void()> grabChanged;
    /* Ctrl+Alt+F */
    sigc::signal<void()> fullScreenToggled;
    /* The focus came or went, or the window became active or not */
    sigc::signal<void()> focusChanged;
    /* Shown or hidden: mapped and on the window */
    sigc::signal<void(bool)> exposedChanged;
    /* The window's scale, monitor or refresh rate changed */
    sigc::signal<void()> screenChanged;

protected:
    Gtk::SizeRequestMode get_request_mode_vfunc() const override;
    void measure_vfunc(Gtk::Orientation orientation, int forSize, int &minimum, int &natural,
                       int &minimumBaseline, int &naturalBaseline) const override;
    void size_allocate_vfunc(int width, int height, int baseline) override;
    void snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot> &snapshot) override;
    void on_realize() override;
    void on_unrealize() override;
    void on_map() override;
    void on_unmap() override;

private:
    /* DisplayInput::Sink: to QEMU */
    void keyPress(uint32_t qnum) override;
    void keyRelease(uint32_t qnum) override;
    void mouseAbs(uint32_t x, uint32_t y) override;
    void mouseRel(int dx, int dy) override;
    void mouseButton(uint32_t button, bool down) override;
    bool mouseIsAbsolute() const override;
    /* DisplayInput::Compositor: on the window's surface */
    void setShortcutsInhibited(bool on) override;
    void setPointerLocked(bool on) override;
    void setPointerConfined(bool on) override;

    bool onKeyPressed(guint keyval, guint keycode, Gdk::ModifierType state);
    void onKeyReleased(guint keyval, guint keycode, Gdk::ModifierType state);
    bool onEvent(const Glib::RefPtr<const Gdk::Event> &event);
    bool onScroll(double dx, double dy);
    /* The frame clock's layout, after GTK's: the surface where GTK put the widget */
    static void onLayout(GdkFrameClock *clock, gpointer self);
    void place();
    void updateSize();
    void updateCursor();
    void scheduleUiInfo();
    GdkSurface *toplevelSurface() const;
    void setExposed(bool exposed);

    DBusDisplay *m_display;
    DisplaySurface *m_surface;
    Options m_opts;
    Renderer *m_renderer = nullptr;
    std::unique_ptr<QWindow> m_window;
    DisplayInput m_input;
    std::unique_ptr<PointerConstraints> m_constraints;
    Glib::RefPtr<Gtk::EventControllerScroll> m_scroll;
    bool m_inputEnabled = true;
    bool m_exposed = false;
    bool m_inhibited = false;
    /* the hotkeys' keys down: their repeats and releases are not the guest's */
    std::set<guint> m_hotkeys;
    GdkTexture *m_cursorTexture = nullptr;
    int m_cursorHotX = 0, m_cursorHotY = 0;
    /* realized */
    GdkFrameClock *m_clock = nullptr;
    gulong m_layoutId = 0;
    GdkSurface *m_watched = nullptr;    // the window's surface: its scale, its monitor
    gulong m_scaleId = 0, m_enterMonitorId = 0;
    Gtk::Window *m_root = nullptr;
    sigc::connection m_activeWatch;
    sigc::connection m_uiInfoTimer;
    QMetaObject::Connection m_modeWatch;
    int m_width = 0, m_height = 0;
};
