import QtCore
import QtQuick
import QtQuick.Controls
import ReolinkApp
import ReolinkApp.Core

// Live View on Site tabs (wayfinder/site-tabs-redesign): the panes are exactly the selected
// cameras, always packed as a mosaic, grouped by Site in Site tab order and in tap order
// within a Site. Pick cameras from a Site tab's drawer. Double-click a pane to maximise it;
// ← / → step a maximised pane. Fullscreen (F11) hides the Site tabs until the pointer
// reaches the top edge.
Item {
    id: page

    property bool fullscreen: false
    property bool active: true      // true only while Live View is the shown page
    property bool audioSuppressed: false // the doorbell pop-up is up: panes stay silent
    property string doorbellCamera: "" // "hostId:channel" while the doorbell pop-up is up: fixed highlight
    property var menus: null        // DeviceMenus, for the right-click menus on the Site tabs
    signal fullscreenToggled()
    signal editHighlightRulesRequested()
    signal popOut(int deviceRow, string label)
    signal openBoschTourRequested()

    property string maximizedKey: ""   // "hostId:channel" or ""
    property string selectedKey: ""    // the pane with sound

    // ---- The selection ---------------------------------------------------------------
    // Stored in [liveSelection]. On the first launch it is taken from the old grid's
    // first page ([liveGrid], left in place for one release); with nothing there, the first
    // four cameras once they have arrived.
    SiteSelection {
        id: selection
        sites: Sites.map
        onSaveRequested: (text) => selStore.setValue("keys", text)
        onChanged: {
            if (page.maximizedKey !== "" && !contains(page.maximizedKey))
                page.maximizedKey = "";
            page.rebuildMosaic();
        }
        // A change after Clear all ends its Undo: the toast goes with it.
        onUndoChanged: if (!canUndo) undoToast.dismiss()
    }
    // ---- The Site budget ----------------------------------------------------------------
    // Each Site's link carries so many Mbit/s; every pane costs its camera's bitrate, sub or
    // HD. Live View counts on its own (it stops streaming when hidden). A maximised pane is
    // one stream alone and always goes HD.
    SiteBudget {
        id: budget
        sites: Sites.map
        selection: selection
        budgets: Sites.budgets
        defaultBudget: Sites.defaultBudget
        costs: Sites.costs
        hd: PanePrefs.hd
    }
    // Why switching this pane to HD would go over its Site's budget; "" when it fits.
    function hdRefusal(key) {
        if (budget.canHd(key))
            return "";
        var site = Sites.siteOfKey(key);
        return qsTr("%1 is at %2 / %3 Mbit/s; HD needs %4 more.")
                .arg(site).arg(Sites.mbps(budget.used(site))).arg(Sites.mbps(budget.budgetOf(site)))
                .arg(Sites.mbps(budget.hdExtra(key))) + " " + Sites.overBudgetAdvice;
    }

    property bool seedFirstCameras: false
    Settings { id: selStore; category: "liveSelection" }   // keys, migrated
    Settings { id: gridStore; category: "liveGrid" }        // read once, for the migration
    function restoreSelection() {
        if (String(selStore.value("migrated", false)) === "true") {
            selection.saved = String(selStore.value("keys", ""));
            return;
        }
        var keys = selection.fromGrid(String(gridStore.value("arrangement", "")),
                                      parseInt(gridStore.value("preset", 4)));
        selection.saved = keys.join(",");
        selStore.setValue("keys", keys.join(","));
        selStore.setValue("migrated", true);
        seedFirstCameras = keys.length === 0;
    }
    function seedIfNeeded() {
        if (!seedFirstCameras || Devices.count === 0)
            return;
        seedFirstCameras = false;
        if (selection.count > 0)
            return;
        for (var r = 0; r < Devices.count && selection.count < 4; ++r) {
            var k = cameraKey(r);
            if (k && budget.canAdd(k))
                selection.toggle(k);
        }
    }
    // Keys of a recorder that has been removed are dropped once the device list settles
    // (an NVR swaps its placeholder row for its channels while it connects).
    function pruneSelection() {
        if (Devices.count > 0)
            selection.pruneHosts(Devices.hostIds());
    }
    Timer { id: pruneTimer; interval: 8000; onTriggered: page.pruneSelection() }

    // ---- Mosaic ------------------------------------------------------------------------
    property bool mosaicAnimate: false   // a re-pack from new panes or shapes slides
    property var mosaicRectByRow: ({})   // device row -> rect
    property var paneRatios: ({})        // "hostId:channel" -> drawn width/height

    function cameraKey(row) { return PanePrefs.cameraKey(row); }
    function rowsByKey() {
        var byKey = ({});
        for (var r = 0; r < Devices.count; ++r)
            byKey[cameraKey(r)] = r;
        return byKey;
    }
    // The rows shown, in pane order. A selected camera whose recorder has not answered yet
    // gets its pane when it arrives.
    function mosaicRows() {
        var byKey = rowsByKey(), rows = [];
        selection.paneOrder.forEach(k => { if (byKey[k] !== undefined) rows.push(byKey[k]); });
        return rows;
    }
    function selectionMissing() {
        var byKey = rowsByKey();
        return selection.paneOrder.some(k => byKey[k] === undefined);
    }
    // At launch the NVRs' cameras arrive a few seconds after the window opens. The first
    // build waits for the selected cameras (at most 8 s), so the mosaic opens in its final
    // layout rather than re-packing as each NVR answers.
    property bool mosaicWaitOver: false
    Timer {
        id: mosaicWait
        interval: 8000
        running: true
        onTriggered: {
            page.mosaicWaitOver = true;
            page.seedIfNeeded();
            page.pruneSelection();
            page.rebuildMosaic();
        }
    }
    function rebuildMosaic() {
        if (!mosaicWaitOver && selectionMissing())
            return;
        mosaicSettle.stop();
        mosaic.panes = mosaicRows().map(r => ({ row: r,
                                                large: PanePrefs.has("large", cameraKey(r)),
                                                ratio: paneRatios[cameraKey(r)] || 16 / 9 }));
    }
    // Shape changes and cameras arriving are gathered for half a second, then re-packed
    // together in one move (cameras connecting at launch cause one, not 18).
    function scheduleMosaic() {
        if (!mosaicSettle.running)
            mosaicSettle.start();
    }
    Timer { id: mosaicSettle; interval: 500; onTriggered: page.rebuildMosaic() }
    Timer { id: mosaicAnimateEnd; interval: 300; onTriggered: page.mosaicAnimate = false }

    // The drawn shape from the decoded (or remembered) frame shape, the rotation override
    // and the fisheye view, so a rotation or view change applies before any stream does.
    function drawnRatio(frame, rotation, fisheye, mode) {
        if (fisheye && mode !== "fisheye") {
            if (mode === "dual") return 2.1 / 2;     // FisheyeView: two 2.1:1 strips stacked
            if (mode === "panorama") return 3.3;
            if (mode === "fisheye_ptz") return 2.5;
            return 16 / 9;                           // ptz, quad: they fill any pane
        }
        var r = frame > 0 ? frame : 16 / 9;          // never seen: 16:9
        return rotation % 180 !== 0 ? 1 / r : r;
    }
    function noteRatio(key, r) {
        if (!key || !(r > 0))
            return;
        var old = paneRatios[key];
        if (old && Math.abs(old - r) / old < 0.005)
            return;
        paneRatios[key] = r;
        scheduleMosaic();
    }
    // Each camera's last decoded shape, kept between launches.
    function rememberedFrame(key) {
        var v = parseFloat(shapeStore.value(key, 0));
        return v > 0 ? v : 0;
    }
    function rememberFrame(key, r) {
        if (!key || !(r > 0))
            return;
        var old = rememberedFrame(key);
        if (!old || Math.abs(old - r) / old >= 0.005)
            shapeStore.setValue(key, r.toFixed(4));
    }
    // A camera's shape for its drawer card: as drawn in a pane, else as remembered.
    function cardRatio(key) {
        if (paneRatios[key] > 0)
            return paneRatios[key];
        var row = rowsByKey()[key];
        var c = row !== undefined ? Devices.cameraInfo(row) : null;
        return drawnRatio(rememberedFrame(key), c ? c.rotationOverride || 0 : 0, false, "fisheye");
    }
    function mapMosaicRects() {
        var m = ({});
        var p = mosaic.layoutPanes, r = mosaic.rects;
        for (var i = 0; i < p.length && i < r.length; ++i)
            m[p[i].row] = r[i];
        mosaicRectByRow = m;
    }

    // ← / →: a maximised pane steps to its neighbour in pane order, wrapping, over any
    // selected camera whose recorder has not answered yet.
    function step(dir) {
        var byKey = rowsByKey(), k = maximizedKey;
        for (var i = 0; i < selection.count; ++i) {
            k = selection.neighbour(k, dir);
            if (k === "" || k === maximizedKey)
                return;
            if (byKey[k] !== undefined) {
                maximizedKey = k;
                selectedKey = k;
                return;
            }
        }
    }
    readonly property bool canStep: maximizedKey !== "" && selection.count > 1
    Shortcut {
        sequence: "Left"
        enabled: page.active && page.canStep
        onActivated: page.step(-1)
    }
    Shortcut {
        sequence: "Right"
        enabled: page.active && page.canStep
        onActivated: page.step(1)
    }

    Settings {
        id: shapeStore
        category: "mosaicShapes"       // "hostId:channel" = decoded width/height
    }
    // The Highlights switch (wayfinder/detection-highlights), remembered.
    Settings {
        id: highlightStore
        category: "highlights"
        property bool on: true
    }

    Connections {
        target: Devices
        function onRowsRemoved() { page.scheduleMosaic(); pruneTimer.restart(); }
        function onRowsInserted() {
            page.scheduleMosaic();
            if (page.mosaicWaitOver)
                page.seedIfNeeded();   // no cameras had arrived when the wait ended
        }
        function onModelReset() { page.scheduleMosaic(); }
    }

    Component.onCompleted: {
        restoreSelection();
        Highlights.enabled = highlightStore.on;
    }

    // ---- Site tabs ---------------------------------------------------------------------
    // In fullscreen the strip hides above the top edge and slides in while the pointer is
    // there (or over the strip or its drawer).
    readonly property bool tabsShown: !fullscreen || topEdge.hovered || tabs.hovered || tabs.drawerOpen
    SiteTabs {
        id: tabs
        z: 10
        width: parent.width
        height: implicitHeight
        y: page.tabsShown ? 0 : -height
        Behavior on y { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
        selection: selection
        budget: budget
        menus: page.menus
        ratioOf: (key) => page.cardRatio(key)
        onCleared: (n) => undoToast.show(n === 1 ? qsTr("Cleared 1 camera ·") : qsTr("Cleared %1 cameras ·").arg(n))
        showSaved: true
        // Cameras a recalled selection left out are named.
        onRecalled: (name, skipped) => {
            if (skipped.length === 0)
                return;
            var byKey = page.rowsByKey();
            var names = skipped.map(k => byKey[k] !== undefined ? Devices.nameAt(byKey[k]) : k);
            undoToast.show(qsTr("%1: left out %2 (over the Site budget)").arg(name).arg(names.join(", ")), false);
        }

        leftItems: [
        // Highlights: a camera's panes get a coloured border when it detects a person,
        // vehicle, pet or motion, by the Highlight rules. One switch for borders and sounds.
        Rectangle {
            width: highlightLabel.implicitWidth + 20
            height: 26
            radius: Theme.radius
            color: Highlights.enabled ? Theme.accentDim
                 : highlightArea.containsMouse ? Theme.surfaceAlt : Theme.surface
            border.color: Theme.border
            Text {
                id: highlightLabel
                anchors.centerIn: parent
                text: Highlights.enabled && AlertSounds.muted ? qsTr("Highlights \u00b7 muted")
                                                              : qsTr("Highlights")
                color: Highlights.enabled ? Theme.text : Theme.textMuted
                font.pixelSize: 12
            }
            MouseArea {
                id: highlightArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    Highlights.enabled = !Highlights.enabled;
                    highlightStore.on = Highlights.enabled;
                }
            }
            ToolTip {
                visible: highlightArea.containsMouse
                delay: 500
                x: (parent.width - width) / 2
                y: parent.height + 6
                contentItem: Text {
                    text: qsTr("Highlights: outline cameras that detect people, vehicles, pets or motion, and play their sounds")
                    color: Theme.text; font.pixelSize: 11
                }
                background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: 4 }
            }
        },

        // Highlights menu: the rules live in Settings.
        Rectangle {
            width: 26
            height: 26
            radius: Theme.radius
            color: highlightMenuArea.containsMouse || highlightMenu.visible ? Theme.surfaceAlt : Theme.surface
            border.color: Theme.border
            Text { anchors.centerIn: parent; text: "▾"; color: Theme.textMuted; font.pixelSize: 11 }
            MouseArea {
                id: highlightMenuArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: highlightMenu.visible ? highlightMenu.close()
                                                 : highlightMenu.popup(0, parent.height + 4)
            }
            ThemedMenu {
                id: highlightMenu
                ThemedMenuItem {
                    visible: AlertSounds.muted
                    height: visible ? implicitHeight : 0
                    text: AlertSounds.mutedUntil > 0
                          ? qsTr("Unmute sounds (muted until %1)")
                                .arg(Qt.formatTime(new Date(AlertSounds.mutedUntil), "HH:mm"))
                          : qsTr("Unmute sounds")
                    onTriggered: AlertSounds.unmute()
                }
                ThemedMenuItem {
                    text: qsTr("Mute sounds for 15 minutes")
                    onTriggered: AlertSounds.muteFor(15)
                }
                ThemedMenuItem {
                    text: qsTr("Mute sounds for 1 hour")
                    onTriggered: AlertSounds.muteFor(60)
                }
                ThemedMenuItem {
                    visible: AlertSounds.mutedUntil !== -1
                    height: visible ? implicitHeight : 0
                    text: qsTr("Mute sounds until I unmute")
                    onTriggered: AlertSounds.muteFor(0)
                }
                ThemedMenuSeparator {}
                ThemedMenuItem {
                    text: qsTr("Edit rules…")
                    onTriggered: page.editHighlightRulesRequested()
                }
            }
        }
        ]
        rightItems: [
            // Previous / next camera while one is maximised.
            StripButton {
                visible: page.canStep
                glyph: "◀"
                tip: qsTr("Previous camera (←)")
                onActivated: page.step(-1)
            },
            StripButton {
                visible: page.canStep
                glyph: "▶"
                tip: qsTr("Next camera (→)")
                onActivated: page.step(1)
            },
            StripButton {
                glyph: "⛶"
                tip: qsTr("Fullscreen (F11)")
                onActivated: page.fullscreenToggled()
            }
        ]
    }
    // The top edge in fullscreen, where the pointer brings the Site tabs back.
    Item {
        z: 11
        width: parent.width
        height: 4
        visible: page.fullscreen
        HoverHandler { id: topEdge }
    }

    component StripButton: Rectangle {
        id: sb
        property string glyph: ""
        property string tip: ""
        signal activated()
        width: 30
        height: 26
        radius: Theme.radius
        color: tbArea.containsMouse ? Theme.surfaceAlt : Theme.surface
        border.color: Theme.border
        Text { anchors.centerIn: parent; text: parent.glyph; color: Theme.textMuted; font.pixelSize: 12 }
        MouseArea {
            id: tbArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: sb.activated()
        }
        ToolTip {
            visible: tbArea.containsMouse
            delay: 500
            y: parent.height + 6
            contentItem: Text { text: sb.tip; color: Theme.text; font.pixelSize: 11 }
            background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: 4 }
        }
    }

    Item {
        id: gridArea
        anchors.fill: parent
        anchors.topMargin: page.fullscreen ? 0 : tabs.height + Theme.spacing
        anchors.leftMargin: page.fullscreen ? 0 : Theme.spacing
        anchors.rightMargin: page.fullscreen ? 0 : Theme.spacing
        anchors.bottomMargin: page.fullscreen ? 0 : Theme.spacing
        readonly property int gap: 4

        Text {
            anchors.centerIn: parent
            visible: page.maximizedKey === "" && mosaic.layoutPanes.length === 0
            horizontalAlignment: Text.AlignHCenter
            text: selection.count === 0 ? qsTr("Pick cameras from a Site tab above to watch them here.")
                : page.mosaicWaitOver ? qsTr("None of the selected cameras are here yet")
                : qsTr("Loading cameras…")
            color: Theme.textMuted
            font.pixelSize: 13
        }

        MosaicLayout {
            id: mosaic
            width: gridArea.width
            height: gridArea.height
            gap: gridArea.gap
            // A swap between same-shaped panes changes only layoutPanes, not rects.
            onLayoutPanesChanged: {
                page.mosaicAnimate = true;
                mosaicAnimateEnd.restart();
                page.mapMosaicRects();
            }
            onRectsChanged: page.mapMosaicRects()
            // A held layout re-packed once a resize settled slides there.
            onSettled: {
                page.mosaicAnimate = true;
                mosaicAnimateEnd.restart();
            }
        }

        // Device-backed panes (model gives live name/status/caps updates); only the selected
        // ones are shown, and only shown panes stream.
        Repeater {
            model: Devices
            LivePane {
                id: pane
                required property int index
                required property var hostId
                required property int channel
                required property string name
                required property bool hasPtz
                required property bool hasZoom
                required property bool hasAudio
                required property bool hasSiren
                required property bool hasFloodlight
                required property bool hasTalk
                required property int rotationOverride
                required property bool isFisheye
                required property string viewMode
                required property bool online

                readonly property string cameraKey: hostId + ":" + channel
                readonly property bool isMaximized: page.maximizedKey === cameraKey
                readonly property var mrect: page.mosaicRectByRow[index]
                readonly property bool inMosaic: mrect !== undefined && selection.contains(cameraKey)

                visible: isMaximized || (page.maximizedKey === "" && inMosaic)
                width: isMaximized ? gridArea.width : inMosaic ? mrect.width : 0
                height: isMaximized ? gridArea.height : inMosaic ? mrect.height : 0
                x: isMaximized || !inMosaic ? 0 : mrect.x
                y: isMaximized || !inMosaic ? 0 : mrect.y
                offline: inMosaic && !online
                keepLastFrame: true

                // The shape this camera takes in the mosaic.
                readonly property real mosaicRatio: page.drawnRatio(
                    frameRatio > 0 ? frameRatio : page.rememberedFrame(cameraKey),
                    rotationOverride, isFisheye, viewMode)
                onFrameRatioChanged: page.rememberFrame(cameraKey, frameRatio)
                onMosaicRatioChanged: page.noteRatio(cameraKey, mosaicRatio)
                Component.onCompleted: page.noteRatio(cameraKey, mosaicRatio)
                paneIndex: selection.paneOrder.indexOf(cameraKey)
                deviceRow: index
                cameraIdentity: ({ hostId: pane.hostId, channel: pane.channel })
                label: name
                viewRotation: rotationOverride
                fisheyeLens: isFisheye
                fisheyeMode: viewMode
                selected: page.selectedKey === cameraKey
                audioAllowed: !page.audioSuppressed
                pageActive: page.active && page.visible
                // Sub stream in the mosaic, main stream when maximised (DESIGN §5.7); a pane
                // returns to the HD choice remembered for its camera.
                forceMain: isMaximized
                restMain: PanePrefs.has("hd", cameraKey)
                hdRefusal: () => page.hdRefusal(cameraKey)
                watchPicture: true
                onQualityChosen: (main) => {
                    if (!isMaximized)
                        PanePrefs.set("hd", cameraKey, main);
                }
                showLarge: inMosaic && !isMaximized
                large: PanePrefs.has("large", cameraKey)
                onLargeToggled: {
                    PanePrefs.toggle("large", cameraKey);
                    page.rebuildMosaic();
                }
                // Drag a pane onto another of the same Site to swap their places.
                dropFilter: (src => selection.sameSite(cameraKey, page.cameraKey(src)))
                onCameraDropped: (targetPane, row) => selection.swap(cameraKey, page.cameraKey(row))
                capPtz: hasPtz
                capZoom: hasZoom
                capAudio: hasAudio
                capSiren: hasSiren
                capFloodlight: hasFloodlight
                capTalk: hasTalk
                onToggleMaximize: {
                    page.maximizedKey = page.maximizedKey === cameraKey ? "" : cameraKey;
                    // A double-click arrives as click + double-click; the click may just
                    // have deselected this pane. Maximising keeps it selected.
                    page.selectedKey = cameraKey;
                }
                // Click selects (blue border + sound); clicking the selected pane again
                // deselects it and mutes.
                onClicked: page.selectedKey = page.selectedKey === cameraKey ? "" : cameraKey
                onPopOut: (row, lbl) => page.popOut(row, lbl)
                onOpenBoschTourRequested: page.openBoschTourRequested()
                highlight: page.doorbellCamera === cameraKey
                           ? ({ kind: "doorbell", colour: Theme.warning, style: "steady", since: 0, flashUntil: 0, last: 0 })
                           : Highlights.looks[cameraKey] || null

                // Only a re-pack slides; a resize follows the window directly.
                readonly property bool slides: !pane.isMaximized && page.mosaicAnimate
                Behavior on x {
                    enabled: pane.slides
                    NumberAnimation { duration: 250; easing.type: Easing.OutCubic }
                }
                Behavior on y {
                    enabled: pane.slides
                    NumberAnimation { duration: 250; easing.type: Easing.OutCubic }
                }
                Behavior on width {
                    enabled: pane.slides
                    NumberAnimation { duration: 250; easing.type: Easing.OutCubic }
                }
                Behavior on height {
                    enabled: pane.slides
                    NumberAnimation { duration: 250; easing.type: Easing.OutCubic }
                }
            }
        }

        // Tapping the video while a drawer is open only closes the drawer.
        MouseArea {
            anchors.fill: parent
            z: 5
            visible: tabs.drawerOpen
            onPressed: tabs.close()
        }
    }

    UndoToast {
        id: undoToast
        onUndo: selection.undo()
        onExpired: selection.dropUndo()
    }
}
