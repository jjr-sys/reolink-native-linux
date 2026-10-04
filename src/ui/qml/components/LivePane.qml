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
    // Ceiling-mounted fisheye cameras: which dewarped view to show (see FisheyeView).
    // "fisheye" is the raw circle, i.e. no dewarping.
    property bool fisheyeLens: false
    property string fisheyeMode: "fisheye"
    readonly property bool dewarp: fisheyeLens && fisheyeMode !== "fisheye"
    onDewarpChanged: { if (dewarp) { zoom = 1.0; panX = 0; panY = 0; } }
    property bool pageActive: true // false when the Live View page isn't on screen
    property bool audioAllowed: true // false while something else (the doorbell pop-up) has the sound

    // Activity mode: this camera holds a tile because of a detection (badge text).
    property string activityKind: ""
    property string activityText: ""

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
    signal openBoschTourRequested()
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
            if (effectiveMain && !bcFallback && !Devices.isDirectAt(deviceRow))
                key = "bc:" + deviceRow;
            else if (!effectiveMain && bcSub && !Devices.isDirectAt(deviceRow))
                key = "bcs:" + deviceRow;
            else
                key = Devices.liveUrl(deviceRow, effectiveMain);
        }
        if (key === streamKey)
            return;
        streamKey = key;
        if (key === "") {
            player.stop();
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
    onDeviceRowChanged: {
        talk.stop(); ptzOpen = false; boschPtzOpen = false;
        if (boschTourOwnedHere && BoschTour.selectionTourMode)
            BoschTour.endSelectionTour();
        boschTourOwnedHere = false;
        bcFallback = false; bcSub = false; releaseStream(); updateSource();
    }
    onLabelChanged: {
        if (!isBoschFeed) {
            boschPtzOpen = false;
            if (boschTourOwnedHere && BoschTour.selectionTourMode)
                BoschTour.endSelectionTour();
            boschTourOwnedHere = false;
        }
    }
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
        muted: !(root.audioOn && root.audioAllowed && root.visible && root.pageActive && !talk.active)
    }

    // Microphone -> this camera's speaker.
    TalkSession { id: talk }

    Component.onDestruction: {
        talk.stop(); player.stop();
        if (root.boschTourOwnedHere && BoschTour.selectionTourMode)
            BoschTour.endSelectionTour();
    }

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
        id: videoArea
        anchors.fill: parent
        anchors.margins: 1
        clip: true

        VideoOutput {
            id: video
            anchors.fill: parent
            // Dewarping samples the whole frame as a texture, so it must fill the item.
            fillMode: root.dewarp ? VideoOutput.Stretch : VideoOutput.PreserveAspectFit
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

        // The dewarped views. Created only while a dewarp mode is on, so an ordinary
        // camera, or a fisheye one showing its raw circle, pays nothing for it.
        Loader {
            id: fisheyeLoader
            anchors.fill: parent
            active: root.dewarp && player.state === StreamPlayer.Streaming
            sourceComponent: FisheyeView {
                sourceItem: video
                viewMode: root.fisheyeMode
            }
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
                if (fisheyeLoader.item && (m.buttons & Qt.LeftButton)) {
                    fisheyeLoader.item.dragBy(m.x, m.y, m.x - lastX, m.y - lastY);
                    lastX = m.x; lastY = m.y;
                    return;
                }
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
                if (fisheyeLoader.item) {
                    fisheyeLoader.item.zoomBy(w.x, w.y, w.angleDelta.y > 0);
                    return;
                }
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

    // Why this camera is here (Activity mode): "Person · 14:02".
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
        anchors.centerIn: videoArea
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
        anchors.horizontalCenter: videoArea.horizontalCenter
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
    property bool boschPtzOpen: false
    property bool boschTourOwnedHere: false
    readonly property bool isBoschFeed: root.hasSource
        && root.label.toLowerCase().indexOf("bosch") !== -1

    function positionSequenceIndex(token) {
        var sequence = BoschTour.selectionTourRoute;
        for (var i = 0; i < sequence.length; ++i) {
            if (String(sequence[i].token) === String(token))
                return i;
        }
        return -1;
    }

    function activateTourSelection() {
        if (BoschTour.selectionTourMode) {
            root.boschTourOwnedHere = false;
            BoschTour.endSelectionTour();
        } else {
            root.boschTourOwnedHere = BoschTour.beginSelectionTour();
        }
    }

    function toggleTourPosition(preset) {
        if (!BoschTour.selectionTourMode) {
            BoschTour.gotoPreset(String(preset.token));
            return;
        }
        BoschTour.toggleSelectionTourPosition(String(preset.token));
    }

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

    Loader {
        active: root.boschPtzOpen && root.isBoschFeed
        anchors.right: toolbar.right
        anchors.bottom: toolbar.top
        anchors.bottomMargin: 8
        z: 23
        sourceComponent: PtzPad {
            deviceRow: -1
            showZoom: true
            boschCamera: true
            onOpenBoschTourRequested: root.openBoschTourRequested()
        }
    }

    // ---- Floating toolbar (hover) -----------------------------------------
    HoverHandler { id: paneHover }

    Rectangle {
        id: toolbar
        visible: root.hasSource && (paneHover.hovered || root.ptzOpen || root.boschPtzOpen)
        anchors.bottom: videoArea.bottom
        anchors.horizontalCenter: videoArea.horizontalCenter
        anchors.bottomMargin: 8
        radius: Theme.radius
        color: "#cc0d141b"
        border.color: Theme.border
        height: Math.max(36, toolRow.implicitHeight + 16)
        width: Math.min(toolRow.implicitWidth + 16, Math.max(1, videoArea.width - 16))

        Flickable {
            id: toolScroll
            anchors.fill: parent
            anchors.margins: 8
            clip: true
            contentWidth: Math.max(width, toolRow.implicitWidth)
            contentHeight: toolRow.implicitHeight
            flickableDirection: Flickable.HorizontalFlick
            boundsBehavior: Flickable.StopAtBounds
            interactive: contentWidth > width

            Row {
                id: toolRow
                x: Math.max(0, (toolScroll.width - implicitWidth) / 2)
                y: (toolScroll.height - implicitHeight) / 2
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
                    TapHandler {
                        gesturePolicy: TapHandler.ReleaseWithinBounds
                        onTapped: parent.activated()
                    }
                }

                component IrisButton: Rectangle {
                    id: irisButton
                    property string direction: "open"
                    property string label: "Open"
                    property bool requested: false
                    readonly property string statusTip: direction === "open"
                        ? qsTr("Open iris · %1").arg(BoschTour.irisStatus)
                        : qsTr("Close iris · %1").arg(BoschTour.irisStatus)
                    width: 48; height: 28; radius: 4
                    visible: root.isBoschFeed && BoschTour.connected
                    color: irisPress.pressed ? Theme.accentDim : Theme.surface
                    opacity: root.isBoschFeed && BoschTour.connected && BoschTour.irisAvailable ? 1 : 0.45
                    border.color: Theme.border
                    Text {
                        anchors.centerIn: parent
                        text: irisButton.label
                        color: Theme.text
                        font.pixelSize: 10
                        font.bold: true
                    }
                    MouseArea {
                        id: irisPress
                        anchors.fill: parent
                        enabled: root.isBoschFeed && BoschTour.connected && BoschTour.irisAvailable
                        Accessible.name: irisButton.direction === "open"
                            ? qsTr("Open iris; hold to adjust") : qsTr("Close iris; hold to adjust")
                        onPressed: irisButton.requested = BoschTour.startIris(irisButton.direction)
                        onReleased: {
                            if (irisButton.requested)
                                BoschTour.stopIris();
                            irisButton.requested = false;
                        }
                        onCanceled: {
                            if (irisButton.requested)
                                BoschTour.stopIris();
                            irisButton.requested = false;
                        }
                    }
                    HoverHandler { id: irisHover }
                    ToolTip {
                        visible: irisHover.hovered
                        delay: 500
                        text: irisButton.statusTip
                        contentItem: Text { text: irisButton.statusTip; color: Theme.text; font.pixelSize: 11 }
                        background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: 4 }
                    }
                }

                ToolButton {
                    glyph: root.qualityMain ? "HD" : "SD"
                    tip: root.qualityMain ? qsTr("Switch to Fluent (SD)") : qsTr("Switch to Clear (HD)")
                    active: root.qualityMain
                    onActivated: root.qualityMain = !root.qualityMain
                }

                IrisButton { direction: "open"; label: qsTr("Open") }
                IrisButton { direction: "close"; label: qsTr("Close") }
                ToolButton {
                    glyph: "A"
                    active: BoschTour.irisStatus.startsWith("AUTO")
                    enabledTool: root.isBoschFeed && BoschTour.connected
                    tip: qsTr("Restore automatic exposure · current iris: %1").arg(BoschTour.irisStatus)
                    onActivated: BoschTour.setIrisAuto()
                }
                ToolButton {
                    glyph: "I"
                    active: BoschTour.irisStatus.startsWith("MANUAL")
                    enabledTool: root.isBoschFeed && BoschTour.connected
                    tip: qsTr("Iris state: %1").arg(BoschTour.irisStatus)
                    onActivated: BoschTour.refreshIris()
                }

                Repeater {
                    model: root.isBoschFeed && BoschTour.connected ? BoschTour.presets : []
                    delegate: Rectangle {
                        id: presetButton
                        required property var modelData
                        readonly property string fullPositionName: String(modelData.name)
                        readonly property string positionButtonText: root.forceMain
                            ? fullPositionName
                            : fullPositionName.trim().length > 0
                                ? fullPositionName.trim().charAt(0).toUpperCase() : ""
                        readonly property int tourIndex: root.positionSequenceIndex(modelData.token)
                        readonly property string positionTip: !BoschTour.selectionTourMode
                            ? qsTr("Camera Position")
                            : tourIndex >= 0
                                ? qsTr("Remove %1 from tour (step %2)")
                                      .arg(fullPositionName).arg(tourIndex + 1)
                                : qsTr("Add %1 to tour").arg(fullPositionName)
                        readonly property bool positionEnabled: BoschTour.connected
                            && (!BoschTour.busy || BoschTour.selectionTourRestoring)
                            && (BoschTour.selectionTourMode || BoschTour.selectionTourRestoring
                                || BoschTour.tourState === "stopped")
                        width: root.forceMain
                            ? Math.max(74, Math.min(150,
                                fullPositionName.length * 6.3 + (tourIndex >= 0 ? 28 : 14)))
                            : 28
                        height: 28
                        radius: 4
                        color: tourIndex >= 0 ? Theme.accentDim
                             : positionHover.hovered ? Theme.surfaceAlt : "transparent"
                        border.color: tourIndex >= 0 || positionHover.hovered ? Theme.accent : Theme.border

                        Text {
                            id: positionLabel
                            anchors.fill: parent
                            anchors.leftMargin: root.forceMain
                                ? (presetButton.tourIndex >= 0 ? 22 : 5) : 2
                            anchors.rightMargin: root.forceMain ? 5 : 2
                            anchors.topMargin: !root.forceMain && presetButton.tourIndex >= 0 ? 8 : 0
                            text: presetButton.positionButtonText
                            color: presetButton.positionEnabled ? Theme.text : Theme.textMuted
                            font.pixelSize: root.forceMain ? 10 : 11
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                        Rectangle {
                            visible: presetButton.tourIndex >= 0
                            anchors.left: parent.left
                            anchors.leftMargin: root.forceMain ? 0 : 1
                            y: root.forceMain ? (parent.height - height) / 2 : 1
                            width: root.forceMain ? 17 : 12
                            height: width
                            radius: width / 2
                            color: Theme.accent
                            Text {
                                anchors.centerIn: parent
                                text: presetButton.tourIndex + 1
                                color: Theme.window
                                font.pixelSize: root.forceMain ? 9 : 8
                                font.bold: true
                            }
                        }
                        HoverHandler { id: positionHover }
                        ToolTip {
                            visible: positionHover.hovered
                            delay: 500
                            text: presetButton.positionTip
                            contentItem: Text { text: presetButton.positionTip; color: Theme.text; font.pixelSize: 11 }
                            background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: 4 }
                        }
                        TapHandler {
                            enabled: presetButton.positionEnabled
                            gesturePolicy: TapHandler.ReleaseWithinBounds
                            onTapped: root.toggleTourPosition(presetButton.modelData)
                        }
                    }
                }

                ToolButton {
                    visible: root.isBoschFeed && !BoschTour.connected
                    glyph: "!"
                    active: true
                    tip: qsTr("Home Assistant is disconnected. Reconnect to restore Bosch PTZ, iris, positions, and tours.")
                    onActivated: root.openBoschTourRequested()
                }

                ToolButton {
                    glyph: "◉"
                    tip: qsTr("Save snapshot")
                    onActivated: Devices.snapshot(root.deviceRow)
                }
                ToolButton {
                    glyph: "⏺"; active: player.recording
                    tip: player.recording ? qsTr("Stop recording") : qsTr("Record video")
                    onActivated: player.recording ? player.stopRecording() : player.startRecording()
                }
                ToolButton {
                    glyph: "⟳"; active: root.viewRotation !== 0
                    tip: root.viewRotation !== 0 ? qsTr("Rotate view 90° (now %1°)").arg(root.viewRotation)
                                                : qsTr("Rotate view 90°")
                    onActivated: Devices.setRotationOverride(root.deviceRow, root.viewRotation + 90)
                }
                ToolButton {
                    id: viewModeButton
                    glyph: "◔"; active: root.dewarp; enabledTool: root.fisheyeLens
                    tip: qsTr("Fisheye view mode")
                    onActivated: viewModeMenu.popup(viewModeButton, 0, -viewModeMenu.implicitHeight - 6)
                }
                ToolButton {
                    glyph: "⊕"; active: root.zoom > 1.01
                    tip: root.zoom > 1.01 ? qsTr("Reset zoom") : qsTr("Digital zoom (scroll to adjust)")
                    onActivated: { if (root.zoom > 1.01) { root.zoom = 1; root.panX = 0; root.panY = 0; }
                                   else root.zoom = 2; }
                }
                ToolButton {
                    glyph: "⇅"; active: root.isBoschFeed ? root.boschPtzOpen : root.ptzOpen
                    enabledTool: root.capPtz || (root.isBoschFeed && root.width >= 760 && root.height >= 460)
                    tip: qsTr("PTZ controls")
                    onActivated: {
                        if (root.isBoschFeed) {
                            root.boschPtzOpen = !root.boschPtzOpen;
                            root.ptzOpen = false;
                            if (root.boschPtzOpen && BoschTour.selectionTourMode)
                                root.activateTourSelection();
                        } else {
                            root.ptzOpen = !root.ptzOpen;
                        }
                    }
                }
                ToolButton {
                    glyph: "T"; active: BoschTour.selectionTourMode
                    enabledTool: root.isBoschFeed && root.width >= 760 && root.height >= 460
                    tip: !BoschTour.connected
                         ? qsTr("Reconnect Home Assistant to restore positions and tours")
                         : BoschTour.selectionTourMode
                             ? qsTr("Turn off tour and clear positions")
                             : qsTr("Select positions for a tour")
                    onActivated: {
                        if (!BoschTour.connected) {
                            root.openBoschTourRequested();
                            return;
                        }
                        if (!BoschTour.selectionTourMode) {
                            root.boschPtzOpen = false;
                            root.ptzOpen = false;
                        }
                        root.activateTourSelection();
                    }
                }
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
                    onActivated: Devices.applySetting(root.deviceRow, "AudioAlarmPlay",
                        { "alarm_mode": "times", "times": 1, "channel": Devices.channelOf(root.deviceRow) })
                }
                ToolButton {
                    glyph: "💡"; active: root.floodOn; enabledTool: root.capFloodlight
                    tip: qsTr("Toggle floodlight")
                    onActivated: {
                        root.floodOn = !root.floodOn;
                        Devices.toggleFloodlight(root.deviceRow);
                    }
                }
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

    ThemedMenu {
        id: viewModeMenu
        Repeater {
            model: [
                { mode: "fisheye",     label: qsTr("Fisheye (original)") },
                { mode: "ptz",         label: qsTr("Virtual PTZ (drag to look around, scroll to zoom)") },
                { mode: "panorama",    label: qsTr("Panorama 360°") },
                { mode: "dual",        label: qsTr("Dual panorama (2 × 180°)") },
                { mode: "quad",        label: qsTr("Quad view (4 directions)") },
                { mode: "fisheye_ptz", label: qsTr("Fisheye + virtual PTZ") }
            ]
            ThemedMenuItem {
                required property var modelData
                text: (root.fisheyeMode === modelData.mode ? "✓  " : "     ") + modelData.label
                onTriggered: Devices.setViewMode(root.deviceRow, modelData.mode)
            }
        }
    }
}
