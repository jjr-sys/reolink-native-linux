import QtQuick
import QtQuick.Controls
import QtQuick.Window
import QtMultimedia
import ReolinkApp
import ReolinkApp.Core

// One pane of Playback's synced mosaic: one camera's footage, its own recordings for the
// selected day, and an honest per-camera answer when the shared playhead sits where THIS
// camera has no footage. The page owns the playhead and drives every pane from one
// timeline; sync comes from asking each camera for the same wall-clock start time (the
// protocol has no cross-stream sync mechanism). Low quality unless the page sets hdActive.
Rectangle {
    id: root
    color: Theme.paneBackground
    border.color: selected ? Theme.accent : Theme.border
    border.width: 1
    clip: true

    property int deviceRow: -1
    property int paneIndex: -1     // place in the mosaic's pane order
    // Manual per-camera view rotation (matches the live pane's).
    property int viewRotation: 0
    property string label: ""
    property bool selected: false
    // Sound: the page owns the mute choice and says whether THIS pane is the one
    // to hear (only the selected pane is audible).
    property bool audioMuted: true
    property bool audioActive: true
    // Keep the last picture up while a skip re-opens the stream (see PlaybackPage.skip).
    property bool holding: false
    function freeze() { if (streaming) { holding = true; holdTimer.restart(); } }
    Timer { id: holdTimer; interval: 6000; onTriggered: root.holding = false }
    // Pause, Stop or a recording's end keep the last picture on screen instead of going black
    // (wayfinder/fisheye-and-framing step 07). It stays until a new picture replaces it, the
    // day changes, the playhead lands where this camera has no footage, or the pane leaves the
    // mosaic. Only a stream that really showed a frame leaves one (a refused stream can reach
    // Streaming without any).
    property bool kept: false
    property bool _framed: false   // this stream has put a frame on screen
    Connections {
        target: video.videoSink
        enabled: root.streaming && !root._framed
        function onVideoFrameChanged() { root._framed = true; }
    }
    onStreamingChanged: {
        if (streaming) {
            holding = false;
            kept = false;
            Qt.callLater(_align);
            Qt.callLater(measureRatio);
        } else {
            kept = _framed;
            _framed = false;
        }
    }
    onDayEpochChanged: kept = false
    onPlayheadSecsChanged: if (kept && !covered(playheadSecs)) kept = false
    readonly property bool showsPicture: streaming || holding || kept

    // The decoded picture's shape (width / height, turned by the rotation), for the mosaic
    // to remember; 0 until a picture is on screen. Measured as Live View does, ignoring
    // changes under 3 % so a re-pack resizing the pane doesn't read back as a new shape.
    readonly property real frameRatio: measuredRatio
    property real measuredRatio: 0
    function measureRatio() {
        var r = video.contentRect;
        if (!streaming || r.width < 2 || r.height < 2)
            return;
        var m = (viewRotation % 180 !== 0) ? r.height / r.width : r.width / r.height;
        if (!(measuredRatio > 0) || Math.abs(m - measuredRatio) / measuredRatio > 0.03)
            measuredRatio = m;
    }
    Connections {
        target: video
        function onContentRectChanged() { root.measureRatio(); }
    }
    property real speed: 1.0
    // Save the picture on screen as a JPEG named for this camera and the recorded
    // moment; returns the path, or "" if there is no picture yet.
    function snapshot(epoch) {
        if (!streaming)
            return "";
        var path = Downloads.snapshotPath(deviceRow, epoch);
        return path !== "" && player.saveSnapshot(path) ? path : "";
    }
    readonly property bool hasAudio: player.hasAudio
    // This camera's recordings for the selected day ({start,end,type} seconds).
    property var segments: []
    // Bound to the page's shared playhead — drives the "no footage" verdict.
    property real playheadSecs: 0

    signal clicked()
    // Double-click toggles maximize (the page owns which pane is maximized).
    signal doubleClicked()
    // A pane was dropped onto this one (to swap places).
    signal cameraDropped(int pane, int row)
    // Optional (deviceRow) => bool: whether a camera may be dropped here at all.
    property var dropFilter: null
    function dropAllowed(drag) {
        return !root.dropFilter || !drag.source || root.dropFilter(drag.source.deviceRow);
    }

    // True while a drag hovers this pane.
    property bool dropTarget: false

    readonly property bool hasSource: deviceRow >= 0
    readonly property bool streaming: player.state === StreamPlayer.Streaming
    // Opening or playing: a resume must leave such a pane alone.
    readonly property bool busy: player.state === StreamPlayer.Connecting || streaming

    // ---- No picture ---------------------------------------------------------
    // Recorders refuse streams over their budget silently: the stream stays connecting, or
    // "streams" without a frame. With footage at the playhead and no new frame for 8 s, the
    // pane says "No picture: retrying" and reopens at the playhead, again after 16 s, 32 s,
    // then every 60 s, until a frame comes. A first picture gets 20 s: with several
    // sessions starting together, an NVR takes 6 to 11 s to send one (the 8MP-plays-alone rule is not enforced; this
    // shows it failing).
    property bool noPicture: false
    property real _lastFrames: -1
    property int _stillTicks: 0
    property int _retries: 0
    readonly property var _retryTicks: [4, 8, 16, 30]   // 2 s ticks: 8, 16, 32, 60 s
    readonly property var _firstTicks: [10, 10, 16, 30] // before a first picture: 20 s
    Timer {
        interval: 2000
        repeat: true
        running: root.visible && root.hasSource && !root._suppressResume
                 && (player.state === StreamPlayer.Connecting || player.state === StreamPlayer.Streaming)
                 && root.covered(root.playheadSecs)
        onRunningChanged: {
            root._stillTicks = 0;
            root._lastFrames = -1;
            // A retry reopens the stream (stopping it for a moment): the state stays. A stop,
            // a gap or the pane leaving ends it.
            if (!running && (root._suppressResume || !root.visible || !root.covered(root.playheadSecs))) {
                root.noPicture = false;
                root._retries = 0;
            }
        }
        onTriggered: {
            var f = player.framesDecoded;
            if (root._lastFrames >= 0 && f !== root._lastFrames) {
                root._stillTicks = 0;
                root.noPicture = false;
                root._retries = 0;
            } else {
                root._stillTicks++;
            }
            root._lastFrames = f;
            var ticks = player.state === StreamPlayer.Connecting ? root._firstTicks : root._retryTicks;
            if (root._stillTicks >= ticks[Math.min(root._retries, 3)]) {
                root.noPicture = true;
                root._retries++;
                root._stillTicks = 0;
                // Reopen, not a seek in place: a stream "playing" without frames is the
                // refused one. (_suppressResume stays false, so this state survives the stop.)
                session.stop();
                root.playAtSecs(root.playheadSecs, root.epochOf(root.playheadSecs));
            }
        }
    }

    // ---- HD ---------------------------------------------------------------
    // Full-resolution main stream over native Baichuan. The page decides: the selected
    // pane's HD choice, under the Site budget.
    property bool hdActive: false
    // Epoch at 00:00 of the selected day, so the pane can turn playhead
    // seconds into wall-clock time without reaching into the page.
    property real dayEpoch: 0

    function epochOf(sec) { return Math.floor(dayEpoch + sec); }

    // Transport and seek-or-reopen live in PlaybackSession.
    PlaybackSession {
        id: session
        devices: Devices
        player: player
        deviceRow: root.deviceRow
        hd: root.hdActive
        speed: root.speed
        onReopenNeeded: root.playAtSecs(root.playheadSecs, root.epochOf(root.playheadSecs))
    }

    function covered(sec) {
        for (var i = 0; i < segments.length; i++)
            if (sec >= segments[i].start && sec <= segments[i].end)
                return true;
        return false;
    }

    // The FLV endpoint streams ONE recording file per connection, and events
    // are separate files — continuous footage EOFs at every event boundary.
    // Reopen at the next second when footage continues past the playhead.
    property real _lastAutoResume: -10
    property bool _suppressResume: false

    Connections {
        target: player
        function onStateChanged() {
            if (player.state !== StreamPlayer.Stopped)
                return;
            // Deferred: stop() fires synchronously inside playAtSecs()'s own
            // stop-then-start; re-entering from here would double-start.
            Qt.callLater(function () {
                if (player.state !== StreamPlayer.Stopped || !root.visible
                    || !root.hasSource || root._suppressResume)
                    return;
                if (!root.covered(root.playheadSecs + 1))
                    return;
                if (root.playheadSecs <= root._lastAutoResume + 2)
                    return;
                root._lastAutoResume = root.playheadSecs;
                root.playAtSecs(root.playheadSecs + 1, root.epochOf(root.playheadSecs + 1));
            });
        }
    }

    // Play this camera from the given moment (epoch = wall-clock seconds).
    // A gap is a normal outcome, not an error: stop and let the overlay say so.
    function playAtSecs(sec, epoch) {
        _syncTries = 0;
        _open(sec, epoch);
    }
    function _open(sec, epoch) {
        if (!hasSource)
            return -1;
        _suppressResume = false;
        _openSec = sec;
        _openWall = Date.now();
        catchUpTimer.stop();
        catchUp = 1.0;
        const result = session.playAt(epoch, covered(sec));
        // A gap stops the stream inside playAt (keeping its frame): say "No footage" instead.
        if (!covered(sec))
            kept = false;
        return result;
    }

    // ---- Keeping in step with the playhead -------------------------------------
    // Each recorder takes its own time to send a first picture (5 to 7 s at Farm), so
    // panes asked for the same moment start at different wall-clock times and the late
    // ones lag the shared playhead. When a pane's first picture comes it compares itself
    // with the playhead and, more than `syncTolerance` seconds out, plays a little faster
    // (1.5x) or slower (0.7x) until level: no reopen, so no black gap. Only a pane more
    // than 10 s out reopens at the playhead plus its own start delay. Adjusted after the
    // first-picture change has finished, never inside it.
    property real syncTolerance: 0.4
    property real catchUp: 1.0          // multiplies `speed` while a pane catches up
    property real _openSec: 0
    property real _openWall: 0
    property int _syncTries: 0
    Timer { id: catchUpTimer; onTriggered: root.catchUp = 1.0 }
    function _align() {
        if (!streaming || !covered(playheadSecs))
            return;
        var drift = _openSec - playheadSecs;       // + ahead of the playhead, - behind
        if (Math.abs(drift) <= syncTolerance)
            return;
        if (Math.abs(drift) > 10) {
            if (_syncTries >= 1)
                return;
            _syncTries++;
            var delay = (Date.now() - _openWall) / 1000 * speed;
            var target = Math.min(86399, playheadSecs + delay);
            _open(target, epochOf(target));
            return;
        }
        var rate = drift < 0 ? 1.5 : 0.7;
        catchUp = rate;
        catchUpTimer.interval = Math.round(Math.abs(drift) / (Math.abs(rate - 1) * Math.max(0.25, speed)) * 1000);
        catchUpTimer.restart();
    }

    function stopPlayback() {
        _suppressResume = true; // an ordered stop must stay stopped
        player.stop();
    }

    // A pane that leaves the mosaic (deselected, or its page hides)
    // must not keep a playback stream open on the connection-limited NVR.
    onVisibleChanged: if (!visible) { _suppressResume = true; player.stop(); kept = false; }

    // Retry on connection error: NVRs are connection-limited and may
    // momentarily refuse a playback stream while others are opening.
    StreamPlayer {
        id: player
        videoSink: video.videoSink
        retryOnError: true
        playback: true
        speed: root.speed * root.catchUp
        // Off screen or not the chosen pane: no decode, no device held open.
        muted: root.audioMuted || !root.audioActive || !root.visible
    }
    Component.onDestruction: player.stop()

    // ---- Video with digital zoom (same interaction as a live pane) --------
    property real zoom: 1.0
    property real panX: 0
    property real panY: 0

    Item {
        anchors.fill: parent
        anchors.margins: 1
        clip: true
        VideoOutput {
            id: video
            anchors.fill: parent
            fillMode: VideoOutput.PreserveAspectFit
            visible: root.showsPicture
            orientation: root.viewRotation
            transform: [
                Scale {
                    origin.x: video.width / 2
                    origin.y: video.height / 2
                    xScale: root.zoom
                    yScale: root.zoom
                },
                Translate { x: root.panX; y: root.panY }
            ]
        }
    }

    // Name + zoom badge (matches the live grid's).
    Rectangle {
        visible: root.hasSource
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.margins: 6
        radius: 3
        color: "#80000000"
        width: nameRow.implicitWidth + 12
        height: nameRow.implicitHeight + 6
        Row {
            id: nameRow
            anchors.centerIn: parent
            spacing: 6
            Text { text: root.label; color: "white"; font.pixelSize: 11 }
            Text { visible: root.hdActive; text: qsTr("HD"); color: Theme.accent; font.pixelSize: 11; font.bold: true }
            Text {
                visible: root.zoom > 1.01
                text: root.zoom.toFixed(1) + "×"
                color: Theme.accent
                font.pixelSize: 11
            }
        }
    }

    // Per-camera state at the shared playhead; over a kept picture (reopening, an error) on
    // a dark backing so it reads.
    Rectangle {
        anchors.centerIn: stateColumn
        width: stateColumn.width + 20
        height: stateColumn.height + 14
        radius: Theme.radius
        color: "#a0000000"
        visible: stateColumn.visible && root.showsPicture
    }
    Column {
        id: stateColumn
        anchors.centerIn: parent
        spacing: Theme.spacing
        visible: !root.showsPicture || root.noPicture || player.state === StreamPlayer.Error
                 || (root.kept && player.state === StreamPlayer.Connecting)
        BusyIndicator {
            anchors.horizontalCenter: parent.horizontalCenter
            running: player.state === StreamPlayer.Connecting
            visible: running; width: 28; height: 28
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            width: Math.min(implicitWidth, root.width - 20)
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            color: player.state === StreamPlayer.Error ? Theme.danger : Theme.textMuted
            font.pixelSize: 11
            text: {
                if (!root.hasSource) return qsTr("No camera");
                if (root.noPicture) return qsTr("No picture: retrying");
                if (player.state === StreamPlayer.Connecting) return qsTr("Connecting…");
                if (player.state === StreamPlayer.Error) return player.errorString;
                if (!root.covered(root.playheadSecs))
                    return qsTr("No footage at this time");
                return qsTr("Ready");
            }
        }
    }

    // ---- Drag and drop (mirrors Live View) ---------------------------------
    // Dropping a pane on another of the same Site swaps their places.
    DropArea {
        anchors.fill: parent
        keys: ["reolink/camera"]
        onEntered: (drag) => {
            if (!root.dropAllowed(drag)) {
                drag.accepted = false;   // refused: no highlight, no drop
                return;
            }
            root.dropTarget = !(drag.source && drag.source.sourcePane === root.paneIndex);
        }
        onExited: root.dropTarget = false
        onDropped: (drop) => {
            root.dropTarget = false;
            if (drop.source && drop.source.deviceRow >= 0 && root.dropAllowed(drop))
                root.cameraDropped(root.paneIndex, drop.source.deviceRow);
        }
    }

    Rectangle {
        anchors.fill: parent
        visible: root.dropTarget
        color: Theme.accent
        opacity: 0.18
        border.color: Theme.accent
        border.width: 2
        z: 50
    }

    // Drag ghost, parented to the window so the pane's clip can't cut it off.
    Item {
        id: paneDrag
        parent: Window.contentItem
        width: 190
        height: 108
        visible: Drag.active
        z: 100
        Drag.active: false
        Drag.keys: ["reolink/camera"]
        Drag.hotSpot: Qt.point(width / 2, height / 2)
        property int deviceRow: root.deviceRow
        property int sourcePane: root.paneIndex

        Rectangle {
            anchors.fill: parent
            radius: Theme.radius
            color: Theme.surface
            border.color: Theme.accent
            border.width: 2
            opacity: 0.95
            Text {
                anchors.centerIn: parent
                width: parent.width - 16
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                text: root.label
                color: Theme.text
                font.pixelSize: 12
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        z: -1

        property real pressX: 0
        property real pressY: 0
        property real lastX: 0
        property real lastY: 0
        property bool dragging: false
        // Zoomed in, dragging pans the image — the older and more frequent
        // gesture — so a pane can only be picked up at 1× (same rule as live).
        readonly property bool canDrag: root.hasSource && root.zoom <= 1.0

        // Keep the zoomed footage pinned to the viewport edges.
        function clampPan() {
            var mx = Math.max(0, (video.contentRect.width * root.zoom - video.width) / 2);
            var my = Math.max(0, (video.contentRect.height * root.zoom - video.height) / 2);
            root.panX = Math.max(-mx, Math.min(mx, root.panX));
            root.panY = Math.max(-my, Math.min(my, root.panY));
        }

        onClicked: {
            if (dragging)      // the release that ended a drag isn't a click
                return;
            root.clicked();
        }
        onDoubleClicked: if (root.hasSource) root.doubleClicked()
        onPressed: (m) => {
            pressX = m.x; pressY = m.y;
            lastX = m.x; lastY = m.y;
            dragging = false;
        }
        onPositionChanged: (m) => {
            if (!(m.buttons & Qt.LeftButton))
                return;
            if (root.zoom > 1.0) {
                root.panX += (m.x - lastX);
                root.panY += (m.y - lastY);
                lastX = m.x; lastY = m.y;
                clampPan();
                return;
            }
            if (!canDrag)
                return;
            if (!dragging
                && Math.hypot(m.x - pressX, m.y - pressY) > Theme.dragThreshold) {
                dragging = true;
                paneDrag.Drag.active = true;
            }
            if (dragging) {
                var p = mapToItem(paneDrag.parent, m.x, m.y);
                paneDrag.x = p.x - paneDrag.width / 2;
                paneDrag.y = p.y - paneDrag.height / 2;
            }
        }
        onReleased: {
            if (!dragging)
                return;
            paneDrag.Drag.drop();
            paneDrag.Drag.active = false;
        }
        onCanceled: {
            if (!dragging)
                return;
            paneDrag.Drag.active = false;
            dragging = false;
        }
        onWheel: (w) => {
            if (!root.hasSource)
                return;
            var z = root.zoom * (w.angleDelta.y > 0 ? 1.15 : 0.87);
            root.zoom = Math.max(1.0, Math.min(8.0, z));
            if (root.zoom <= 1.0) { root.panX = 0; root.panY = 0; }
            else clampPan();
        }
    }
}
