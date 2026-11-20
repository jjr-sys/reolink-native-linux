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
    property alias zoom: surface.zoom
    property alias panX: surface.panX
    property alias panY: surface.panY
    PictureSurface {
        id: surface
        viewport: Qt.size(video.width, video.height)
        contentRect: video.contentRect
    }

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
            onPressed: (m) => { lastX = m.x; lastY = m.y; }
            onPositionChanged: (m) => {
                if (surface.panning && (m.buttons & Qt.LeftButton)) {
                    surface.panBy(m.x - lastX, m.y - lastY);
                    lastX = m.x; lastY = m.y;
                }
            }
            onWheel: (w) => {
                surface.zoomStep(w.angleDelta.y > 0);
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
