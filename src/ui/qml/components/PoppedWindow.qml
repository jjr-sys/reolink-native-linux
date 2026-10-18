import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtMultimedia
import ReolinkApp
import ReolinkApp.Core

// A detached live-view window for one camera — drag it to a second monitor for
// multi-screen viewing (DESIGN §6.2). Shares live transport and recovery rules
// with the grid. Closing releases the live stream; Esc closes.
Window {
    id: win
    property var cameraIdentity: ({})
    property string label: ""

    width: 960
    height: 540
    minimumWidth: 320
    minimumHeight: 180
    visible: true
    color: Theme.paneBackground
    title: label + " — Reolink Client"

    // Digital zoom: wheel to zoom, drag to pan when zoomed (as in a live pane).
    property real zoom: 1.0
    property real panX: 0
    property real panY: 0

    Item {
        anchors.fill: parent
        clip: true

        VideoOutput {
            id: video
            anchors.fill: parent
            fillMode: VideoOutput.PreserveAspectFit
            visible: player.state === StreamPlayer.Streaming
            transform: [
                Scale {
                    origin.x: video.width / 2
                    origin.y: video.height / 2
                    xScale: win.zoom
                    yScale: win.zoom
                },
                Translate { x: win.panX; y: win.panY }
            ]
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            property real lastX: 0
            property real lastY: 0
            cursorShape: win.zoom > 1.0 ? Qt.OpenHandCursor : Qt.ArrowCursor
            // Pin the zoomed footage to the window edges (no panning it off into
            // empty space; zooming out reels it back in).
            function clampPan() {
                var mx = Math.max(0, (video.contentRect.width * win.zoom - video.width) / 2);
                var my = Math.max(0, (video.contentRect.height * win.zoom - video.height) / 2);
                win.panX = Math.max(-mx, Math.min(mx, win.panX));
                win.panY = Math.max(-my, Math.min(my, win.panY));
            }
            onPressed: (m) => { lastX = m.x; lastY = m.y; }
            onPositionChanged: (m) => {
                if (win.zoom > 1.0 && (m.buttons & Qt.LeftButton)) {
                    win.panX += (m.x - lastX);
                    win.panY += (m.y - lastY);
                    lastX = m.x; lastY = m.y;
                    clampPan();
                }
            }
            onWheel: (w) => {
                var z = win.zoom * (w.angleDelta.y > 0 ? 1.15 : 0.87);
                win.zoom = Math.max(1.0, Math.min(8.0, z));
                if (win.zoom <= 1.0) { win.panX = 0; win.panY = 0; }
                else clampPan();
            }
        }
    }
    // Camera audio: off until asked for (a popped-out window is usually one of
    // several), and only offered when the stream carries a track.
    property bool audioOn: false
    LiveStream {
        id: player
        devices: Devices
        camera: win.cameraIdentity
        mainStream: true
        active: win.visible
        videoSink: video.videoSink
        muted: !win.audioOn
    }
    onClosing: player.release()

    Text {
        anchors.centerIn: parent
        visible: player.state !== StreamPlayer.Streaming
        text: player.errorString !== "" ? player.errorString : qsTr("Connecting…")
        color: Theme.textMuted
        font.pixelSize: 13
    }

    Rectangle { // name overlay
        visible: player.state === StreamPlayer.Streaming
        anchors.top: parent.top; anchors.left: parent.left; anchors.margins: 8
        radius: 3; color: "#80000000"
        width: nmRow.implicitWidth + 12; height: nmRow.implicitHeight + 6
        Row {
            id: nmRow; anchors.centerIn: parent; spacing: 6
            Text { text: win.label; color: "white"; font.pixelSize: 12 }
            Text {
                visible: player.degraded
                text: qsTr("Degraded · RTSP fallback")
                color: Theme.warning
                font.pixelSize: 11
            }
            Text {
                visible: win.zoom > 1.01
                text: win.zoom.toFixed(1) + "×"
                color: Theme.accent; font.pixelSize: 12
            }
        }
    }

    Rectangle { // listen / mute
        visible: player.state === StreamPlayer.Streaming && player.hasAudio
        anchors.top: parent.top; anchors.right: parent.right; anchors.margins: 8
        width: 32; height: 28; radius: 4
        color: win.audioOn ? Theme.accentDim : "#80000000"
        Text {
            anchors.centerIn: parent
            text: win.audioOn ? "🔊" : "🔇"
            color: "white"; font.pixelSize: 15
        }
        TapHandler { onTapped: win.audioOn = !win.audioOn }
        HoverHandler { id: audioHover }
        ToolTip.visible: audioHover.hovered
        ToolTip.delay: 500
        ToolTip.text: win.audioOn ? qsTr("Mute") : qsTr("Listen (unmute this camera)")
    }

    Shortcut { sequence: "Escape"; onActivated: win.close() }
}
