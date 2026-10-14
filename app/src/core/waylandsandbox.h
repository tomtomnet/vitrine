// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>

/*
 * A socket of this computer's Wayland compositor for clients it treats as
 * a sandbox's (wp_security_context_v1, as Flatpak's are): QEMU's connection
 * for the nested desktop's own window (NestedDesktop, SDL or no display).
 * KWin then withholds from the VM what it offers any client outside
 * Flatpak or snap: the clipboard of whatever has the focus (data control),
 * fake input, window management, screencast.  The nested desktop uses none.
 *
 * The compositor listens on the socket itself, also once the connection
 * that set it up is gone, until the read end of a pipe it was given hangs
 * up.  QEMU inherits the write end: the socket lives as long as QEMU, with
 * no process of vitrine's (the research's launcher keeps a helper running).
 */
namespace WaylandSandbox {

/*
 * Listens on @path (a socket there is replaced; its folder should be the
 * user's own), hands it to the compositor at the socket @compositor as the
 * sandbox engine "vitrine"'s @appId, @instanceId, and returns the pipe's
 * write end: close-on-exec, which the caller clears for the process that
 * inherits it, closing its own copy after.  -1 with @error, @path removed:
 * no compositor there, or one without wp_security_context_v1.  One round
 * trip with the compositor.
 */
int listen(const QString &compositor, const QString &path, const QString &appId,
           const QString &instanceId, QString *error);

}
