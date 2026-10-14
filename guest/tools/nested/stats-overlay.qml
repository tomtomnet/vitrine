// The stats overlay of the nested desktop (wl-cross-domain-proxy --overlay-command, Ctrl+Alt+S):
// a layer-shell window above everything in the top-left corner, click-through, that never takes
// the keyboard. It reads the proxy's JSON file (its path is the argument after "--") four times a
// second (each repaint is a frame the nested KWin composites: at 10 Hz the overlay doubled the
// repeated refreshes of a launcher loop, 2.1 -> 4.4 %, at 4 Hz +1 point; 2026-10-08) and shows the frames the host presented per second (FPS) and those the guest committed
// (UPS, its updates) - over the last second, and the rate of the last burst of frames (an
// animation's own, "burst") - the frame and input latencies (median, p99), the frames the host
// dropped, and two graphs of the last frames' intervals: host = between presentations (a
// refresh shown twice is a bar twice as high), guest = between the guest's commits; pauses
// between bursts are short grey marks.
//   QML_XHR_ALLOW_FILE_READ=1 qml stats-overlay.qml -- /run/user/1000/crosvm-desktop-stats.json
import QtQuick
import QtQuick.Window
import org.kde.layershell 1.0 as LayerShell

Window {
    id: win

    property var stats: null
    property string file: {
        const args = Qt.application.arguments
        const i = args.indexOf("--")
        return i >= 0 && i + 1 < args.length ? args[i + 1] : ""
    }
    readonly property real period: stats && stats.refresh_hz > 0 ? 1000 / stats.refresh_hz : 1000 / 60

    width: 300
    height: panel.height + 16
    color: "transparent"
    flags: Qt.FramelessWindowHint | Qt.WindowTransparentForInput | Qt.WindowDoesNotAcceptFocus
    title: "crosvm-desktop stats"

    LayerShell.Window.scope: "crosvm-desktop-stats"
    LayerShell.Window.layer: LayerShell.Window.LayerOverlay
    LayerShell.Window.anchors: LayerShell.Window.AnchorTop | LayerShell.Window.AnchorLeft
    LayerShell.Window.keyboardInteractivity: LayerShell.Window.KeyboardInteractivityNone
    LayerShell.Window.exclusionZone: -1

    function fmt(v, digits) {
        return v === undefined || v === null ? "-" : Number(v).toFixed(digits)
    }
    function pair(p) {
        return p ? fmt(p[0], 1) + " ms  (p99 " + fmt(p[1], 1) + ")" : "-"
    }

    Timer {
        interval: 250
        running: win.file !== ""
        repeat: true
        triggeredOnStart: true
        onTriggered: {
            const xhr = new XMLHttpRequest()
            xhr.onreadystatechange = function () {
                if (xhr.readyState !== XMLHttpRequest.DONE || !xhr.responseText)
                    return
                try {
                    win.stats = JSON.parse(xhr.responseText)
                } catch (e) {
                    // a file being replaced: the next read
                }
            }
            xhr.open("GET", "file://" + win.file)
            xhr.send()
        }
    }

    Rectangle {
        id: panel
        x: 8
        y: 8
        width: win.width - 16
        height: column.implicitHeight + 16
        radius: 6
        color: "#e8101418"
        border.color: "#40ffffff"

        Column {
            id: column
            x: 8
            y: 8
            width: parent.width - 16
            spacing: 3

            Row {
                spacing: 16
                Text {
                    text: "FPS " + win.fmt(win.stats ? win.stats.fps : null, 0)
                    color: "#ffffff"
                    font.family: "monospace"
                    font.pixelSize: 15
                    font.bold: true
                }
                Text {
                    text: "UPS " + win.fmt(win.stats ? win.stats.ups : null, 0)
                    color: "#d0d8e0"
                    font.family: "monospace"
                    font.pixelSize: 15
                    font.bold: true
                }
                Text {
                    y: 4
                    text: win.stats && win.stats.refresh_hz > 0 ? win.fmt(win.stats.refresh_hz, 0) + " Hz" : ""
                    color: "#a0a8b0"
                    font.family: "monospace"
                    font.pixelSize: 11
                }
            }
            Text {
                text: "burst  " + (win.stats && win.stats.burst_fps ? win.fmt(win.stats.burst_fps, 0) + " fps" : "-")
                      + "  /  " + (win.stats && win.stats.burst_ups ? win.fmt(win.stats.burst_ups, 0) + " ups" : "-")
                color: "#e0e6ec"
                font.family: "monospace"
                font.pixelSize: 11
            }
            Text {
                text: "frame  " + win.pair(win.stats ? win.stats.frame : null)
                color: "#e0e6ec"
                font.family: "monospace"
                font.pixelSize: 11
            }
            Text {
                text: "input  " + win.pair(win.stats ? win.stats.input : null)
                color: "#e0e6ec"
                font.family: "monospace"
                font.pixelSize: 11
            }
            Text {
                text: "dropped " + win.fmt(win.stats ? win.stats.dropped : null, 0) + "/s   hold "
                      + (win.stats && win.stats.hold ? win.fmt(win.stats.hold[0], 2) + " ms" : "-")
                color: win.stats && win.stats.dropped > 0 ? "#ffb060" : "#e0e6ec"
                font.family: "monospace"
                font.pixelSize: 11
            }

            Repeater {
                model: [
                    { label: "host", key: "present_ms" },
                    { label: "guest", key: "commit_ms" }
                ]
                delegate: Item {
                    required property var modelData
                    width: column.width
                    height: 34

                    Text {
                        id: label
                        anchors.verticalCenter: parent.verticalCenter
                        width: 38
                        text: modelData.label
                        color: "#a0a8b0"
                        font.family: "monospace"
                        font.pixelSize: 10
                    }
                    Canvas {
                        id: graph
                        anchors.left: label.right
                        anchors.right: parent.right
                        height: parent.height
                        property var values: win.stats ? win.stats[modelData.key] : []
                        onValuesChanged: requestPaint()
                        onPaint: {
                            const ctx = getContext("2d")
                            ctx.clearRect(0, 0, width, height)
                            ctx.fillStyle = "#1e252c"
                            ctx.fillRect(0, 0, width, height)
                            // three refresh periods high; lines at one and two
                            const top = 3 * win.period
                            ctx.fillStyle = "#4a5560"
                            for (let k = 1; k <= 2; k++)
                                ctx.fillRect(0, Math.round(height - height * k / 3), width, 1)
                            const v = values || []
                            const n = 240
                            const w = width / n
                            const first = Math.max(0, v.length - n)
                            for (let i = first; i < v.length; i++) {
                                const ms = v[i]
                                const x = (n - (v.length - i)) * w
                                // a pause between bursts, not a late frame: a short grey mark
                                if (ms > 50) {
                                    ctx.fillStyle = "#606870"
                                    ctx.fillRect(x, height - 3, Math.max(w - 0.2, 0.6), 3)
                                    continue
                                }
                                if (ms <= 1.5 * win.period)
                                    ctx.fillStyle = "#5fd068"
                                else if (ms <= 2.5 * win.period)
                                    ctx.fillStyle = "#f0b030"
                                else
                                    ctx.fillStyle = "#ff5050"
                                const h = Math.min(ms, top) / top * height
                                ctx.fillRect(x, height - h, Math.max(w - 0.2, 0.6), h)
                            }
                        }
                    }
                }
            }
        }
    }

    Component.onCompleted: visible = true
}
