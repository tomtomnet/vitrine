// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QSize>
#include <QSizeF>

#include <algorithm>
#include <cmath>

/*
 * The physical size a guest is told its screen has.  QEMU's virtio-gpu puts
 * the display's width_mm and height_mm (the D-Bus display's UIInfo) in the
 * EDID it hands the guest, and takes a 100 DPI screen without them: GNOME
 * then picks 100 % at every new size of the window.  Told the real size, the
 * guest picks the scale it would pick on that monitor, for each size (the
 * default scale of Mutter and KWin follows the DPI), the login screen too.
 * A scale the user picked by hand on the host is not copied: the guest
 * decides by itself, as on the bare monitor.
 *
 * Header only: the D-Bus display client (vmview) and the GTK view call it.
 */
namespace PhysicalSize {

/*
 * What a monitor of @monitorMm millimetres says no monitor is: no size, or
 * its aspect ratio in centimetres, as some EDIDs do (Mutter's own
 * meta_monitor_has_aspect_as_size() looks for the same)
 */
inline bool isUnknown(const QSizeF &monitorMm)
{
    const int w = int(std::lround(monitorMm.width()));
    const int h = int(std::lround(monitorMm.height()));

    return w <= 0 || h <= 0 || (w == 1600 && (h == 900 || h == 1000)) ||
           (w == 160 && (h == 90 || h == 100)) || (w == 16 && (h == 9 || h == 10));
}

/*
 * The millimetres a view covers: @view device pixels of a monitor of
 * @monitor device pixels and @monitorMm millimetres (QScreen::physicalSize(),
 * GdkMonitor's width_mm and height_mm).  Empty, for QEMU's default, when the
 * monitor's size is not known or makes no sense (under 40 or over 1000 DPI).
 */
inline QSize ofView(const QSize &view, const QSize &monitor, const QSizeF &monitorMm)
{
    if (view.isEmpty() || monitor.isEmpty() || isUnknown(monitorMm)) {
        return {};
    }
    const double dpiX = monitor.width() * 25.4 / monitorMm.width();
    const double dpiY = monitor.height() * 25.4 / monitorMm.height();
    if (dpiX < 40 || dpiX > 1000 || dpiY < 40 || dpiY > 1000) {
        return {};
    }
    /* an EDID's detailed timing holds 12 bits of millimetres */
    const auto mm = [](double value) { return int(std::clamp(std::lround(value), 1L, 4095L)); };
    return {mm(view.width() * monitorMm.width() / monitor.width()),
            mm(view.height() * monitorMm.height() / monitor.height())};
}

}
