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
    onVisibleChanged: {
        if (!visible) talk.stop();
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
        active: root.pageActive
        viewVisible: root.visible
        videoSink: video.videoSink
        muted: !(root.audioOn && root.audioAllowed && !talk.active)
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
                case StreamPlayer.Error: return player.errorString
                    + (player.recovering ? qsTr(" · Reconnecting…") : "");
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

                component ToolPickerButton: Rectangle {
                    id: pickerButton
                    property string label: ""
                    property string kind: "positions"
                    property string tip: ""
                    property bool active: false
                    property bool enabledTool: true
                    signal activated()
                    width: kind === "scene" ? 112 : 88
                    height: 28
                    radius: 4
                    color: active ? Theme.accentDim : (pickerHover.hovered ? Theme.surfaceAlt : "transparent")
                    opacity: enabledTool ? 1 : 0.48
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        anchors.right: pickerArrow.left
                        anchors.verticalCenter: parent.verticalCenter
                        text: pickerButton.label
                        color: pickerButton.active ? Theme.text : Theme.textMuted
                        font.pixelSize: 10
                        font.bold: true
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }
                    Text {
                        id: pickerArrow
                        anchors.right: parent.right
                        anchors.rightMargin: 7
                        anchors.verticalCenter: parent.verticalCenter
                        text: "⌃"
                        color: pickerButton.active ? Theme.text : Theme.textMuted
                        font.pixelSize: 13
                    }
                    HoverHandler { id: pickerHover }
                    ToolTip {
                        visible: pickerHover.hovered && pickerButton.tip !== ""
                        delay: 450
                        text: pickerButton.tip
                        contentItem: Text { text: pickerButton.tip; color: Theme.text; font.pixelSize: 11 }
                        background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: 4 }
                    }
                    TapHandler {
                        enabled: pickerButton.enabledTool
                        gesturePolicy: TapHandler.ReleaseWithinBounds
                        onTapped: root.openBoschPicker(pickerButton, pickerButton.kind)
                    }
                }

                ToolButton {
                    glyph: root.qualityMain ? "HD" : "SD"
                    tip: root.qualityMain ? qsTr("Switch to Fluent (SD)") : qsTr("Switch to Clear (HD)")
                    active: root.qualityMain
                    onActivated: root.qualityMain = !root.qualityMain
                }

                ToolPickerButton {
                    visible: root.isBoschFeed
                    label: qsTr("Positions")
                    kind: "positions"
                    active: boschPicker.visible && boschPicker.kind === "positions"
                    enabledTool: BoschTour.connected
                    tip: BoschTour.connected
                        ? qsTr("Choose a camera position%1").arg(BoschTour.selectionTourMode
                            ? qsTr(" · tap to add or remove tour stops") : "")
                        : qsTr("Home Assistant is disconnected")
                }
                ToolPickerButton {
                    visible: root.isBoschFeed
                    label: qsTr("Marked range")
                    kind: "scene"
                    active: (boschPicker.visible && boschPicker.kind === "scene") || BoschTour.sceneModePending
                    enabledTool: BoschTour.sceneModeAvailable && !BoschTour.sceneModePending
                    tip: BoschTour.sceneModeName === ""
                        ? (BoschTour.sceneModeError || qsTr("Marked-range mode unavailable"))
                        : qsTr("Marked range: %1%2%3").arg(BoschTour.sceneModeName)
                            .arg(BoschTour.sceneModeStale ? qsTr(" · stale") : "")
                            .arg(BoschTour.sceneModeError === "" ? "" : " · " + BoschTour.sceneModeError)
                }

                ToolButton {
                    visible: root.isBoschFeed && !BoschTour.connected
                    glyph: "!"
                    active: true
                    tip: qsTr("Home Assistant is disconnected. Reconnect to restore Bosch PTZ, positions, scene modes, and tours.")
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
                                        || BoschTour.tourState === "stopped"))
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
