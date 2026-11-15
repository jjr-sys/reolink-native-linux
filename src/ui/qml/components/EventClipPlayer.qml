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
// A fisheye camera's circle (the recording, or the snapshot before it) is shown in the
// camera's view with a Playback pane's controls: hover icons, right-click View, drag to aim,
// scroll to zoom, double-click to put a turned view back (fisheye-and-framing step 10).
// Its controls are the pane bar every video pane has (PaneBar, live-replay ticket 11) over the
// picture: ❚❚, ↺ / ↻ and the clip's line seek within the window; speeds and reverse are
// greyed for now (ticket 12); HD, sound, ↑ ↓ and ⋯ Open in Playback.
Item {
    id: root

    property bool hd: true          // videos and stills in HD (step 10); the HD button turns it off
    property bool soundOn: false
    property bool closable: true
    // The picture's shape (wayfinder/fisheye-and-framing step 06): the video's own size once
    // its first picture arrives, before that this event's snapshot, else the camera's shape
    // from the feed.
    property real snapshotRatio: 16 / 9
    // The shown camera's fisheye flag, view and aim key (shared with the other tabs).
    property bool fisheyeLens: false
    property string fisheyeMode: "fisheye"
    property string cameraKey: ""
    signal viewPicked(string view)
    // A circle drawn through the view, at the view's shape (frameshape, tested).
    // (Frigate's clips are its flat copy, never a circle.)
    readonly property bool dewarp: !clip.frigate && FrameShape.dewarps(pictureRatio, fisheyeLens, fisheyeMode)
    readonly property bool circle: !clip.frigate && FrameShape.isCircle(pictureRatio, fisheyeLens)
    readonly property real shownRatio: FrameShape.shownRatio(pictureRatio, fisheyeLens, fisheyeMode)
    readonly property real pictureRatio: {
        const r = (clip.frigate ? mediaVideo : video).sourceRect;
        if (r.width > 0 && r.height > 0)
            return r.width / r.height;
        if (eventSnapshot.implicitWidth > 0 && eventSnapshot.implicitHeight > 0)
            return eventSnapshot.implicitWidth / eventSnapshot.implicitHeight;
        return snapshotRatio;
    }
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
    // The pane bar: pause and play on where it stopped (a recorder's stream is closed while
    // paused, freeing the recorder; the last picture stays); seek to a moment in the window.
    property real _pausedAt: 0
    readonly property bool paused: phase === "paused" || phase === "ended"
    function togglePause() {
        if (phase === "playing") {
            _pausedAt = root.position;
            if (clip.frigate) {
                media.pause();
            } else {
                session.stop();
                player.stop();
            }
            phase = "paused";
        } else if (phase === "paused") {
            seekTo(_pausedAt);
        } else if (phase === "ended") {
            seekTo(clip.from);   // from the window's start again
        }
    }
    function seekTo(at) {
        if (!clip.active)
            return;
        at = clip.skipFrom(at, 0);
        if (clip.frigate) {
            media.position = Math.max(0, (at - clip.from) * 1000);
            media.play();
            phase = "playing";
            return;
        }
        session.stop();
        player.stop();
        _open(at);
    }
    function skip(secs) { seekTo(clip.skipFrom(phase === "paused" ? _pausedAt : root.position, secs)); }
    function _openInPlayback() {
        const at = Math.floor(phase === "paused" ? _pausedAt
                              : phase === "playing" || phase === "ended" ? root.position : clip.timestamp);
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
        // A refused or dropped stream is retried (the NVR is connection-limited) and starts
        // again from where it was opened: back to "starting" so the clock restarts with the
        // picture. The start time limit gives up and closes the stream.
        // Streaming is posted with the first decoded frame (framesDecoded only ticks every
        // 100 frames, seconds later).
        onStateChanged: {
            if (state === StreamPlayer.Streaming) {
                root._firstPicture();
            } else if (state === StreamPlayer.Connecting && root.phase === "playing") {
                root._firstFrameMs = 0;
                root.phase = "starting";
            }
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
        id: column
        visible: clip.active
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        RowLayout {   // title, close
            id: titleRow
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

        // The picture in its own shape, the largest that fits above the controls: no black bars.
        Rectangle {
            id: picture
            readonly property size box: FrameShape.fitIn(
                root.width - 2 * column.anchors.margins,
                root.height - 2 * column.anchors.margins - titleRow.height
                    - (column.children.length - 1) * column.spacing,
                root.shownRatio)
            Layout.preferredWidth: box.width
            Layout.preferredHeight: box.height
            Layout.alignment: Qt.AlignHCenter
            color: Theme.paneBackground
            radius: 4
            clip: true
            VideoOutput {
                id: video
                anchors.fill: parent
                // Dewarping samples the whole frame as a texture, so it must fill the item.
                fillMode: root.dewarp ? VideoOutput.Stretch : VideoOutput.PreserveAspectFit
                visible: !clip.frigate && (root.phase === "playing" || root.phase === "ended"
                                           || root.phase === "paused"
                                           || (root.phase === "starting" && root._hadPicture))
            }
            VideoOutput {
                id: mediaVideo
                anchors.fill: parent
                fillMode: VideoOutput.PreserveAspectFit
                visible: clip.frigate && (root.phase === "playing" || root.phase === "ended"
                                          || root.phase === "paused")
            }
            // The snapshot until the first picture: no black flash.
            Image {
                id: eventSnapshot
                anchors.fill: parent
                visible: (root.phase === "starting" && !root._hadPicture) || root.phase === "failed"
                source: root._thumbnail.length > 0 ? "file://" + root._thumbnail : ""
                // Stretched for the view, which turns on as soon as the picture has loaded.
                fillMode: root.dewarp ? Image.Stretch : Image.PreserveAspectFit
                asynchronous: true
            }
            // The camera's view over whichever circle is up: the recording, or its snapshot
            // until the first picture. Switching view never restarts the stream.
            Loader {
                id: fisheyeLoader
                anchors.fill: parent
                readonly property Item circleItem: video.visible ? video
                                               : (eventSnapshot.visible && eventSnapshot.status === Image.Ready
                                                  ? eventSnapshot : null)
                active: root.dewarp && circleItem !== null
                sourceComponent: FisheyeView {
                    sourceItem: fisheyeLoader.circleItem
                    viewMode: root.fisheyeMode
                    cameraKey: root.cameraKey
                }
            }
            MouseArea {   // aim: drag turns, scroll zooms, double-click puts a turned view back
                id: aimArea
                anchors.fill: parent
                // Not under the end / failure overlay, whose buttons take the clicks.
                enabled: fisheyeLoader.item !== null && root.phase !== "ended" && root.phase !== "failed"
                cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                property real lastX: 0
                property real lastY: 0
                onPressed: (m) => {
                    if (viewIcons.covers(aimArea, m.x, m.y)) {   // a view icon's click
                        m.accepted = false;
                        return;
                    }
                    lastX = m.x; lastY = m.y;
                }
                onPositionChanged: (m) => {
                    if (fisheyeLoader.item)   // the view can go while the snapshot hands over to the video
                        fisheyeLoader.item.dragBy(m.x, m.y, m.x - lastX, m.y - lastY);
                    lastX = m.x; lastY = m.y;
                }
                onDoubleClicked: FisheyeAims.reset(root.cameraKey, root.fisheyeMode)
                onWheel: (w) => {
                    if (fisheyeLoader.item && w.angleDelta.y !== 0)
                        fisheyeLoader.item.zoomBy(w.x, w.y, w.angleDelta.y > 0);
                }
            }
            HoverHandler { id: pictureHover }
            TapHandler {
                acceptedButtons: Qt.RightButton
                enabled: root.circle
                onTapped: (point) => {
                    viewMenu.aimed = root.dewarp && FisheyeAims.aimed(root.cameraKey, root.fisheyeMode);
                    viewMenu.popup(picture, point.position.x, point.position.y);
                }
            }
            FisheyeViewIcons {
                id: viewIcons
                anchors.top: parent.top
                anchors.right: parent.right
                shown: root.circle && pictureHover.hovered
                current: root.fisheyeMode
                onPicked: (view) => root.viewPicked(view)
            }
            FisheyeViewMenu {
                id: viewMenu
                current: root.fisheyeMode
                onPicked: (view) => root.viewPicked(view)
                onResetAim: FisheyeAims.reset(root.cameraKey, root.fisheyeMode)
            }
            PaneBar {
                id: paneBar
                anchors.fill: parent
                z: 5
                pageKind: "events"
                camera: clip.frigate ? "frigate" : "recorder"
                paused: root.paused
                shown: pictureHover.hovered
                toolList: [{ id: "newer", glyph: "↑", tip: qsTr("Newer event (↑)") },
                           { id: "older", glyph: "↓", tip: qsTr("Older event (↓)") },
                           { id: "open", glyph: "▤", tip: qsTr("Open in Playback") }]
                hdShown: !clip.frigate
                hdOn: root.hd
                hdTip: root.hd ? qsTr("Switch to SD (sub stream)") : qsTr("Full resolution (main stream)")
                soundShown: true
                soundOn: root.soundOn
                soundTip: root.soundOn ? qsTr("Mute") : qsTr("Sound on")
                segments: clip.openEnded ? [] : [{ from: 0, to: 1 }]
                head: clip.openEnded ? -1 : clip.progress(root.phase === "paused" ? root._pausedAt : root.position)
                headColour: Theme.accent
                ticks: clip.openEnded ? [] : [clip.progress(clip.timestamp)]
                labelAt: (pos) => root.offsetText(clip.timeAt(pos) - clip.timestamp)
                seekable: !clip.openEnded && root.phase !== "failed"
                timeText: {
                    var at = root.phase === "paused" ? root._pausedAt : root.position;
                    return Qt.formatTime(new Date(at * 1000), "hh:mm:ss") + "  <b>"
                        + root.offsetText(at - clip.timestamp) + "</b>"
                        + (clip.openEnded ? "" : "  /  " + root.offsetText(clip.to - clip.timestamp));
                }
                onAction: (id) => {
                    switch (id) {
                    case "hd": root.setHd(!root.hd); break;
                    case "sound": root.soundOn = !root.soundOn; break;
                    case "pause": root.togglePause(); break;
                    case "skipBack": root.skip(-10); break;
                    case "skipForward": root.skip(10); break;
                    case "forward": if (root.paused) root.togglePause(); break;
                    }
                }
                onTool: (id) => {
                    if (id === "newer") root.stepRequested(-1);
                    else if (id === "older") root.stepRequested(1);
                    else if (id === "open") root._openInPlayback();
                }
                onSeekReleased: (pos) => root.seekTo(clip.timeAt(pos))
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

        Item { Layout.fillHeight: true }
    }
}
