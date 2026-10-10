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
    signal openFootagePending(string camera, string when, string transcript, string modelLabel)
    signal footageFetched(string url)
    signal footageFailedMsg(string message)

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
    // The NVR keeps recordings 7 days. Within that, an audio event's picture and sound come
    // from the NVR itself (fetched over its download API, not a streaming session).
    readonly property real nvrKeepSecs: 7 * 86400 - 3600
    property int footageRow: -1
    property var footageHostId: -1
    property int footageChannel: 0
    property real footageStart: 0
    property string footageWhen: ""
    property string footageState: ""   // "" | "loading" | error text
    function footageAvailable(cameraKey, endTime) {
        return Devices.rowOfName(cameraKey) >= 0 && (Date.now() / 1000 - endTime) < nvrKeepSecs;
    }
    function watchFootage(cameraKey, startTime, endTime, whenText, transcript, modelLabel) {
        var row = Devices.rowOfName(cameraKey);
        if (row < 0) { footageState = qsTr("Camera not found: no camera named \u201c%1\u201d.").arg(cameraKey); return; }
        footageRow = row;
        footageHostId = Devices.hostIdOf(row);
        footageChannel = Devices.channelOf(row);
        footageStart = Math.floor(startTime) - 5;
        footageWhen = whenText;
        footageState = "";
        // The player opens at once and shows progress; the recording is fetched behind it.
        page.openFootagePending(Devices.nameAt(row), whenText, transcript, modelLabel || "");
        var from = Math.floor(startTime) - 3;
        Devices.fetchFootage(row, from, Math.max(6, Math.ceil(endTime - startTime) + 6));
    }
    Connections {
        target: Devices
        function onFootageReady(row, path) {
            if (row !== page.footageRow) return;
            page.footageFetched("file://" + path);
        }
        function onFootageFailed(row, error) {
            if (row !== page.footageRow) return;
            page.footageFailedMsg(qsTr("Could not get the recording from the NVR: %1. If it is busy, close a live view and try again.").arg(error));
        }
    }
    onShowAudioChanged: if (!showAudio) { stopClip(); AudioAlerts.showAll = false; }
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
            // Re-pull each Frigate property's own event history — the inbox only
            // ever files what it saw live, so a quiet camera (or one that was
            // cleared) can lag behind what Frigate itself actually recorded.
            Rectangle {
                implicitWidth: syncText.implicitWidth + 20
                height: 30
                radius: Theme.radius
                color: syncHover.hovered ? Theme.surfaceAlt : "transparent"
                border.color: Theme.border
                Text {
                    id: syncText
                    anchors.centerIn: parent
                    text: qsTr("Sync Frigate history")
                    color: Theme.textMuted
                    font.pixelSize: 12
                }
                HoverHandler { id: syncHover }
                TapHandler { onTapped: Events.syncAllFrigateHistory() }
            }
            Rectangle {
                implicitWidth: notifText.implicitWidth + 20
                height: 30
                radius: Theme.radius
                color: notifHover.hovered ? Theme.surfaceAlt : "transparent"
                border.color: Theme.border
                Text {
                    id: notifText
                    anchors.centerIn: parent
                    text: Events.desktopNotifications ? qsTr("Notifications: On") : qsTr("Notifications: Off")
                    color: Events.desktopNotifications ? Theme.text : Theme.textMuted
                    font.pixelSize: 12
                }
                HoverHandler { id: notifHover }
                TapHandler { onTapped: Events.desktopNotifications = !Events.desktopNotifications }
            }
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
        Row {
            visible: page.showAudio && AudioAlerts.enabled
            spacing: 6
            Repeater {
                model: [ { all: false, label: qsTr("Alerts only") }, { all: true, label: qsTr("All speech (7 days, as on the NVR)") } ]
                Rectangle {
                    required property var modelData
                    property bool sel: AudioAlerts.showAll === modelData.all
                    implicitWidth: scopeText.implicitWidth + 24
                    height: 28
                    radius: 14
                    color: sel ? Theme.accentDim : Theme.surface
                    border.color: sel ? Theme.accent : Theme.border
                    Text { id: scopeText; anchors.centerIn: parent; text: parent.modelData.label
                           color: parent.sel ? Theme.text : Theme.textMuted; font.pixelSize: 12 }
                    TapHandler { onTapped: AudioAlerts.showAll = parent.modelData.all }
                }
            }
        }
        Text {
            visible: page.showAudio && AudioAlerts.enabled && !AudioAlerts.reachable
            color: Theme.textMuted
            font.pixelSize: 12
            text: qsTr("The audio service is not answering.")
        }
        Text {
            visible: page.showAudio && page.footageState.length > 0
            color: page.footageState === "loading" ? Theme.textMuted : Theme.danger
            font.pixelSize: 12
            wrapMode: Text.Wrap
            Layout.fillWidth: true
            text: page.footageState === "loading" ? qsTr("Fetching the recording from the NVR\u2026") : page.footageState
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
                required property string sttModel
                required property string megaAsrTranscript
                required property string megaAsrModel
                required property bool hasClip
                required property bool flagged
                required property string cameraKey
                required property var startTime
                required property var endTime
                width: ListView.view.width
                height: audioCol.implicitHeight + 20
                radius: Theme.radius
                color: Theme.surface
                border.color: Qt.rgba(1, 1, 1, 0.04)
                Rectangle { anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                            width: 3; radius: Theme.radius; color: flagged ? Theme.danger : Theme.border }
                ColumnLayout {
                    id: audioCol
                    anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                    anchors.leftMargin: 14; anchors.rightMargin: 14; anchors.topMargin: 10
                    spacing: 4
                    RowLayout {
                        Layout.fillWidth: true
                        Text { text: triggerText; color: flagged ? Theme.danger : Theme.textMuted; font.pixelSize: 14; font.bold: true }
                        Text { text: camera; color: Theme.text; font.pixelSize: 13; Layout.fillWidth: true; elide: Text.ElideRight }
                        Text { text: timeText; color: Theme.textMuted; font.pixelSize: 12 }
                    }
                    Text { visible: scoreText.length > 0; text: scoreText; color: Theme.textMuted; font.pixelSize: 11 }
                    // Two independent transcripts can exist for the same moment: the fast model
                    // that ran at trigger time, and (for raised_voice/threat_speech, added later
                    // and asynchronously) ticket 19's bigger backlog-pass model. Each labeled with
                    // whichever model actually produced it, never assumed from current config.
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        visible: transcript.length > 0
                        Text { text: sttModel; visible: sttModel.length > 0; color: Theme.textMuted; font.pixelSize: 10 }
                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: "\u201c" + transcript + "\u201d"
                            color: Theme.text
                            font.pixelSize: 13
                            font.italic: true
                            // Tapping the words opens the recording of that moment.
                            TapHandler {
                                enabled: page.footageAvailable(cameraKey, endTime)
                                onTapped: page.watchFootage(cameraKey, startTime, endTime, timeText, transcript, sttModel)
                            }
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        visible: megaAsrTranscript.length > 0
                        Text { text: megaAsrModel; color: Theme.textMuted; font.pixelSize: 10 }
                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: "\u201c" + megaAsrTranscript + "\u201d"
                            color: Theme.text
                            font.pixelSize: 13
                            font.italic: true
                            TapHandler {
                                enabled: page.footageAvailable(cameraKey, endTime)
                                onTapped: page.watchFootage(cameraKey, startTime, endTime, timeText, megaAsrTranscript, megaAsrModel)
                            }
                        }
                    }
                    Row {
                        spacing: 16
                        Text {
                            visible: page.footageAvailable(cameraKey, endTime)
                            text: qsTr("\u25b6 Watch on NVR")
                            color: Theme.accent
                            font.pixelSize: 12
                            TapHandler {
                                onTapped: {
                                    var t = transcript.length > 0 ? transcript : megaAsrTranscript;
                                    var m = transcript.length > 0 ? sttModel : megaAsrModel;
                                    page.watchFootage(cameraKey, startTime, endTime, timeText, t, m);
                                }
                            }
                        }
                        Text {
                            visible: hasClip
                            text: page.playingId === eventId ? qsTr("\u25a0 Stop saved audio") : qsTr("\u25b6 Play saved audio")
                            color: Theme.accent
                            font.pixelSize: 12
                            TapHandler { onTapped: page.toggleClip(eventId) }
                        }
                        Text {
                            visible: !hasClip && !page.footageAvailable(cameraKey, endTime)
                            text: qsTr("Recording no longer on the NVR")
                            color: Theme.textMuted
                            font.pixelSize: 11
                        }
                    }
                }
            }
            footer: Item {
                width: audioList.width
                height: AudioAlerts.canLoadMore ? 40 : 0
                visible: AudioAlerts.canLoadMore
                Text { anchors.centerIn: parent; text: qsTr("Load older"); color: Theme.accent; font.pixelSize: 12
                       TapHandler { onTapped: AudioAlerts.loadMore() } }
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

            // Property filter: a chip-styled dropdown listing the properties/sites
            // (e.g. "Lascelles", "Farm"). Cameras with the same name on different
            // properties (two "Kitchen"s) only get told apart once this is combined
            // with the camera filter below, so picking a camera also pins its property.
            Rectangle {
                id: propChip
                property bool active: Events.propertyFilter !== ""
                implicitWidth: propChipText.implicitWidth + 34
                height: 30
                radius: 15
                color: active ? Theme.accentDim : (propChipHover.hovered ? Theme.surfaceAlt : Theme.surface)
                border.color: active ? Theme.accent : Theme.border
                Row {
                    anchors.centerIn: parent
                    spacing: 5
                    Text {
                        id: propChipText
                        text: propChip.active ? Events.propertyFilter : qsTr("All properties")
                        color: propChip.active ? Theme.text : Theme.textMuted
                        font.pixelSize: 12
                    }
                    Text { text: "\u25be"; color: Theme.textMuted; font.pixelSize: 10
                           anchors.verticalCenter: parent.verticalCenter }
                }
                HoverHandler { id: propChipHover }
                TapHandler { onTapped: propMenu.popup() }
                ThemedMenu {
                    id: propMenu
                    // Devices.count is read only to make this recompute when the
                    // device list changes; propertyNames() itself isn't reactive.
                    property var names: { Devices.count; return Devices.propertyNames() }
                    ThemedMenuItem {
                        text: qsTr("All properties")
                        onTriggered: { Events.propertyFilter = ""; Events.cameraFilter = "" }
                    }
                    ThemedMenuSeparator {}
                    Instantiator {
                        model: propMenu.names
                        delegate: ThemedMenuItem {
                            required property string modelData
                            text: modelData
                            onTriggered: { Events.propertyFilter = modelData; Events.cameraFilter = "" }
                        }
                        onObjectAdded: (i, o) => propMenu.insertItem(i + 2, o)
                        onObjectRemoved: (i, o) => propMenu.removeItem(o)
                    }
                }
            }

            // Camera filter: a chip-styled dropdown listing the cameras, scoped to
            // the selected property (if any). Picking one pins its property too, so
            // two same-named cameras on different properties can't be confused.
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
                            required property string hostName
                            text: hostName.length > 0 ? name + " \u2014 " + hostName : name
                            visible: Events.propertyFilter === "" || hostName === Events.propertyFilter
                            height: visible ? implicitHeight : 0
                            onTriggered: {
                                Events.propertyFilter = hostName
                                Events.cameraFilter = name
                            }
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
                required property string property
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
                            text: property.length > 0 ? camera + "  ·  " + property : camera
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
                    text: (Events.filter.length > 0 || Events.cameraFilter.length > 0
                           || Events.propertyFilter.length > 0)
                          ? qsTr("No events match this filter")
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

        // ---- History depth ----
        // The inbox only ever holds events seen live since it was last connected,
        // capped at 100 total across every camera on every property, so a busy
        // camera can push a quiet one's older events out. This says plainly how
        // far back the current view actually reaches.
        Text {
            Layout.fillWidth: true
            Layout.topMargin: 2
            visible: Events.count > 0
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            font.pixelSize: 11
            text: {
                const oldest = Events.oldestVisibleTimestamp
                if (oldest <= 0)
                    return ""
                const when = Qt.formatDateTime(new Date(oldest * 1000), "MMM d, yyyy hh:mm")
                return Events.historyTrimmed
                       ? qsTr("Showing back to %1 — older events were trimmed to keep the shared 100-event limit").arg(when)
                       : qsTr("Showing the complete history, back to %1").arg(when)
            }
        }
    }
}
