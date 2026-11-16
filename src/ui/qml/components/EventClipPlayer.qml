import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import QtMultimedia
import ReolinkApp
import ReolinkApp.Core
import "TransportActions.js" as TransportActions

// The Events page's clip player (wayfinder/events-feed step 22): the picked event's footage
// beside the feed. A Reolink recorder plays its sub stream (HD on request) from 5 s before
// the event through Playback's engine at 1×; a Frigate camera plays Frigate's own clip
// from 15 s before. The event's snapshot stays up until the first picture. At +30 s it
// stops (freeing the recorder's connection, keeping the last picture) with Keep playing
// and Open in Playback. Sound starts muted. EventClip holds the window maths and the player's
// state (its phase, playhead, give-up times and speed group); this file binds the engines to it.
// A fisheye camera's circle (the recording, or the snapshot before it) is shown in the
// camera's view with a Playback pane's controls: hover icons, right-click View, drag to aim,
// scroll to zoom, double-click to put a turned view back (fisheye-and-framing step 10).
// Its controls are the pane bar every video pane has (PaneBar, live-replay ticket 11) over the
// picture: ❚❚, ↺ / ↻ and the clip's line seek within the window; HD, sound, ↑ ↓ and ⋯ Open in
// Playback. Speeds and reverse (ticket 12): a recorder's clip plays through the same engine as
// Playback (PlaybackSession driven by a PlaybackGroup: the speed ladder both ways, HD to 4×,
// reverse by windows), backwards stopping at the window's start; a Frigate clip (a downloaded
// file) plays forwards at any speed.
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
    property real _requestedMs: 0   // when the open was asked for (the first-picture log)

    // EventClip holds the window, the phase, the playhead and the speed group (architecture
    // review #2, candidate 2): this file tells it what happened and does what it asks.
    EventClip {
        id: clip
        devices: Devices
        frameEpochMs: player.frameEpochMs
        mediaPositionMs: media.position
        onOpenWanted: (at) => {
            root._requestedMs = Date.now();
            if (clip.frigate) {
                loader.load(clip.clipUrl);
                return;
            }
            session.deviceRow = Devices.rowOfHostChannel(clip.hostId, clip.channel);
            // The feed's event is the timeline's verdict: covered. The session's route follows
            // hd, speed and direction (sub stream, HD, reverse windows).
            const out = session.playAt(at, true);
            if (out === PlaybackSession.NoSource || out === PlaybackSession.Gap)
                clip.openFailed();
        }
        onStopAllWanted: { loader.cancel(); media.stop(); session.stop(); player.stop(); }
        onStopStreamWanted: { session.stop(); player.stop(); }
        onPauseMediaWanted: media.pause()
        onSeekMediaWanted: (ms) => { media.position = ms; media.play(); }
        onRetryHdWanted: session.clearLinkSlow()
        onNotice: (which) => notice.show(which === EventClip.StartOfClip
            ? qsTr("Start of the clip — Open in Playback for earlier")
            : qsTr("The recorder didn't send this part — try again or Open in Playback"))
    }
    // The clip's speed and direction (ticket 12): one ladder both ways, as Playback's.
    readonly property PlaybackGroup speedGroup: clip.group
    readonly property bool paused: clip.paused
    readonly property bool playing: clip.phase === EventClip.Playing
    readonly property real position: clip.position
    Timer {   // the wall clock, while the clip needs it (give-up, a recorder's playhead)
        interval: 200
        repeat: true
        running: clip.ticking
        onTriggered: clip.tick(Date.now())
    }

    // Play an event: { hostId, channel, timestamp, thumbnail, title, eventId }.
    function play(ev) {
        _title = ev.title;
        _thumbnail = ev.thumbnail || "";
        _eventId = ev.eventId;
        clip.play(ev.hostId, ev.channel, ev.timestamp);
    }
    function close() {
        clip.close();
        _eventId = -1;
        closed();
    }
    // Leaving the page or hiding the window: stop, and free the recorder.
    function stop() { if (clip.active) close(); }
    function keepPlaying() { clip.keepPlaying(); }
    function setHd(on) {
        if (root.hd === on)
            return;
        root.hd = on;
        clip.hdChanged();
    }
    // The pane bar and the keys (TransportActions): pause, seek, skip, the speed buttons.
    function togglePause() { clip.togglePause(); }
    function seekTo(at) { clip.seekTo(at); }
    function skip(secs) { clip.skip(secs); }
    function speedAction(fn) { fn(); clip.speedChanged(); }
    function _streamStopped() { clip.streamStopped(player.state === StreamPlayer.Stopped); }
    function _firstPicture() {
        if (!clip.firstPicture(Date.now()))
            return;
        console.log("clip: first picture after", Date.now() - _requestedMs, "ms; host", clip.hostId,
                    "channel", clip.channel, clip.frigate ? "frigate" : (root.hd ? "main" : "sub"));
    }
    // The page's keys while a clip is up: J « / K ❚❚ / L » / ← → ±10 s (as Live and Playback).
    function key(k) {
        if (!clip.active)
            return false;
        const id = TransportActions.keyId(k);
        if (id === "" || (k === "J" && clip.frigate))
            return false;
        return TransportActions.apply(id, clip.group, root);
    }
    function _openInPlayback() {
        const at = Math.floor(clip.handOffAt());
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
        speed: clip.group.speed
        direction: clip.group.direction
        // A change of direction (or of route at a new speed) while open reopens at the picture.
        onReopenNeeded: clip.reopenNeeded()
    }
    StreamPlayer {
        id: player
        videoSink: video.videoSink
        playback: true
        retryOnError: true
        speed: clip.group.speed
        // HD reverse holds groups at the picture's size on screen (live-replay ticket 06).
        holdPaneSize: Qt.size(Math.round(picture.width * Screen.devicePixelRatio),
                              Math.round(picture.height * Screen.devicePixelRatio))
        holdFisheye: root.fisheyeLens
        onLinkTooSlowChanged: if (linkTooSlow) session.linkSlow = true
        // Sound plays at 1× forward only (as Playback).
        muted: !root.soundOn || !root.visible || clip.group.offNormal
        // A refused or dropped stream is retried (the NVR is connection-limited) and starts
        // again from where it was opened: back to "starting" so the clock restarts with the
        // picture. The start time limit gives up and closes the stream.
        // Streaming is posted with the first decoded frame (framesDecoded only ticks every
        // 100 frames, seconds later).
        onStateChanged: {
            if (state === StreamPlayer.Streaming) {
                root._firstPicture();
            } else if (state === StreamPlayer.Connecting) {
                clip.streamReconnecting();
            } else if (state === StreamPlayer.Stopped) {
                // Deferred: an ordered stop() also posts Stopped, and moves the phase first.
                Qt.callLater(root._streamStopped);
            }
        }
    }
    ClipLoader {
        id: loader
        onStateChanged: {
            if (state === ClipLoader.Ready) {
                media.source = localUrl;
                media.play();
            } else if (state === ClipLoader.Error) {
                clip.loadFailed();
            }
        }
    }
    MediaPlayer {
        id: media
        videoOutput: mediaVideo
        // Any speed forwards on the downloaded file (ticket 12); sound at 1× only.
        playbackRate: clip.group.direction > 0 ? clip.group.speed : 1
        audioOutput: AudioOutput { muted: !root.soundOn || clip.group.offNormal }
        onPositionChanged: if (position > 0) root._firstPicture()
        onMediaStatusChanged: if (mediaStatus === MediaPlayer.EndOfMedia) clip.mediaEnded(Date.now() / 1000)
    }
    Component.onDestruction: { loader.cancel(); media.stop(); session.stop(); player.stop(); }

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
                visible: !clip.frigate && (clip.phase === EventClip.Playing || clip.paused
                                           || (clip.phase === EventClip.Starting && clip.hadPicture))
            }
            VideoOutput {
                id: mediaVideo
                anchors.fill: parent
                fillMode: VideoOutput.PreserveAspectFit
                visible: clip.frigate && (clip.phase === EventClip.Playing || clip.paused)
            }
            // The snapshot until the first picture: no black flash.
            Image {
                id: eventSnapshot
                anchors.fill: parent
                visible: (clip.phase === EventClip.Starting && !clip.hadPicture) || clip.phase === EventClip.Failed
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
                enabled: fisheyeLoader.item !== null && clip.phase !== EventClip.Ended && clip.phase !== EventClip.Failed
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
            // A short message on the picture ("Start of the clip").
            Rectangle {
                id: notice
                z: 6
                visible: opacity > 0
                opacity: 0
                anchors.centerIn: parent
                width: Math.min(parent.width - 16, noticeText.implicitWidth + 20)
                height: noticeText.implicitHeight + 12
                radius: 4
                color: "#dd0d141b"
                function show(t) { noticeText.text = t; opacity = 1; noticeTimer.restart(); }
                Text {
                    id: noticeText
                    anchors.centerIn: parent
                    width: Math.min(implicitWidth, parent.width - 20)
                    elide: Text.ElideRight
                    color: Theme.text
                    font.pixelSize: 12
                }
                Timer { id: noticeTimer; interval: 4000; onTriggered: notice.opacity = 0 }
                Behavior on opacity { NumberAnimation { duration: 200 } }
            }
            PaneBar {
                id: paneBar
                anchors.fill: parent
                z: 5
                pageKind: "events"
                camera: clip.frigate ? "frigate" : "recorder"
                speedGroup: clip.group
                paused: clip.paused
                shown: pictureHover.hovered
                toolList: [{ id: "newer", glyph: "↑", tip: qsTr("Newer event (↑)") },
                           { id: "older", glyph: "↓", tip: qsTr("Older event (↓)") },
                           { id: "open", glyph: "▤", tip: qsTr("Open in Playback") }]
                hdShown: !clip.frigate
                hdOn: root.hd
                // HD holds up to 4× while the link keeps up (PlaybackSession::chooseRoute).
                hdText: root.hd && !clip.group.sdAtSpeed && !session.hdHeldBack ? "HD" : "SD"
                hdTip: !root.hd ? qsTr("Full resolution (main stream)")
                     : clip.group.sdAtSpeed ? qsTr("HD chosen · SD above 4×: switch to SD")
                     : session.hdHeldBack ? qsTr("HD chosen · SD: the link is too slow · switch to SD")
                     : qsTr("Switch to SD (sub stream)")
                soundShown: true
                soundOn: root.soundOn
                soundTip: root.soundOn ? qsTr("Mute") : qsTr("Sound on")
                segments: clip.openEnded ? [] : [{ from: 0, to: 1 }]
                head: clip.openEnded ? -1 : clip.progress(clip.shownPosition)
                headColour: Theme.accent
                ticks: clip.openEnded ? [] : [clip.progress(clip.timestamp)]
                labelAt: (pos) => root.offsetText(clip.timeAt(pos) - clip.timestamp)
                seekable: !clip.openEnded && clip.phase !== EventClip.Failed
                timeText: {
                    var at = clip.shownPosition;
                    return Qt.formatTime(new Date(at * 1000), "hh:mm:ss") + "  <b>"
                        + root.offsetText(at - clip.timestamp) + "</b>"
                        + (clip.openEnded ? "" : "  /  " + root.offsetText(clip.to - clip.timestamp));
                }
                onAction: (id) => {
                    switch (id) {
                    case "hd": root.setHd(!root.hd); break;
                    case "sound": root.soundOn = !root.soundOn; break;
                    default: TransportActions.apply(id, clip.group, root); break;
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
                visible: clip.phase === EventClip.Starting
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
                visible: clip.phase === EventClip.Ended || clip.phase === EventClip.Failed
                anchors.fill: parent
                color: "#80000000"
                Column {
                    anchors.centerIn: parent
                    spacing: 10
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        visible: clip.phase === EventClip.Failed
                        text: qsTr("Couldn't play this event")
                        color: "white"
                        font.pixelSize: 13
                    }
                    Row {
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: 8
                        Repeater {
                            model: clip.phase === EventClip.Ended
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
