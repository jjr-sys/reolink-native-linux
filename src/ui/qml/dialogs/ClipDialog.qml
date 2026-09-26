import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import ReolinkApp
import ReolinkApp.Core

// Plays the recorded clip for a detection on a camera whose recordings live on a
// server of their own (Frigate). The server cuts the clip when asked, so the first
// picture takes a moment. Play/pause, a scrub bar, speed, volume and an A-B loop.
//
// Keys: Space play/pause, Left/Right ±5 s (Shift ±1 s), [ set loop start, ] set loop end,
// L loop on/off, Backspace clear loop, M mute, Home/End jump to start/end.
Dialog {
    id: dialog
    modal: true
    width: Math.min(900, Overlay.overlay ? Overlay.overlay.width - 60 : 900)
    height: width * 9 / 16 + 240 + (transcript !== "" ? 70 : 0)
    anchors.centerIn: Overlay.overlay
    padding: Theme.spacing * 2

    property string camera: ""
    property string when: ""
    property string url: ""
    // Audio events: what was said, and the state of an NVR fetch still in progress.
    property string transcript: ""
    property bool pending: false
    property string pendingError: ""

    // Loop segment, in ms. loopEnd <= 0 means "to the end of the clip".
    property bool loopOn: false
    property real loopStart: 0
    property real loopEnd: 0
    readonly property real duration: Math.max(0, player.duration > 0 ? player.duration : loader.durationMs)
    readonly property real effLoopEnd: loopEnd > loopStart ? loopEnd : duration
    readonly property bool hasSegment: loopStart > 0 || (loopEnd > 0 && loopEnd < duration)
    readonly property bool loading: loader.state === ClipLoader.Loading
                                    || (loader.state === ClipLoader.Ready
                                        && (player.mediaStatus === MediaPlayer.LoadingMedia
                                            || player.mediaStatus === MediaPlayer.NoMedia))
    readonly property bool failed: loader.state === ClipLoader.Error || player.error !== MediaPlayer.NoError
    readonly property string failText: loader.state === ClipLoader.Error ? loader.errorString : player.errorString
    readonly property var speeds: [0.25, 0.5, 1, 1.5, 2]

    // The click that closes the dialog is also delivered to whatever is under it. On the
    // Events page that is a row, whose tap opens the clip again. Ignore opens that arrive
    // right after a close.
    property double closedAt: 0
    function openClip(cameraName, timeText, clipUrl, transcriptText) {
        if (Date.now() - closedAt < 500)
            return;
        camera = cameraName;
        when = timeText;
        url = clipUrl;
        transcript = transcriptText || "";
        pending = false;
        pendingError = "";
        open();
    }
    // Opens at once while the recording is still being fetched from the NVR; provide() or
    // fail() finish it.
    function openPending(cameraName, timeText, transcriptText) {
        if (Date.now() - closedAt < 500)
            return;
        camera = cameraName;
        when = timeText;
        url = "";
        transcript = transcriptText || "";
        pending = true;
        pendingError = "";
        open();
    }
    function provide(clipUrl) {
        if (!visible) return;
        pending = false;
        url = clipUrl;
        play();
    }
    function fail(message) {
        if (!visible) return;
        pending = false;
        pendingError = message;
    }
    function play() {
        // Frigate's clip is an unindexed stream (no duration, no seeking). Fetch it and
        // make it a normal MP4 first; the player starts when the file is ready.
        clearLoop();
        player.stop();
        player.source = "";
        loader.load(url);
    }
    function clearLoop() { loopOn = false; loopStart = 0; loopEnd = 0; }
    function seekTo(ms) {
        if (duration <= 0) return;
        player.position = Math.max(0, Math.min(duration, ms));
    }
    function togglePause() {
        if (player.playbackState === MediaPlayer.PlayingState) {
            player.pause();
        } else {
            // Finished (or parked at the end): start again from the loop start / beginning.
            if (player.mediaStatus === MediaPlayer.EndOfMedia || player.position >= effLoopEnd - 50)
                seekTo(loopOn ? loopStart : 0);
            player.play();
        }
    }
    function markStart() {
        if (duration <= 0) return;
        loopStart = Math.min(player.position, duration - 100);
        if (loopEnd <= loopStart) loopEnd = 0;
        loopOn = true;
    }
    function markEnd() {
        if (duration <= 0) return;
        if (player.position <= loopStart + 100) return; // an end before the start is meaningless
        loopEnd = player.position;
        loopOn = true;
    }
    function fmt(ms) {
        var s = Math.max(0, ms) / 1000;
        var m = Math.floor(s / 60);
        var r = s - m * 60;
        return m + ":" + (r < 10 ? "0" : "") + r.toFixed(1);
    }

    onOpened: { body.forceActiveFocus(); if (!pending && url !== "") play(); }
    onClosed: { closedAt = Date.now(); pending = false; pendingError = ""; loader.cancel(); player.stop(); player.source = ""; }

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

    ClipLoader {
        id: loader
        onStateChanged: {
            if (state === ClipLoader.Ready) {
                player.source = localUrl;
                player.play();
            }
        }
    }

    MediaPlayer {
        id: player
        videoOutput: video
        audioOutput: AudioOutput { id: audio }
        loops: 1
        playbackRate: dialog.speeds[speedIndex]
        property int speedIndex: 2

        // Seeking from inside positionChanged is ignored by the backend, so wrap on the next tick.
        property Timer wrap: Timer {
            interval: 0
            onTriggered: player.position = dialog.loopStart
        }
        // Keep the position inside the loop segment. The backend reports position in
        // coarse steps, so the jump back lands a little after the end mark at most.
        onPositionChanged: {
            if (dialog.loopOn && dialog.loopEnd > dialog.loopStart && position >= dialog.loopEnd
                    && playbackState === MediaPlayer.PlayingState)
                wrap.start();
        }
        // Reaching the real end of the clip: wrap to the loop start when looping.
        onMediaStatusChanged: {
            if (mediaStatus === MediaPlayer.EndOfMedia && dialog.loopOn) {
                position = dialog.loopStart;
                play();
            }
        }
    }

    ColumnLayout {
        id: body
        anchors.fill: parent
        spacing: Theme.spacing
        focus: true
        Keys.onPressed: (e) => {
            var step = (e.modifiers & Qt.ShiftModifier) ? 1000 : 5000;
            switch (e.key) {
            case Qt.Key_Space: togglePause(); break;
            case Qt.Key_Left: seekTo(player.position - step); break;
            case Qt.Key_Right: seekTo(player.position + step); break;
            case Qt.Key_Home: seekTo(0); break;
            case Qt.Key_End: seekTo(duration); break;
            case Qt.Key_BracketLeft: markStart(); break;
            case Qt.Key_BracketRight: markEnd(); break;
            case Qt.Key_L: loopOn = !loopOn; break;
            case Qt.Key_Backspace: clearLoop(); break;
            case Qt.Key_M: audio.muted = !audio.muted; break;
            default: return;
            }
            e.accepted = true;
        }


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
            TapHandler { onTapped: dialog.togglePause() }
            Text {
                anchors.centerIn: parent
                visible: dialog.failed || dialog.loading || dialog.pending || dialog.pendingError !== ""
                width: parent.width - 40
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                color: (dialog.failed || dialog.pendingError !== "") ? Theme.danger : Theme.textMuted
                font.pixelSize: 13
                text: dialog.pendingError !== "" ? dialog.pendingError
                      : dialog.failed ? qsTr("Could not play the clip: %1").arg(dialog.failText)
                      : dialog.pending ? qsTr("Fetching the recording from the NVR…")
                                       : qsTr("Getting the clip…")
            }
        }

        // ---- What was said (audio events) ----
        Text {
            visible: dialog.transcript !== ""
            Layout.fillWidth: true
            Layout.maximumHeight: 64
            wrapMode: Text.Wrap
            elide: Text.ElideRight
            color: Theme.text
            font.pixelSize: 14
            font.italic: true
            text: "\u201c" + dialog.transcript + "\u201d"
        }

        // ---- Scrub bar ----
        Item {
            id: scrub
            Layout.fillWidth: true
            implicitHeight: 28
            enabled: dialog.duration > 0

            readonly property real pad: 6
            readonly property real trackW: width - pad * 2
            function xOf(ms) { return pad + (dialog.duration > 0 ? ms / dialog.duration : 0) * trackW; }
            function msAt(x) { return Math.max(0, Math.min(1, (x - pad) / trackW)) * dialog.duration; }

            Rectangle { // track
                id: track
                x: scrub.pad; width: scrub.trackW
                anchors.verticalCenter: parent.verticalCenter
                height: 6; radius: 3
                color: Theme.surfaceAlt
                border.color: Theme.border
            }
            Rectangle { // loop segment
                visible: dialog.hasSegment
                x: scrub.xOf(dialog.loopStart)
                width: Math.max(2, scrub.xOf(dialog.effLoopEnd) - x)
                anchors.verticalCenter: parent.verticalCenter
                height: 10; radius: 2
                color: dialog.loopOn ? Theme.accentDim : Theme.border
                opacity: 0.85
            }
            Rectangle { // played
                x: scrub.pad
                width: Math.max(0, scrub.xOf(player.position) - scrub.pad)
                anchors.verticalCenter: parent.verticalCenter
                height: 6; radius: 3
                color: Theme.accent
            }
            Repeater { // loop A / B markers
                model: dialog.hasSegment ? [dialog.loopStart, dialog.effLoopEnd] : []
                Rectangle {
                    required property real modelData
                    x: scrub.xOf(modelData) - 1
                    width: 2; height: 18
                    anchors.verticalCenter: parent.verticalCenter
                    color: Theme.text
                }
            }
            Rectangle { // playhead
                x: scrub.xOf(player.position) - 6
                width: 12; height: 12; radius: 6
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.text
                border.color: Theme.accent
                border.width: 2
                scale: scrubArea.pressed ? 1.25 : 1
            }
            // Where a click would land, while hovering.
            Text {
                visible: scrubArea.containsMouse && dialog.duration > 0
                x: Math.max(0, Math.min(scrub.width - width, scrubArea.mouseX - width / 2))
                y: -14
                text: dialog.fmt(scrub.msAt(scrubArea.mouseX))
                color: Theme.textMuted
                font.pixelSize: 11
            }
            MouseArea {
                id: scrubArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onPressed: (m) => dialog.seekTo(scrub.msAt(m.x))
                onPositionChanged: (m) => { if (pressed) dialog.seekTo(scrub.msAt(m.x)); }
            }
        }

        // ---- Controls: two rows so nothing overflows in a small window ----
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing

            Btn {
                label: player.playbackState === MediaPlayer.PlayingState ? "⏸" : "▶"
                onTapped: dialog.togglePause()
            }
            Btn { label: "−5s"; onTapped: dialog.seekTo(player.position - 5000) }
            Btn { label: "+5s"; onTapped: dialog.seekTo(player.position + 5000) }
            Text {
                text: dialog.fmt(player.position) + " / " + dialog.fmt(dialog.duration)
                color: Theme.textMuted
                font.pixelSize: 12
                font.family: "monospace"
                Layout.leftMargin: Theme.spacing
            }
            Item { Layout.fillWidth: true }
            Btn {
                label: dialog.speeds[player.speedIndex] + "×"
                onTapped: player.speedIndex = (player.speedIndex + 1) % dialog.speeds.length
            }
            Btn { label: audio.muted ? "Unmute" : "Mute"; onTapped: audio.muted = !audio.muted }
            Slider {
                id: vol
                Layout.preferredWidth: 90
                from: 0; to: 1; value: 1
                onMoved: audio.volume = QtMultimedia.convertVolume(value, QtMultimedia.LinearVolumeScale,
                                                                   QtMultimedia.LogarithmicVolumeScale)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing

            // Loop: mark A and B at the playhead, then the segment repeats.
            Btn { label: qsTr("Set A  [") ; active: dialog.loopStart > 0; onTapped: dialog.markStart() }
            Btn { label: qsTr("Set B  ]"); active: dialog.loopEnd > 0; onTapped: dialog.markEnd() }
            Btn { label: qsTr("Loop"); active: dialog.loopOn; onTapped: dialog.loopOn = !dialog.loopOn }
            Text {
                visible: dialog.hasSegment
                text: dialog.fmt(dialog.loopStart) + " – " + dialog.fmt(dialog.effLoopEnd)
                color: dialog.loopOn ? Theme.accent : Theme.textMuted
                font.pixelSize: 12
                font.family: "monospace"
            }
            Btn { label: qsTr("Clear"); visible: dialog.hasSegment || dialog.loopOn; onTapped: dialog.clearLoop() }
            Item { Layout.fillWidth: true }
            Btn { label: qsTr("Replay"); onTapped: dialog.play() }
            Btn { objectName: "closeButton"; label: qsTr("Close"); onTapped: dialog.close() }
        }
    }

    component Btn: Rectangle {
        id: btn
        property string label
        property bool active: false
        signal tapped()
        implicitWidth: Math.max(36, btnText.implicitWidth + 20)
        implicitHeight: 32
        radius: Theme.radius
        color: active ? Theme.accentDim : (btnHover.hovered ? Theme.surfaceAlt : "transparent")
        border.color: active ? Theme.accent : Theme.border
        Text { id: btnText; anchors.centerIn: parent; text: btn.label; color: Theme.text; font.pixelSize: 13 }
        HoverHandler { id: btnHover }
        TapHandler { onTapped: btn.tapped() }
    }
}
