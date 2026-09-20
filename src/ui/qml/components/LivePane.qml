import QtQuick
import QtQuick.Controls
import QtQuick.Window
import QtMultimedia
import ReolinkApp
import ReolinkApp.Core

// One cell of the live grid: video + name overlay + connection-state overlay +
// a hover floating toolbar and an optional PTZ joystick. Double-click toggles
// maximize (handled by the page). Digital zoom via wheel + drag.
Rectangle {
    id: root
    color: Theme.paneBackground
    border.color: selected ? Theme.accent : Theme.border
    border.width: selected && activityKind !== "" ? 3 : 1
    clip: true

    property int paneIndex: -1     // grid slot
    property int deviceRow: -1     // row in the Devices model (-1 = empty slot)
    property string label: ""
    property bool selected: false
    property bool forceMain: false // maximizing DEFAULTS a pane to the main stream
    // Manual per-camera view rotation (degrees; sidebar right-click sets it).
    property int viewRotation: 0
    property bool pageActive: true // false when the Live View page isn't on screen

    // Activity mode: this camera holds a tile because of a detection. replayFrom
    // (epoch s, 0 = none) plays the recording from just before it; the Live button,
    // or the NVR refusing the extra session, drops back to the live picture.
    property string activityKind: ""
    property string activityText: ""
    property real replayFrom: 0
    property bool replayLive: false     // the user pressed Live
    property bool replayFailed: false   // the replay could not start
    readonly property bool replaying: replayFrom > 0 && !replayLive && !replayFailed
    onReplayFromChanged: { replayLive = false; replayFailed = false; updateSource(); }
    onReplayingChanged: updateSource()

    // Capabilities (from the Devices model; false for empty slots). Named cap*
    // to avoid colliding with the identically-named model roles in the delegate.
    property bool capPtz: false
    property bool capZoom: false
    property bool capAudio: false
    property bool capSiren: false
    property bool capFloodlight: false
    property bool capTalk: false
    // Two-way talk: driven by the TalkSession below; true from click until stopped.
    readonly property bool talkActive: talk.active
    property bool floodOn: false // white-LED/floodlight on state (optimistic)

    // Camera audio follows the selected pane (the one with the accent border): the
    // page selects exactly one at a time, so mixing sixteen cameras never happens.
    readonly property bool audioOn: selected

    // User stream-quality preference: false = Fluent (sub), true = Clear (main).
    // This alone decides which stream plays — so the SD/HD toolbar toggle works in
    // every layout, including a maximized pane. Maximizing/restoring just seeds a
    // sensible default (main when big, sub in the grid) via onForceMainChanged; the
    // user can still override it with the toggle afterwards.
    property bool qualityMain: false
    readonly property bool effectiveMain: qualityMain
    onForceMainChanged: qualityMain = forceMain
    readonly property bool hasSource: deviceRow >= 0
    // What this pane is currently streaming: an RTSP URL, or "bc:<row>" for native
    // Baichuan HD live. HD (main) live goes over Baichuan because the NVR's RTSP
    // output for 8K duo main streams is itself corrupt (verified against raw
    // captures) — the official clients use Baichuan for live too. The grid's sub
    // streams stay RTSP (clean, and the NVR only allows ~1 Baichuan session).
    property string streamKey: ""
    property bool bcFallback: false // Baichuan slot busy → RTSP main for this round
    // The NVR answered "stream not found" over RTSP (it relays some cameras, e.g. a
    // third-party one, only over its native protocol) → use Baichuan for the sub stream.
    property bool bcSub: false

    // These carry the DEVICE ROW, not the pane index: the page tracks which
    // camera is maximized/selected, so both survive a rearrange.
    signal toggleMaximize(int index)
    signal clicked(int index)
    signal popOut(int deviceRow, string label)
    // A camera was dropped onto this pane — from the sidebar or another pane.
    signal cameraDropped(int pane, int row)

    // True while a drag hovers this pane, so it can show where it would land.
    property bool dropTarget: false

    // A pane hidden by a layout change holds its stream this long before
    // dropping it, so coming straight back costs nothing.
    property int keepAliveMs: 5000

    // Drop the stream for good. Separate from updateSource() so the deferred
    // path and the immediate one can't disagree about what "stopped" means.
    function releaseStream() {
        keepAlive.stop();
        if (streamKey === "")
            return;
        streamKey = "";
        player.stop();
    }

    Timer {
        id: keepAlive
        interval: root.keepAliveMs
        onTriggered: root.releaseStream()
    }

    function updateSource() {
        // Hidden by a layout change — maximizing another pane, or switching to a
        // preset this slot falls outside of. Hold the stream briefly rather than
        // tearing it down: restoring, or flipping 6 -> 4 -> 6, then reuses the
        // running session instead of reconnecting every pane (each reconnect
        // waits for the next keyframe, so it costs seconds per camera).
        //
        // Leaving Live View entirely is deliberately NOT covered: Playback needs
        // the NVR's session slots, and hidden panes holding them starve it.
        if (deviceRow >= 0 && pageActive && !visible) {
            if (streamKey !== "")
                keepAlive.restart();
            return;
        }
        keepAlive.stop();   // visible again (or shutting down): cancel the drop

        // Only stream when the pane is laid out AND its page is actually on screen
        // — otherwise hidden Live View panes keep streaming and exhaust the NVR's
        // limited session slots (starving Playback and other cameras).
        var want = deviceRow >= 0 && visible && pageActive;
        var key = "";
        if (want) {
            if (replaying)
                key = "rp:" + deviceRow + ":" + replayFrom;
            else if (effectiveMain && !bcFallback)
                key = "bc:" + deviceRow;
            else if (!effectiveMain && bcSub)
                key = "bcs:" + deviceRow;
            else
                key = Devices.liveUrl(deviceRow, effectiveMain);
        }
        if (key === streamKey)
            return;
        streamKey = key;
        if (key === "") {
            player.stop();
        } else if (key.substring(0, 3) === "rp:") {
            player.loop = false;
            Devices.startBaichuanPlayback(root.deviceRow, replayFrom, player, false);
        } else if (key.substring(0, 3) === "bc:") {
            player.loop = false;
            Devices.startBaichuanLive(root.deviceRow, player, true);
        } else if (key.substring(0, 4) === "bcs:") {
            player.loop = false;
            Devices.startBaichuanLive(root.deviceRow, player, false);
        } else {
            // Set loop before start() so the worker sees the right value from
            // frame one (the declarative `loop:` binding can lag the handler).
            player.loop = !key.startsWith("rtsp://") && !key.startsWith("rtmp://")
                       && !key.startsWith("tcp://") && !key.startsWith("udp://");
            // Declared size for the stream we're opening, so a transmitted-rotated
            // main stream (e.g. Duo 3) is presented upright.
            player.expectedSize = Devices.declaredSize(root.deviceRow, root.effectiveMain);
            player.source = key;
            player.start();
        }
    }
    // A pane pointed at a different camera must let the old one go now — its
    // session is wanted elsewhere (a swap hands it straight to another pane).
    // Talk ends with anything that takes the pane away from this camera: the camera
    // is swapped, the pane is hidden by a layout change, or the page is left —
    // the camera's speaker must never be left open behind the user's back.
    onDeviceRowChanged: { talk.stop(); bcFallback = false; bcSub = false; releaseStream(); updateSource(); }
    onVisibleChanged: { if (!visible) talk.stop(); updateSource(); }
    onPageActiveChanged: { if (!pageActive) talk.stop(); updateSource(); }
    onEffectiveMainChanged: { bcFallback = false; updateSource(); }
    Component.onCompleted: updateSource()

    // The NVR grants only ~1 Baichuan session. If HD live can't get the slot,
    // fall back to RTSP main once (artifact-prone on 8K duos, but beats a dead
    // pane); the next SD/HD flip or camera change retries Baichuan.
    Connections {
        target: player
        function onStateChanged() {
            // The NVR may refuse one more recorded-playback session: show live instead.
            if (player.state === StreamPlayer.Error && root.streamKey.substring(0, 3) === "rp:") {
                root.replayFailed = true;
                return;
            }
            if (player.state === StreamPlayer.Error && !root.bcFallback
                && root.streamKey.substring(0, 3) === "bc:") {
                root.bcFallback = true;
                root.updateSource();
            } else if (player.state === StreamPlayer.Error && !root.bcSub && !root.effectiveMain
                       && root.streamKey.startsWith("rtsp://")
                       && player.errorString.indexOf("Stream not found") === 0) {
                root.bcSub = true;
                root.updateSource();
            }
        }
    }

    // Recompute when the backing device finishes priming / changes.
    Connections {
        target: Devices
        function onDataChanged(topLeft, bottomRight) {
            if (root.deviceRow >= topLeft.row && root.deviceRow <= bottomRight.row)
                root.updateSource();
        }
    }

    StreamPlayer {
        id: player
        videoSink: video.videoSink
        // Hold the picture back ~1 s so network stalls do not show (playback is untouched).
        smoothing: true
        // Audible only while asked for AND actually on screen: a pane held briefly
        // after a layout change must not keep talking.
        // Also silent while we talk: the camera's own sound coming out of the
        // laptop would be picked up by the microphone and fed straight back.
        muted: !(root.audioOn && root.visible && root.pageActive && !talk.active)
    }

    // Microphone -> this camera's speaker.
    TalkSession { id: talk }

    Component.onDestruction: { talk.stop(); player.stop(); }

    // ---- Drag and drop -----------------------------------------------------
    // Dropping a camera here re-points this cell at it. Nothing needs to stop
    // the outgoing stream by hand: changing deviceRow (or dropping out of the
    // visible preset) runs updateSource(), and both StreamPlayer.setSource and
    // setPacketSource stop the running session first — which for an HD pane also
    // fires the Baichuan client's stop callback and frees the NVR's single slot.
    DropArea {
        anchors.fill: parent
        keys: ["reolink/camera"]
        onEntered: (drag) => {
            // Dragging a pane onto itself is a no-op; don't advertise a drop.
            root.dropTarget = !(drag.source && drag.source.sourcePane === root.paneIndex);
        }
        onExited: root.dropTarget = false
        onDropped: (drop) => {
            root.dropTarget = false;
            if (drop.source && drop.source.deviceRow >= 0)
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

    // The thing that actually gets dragged when this pane is picked up. It lives
    // in the window overlay so it isn't clipped by the pane it came from.
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

    // ---- Video with digital zoom ------------------------------------------
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
            visible: player.state === StreamPlayer.Streaming
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

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            drag.target: undefined
            property real lastX: 0
            property real lastY: 0

            // Keep the zoomed footage pinned to the viewport edges — panning must
            // not drag it off into empty space, and zooming out reels it back in.
            function clampPan() {
                var mx = Math.max(0, (video.contentRect.width * root.zoom - video.width) / 2);
                var my = Math.max(0, (video.contentRect.height * root.zoom - video.height) / 2);
                root.panX = Math.max(-mx, Math.min(mx, root.panX));
                root.panY = Math.max(-my, Math.min(my, root.panY));
            }
            // Drag state. A pane is picked up only when it isn't zoomed in —
            // while zoomed, dragging pans the image, which is the older and more
            // frequent gesture.
            property real pressX: 0
            property real pressY: 0
            property bool dragging: false
            readonly property bool canDrag: root.hasSource && root.zoom <= 1.0

            onClicked: {
                if (dragging)      // the release that ended a drag isn't a click
                    return;
                root.clicked(root.deviceRow);
            }
            onDoubleClicked: if (root.hasSource) root.toggleMaximize(root.deviceRow)
            onPressed: (m) => {
                lastX = m.x; lastY = m.y;
                pressX = m.x; pressY = m.y;
                dragging = false;
            }
            onPositionChanged: (m) => {
                if (root.zoom > 1.0 && (m.buttons & Qt.LeftButton)) {
                    root.panX += (m.x - lastX);
                    root.panY += (m.y - lastY);
                    lastX = m.x; lastY = m.y;
                    clampPan();
                    return;
                }
                if (!(m.buttons & Qt.LeftButton) || !canDrag)
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
                if (!root.capZoom && !root.hasSource) return;
                var z = root.zoom * (w.angleDelta.y > 0 ? 1.15 : 0.87);
                root.zoom = Math.max(1.0, Math.min(8.0, z));
                if (root.zoom <= 1.0) { root.panX = 0; root.panY = 0; }
                else clampPan();
            }
        }
    }

    // ---- Name + zoom badge -------------------------------------------------

    // Activity marker: amber inside the selection's blue border, so both stay visible.
    Rectangle {
        visible: root.activityKind !== ""
        z: 19
        anchors.fill: parent
        anchors.margins: root.selected ? 3 : 0
        color: "transparent"
        border.color: "#f0a030"
        border.width: 3
    }

    // Why this camera is here (Activity mode): "Person · 14:02", with a Live button
    // while the tile is replaying the moment.
    Rectangle {
        visible: root.activityKind !== ""
        z: 20
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 6
        radius: 3
        color: "#e6f0a030"
        width: badgeRow.implicitWidth + 12
        height: badgeRow.implicitHeight + 6
        Row {
            id: badgeRow
            anchors.centerIn: parent
            spacing: 8
            Text { text: root.activityText; color: "#101010"; font.pixelSize: 11; font.bold: true }
            Text {
                visible: root.replaying
                text: qsTr("● Replay")
                color: "#101010"
                font.pixelSize: 11
            }
            Text {
                visible: root.replaying
                text: qsTr("Live ▸")
                color: "#101010"
                font.pixelSize: 11
                font.bold: true
                font.underline: true
                TapHandler { onTapped: root.replayLive = true }
            }
        }
    }

    Rectangle {
        visible: root.hasSource && player.state === StreamPlayer.Streaming
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
            // Live-microphone marker: you are transmitting to this camera.
            Text {
                visible: talk.state === TalkSession.Talking
                text: "🎙"
                font.pixelSize: 11
            }
            // Shows which camera you are hearing (only when it has sound to play).
            Text {
                visible: root.audioOn && player.hasAudio && !player.muted
                text: "🔊"
                font.pixelSize: 11
            }
            Text {
                visible: root.zoom > 1.01
                text: root.zoom.toFixed(1) + "×"
                color: Theme.accent
                font.pixelSize: 11
            }
        }
    }

    // ---- Connection-state overlay -----------------------------------------
    Column {
        anchors.centerIn: parent
        spacing: Theme.spacing
        visible: player.state !== StreamPlayer.Streaming

        BusyIndicator {
            anchors.horizontalCenter: parent.horizontalCenter
            running: player.state === StreamPlayer.Connecting
            visible: running
            width: 32
            height: 32
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
                switch (player.state) {
                case StreamPlayer.Connecting: return qsTr("Connecting…");
                case StreamPlayer.Error: return player.errorString;
                case StreamPlayer.Stopped: return qsTr("Stopped");
                default: return "";
                }
            }
        }
    }

    // ---- Talk status ---------------------------------------------------------
    // Connecting, or why talking failed. Tap an error to dismiss it.
    Rectangle {
        visible: root.hasSource &&
                 (talk.state === TalkSession.Error || talk.state === TalkSession.Connecting)
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.topMargin: 30
        radius: 3
        color: talk.state === TalkSession.Error ? "#cc7a1f1f" : "#cc0d141b"
        width: talkMsg.width + 16
        height: talkMsg.implicitHeight + 8
        z: 20
        Text {
            id: talkMsg
            anchors.centerIn: parent
            width: Math.min(implicitWidth, root.width - 36)
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            color: "white"
            font.pixelSize: 11
            text: talk.state === TalkSession.Error
                  ? qsTr("Talk: %1").arg(talk.errorString)
                  : qsTr("Connecting to camera speaker…")
        }
        TapHandler { onTapped: talk.stop() }
    }

    // ---- PTZ joystick overlay ---------------------------------------------
    property bool ptzOpen: false
    Loader {
        active: root.ptzOpen && root.capPtz
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.rightMargin: 10
        sourceComponent: PtzPad {
            deviceRow: root.deviceRow
            showZoom: root.capZoom
        }
    }

    // ---- Floating toolbar (hover) -----------------------------------------
    HoverHandler { id: paneHover }

    Rectangle {
        id: toolbar
        visible: root.hasSource && (paneHover.hovered || root.ptzOpen)
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottomMargin: 8
        radius: Theme.radius
        color: "#cc0d141b"
        border.color: Theme.border
        height: 34
        width: toolRow.implicitWidth + 16

        Row {
            id: toolRow
            anchors.centerIn: parent
            spacing: 2

            component ToolButton: Rectangle {
                property string glyph: ""
                property string tip: ""
                property bool active: false
                property bool enabledTool: true
                signal activated()
                width: 28; height: 28; radius: 4
                visible: enabledTool
                color: active ? Theme.accentDim : (hover.hovered ? Theme.surfaceAlt : "transparent")
                Text {
                    anchors.centerIn: parent
                    text: parent.glyph
                    color: parent.active ? Theme.text : Theme.textMuted
                    font.pixelSize: 14
                }
                HoverHandler { id: hover }
                ToolTip {
                    visible: hover.hovered && tip !== ""
                    delay: 500
                    x: (parent.width - width) / 2
                    y: -height - 8
                    contentItem: Text { text: tip; color: Theme.text; font.pixelSize: 11 }
                    background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: 4 }
                }
                // ReleaseWithinBounds takes an exclusive grab on press so the
                // full-pane MouseArea (click-to-select / pan) behind the video
                // can't steal the tap.
                TapHandler {
                    gesturePolicy: TapHandler.ReleaseWithinBounds
                    onTapped: parent.activated()
                }
            }

            // Quality: Fluent / (Balanced) / Clear. Balanced omitted until wired.
            ToolButton {
                glyph: root.qualityMain ? "HD" : "SD"
                tip: root.qualityMain ? qsTr("Switch to Fluent (SD)") : qsTr("Switch to Clear (HD)")
                active: root.qualityMain
                onActivated: root.qualityMain = !root.qualityMain
            }
            ToolButton {
                glyph: "◉"; enabledTool: true
                tip: qsTr("Save snapshot")
                onActivated: Devices.snapshot(root.deviceRow)
            }
            // Manual record: taps the displayed stream, no second connection.
            ToolButton {
                glyph: "⏺"; active: player.recording
                tip: player.recording ? qsTr("Stop recording") : qsTr("Record video")
                onActivated: player.recording ? player.stopRecording() : player.startRecording()
            }
            ToolButton {
                glyph: "⊕"; active: root.zoom > 1.01
                tip: root.zoom > 1.01 ? qsTr("Reset zoom") : qsTr("Digital zoom (scroll to adjust)")
                onActivated: { if (root.zoom > 1.01) { root.zoom = 1; root.panX = 0; root.panY = 0; }
                               else root.zoom = 2; }
            }
            ToolButton {
                glyph: "⇅"; active: root.ptzOpen; enabledTool: root.capPtz
                tip: qsTr("PTZ controls")
                onActivated: root.ptzOpen = !root.ptzOpen
            }
            // Two-way talk over the Baichuan talk channel (DESIGN §5.4). Click to
            // start, click again to stop; a failure shows on the tile.
            ToolButton {
                glyph: "🎙"; active: talk.active; enabledTool: root.capTalk
                tip: talk.state === TalkSession.Talking ? qsTr("Talking — click to stop")
                   : talk.state === TalkSession.Connecting ? qsTr("Connecting…")
                   : qsTr("Two-way talk (click to start, click again to stop)")
                onActivated: talk.active ? talk.stop() : Devices.startTalk(root.deviceRow, talk)
            }
            ToolButton {
                glyph: "📢"; enabledTool: root.capSiren
                tip: qsTr("Sound siren")
                // AudioAlarmPlay takes its fields FLAT in param (verified on real
                // firmware — a Set*-style wrapped object gets rspCode -4 "param
                // error"). alarm_mode "times"/1 sounds a single blast.
                onActivated: Devices.applySetting(root.deviceRow, "AudioAlarmPlay",
                    { "alarm_mode": "times", "times": 1, "channel": Devices.channelOf(root.deviceRow) })
            }
            ToolButton {
                glyph: "💡"; active: root.floodOn; enabledTool: root.capFloodlight
                tip: qsTr("Toggle floodlight")
                onActivated: {
                    root.floodOn = !root.floodOn;       // optimistic; toast confirms
                    Devices.toggleFloodlight(root.deviceRow);
                }
            }
            // Pop out into a detached window (drag to another monitor).
            ToolButton {
                glyph: "⧉"
                tip: qsTr("Pop out to a window")
                onActivated: if (root.hasSource) root.popOut(root.deviceRow, root.label)
            }
            ToolButton {
                glyph: "⛶"
                tip: root.forceMain ? qsTr("Restore grid") : qsTr("Maximize")
                onActivated: if (root.hasSource) root.toggleMaximize(root.deviceRow)
            }
        }
    }
}
