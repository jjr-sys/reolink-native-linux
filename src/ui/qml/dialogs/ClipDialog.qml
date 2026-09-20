import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import ReolinkApp
import ReolinkApp.Core

// Plays the recorded clip for a detection on a camera whose recordings live on a
// server of their own (Frigate). The server cuts the clip when asked, so the first
// picture takes a moment; the clip plays once, with Replay and Close.
Dialog {
    id: dialog
    modal: true
    width: Math.min(900, Overlay.overlay ? Overlay.overlay.width - 60 : 900)
    height: width * 9 / 16 + 130
    anchors.centerIn: Overlay.overlay
    padding: Theme.spacing * 2

    property string camera: ""
    property string when: ""
    property string url: ""

    function openClip(cameraName, timeText, clipUrl) {
        camera = cameraName;
        when = timeText;
        url = clipUrl;
        open();
    }
    function play() {
        player.stop();
        player.source = url;
        player.start();
    }
    onOpened: play()
    onClosed: player.stop()

    title: qsTr("Detection clip")
    background: Rectangle { color: Theme.surface; border.color: Theme.border; radius: Theme.radius }
    header: Item {
        implicitHeight: 44
        Text {
            anchors.left: parent.left
            anchors.leftMargin: Theme.spacing * 2
            anchors.verticalCenter: parent.verticalCenter
            text: dialog.camera + (dialog.when !== "" ? "  ·  " + dialog.when : "")
            color: Theme.text
            font.pixelSize: 14
            font.bold: true
        }
    }

    StreamPlayer {
        id: player
        videoSink: video.videoSink
        loop: false
        playback: true
        muted: false
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spacing

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.paneBackground
            border.color: Theme.border
            radius: 4
            clip: true
            VideoOutput {
                id: video
                anchors.fill: parent
                anchors.margins: 1
                fillMode: VideoOutput.PreserveAspectFit
            }
            Text {
                anchors.centerIn: parent
                visible: player.state !== StreamPlayer.Streaming
                width: parent.width - 40
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                color: player.state === StreamPlayer.Error ? Theme.danger : Theme.textMuted
                font.pixelSize: 13
                text: player.state === StreamPlayer.Error ? qsTr("Could not play the clip: %1").arg(player.errorString)
                    : player.state === StreamPlayer.Stopped ? qsTr("End of clip")
                    : qsTr("Getting the clip…")
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing
            Item { Layout.fillWidth: true }
            Repeater {
                model: [
                    { label: qsTr("Replay"), act: "replay" },
                    { label: qsTr("Close"), act: "close" }
                ]
                Rectangle {
                    required property var modelData
                    implicitWidth: btnText.implicitWidth + 32
                    implicitHeight: 34
                    radius: Theme.radius
                    color: btnHover.hovered ? Theme.surfaceAlt : "transparent"
                    border.color: Theme.border
                    Text { id: btnText; anchors.centerIn: parent; text: parent.modelData.label; color: Theme.text; font.pixelSize: 13 }
                    HoverHandler { id: btnHover }
                    TapHandler { onTapped: parent.modelData.act === "replay" ? dialog.play() : dialog.close() }
                }
            }
        }
    }
}
