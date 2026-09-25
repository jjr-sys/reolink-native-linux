import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import ReolinkApp
import ReolinkApp.Core

// Event / Notification Center: chronological detection inbox with AI-type
// filters. Content is a centered, max-width column so cards stay readable on
// wide windows. Clicking an event jumps to Playback at that camera/time.
Item {
    id: page
    signal jumpToPlayback(var hostId, var channel, var timestamp)

    readonly property int columnWidth: Math.min(width - Theme.spacing * 4, 820)

    function iconFor(type) {
        switch (type) {
        case "person": return "🚶";
        case "vehicle": return "🚗";
        case "pet": return "🐾";
        case "visitor": return "🔔";
        case "offline": return "⚠";
        case "online": return "✔";
        default: return "👁";
        }
    }
    function labelFor(type) {
        switch (type) {
        case "person": return qsTr("Person");
        case "vehicle": return qsTr("Vehicle");
        case "pet": return qsTr("Pet");
        case "visitor": return qsTr("Visitor");
        case "offline": return qsTr("Went offline");
        case "online": return qsTr("Back online");
        default: return qsTr("Motion");
        }
    }
    function colorFor(type) {
        switch (type) {
        case "person": return Theme.accent;
        case "vehicle": return "#e08a3c";
        case "pet": return Theme.online;
        case "visitor": return Theme.danger;
        case "offline": return Theme.danger;
        case "online": return Theme.online;
        default: return Theme.textMuted;
        }
    }

    onVisibleChanged: if (visible) Events.markAllRead()

    // Audio events (raised voice / threat speech) come from the audio service, not the
    // detection inbox, so they are a separate view. Shown only when the service is set up.
    property bool showAudio: false
    property string playingId: ""
    property string clipError: ""
    Connections {
        target: AudioAlerts
        function onClipReady(eventId, path, error) {
            if (eventId !== page.playingId) return;
            if (path.length > 0) { clipPlayer.source = "file://" + path; clipPlayer.play(); }
            else { page.clipError = error; page.playingId = ""; }
        }
    }
    onShowAudioChanged: if (!showAudio) stopClip()
    function stopClip() { clipPlayer.stop(); clipPlayer.source = ""; playingId = ""; AudioAlerts.discardClips(); }
    function toggleClip(id) {
        clipError = "";
        if (playingId === id) { stopClip(); return; }
        stopClip();
        playingId = id;
        AudioAlerts.fetchClip(id);
    }
    MediaPlayer {
        id: clipPlayer
        audioOutput: AudioOutput {}
        onPlaybackStateChanged: if (playbackState === MediaPlayer.StoppedState && source.toString() !== "") page.playingId = ""
    }

    ColumnLayout {
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.topMargin: Theme.spacing * 2
        anchors.bottomMargin: Theme.spacing
        width: page.columnWidth
        spacing: Theme.spacing

        // ---- Header ----
        RowLayout {
            Layout.fillWidth: true
            Text {
                text: qsTr("Events")
                color: Theme.text
                font.pixelSize: 20
                font.bold: true
            }
            Text {
                text: Events.count > 0 ? Events.count : ""
                color: Theme.textMuted
                font.pixelSize: 13
                Layout.alignment: Qt.AlignBottom
                bottomPadding: 2
            }
            Item { Layout.fillWidth: true }
            Rectangle {
                visible: Events.count > 0
                implicitWidth: clearText.implicitWidth + 20
                height: 30
                radius: Theme.radius
                color: clearHover.hovered ? Theme.surfaceAlt : "transparent"
                border.color: Theme.border
                Text {
                    id: clearText
                    anchors.centerIn: parent
                    text: qsTr("Clear all")
                    color: Theme.textMuted
                    font.pixelSize: 12
                }
                HoverHandler { id: clearHover }
                TapHandler { onTapped: Events.clear() }
            }
        }

        // ---- Audio view switch (only when the audio service is set up) ----
        Row {
            visible: AudioAlerts.enabled
            spacing: 6
            Repeater {
                model: [ { audio: false, label: qsTr("Detections") },
                         { audio: true, label: qsTr("Audio") + (AudioAlerts.count > 0 ? " (" + AudioAlerts.count + ")" : "") } ]
                Rectangle {
                    required property var modelData
                    property bool sel: page.showAudio === modelData.audio
                    implicitWidth: viewText.implicitWidth + 24
                    height: 30
                    radius: 15
                    color: sel ? Theme.accentDim : Theme.surface
                    border.color: sel ? Theme.accent : Theme.border
                    Text { id: viewText; anchors.centerIn: parent; text: parent.modelData.label
                           color: parent.sel ? Theme.text : Theme.textMuted; font.pixelSize: 12 }
                    TapHandler { onTapped: page.showAudio = parent.modelData.audio }
                }
            }
        }

        // ---- Transcription paused notice ----
        Rectangle {
            visible: AudioAlerts.enabled && AudioAlerts.transcriptionPaused
            Layout.fillWidth: true
            implicitHeight: pausedRow.implicitHeight + 16
            radius: Theme.radius
            color: Theme.surface
            border.color: Theme.danger
            RowLayout {
                id: pausedRow
                anchors.fill: parent
                anchors.margins: 8
                spacing: 10
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: Theme.text
                    font.pixelSize: 12
                    text: qsTr("Transcription paused: the server was overloaded. Loud voices are still detected, but threatening words are not until you turn it back on.")
                          + (AudioAlerts.pausedReason.length > 0 ? " (" + AudioAlerts.pausedReason + ")" : "")
                }
                Rectangle {
                    implicitWidth: resumeText.implicitWidth + 20
                    height: 28
                    radius: Theme.radius
                    color: resumeHover.hovered ? Theme.accentDim : "transparent"
                    border.color: Theme.accent
                    Text { id: resumeText; anchors.centerIn: parent; text: qsTr("Turn back on"); color: Theme.text; font.pixelSize: 12 }
                    HoverHandler { id: resumeHover }
                    TapHandler { onTapped: AudioAlerts.resumeTranscription() }
                }
            }
        }

        // ---- Audio events ----
        Text {
            visible: page.showAudio && AudioAlerts.enabled && !AudioAlerts.reachable
            color: Theme.textMuted
            font.pixelSize: 12
            text: qsTr("The audio service is not answering.")
        }
        Text {
            visible: page.showAudio && page.clipError.length > 0
            color: Theme.danger
            font.pixelSize: 12
            text: page.clipError
        }
        ListView {
            id: audioList
            visible: page.showAudio
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: AudioAlerts
            clip: true
            spacing: 6
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            delegate: Rectangle {
                required property string eventId
                required property string camera
                required property string timeText
                required property string triggerText
                required property string scoreText
                required property string transcript
                required property bool hasClip
                width: ListView.view.width
                height: audioCol.implicitHeight + 20
                radius: Theme.radius
                color: Theme.surface
                border.color: Qt.rgba(1, 1, 1, 0.04)
                Rectangle { anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                            width: 3; radius: Theme.radius; color: Theme.danger }
                ColumnLayout {
                    id: audioCol
                    anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                    anchors.leftMargin: 14; anchors.rightMargin: 14; anchors.topMargin: 10
                    spacing: 4
                    RowLayout {
                        Layout.fillWidth: true
                        Text { text: triggerText; color: Theme.danger; font.pixelSize: 14; font.bold: true }
                        Text { text: camera; color: Theme.text; font.pixelSize: 13; Layout.fillWidth: true; elide: Text.ElideRight }
                        Text { text: timeText; color: Theme.textMuted; font.pixelSize: 12 }
                    }
                    Text { visible: scoreText.length > 0; text: scoreText; color: Theme.textMuted; font.pixelSize: 11 }
                    Text {
                        visible: transcript.length > 0
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: "\u201c" + transcript + "\u201d"
                        color: Theme.text
                        font.pixelSize: 13
                        font.italic: true
                    }
                    Text {
                        visible: hasClip
                        text: page.playingId === eventId ? qsTr("\u25a0 Stop clip") : qsTr("\u25b6 Play clip")
                        color: Theme.accent
                        font.pixelSize: 12
                        TapHandler { onTapped: page.toggleClip(eventId) }
                    }
                }
            }
            Text {
                anchors.centerIn: parent
                visible: AudioAlerts.count === 0
                text: qsTr("No audio events")
                color: Theme.textMuted
                font.pixelSize: 13
            }
        }

        // ---- Filter chips ----
        Flow {
            visible: !page.showAudio
            Layout.fillWidth: true
            spacing: 6
            Repeater {
                model: [
                    { key: "", label: qsTr("All") },
                    { key: "person", label: qsTr("Person") },
                    { key: "vehicle", label: qsTr("Vehicle") },
                    { key: "pet", label: qsTr("Pet") },
                    { key: "visitor", label: qsTr("Visitor") },
                    { key: "motion", label: qsTr("Motion") }
                ]
                Rectangle {
                    required property var modelData
                    property bool sel: Events.filter === modelData.key
                    implicitWidth: chipText.implicitWidth + 24
                    height: 30
                    radius: 15
                    color: sel ? Theme.accentDim : (chipHover.hovered ? Theme.surfaceAlt : Theme.surface)
                    border.color: sel ? Theme.accent : Theme.border
                    Text {
                        id: chipText
                        anchors.centerIn: parent
                        text: parent.modelData.label
                        color: parent.sel ? Theme.text : Theme.textMuted
                        font.pixelSize: 12
                    }
                    HoverHandler { id: chipHover }
                    TapHandler { onTapped: Events.filter = parent.modelData.key }
                }
            }

            // Camera filter: a chip-styled dropdown listing the cameras.
            Rectangle {
                id: camChip
                property bool active: Events.cameraFilter !== ""
                implicitWidth: camChipText.implicitWidth + 34
                height: 30
                radius: 15
                color: active ? Theme.accentDim : (camChipHover.hovered ? Theme.surfaceAlt : Theme.surface)
                border.color: active ? Theme.accent : Theme.border
                Row {
                    anchors.centerIn: parent
                    spacing: 5
                    Text {
                        id: camChipText
                        text: camChip.active ? Events.cameraFilter : qsTr("All cameras")
                        color: camChip.active ? Theme.text : Theme.textMuted
                        font.pixelSize: 12
                    }
                    Text { text: "\u25be"; color: Theme.textMuted; font.pixelSize: 10
                           anchors.verticalCenter: parent.verticalCenter }
                }
                HoverHandler { id: camChipHover }
                TapHandler { onTapped: camMenu.popup() }
                ThemedMenu {
                    id: camMenu
                    ThemedMenuItem { text: qsTr("All cameras")
                                     onTriggered: Events.cameraFilter = "" }
                    ThemedMenuSeparator {}
                    Instantiator {
                        model: Devices
                        delegate: ThemedMenuItem {
                            required property string name
                            text: name
                            onTriggered: Events.cameraFilter = name
                        }
                        onObjectAdded: (i, o) => camMenu.insertItem(i + 2, o)
                        onObjectRemoved: (i, o) => camMenu.removeItem(o)
                    }
                }
            }
        }

        // ---- Event list ----
        ListView {
            visible: !page.showAudio
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: Events
            clip: true
            spacing: 6
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: Rectangle {
                required property int index
                required property string type
                required property string camera
                required property string timeText
                required property string thumbnail
                required property var hostId
                required property var channel
                required property var timestamp

                width: ListView.view.width
                height: 72
                radius: Theme.radius
                color: rowHover.hovered ? Theme.surfaceAlt : Theme.surface
                border.color: rowHover.hovered ? Theme.border : Qt.rgba(1, 1, 1, 0.04)

                // Left accent bar in the event's type color.
                Rectangle {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: 3
                    radius: Theme.radius
                    color: page.colorFor(type)
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 14
                    anchors.topMargin: 8
                    anchors.bottomMargin: 8
                    spacing: 12

                    // Thumbnail (16:9) or type icon
                    Rectangle {
                        Layout.preferredWidth: 96
                        Layout.fillHeight: true
                        radius: 4
                        color: Theme.paneBackground
                        clip: true
                        Image {
                            anchors.fill: parent
                            source: thumbnail.length > 0 ? "file://" + thumbnail : ""
                            fillMode: Image.PreserveAspectCrop
                            visible: thumbnail.length > 0
                        }
                        Text {
                            anchors.centerIn: parent
                            visible: thumbnail.length === 0
                            text: page.iconFor(type)
                            font.pixelSize: 24
                            opacity: 0.85
                        }
                    }

                    // Type + camera
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 3
                        Text {
                            text: page.labelFor(type)
                            color: page.colorFor(type)
                            font.pixelSize: 14
                            font.bold: true
                        }
                        Text {
                            text: camera
                            color: Theme.text
                            font.pixelSize: 13
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                    }

                    // Time + affordance
                    ColumnLayout {
                        Layout.alignment: Qt.AlignRight
                        spacing: 3
                        Text {
                            text: timeText
                            color: Theme.textMuted
                            font.pixelSize: 12
                            Layout.alignment: Qt.AlignRight
                        }
                        Text {
                            text: qsTr("View ›")
                            color: rowHover.hovered ? Theme.accent : Theme.textMuted
                            font.pixelSize: 11
                            Layout.alignment: Qt.AlignRight
                        }
                    }
                }

                HoverHandler { id: rowHover }
                TapHandler { onTapped: page.jumpToPlayback(hostId, channel, timestamp) }
            }

            // Empty state
            Column {
                anchors.centerIn: parent
                visible: Events.count === 0
                spacing: 8
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "🔔"; font.pixelSize: 40; opacity: 0.7
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: Events.filter.length > 0 ? qsTr("No events of this type")
                                                   : qsTr("No events yet")
                    color: Theme.text
                    font.pixelSize: 14
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("Detections from your cameras appear here")
                    color: Theme.textMuted
                    font.pixelSize: 12
                }
            }
        }
    }
}
