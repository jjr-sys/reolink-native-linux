import QtQuick
import QtQuick.Controls
import QtQuick.Window
import QtMultimedia
import ReolinkApp
import ReolinkApp.Core
import "HighlightText.js" as HighlightText

// One cell of the live grid: video + name overlay + connection-state overlay +
// the pane bar (PaneBar: transport, HD, sound, tools) and an optional PTZ joystick. Double-click toggles
// maximize (handled by the page). Digital zoom via wheel + drag.
Rectangle {
    id: root
    color: Theme.paneBackground
    border.color: selected ? Theme.accent : Theme.border
    border.width: selected && highlighted ? 3 : 1
    clip: true

    property int paneIndex: -1     // grid slot
    property int deviceRow: -1     // row for camera controls, never stream identity
    property var cameraIdentity: ({})
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
    // The camera's aim store key (FisheyeAims), shared with Playback and Events.
    readonly property string cameraKey: cameraIdentity && cameraIdentity.hostId !== undefined
                                        ? cameraIdentity.hostId + ":" + cameraIdentity.channel : ""
    // Double-click on a turned view puts it back; otherwise it maximises (ticket 13, point 5).
    function resetAimOrMaximize() {
        if (fisheyeLoader.item && FisheyeAims.aimed(cameraKey, fisheyeMode))
            FisheyeAims.reset(cameraKey, fisheyeMode);
        else if (hasSource)
            toggleMaximize(deviceRow);
    }
    onDewarpChanged: { if (dewarp) { zoom = 1.0; panX = 0; panY = 0; } }
    // The picture's shape (width/height) as decoded, before the rotation override; 0 until
    // a frame is on screen. Read off the drawn picture, so the stream's own rotation is
    // included. Not known while dewarping, which stretches the frame over the pane. The
    // drawn size is rounded to pixels, so it moves only on a real change (> 3%): a re-pack
    // resizing the pane must not read back as a new shape.
    readonly property real frameRatio: measuredRatio
    property real measuredRatio: 0
    function measureRatio() {
        var r = video.contentRect;
        if (player.state !== StreamPlayer.Streaming || dewarp || r.width < 2 || r.height < 2)
            return;
        var m = (viewRotation % 180 !== 0) ? r.height / r.width : r.width / r.height;
        if (!(measuredRatio > 0) || Math.abs(m - measuredRatio) / measuredRatio > 0.03)
            measuredRatio = m;
    }
    Connections {
        target: video
        function onContentRectChanged() { root.measureRatio(); }
    }
    // The mosaic shows a camera that stopped answering greyed out, in its place.
    property bool offline: false
    property bool pageActive: true // false when the Live View page isn't on screen
    property bool audioAllowed: true // false while something else (the doorbell pop-up) has the sound

    // Highlight (Highlights.looks entry): { kind, colour, style, since, flashUntil, last },
    // times in epoch ms; null when not highlighted. kind "doorbell" is the pop-up's fixed one.
    property var highlight: null
    // Maximised and replaying: the other shown cameras highlighted now (LiveReplayControls
    // othersHighlighted); a tap asks the page to show that camera live (live-replay ticket 14).
    property var replayOthersHighlighted: []
    signal showCameraRequested(string key)
    readonly property bool highlighted: highlight !== null
    property real clock: Date.now()  // ticks while highlighted: flash phase and "N s ago"
    Timer {
        running: root.highlighted
        interval: 250
        repeat: true
        triggeredOnStart: true
        onTriggered: root.clock = Date.now()
    }
    readonly property bool highlightLit: highlighted
        && (clock >= highlight.flashUntil || Math.floor((clock - highlight.since) / 400) % 2 === 0)
    function highlightLabel() {
        if (!highlighted)
            return "";
        var k = HighlightText.kindText(highlight.kind);
        if (highlight.kind === "doorbell")
            return k;
        var ago = Math.floor((clock - highlight.last) / 1000);
        return ago < 2 ? k : qsTr("%1 · %2 s ago").arg(k).arg(ago);
    }

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
    // In a mosaic, restMain is the HD choice remembered for this camera there: the
    // quality the pane returns to when it is not maximized.
    property bool restMain: false
    property bool qualityMain: restMain
    readonly property bool effectiveMain: qualityMain
    onForceMainChanged: qualityMain = forceMain || restMain
    onRestMainChanged: if (!forceMain) qualityMain = restMain
    signal qualityChosen(bool main) // the SD/HD toggle was used
    // Why this pane may not switch to HD now ("" = it may), e.g. the Site budget
    // (wayfinder/site-tabs-redesign). Not asked while maximised: one pane alone goes HD.
    property var hdRefusal: null
    // Watch for a stream that opens but shows no picture, and retry it (LiveStream).
    property bool watchPicture: false
    // Live replay (wayfinder/live-replay ticket 07): the page's one LiveReplay, and whether
    // this camera can replay (a Reolink recorder camera). While it replays, the live stream
    // stops and its recording plays over the picture (LiveReplayControls).
    property LiveReplay replay: null
    property bool replayable: false
    readonly property bool replaying: replayControls.replaying
    function replayKey(k) { return replayControls.key(k, paneBar.press(k)); }
    function replayBehind(secs, dir, fast) { replayControls.playBehind(secs, dir, fast); }
    function replayGoLive() { replayControls.goLive(); }
    function replayPause() { replayControls.pause(); }
    // What the camera sees now, for "● Live · Person" while replaying.
    readonly property string highlightKindText: highlighted ? HighlightText.kindText(highlight.kind) : ""

    // Mosaic panes can be Normal or Large (about 1.5× the area, from the packer).
    property bool showLarge: false
    property bool large: false
    signal largeToggled()
    readonly property bool hasSource: deviceRow >= 0
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
    // Optional (deviceRow) => bool: whether a camera may be dropped here at all.
    property var dropFilter: null
    function dropAllowed(drag) {
        return !root.dropFilter || !drag.source || root.dropFilter(drag.source.deviceRow);
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
    }
    onLabelChanged: {
        if (!isBoschFeed) {
            boschPtzOpen = false;
            boschPicker.close();
            if (boschTourOwnedHere && BoschTour.selectionTourMode)
                BoschTour.endSelectionTour();
            boschTourOwnedHere = false;
        }
    }
    onIsBoschFeedChanged: {
        if (isBoschFeed && visible && pageActive)
            BoschTour.refreshSceneMode();
        else if (!isBoschFeed)
            boschPicker.close();
    }
    // Another camera on this pane: its replay ends (a model row shift keeps the camera).
    // A camera moving to another pane stays in the replay there; one taken out of the selection
    // leaves it (the page, ticket 15b).
    onVisibleChanged: {
        if (!visible) talk.stop();
        // Hidden (another pane maximised) a replaying pane stays in the replay, playing nothing;
        // a camera taken out of the selection leaves it (the page).
        if (visible && pageActive && isBoschFeed) BoschTour.refreshSceneMode();
    }
    onPageActiveChanged: {
        if (!pageActive) talk.stop();
        if (visible && pageActive && isBoschFeed) BoschTour.refreshSceneMode();
    }

    Timer {
        interval: 30000
        repeat: true
        running: root.visible && root.pageActive && root.isBoschFeed && BoschTour.connected
        onTriggered: BoschTour.refreshSceneMode()
    }

    LiveStream {
        id: player
        devices: Devices
        camera: root.cameraIdentity
        mainStream: root.effectiveMain
        // Replaying, the stream stays connected filling the live buffer, not decoded
        // (live-replay ticket 08b): "● Live" picks up at once.
        active: root.pageActive
        receiveOnly: root.replaying
        viewVisible: root.visible
        watchPicture: root.watchPicture
        videoSink: video.videoSink
        muted: !(root.audioOn && root.audioAllowed && !talk.active) || AudioPrefs.liveMuted
    }

    // The Site tab drawer shows each camera's last-seen frame: kept a few seconds after
    // the picture starts, then every 30 s while it plays.
    property bool keepLastFrame: false
    Timer {
        id: lastFrameTimer
        interval: 3000
        repeat: true
        running: root.keepLastFrame && root.visible && player.state === StreamPlayer.Streaming
        onRunningChanged: interval = 3000
        onTriggered: { player.saveLastFrame(); interval = 30000; }
    }

    // Microphone -> this camera's speaker.
    TalkSession { id: talk }

    Component.onDestruction: {
        talk.stop(); player.release();
        if (root.boschTourOwnedHere && BoschTour.selectionTourMode)
            BoschTour.endSelectionTour();
    }

    // ---- Drag and drop -----------------------------------------------------
    // A camera assignment changes the live stream's stable identity. Its module
    // releases the previous transport before opening the replacement.
    DropArea {
        anchors.fill: parent
        keys: ["reolink/camera"]
        onEntered: (drag) => {
            if (!root.dropAllowed(drag)) {
                drag.accepted = false;   // refused: no highlight, no drop
                return;
            }
            // Dragging a pane onto itself is a no-op; don't advertise a drop.
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
                cameraKey: root.cameraKey
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
            onDoubleClicked: root.resetAimOrMaximize()
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
                if (root.dewarp) {   // a dewarped view zooms itself; no digital zoom over it
                    if (fisheyeLoader.item)
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

    // Highlight border: inside the selection's blue border, so both stay visible.
    Rectangle {
        visible: root.highlightLit
        z: 19
        anchors.fill: parent
        anchors.margins: root.selected ? 3 : 0
        color: "transparent"
        border.color: root.highlighted ? root.highlight.colour : "transparent"
        border.width: 4
    }

    // What was detected, and how long ago the app heard of it: "Person · 8 s ago".
    Rectangle {
        visible: root.highlighted
        z: 20
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 6
        radius: 3
        color: root.highlighted ? root.highlight.colour : "transparent"
        width: badgeRow.implicitWidth + 12
        height: badgeRow.implicitHeight + 6
        Row {
            id: badgeRow
            anchors.centerIn: parent
            spacing: 8
            Text { text: root.highlightLabel(); color: "#101010"; font.pixelSize: 11; font.bold: true }
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
                visible: player.degraded
                text: qsTr("Degraded · RTSP fallback")
                color: Theme.warning
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

    // ---- Offline (mosaic) ------------------------------------------------------
    Rectangle {
        visible: root.offline
        anchors.fill: videoArea
        z: 18
        color: "#b0101418"
        Rectangle {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 6
            radius: 3
            color: Theme.surfaceAlt
            border.color: Theme.border
            width: offlineText.implicitWidth + 12
            height: offlineText.implicitHeight + 6
            Text {
                id: offlineText
                anchors.centerIn: parent
                text: qsTr("Offline")
                color: Theme.textMuted
                font.pixelSize: 11
            }
        }
        Text {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: 9
            text: root.label
            color: Theme.textMuted
            font.pixelSize: 11
        }
    }

    // ---- Connection-state overlay -----------------------------------------
    Column {
        anchors.centerIn: videoArea
        spacing: Theme.spacing
        visible: (player.state !== StreamPlayer.Streaming || player.noPicture) && !root.offline

        BusyIndicator {
            anchors.horizontalCenter: parent.horizontalCenter
            running: player.state === StreamPlayer.Connecting && !player.noPicture
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
                if (player.noPicture) return qsTr("No picture: retrying");
                switch (player.state) {
                case StreamPlayer.Connecting: return qsTr("Connecting…");
                case StreamPlayer.Error: return player.errorString
                    + (player.recovering ? qsTr(" · Reconnecting…") : "");
                case StreamPlayer.Stopped: return qsTr("Stopped");
                default: return "";
                }
            }
        }
    }

    // A short notice over the picture, e.g. why HD was refused.
    Rectangle {
        id: paneNotice
        function show(msg) {
            noticeText.text = msg;
            opacity = 1;
            noticeTimer.restart();
        }
        anchors.horizontalCenter: videoArea.horizontalCenter
        anchors.bottom: videoArea.bottom
        anchors.bottomMargin: 40
        z: 60
        width: Math.min(noticeText.implicitWidth + 20, root.width - 16)
        height: noticeText.implicitHeight + 12
        radius: Theme.radius
        color: Qt.alpha(Theme.surfaceAlt, 0.94)
        border.color: Theme.border
        opacity: 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 180 } }
        Text {
            id: noticeText
            anchors.centerIn: parent
            width: Math.min(implicitWidth, root.width - 36)
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            color: Theme.text
            font.pixelSize: 11
        }
        Timer { id: noticeTimer; interval: 5000; onTriggered: paneNotice.opacity = 0 }
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
        color: talk.state === TalkSession.Error ? Theme.paneBarError : Theme.paneBar
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
        && (root.deviceRow >= 0 ? Devices.isBoschFeedRow(root.deviceRow) : Devices.isBoschFeed(root.label))

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

    function openBoschPicker(anchor, kind) {
        if (!BoschTour.connected)
            return;
        if (boschPicker.visible && boschPicker.kind === kind) {
            boschPicker.close();
            return;
        }
        boschPicker.kind = kind;
        const overlay = Overlay.overlay;
        if (!overlay)
            return;
        const point = anchor.mapToItem(overlay, 0, 0);
        const available = kind === "scene" ? BoschTour.sceneModes : BoschTour.presets;
        const rowCount = Math.max(1, available.length);
        const desiredHeight = Math.min(280, rowCount * 34 + 12);
        boschPicker.x = Math.max(8, Math.min(point.x, overlay.width - boschPicker.width - 8));
        boschPicker.y = Math.max(8, point.y - desiredHeight - 7);
        boschPicker.open();
    }

    Loader {
        active: root.ptzOpen && root.capPtz
        anchors.right: parent.right
        anchors.rightMargin: 10
        // Centred on the right edge, lifted clear of the pane bar on a short pane.
        y: Math.max(0, Math.min((parent.height - height) / 2, paneBar.y + paneBar.pillTop - height - 8))
        z: 32
        sourceComponent: PtzPad {
            deviceRow: root.deviceRow
            showZoom: root.capZoom
        }
    }

    Loader {
        active: root.boschPtzOpen && root.isBoschFeed
        anchors.right: videoArea.right
        anchors.rightMargin: 8
        y: paneBar.y + paneBar.pillTop - height - 8
        z: 32
        sourceComponent: PtzPad {
            deviceRow: -1
            showZoom: true
            boschCamera: true
            onOpenBoschTourRequested: root.openBoschTourRequested()
        }
    }

    HoverHandler { id: paneHover }

    LiveReplayControls {
        id: replayControls
        anchors.fill: videoArea
        z: 30
        replay: root.replay
        cameraKey: root.cameraKey
        deviceRow: root.deviceRow
        replayable: !!root.replay && root.replayable && root.hasSource && !root.offline
        selected: root.selected
        audioAllowed: root.audioAllowed && !talk.active
        // HD as live (maximised, or the pane's HD choice); with several panes replaying, a grid
        // pane plays SD and only a maximised one HD (ticket 15b: memory and recorder links).
        hd: root.effectiveMain && (!replayControls.inStep || root.forceMain)
        viewRotation: root.viewRotation
        fisheyeLens: root.fisheyeLens
        fisheyeMode: root.fisheyeMode
        highlightColour: root.highlightLit && root.highlight ? root.highlight.colour : "transparent"
        liveNote: root.highlightKindText
        othersHighlighted: root.replayOthersHighlighted
        onShowCamera: (key) => root.showCameraRequested(key)
        onClicked: root.clicked(root.deviceRow)
        onDoubleClicked: root.toggleMaximize(root.deviceRow)
    }

    // ---- The pane's bottom bar (live-replay ticket 11, layout A) ----------------------------
    // Live: ↺ and ◀ start Live replay, HD, sound and the camera's tools. Replaying: the whole
    // transport, the "time behind live" line and the replayed picture's tools.
    readonly property bool roomyForBosch: root.width >= 760 && root.height >= 460
    readonly property var liveTools: {
        var t = [], bosch = root.isBoschFeed;
        if (root.replaying) {
            t.push({ id: "snapshot", glyph: "◉", tip: qsTr("Save snapshot") });
            t.push({ id: "rotate", glyph: "⟳", active: root.viewRotation !== 0, tip: qsTr("Rotate view 90°") });
            t.push({ id: "zoom", glyph: "⊕", active: replayControls.zoomed,
                     tip: replayControls.zoomed ? qsTr("Reset zoom") : qsTr("Digital zoom (scroll to adjust)") });
            t.push({ id: "popout", glyph: "⧉", tip: qsTr("Pop out to a window") });
            t.push({ id: "maximise", glyph: "⛶", tip: root.forceMain ? qsTr("Restore grid") : qsTr("Maximize") });
            return t;
        }
        if (bosch) {
            t.push({ id: "positions", wide: true, wideWidth: 88, label: qsTr("Positions"),
                     active: boschPicker.visible && boschPicker.kind === "positions",
                     usable: BoschTour.connected,
                     tip: BoschTour.connected
                          ? qsTr("Choose a camera position%1").arg(BoschTour.selectionTourMode
                              ? qsTr(" · tap to add or remove tour stops") : "")
                          : qsTr("Home Assistant is disconnected") });
            t.push({ id: "scene", wide: true, wideWidth: 112, label: qsTr("Marked range"),
                     active: (boschPicker.visible && boschPicker.kind === "scene") || BoschTour.sceneModePending,
                     usable: BoschTour.sceneModeAvailable && !BoschTour.sceneModePending,
                     tip: BoschTour.sceneModeName === ""
                          ? (BoschTour.sceneModeError || qsTr("Marked-range mode unavailable"))
                          : qsTr("Marked range: %1%2%3").arg(BoschTour.sceneModeName)
                              .arg(BoschTour.sceneModeStale ? qsTr(" · stale") : "")
                              .arg(BoschTour.sceneModeError === "" ? "" : " · " + BoschTour.sceneModeError) });
            if (!BoschTour.connected)
                t.push({ id: "haOffline", glyph: "!", active: true,
                         tip: qsTr("Home Assistant is disconnected. Reconnect to restore Bosch PTZ, positions, scene modes, and tours.") });
        }
        if (root.showLarge)
            t.push({ id: "large", glyph: "⤢", active: root.large,
                     tip: root.large ? qsTr("Large pane: make it Normal") : qsTr("Make this pane Large") });
        t.push({ id: "snapshot", glyph: "◉", tip: qsTr("Save snapshot") });
        t.push({ id: "record", glyph: "⏺", active: player.recording,
                 tip: player.recording ? qsTr("Stop recording") : qsTr("Record video") });
        t.push({ id: "rotate", glyph: "⟳", active: root.viewRotation !== 0,
                 tip: root.viewRotation !== 0 ? qsTr("Rotate view 90° (now %1°)").arg(root.viewRotation)
                                              : qsTr("Rotate view 90°") });
        t.push({ id: "zoom", glyph: "⊕", active: root.zoom > 1.01,
                 tip: root.zoom > 1.01 ? qsTr("Reset zoom") : qsTr("Digital zoom (scroll to adjust)") });
        if (root.capPtz || (bosch && root.roomyForBosch))
            t.push({ id: "ptz", glyph: "⇅", active: bosch ? root.boschPtzOpen : root.ptzOpen,
                     tip: qsTr("PTZ controls") });
        if (bosch && root.roomyForBosch)
            t.push({ id: "tour", glyph: "T", active: BoschTour.selectionTourMode,
                     tip: !BoschTour.connected ? qsTr("Reconnect Home Assistant to restore positions and tours")
                        : BoschTour.selectionTourMode ? qsTr("Turn off tour and clear positions")
                        : qsTr("Select positions for a tour") });
        if (root.capTalk)
            t.push({ id: "talk", glyph: "🎙", active: talk.active,
                     tip: talk.state === TalkSession.Talking ? qsTr("Talking — click to stop")
                        : talk.state === TalkSession.Connecting ? qsTr("Connecting…")
                        : qsTr("Two-way talk (click to start, click again to stop)") });
        if (root.capSiren)
            t.push({ id: "siren", glyph: "📢", tip: qsTr("Sound siren") });
        if (root.capFloodlight)
            t.push({ id: "floodlight", glyph: "💡", active: root.floodOn, tip: qsTr("Toggle floodlight") });
        t.push({ id: "popout", glyph: "⧉", tip: qsTr("Pop out to a window") });
        t.push({ id: "maximise", glyph: "⛶", tip: root.forceMain ? qsTr("Restore grid") : qsTr("Maximize") });
        return t;
    }
    function useTool(id, anchor) {
        switch (id) {
        case "positions": root.openBoschPicker(anchor, "positions"); break;
        case "scene": root.openBoschPicker(anchor, "scene"); break;
        case "haOffline": root.openBoschTourRequested(); break;
        case "large": root.largeToggled(); break;
        case "snapshot":
            if (root.replaying) {
                var f = replayControls.snapshot();
                replayControls.showNotice(f !== "" ? qsTr("Snapshot saved: %1").arg(f.split("/").pop())
                                                   : qsTr("Nothing to save: no picture yet"));
            } else {
                Devices.snapshot(root.deviceRow);
            }
            break;
        case "record": player.recording ? player.stopRecording() : player.startRecording(); break;
        case "rotate": Devices.setRotationOverride(root.deviceRow, root.viewRotation + 90); break;
        case "zoom":
            if (root.replaying) replayControls.toggleZoom();
            else if (root.zoom > 1.01) { root.zoom = 1; root.panX = 0; root.panY = 0; }
            else root.zoom = 2;
            break;
        case "ptz":
            if (root.isBoschFeed) {
                root.boschPtzOpen = !root.boschPtzOpen;
                root.ptzOpen = false;
                if (root.boschPtzOpen && BoschTour.selectionTourMode)
                    root.activateTourSelection();
            } else {
                root.ptzOpen = !root.ptzOpen;
            }
            break;
        case "tour":
            if (!BoschTour.connected) { root.openBoschTourRequested(); break; }
            if (!BoschTour.selectionTourMode) { root.boschPtzOpen = false; root.ptzOpen = false; }
            root.activateTourSelection();
            break;
        case "talk": talk.active ? talk.stop() : Devices.startTalk(root.deviceRow, talk); break;
        case "siren":
            Devices.settings.applySetting(Devices.cameraKeyAt(root.deviceRow), "AudioAlarmPlay",
                { "alarm_mode": "times", "times": 1, "channel": Devices.channelOf(root.deviceRow) });
            break;
        case "floodlight": root.floodOn = !root.floodOn; Devices.settings.toggleFloodlight(Devices.cameraKeyAt(root.deviceRow)); break;
        case "popout": if (root.hasSource) root.popOut(root.deviceRow, root.label); break;
        case "maximise": if (root.hasSource) root.toggleMaximize(root.deviceRow); break;
        }
    }
    function toggleHd() {
        // Several panes replaying: grid panes play SD (ticket 15b); HD is for a maximised pane.
        if (replayControls.inStep && !root.forceMain) {
            paneNotice.show(qsTr("Several cameras are replaying: maximise this one for HD"));
            return;
        }
        var why = !root.qualityMain && !root.forceMain && root.hdRefusal ? root.hdRefusal() : "";
        if (why) {
            paneNotice.show(why);
            return;
        }
        root.qualityMain = !root.qualityMain;
        root.qualityChosen(root.qualityMain);
    }

    PaneBar {
        id: paneBar
        anchors.fill: videoArea
        z: 31
        visible: root.hasSource
        pageKind: root.replaying ? "replay" : "live"
        camera: replayControls.replayable ? "recorder" : "stream"
        speedGroup: replayControls.group
        paused: root.replaying && root.replay.paused
        shown: paneHover.hovered || root.ptzOpen || root.boschPtzOpen || boschPicker.visible
        toolList: root.liveTools
        hdOn: root.replaying ? replayControls.hd : root.qualityMain
        hdTip: (root.replaying ? replayControls.hd : root.qualityMain) ? qsTr("Switch to Fluent (SD)")
                                                                        : qsTr("Switch to Clear (HD)")
        // Sound: the selected pane's (live) or the replay's, as Playback (1× forward only).
        soundShown: root.replaying ? replayControls.hasAudio : player.hasAudio
        soundOn: root.audioOn && (root.replaying
                 ? !AudioPrefs.playbackMuted && !!replayControls.group && !replayControls.group.offNormal
                 : !AudioPrefs.liveMuted)
        soundTip: root.replaying
                  ? (!!replayControls.group && replayControls.group.offNormal ? qsTr("Sound plays at 1× forward only")
                     : AudioPrefs.playbackMuted ? qsTr("Sound on") : qsTr("Mute"))
                  : (!root.audioOn || AudioPrefs.liveMuted ? qsTr("Hear this pane") : qsTr("Mute Live View"))
        segments: replayControls.strip
        head: replayControls.headPos
        headColour: root.replaying ? Theme.warning : Theme.danger
        marks: replayControls.marks
        labelAt: replayControls.replayable ? replayControls.labelAt : null
        seekable: replayControls.oldestBehind > 0 || !root.replaying
        timeText: replayControls.whenText
        timeColour: Theme.warning
        timeAlways: root.replaying
        onAction: (id) => {
            if (id === "hd") root.toggleHd();
            else if (id === "sound") {
                // Only the selected pane plays sound: on another pane this selects it, sound on.
                var muted = root.replaying ? AudioPrefs.playbackMuted : AudioPrefs.liveMuted;
                var on = !(root.audioOn && !muted);
                if (!root.audioOn) root.clicked(root.deviceRow);
                if (root.replaying) AudioPrefs.playbackMuted = !on;
                else AudioPrefs.liveMuted = !on;
            } else replayControls.act(id);
        }
        onTool: (id, anchor) => root.useTool(id, anchor)
        onSeekPressed: replayControls.trackPressed()
        onSeekReleased: (pos) => replayControls.trackReleased(pos)
        onSeekCanceled: replayControls.trackCanceled()
    }

    Popup {
        id: boschPicker
        parent: Overlay.overlay
        z: 1000
        property string kind: "positions"
        readonly property var optionModel: kind === "scene" ? BoschTour.sceneModes : BoschTour.presets
        width: Math.min(230, Overlay.overlay ? Overlay.overlay.width - 16 : 230)
        height: Math.min(Overlay.overlay ? Overlay.overlay.height - 16 : 280,
                         Math.min(280, Math.max(46, optionModel.length * 34 + 12)))
        padding: 6
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: Theme.surface
            border.color: Theme.border
            radius: Theme.radius
        }
        contentItem: Flickable {
            clip: true
            contentWidth: width
            contentHeight: pickerRows.implicitHeight
            flickableDirection: Flickable.VerticalFlick
            boundsBehavior: Flickable.StopAtBounds
            Column {
                id: pickerRows
                width: boschPicker.width - boschPicker.padding * 2
                spacing: 2
                Repeater {
                    model: boschPicker.optionModel.length > 0
                        ? boschPicker.optionModel
                        : [{ id: -1, token: "", name: boschPicker.kind === "scene"
                            ? qsTr("Marked range unavailable") : qsTr("No positions saved") }]
                    delegate: Rectangle {
                        id: pickerRow
                        required property var modelData
                        readonly property int tourOrder: boschPicker.kind === "positions"
                            ? root.positionSequenceIndex(String(modelData.token)) : -1
                        readonly property bool selected: boschPicker.kind === "scene"
                            ? Number(modelData.id) === BoschTour.sceneModeId : tourOrder >= 0
                        readonly property bool selectable: modelData.id !== -1
                            && (boschPicker.kind === "scene"
                                ? BoschTour.sceneModeAvailable && !BoschTour.sceneModePending
                                : BoschTour.connected
                                    && (!BoschTour.busy || BoschTour.selectionTourRestoring)
                                    && (BoschTour.selectionTourMode || BoschTour.selectionTourRestoring
                                        || BoschTour.tourState === "stopped" || BoschTour.touring))
                        width: pickerRows.width
                        height: 32
                        radius: 4
                        color: pickerHover.hovered ? Theme.surfaceAlt
                             : selected ? Theme.accentDim : "transparent"
                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 9
                            anchors.verticalCenter: parent.verticalCenter
                            width: 22
                            horizontalAlignment: Text.AlignHCenter
                            text: pickerRow.selected ? "✓"
                                : pickerRow.tourOrder >= 0 ? String(pickerRow.tourOrder + 1) : ""
                            color: Theme.accent
                            font.pixelSize: 12
                            font.bold: true
                        }
                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 36
                            anchors.right: parent.right
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: String(pickerRow.modelData.name)
                            color: pickerRow.selectable ? Theme.text : Theme.textMuted
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                        HoverHandler { id: pickerHover }
                        TapHandler {
                            enabled: pickerRow.selectable
                            gesturePolicy: TapHandler.ReleaseWithinBounds
                            onTapped: {
                                boschPicker.close();
                                if (boschPicker.kind === "scene")
                                    BoschTour.setSceneMode(Number(pickerRow.modelData.id));
                                else
                                    root.toggleTourPosition(pickerRow.modelData);
                            }
                        }
                    }
                }
            }
        }
    }

    // ---- Fisheye views: hover icons at the right edge, right-click View menu ----------
    FisheyeViewIcons {
        anchors.top: videoArea.top
        anchors.right: videoArea.right
        available: paneBar.visibleNow ? paneBar.pillTop - 8 : videoArea.height
        shown: root.fisheyeLens && root.hasSource && paneHover.hovered
        current: root.fisheyeMode
        onPicked: (view) => Devices.setViewMode(root.deviceRow, view)
    }
    TapHandler {
        acceptedButtons: Qt.RightButton
        enabled: root.fisheyeLens && root.hasSource
        onTapped: (ev) => {
            viewMenu.aimed = root.dewarp && FisheyeAims.aimed(root.cameraKey, root.fisheyeMode);
            viewMenu.popup(ev.position.x, ev.position.y);
        }
    }
    FisheyeViewMenu {
        id: viewMenu
        current: root.fisheyeMode
        onPicked: (view) => Devices.setViewMode(root.deviceRow, view)
        onResetAim: FisheyeAims.reset(root.cameraKey, root.fisheyeMode)
    }
}
