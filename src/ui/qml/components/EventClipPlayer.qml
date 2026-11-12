import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import ReolinkApp
import ReolinkApp.Core

// The Events page's clip player (wayfinder/events-feed step 22): the picked event's footage
// beside the feed. A Reolink recorder plays its sub stream (HD on request) from 5 s before
// the event through Playback's engine at 1×; a Frigate camera plays Frigate's own clip
// from 15 s before. The event's snapshot stays up until the first picture. At +30 s it
// stops (freeing the recorder's connection, keeping the last picture) with Keep playing
// and Open in Playback. Sound starts muted. EventClip holds the window maths.
Item {
    id: root

    property bool hd: false
    property bool soundOn: false
    property bool closable: true
    readonly property bool active: clip.active
    readonly property real eventId: _eventId
    signal openInPlayback(var hostId, var channel, var timestamp)
    signal stepRequested(int direction)   // ↑ -1 newer, ↓ +1 older
    signal closed()

    property string _title: ""
    property string _thumbnail: ""
    property real _eventId: -1
    // "idle", "starting", "playing", "ended" (stopped at the window's end), "failed"
    property string phase: "idle"
    property real _openedAt: 0      // the epoch the stream was opened at
    property real _requestedMs: 0
    property real _firstFrameMs: 0
    property real _nowMs: 0
    property bool _hadPicture: false   // this event showed a picture (a reconnect keeps it up)

    EventClip { id: clip; devices: Devices }

    // Where the picture is, in wall-clock seconds.
    readonly property real position: clip.frigate
        ? clip.from + media.position / 1000
        : (_firstFrameMs > 0 ? _openedAt + (Math.max(_nowMs, _firstFrameMs) - _firstFrameMs) / 1000 : _openedAt)
    Timer {
        interval: 200
        repeat: true
        running: root.phase === "playing" && !clip.frigate
        onTriggered: root._nowMs = Date.now()
    }
    onPositionChanged: if (phase === "playing" && clip.atEnd(position)) root._reachEnd()
    // A recorder that doesn't answer is retried; give up on a picture after a while
    // (a Frigate clip is cut on request, so it gets longer).
    Timer {
        running: root.phase === "starting"
        interval: clip.frigate ? 20000 : 10000
        onTriggered: { root._stopStreams(); root.phase = "failed"; }
    }

    // Play an event: { hostId, channel, timestamp, thumbnail, title, eventId }.
    function play(ev) {
        phase = "idle";   // first: the old position must not end the new window
        _hadPicture = false;
        _stopStreams();
        _title = ev.title;
        _thumbnail = ev.thumbnail || "";
        _eventId = ev.eventId;
        clip.setEvent(ev.hostId, ev.channel, ev.timestamp);
        _open(clip.from);
    }
    function close() {
        _stopStreams();
        clip.clear();
        _eventId = -1;
        phase = "idle";
        closed();
    }
    // Leaving the page or hiding the window: stop, and free the recorder.
    function stop() { if (clip.active) close(); }

    function _open(at) {
        phase = "starting";
        _firstFrameMs = 0;
        _nowMs = 0;
        _requestedMs = Date.now();
        _openedAt = at;
        if (clip.frigate) {
            loader.load(clip.clipUrl);
            return;
        }
        session.deviceRow = Devices.rowOfHostChannel(clip.hostId, clip.channel);
        const covered = true; // the feed's event is the timeline's verdict
        const out = root.hd ? session.playAt(at, covered) : session.playSubAt(at);
        if (out === PlaybackSession.NoSource || out === PlaybackSession.Gap)
            phase = "failed";
    }
    function _stopStreams() {
        loader.cancel();
        media.stop();
        session.stop();
        player.stop();
    }
    function _firstPicture() {
        if (phase !== "starting")
            return;
        _firstFrameMs = Date.now();
        _nowMs = _firstFrameMs;
        _hadPicture = true;
        phase = "playing";
        console.log("clip: first picture after", _firstFrameMs - _requestedMs, "ms; host", clip.hostId,
                    "channel", clip.channel, clip.frigate ? "frigate" : (root.hd ? "main" : "sub"));
    }
    function _reachEnd() {
        if (!clip.frigate) {
            session.stop();
            player.stop();
        }
        phase = "ended";
    }
    function keepPlaying() {
        const from = root.position;
        clip.keepPlaying();
        _open(clip.frigate ? clip.from : from);
    }
    function setHd(on) {
        if (root.hd === on)
            return;
        const at = root.position;
        root.hd = on;
        // Reopen, not seek: a running stream moved in place would stay on the sub stream.
        if (clip.active && !clip.frigate && (phase === "playing" || phase === "starting")) {
            session.stop();
            player.stop();
            _open(at);
        }
    }
    function _openInPlayback() {
        const at = Math.floor(phase === "playing" || phase === "ended" ? root.position : clip.timestamp);
        const h = clip.hostId, c = clip.channel;
        close();
        openInPlayback(h, c, at);
    }
    function offsetText(secs) {
        const s = Math.round(secs);
        const a = Math.abs(s);
        return (s < 0 ? "−" : "+") + Math.floor(a / 60) + ":" + String(a % 60).padStart(2, "0");
    }

    // ---- Engines -------------------------------------------------------------------------
    PlaybackSession {
        id: session
        devices: Devices
        player: player
        hd: root.hd
        speed: 1.0
    }
    StreamPlayer {
        id: player
        videoSink: video.videoSink
        playback: true
        retryOnError: true
        speed: 1.0
        muted: !root.soundOn || !root.visible
        onFramesDecodedChanged: if (framesDecoded > 0) root._firstPicture()
        // A refused or dropped stream is retried (the NVR is connection-limited) and starts
        // again from where it was opened: back to "starting" so the clock restarts with the
        // picture. The start time limit gives up and closes the stream.
        onStateChanged: if (state === StreamPlayer.Connecting && root.phase === "playing") {
            root._firstFrameMs = 0;
            root.phase = "starting";
        }
    }
    ClipLoader {
        id: loader
        onStateChanged: {
            if (state === ClipLoader.Ready) {
                media.source = localUrl;
                media.play();
            } else if (state === ClipLoader.Error && root.phase === "starting") {
                root.phase = "failed";
            }
        }
    }
    MediaPlayer {
        id: media
        videoOutput: mediaVideo
        audioOutput: AudioOutput { muted: !root.soundOn }
        onPositionChanged: if (position > 0) root._firstPicture()
        onMediaStatusChanged: {
            if (mediaStatus !== MediaPlayer.EndOfMedia || root.phase !== "playing")
                return;
            // Kept playing: the next minute, while it is in the past; else the end.
            if (clip.openEnded && clip.to <= Date.now() / 1000 - 5) {
                clip.keepPlaying();
                root._open(clip.from);
            } else {
                root._reachEnd();
            }
        }
    }
    Component.onDestruction: _stopStreams()

    // ---- Layout --------------------------------------------------------------------------
    Rectangle { anchors.fill: parent; color: Theme.surface }

    Text {
        anchors.centerIn: parent
        visible: !clip.active
        text: qsTr("Pick an event…")
        color: Theme.textMuted
        font.pixelSize: 13
    }

    ColumnLayout {
        visible: clip.active
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        RowLayout {   // title, close
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: root._title
                color: Theme.text
                font.pixelSize: 13
                font.bold: true
                elide: Text.ElideRight
            }
            Rectangle {
                visible: root.closable
                width: 26; height: 26; radius: 13
                color: closeHover.hovered ? Theme.surfaceAlt : "transparent"
                Text { anchors.centerIn: parent; text: "✕"; color: Theme.textMuted; font.pixelSize: 13 }
                HoverHandler { id: closeHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: root.close() }
            }
        }

        Rectangle {   // the picture, 16:9
            Layout.fillWidth: true
            Layout.preferredHeight: Math.round(width * 9 / 16)
            color: Theme.paneBackground
            radius: 4
            clip: true
            VideoOutput {
                id: video
                anchors.fill: parent
                fillMode: VideoOutput.PreserveAspectFit
                visible: !clip.frigate && (root.phase === "playing" || root.phase === "ended"
                                           || (root.phase === "starting" && root._hadPicture))
            }
            VideoOutput {
                id: mediaVideo
                anchors.fill: parent
                fillMode: VideoOutput.PreserveAspectFit
                visible: clip.frigate && (root.phase === "playing" || root.phase === "ended")
            }
            // The snapshot until the first picture: no black flash.
            Image {
                anchors.fill: parent
                visible: (root.phase === "starting" && !root._hadPicture) || root.phase === "failed"
                source: root._thumbnail.length > 0 ? "file://" + root._thumbnail : ""
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                sourceSize.width: 640
            }
            Rectangle {
                visible: root.phase === "starting"
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 8
                width: startingText.implicitWidth + 16
                height: 22
                radius: 11
                color: "#a0000000"
                Text {
                    id: startingText
                    anchors.centerIn: parent
                    text: clip.frigate ? qsTr("Fetching the clip…") : qsTr("Starting…")
                    color: "white"
                    font.pixelSize: 11
                }
            }
            // Stopped at the end of the window, or nothing to play.
            Rectangle {
                visible: root.phase === "ended" || root.phase === "failed"
                anchors.fill: parent
                color: "#80000000"
                Column {
                    anchors.centerIn: parent
                    spacing: 10
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        visible: root.phase === "failed"
                        text: qsTr("Couldn't play this event")
                        color: "white"
                        font.pixelSize: 13
                    }
                    Row {
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: 8
                        Repeater {
                            model: root.phase === "ended"
                                   ? [{ label: qsTr("Keep playing"), act: "keep" },
                                      { label: qsTr("Open in Playback"), act: "open" }]
                                   : [{ label: qsTr("Open in Playback"), act: "open" }]
                            Rectangle {
                                required property var modelData
                                width: endText.implicitWidth + 24
                                height: 32
                                radius: Theme.radius
                                color: endHover.hovered ? Theme.accent : Theme.accentDim
                                border.color: Theme.accent
                                Text {
                                    id: endText
                                    anchors.centerIn: parent
                                    text: parent.modelData.label
                                    color: Theme.text
                                    font.pixelSize: 12
                                }
                                HoverHandler { id: endHover; cursorShape: Qt.PointingHandCursor }
                                TapHandler {
                                    onTapped: parent.modelData.act === "keep" ? root.keepPlaying()
                                                                              : root._openInPlayback()
                                }
                            }
                        }
                    }
                }
            }
        }

        // Progress over the window (−5 s to +30 s; Frigate −15 s), or a running clock once
        // Keep playing has made it open-ended.
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 26
            Rectangle {
                visible: !clip.openEnded
                y: 4
                width: parent.width
                height: 4
                radius: 2
                color: Theme.surfaceAlt
                Rectangle {
                    width: parent.width * clip.progress(root.position)
                    height: parent.height
                    radius: 2
                    color: Theme.accent
                }
                Rectangle {   // the event's own moment
                    x: parent.width * clip.progress(clip.timestamp) - 1
                    y: -3
                    width: 2
                    height: 10
                    color: Theme.text
                }
            }
            Text {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                visible: !clip.openEnded
                text: root.offsetText(clip.from - clip.timestamp)
                color: Theme.textMuted
                font.pixelSize: 10
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                text: root.offsetText(root.position - clip.timestamp)
                color: Theme.text
                font.pixelSize: 11
                font.bold: true
            }
            Text {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                visible: !clip.openEnded
                text: root.offsetText(clip.to - clip.timestamp)
                color: Theme.textMuted
                font.pixelSize: 10
            }
        }

        RowLayout {   // controls
            Layout.fillWidth: true
            spacing: 6
            Repeater {
                model: [
                    { key: "sound", label: root.soundOn ? "🔊" : "🔇", tip: root.soundOn ? qsTr("Mute") : qsTr("Sound on"), show: true },
                    { key: "hd", label: qsTr("HD"), tip: qsTr("Full resolution (main stream)"), show: !clip.frigate },
                    { key: "up", label: "↑", tip: qsTr("Newer event (↑)"), show: true },
                    { key: "down", label: "↓", tip: qsTr("Older event (↓)"), show: true }
                ]
                Rectangle {
                    required property var modelData
                    visible: modelData.show
                    readonly property bool on: modelData.key === "hd" && root.hd
                    implicitWidth: ctlText.implicitWidth + 18
                    implicitHeight: 28
                    radius: Theme.radius
                    color: on ? Theme.accentDim : (ctlHover.hovered ? Theme.surfaceAlt : "transparent")
                    border.color: on ? Theme.accent : Theme.border
                    Text {
                        id: ctlText
                        anchors.centerIn: parent
                        text: parent.modelData.label
                        color: Theme.text
                        font.pixelSize: 12
                    }
                    HoverHandler { id: ctlHover; cursorShape: Qt.PointingHandCursor }
                    ToolTip.visible: ctlHover.hovered
                    ToolTip.delay: 500
                    ToolTip.text: modelData.tip
                    TapHandler {
                        onTapped: {
                            switch (parent.modelData.key) {
                            case "sound": root.soundOn = !root.soundOn; break;
                            case "hd": root.setHd(!root.hd); break;
                            case "up": root.stepRequested(-1); break;
                            case "down": root.stepRequested(1); break;
                            }
                        }
                    }
                }
            }
            Item { Layout.fillWidth: true }
            Rectangle {
                implicitWidth: openText.implicitWidth + 18
                implicitHeight: 28
                radius: Theme.radius
                color: openHover.hovered ? Theme.surfaceAlt : "transparent"
                border.color: Theme.border
                Text {
                    id: openText
                    anchors.centerIn: parent
                    text: qsTr("Open in Playback")
                    color: Theme.text
                    font.pixelSize: 12
                }
                HoverHandler { id: openHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: root._openInPlayback() }
            }
        }

        Item { Layout.fillHeight: true }
    }
}
