import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ReolinkApp
import ReolinkApp.Core

// The live grid: 1/4/6/9/16/20/25 presets, optionally packed as a mosaic, double-click a pane to
// maximize/restore, fullscreen hides all chrome (handled by Main).
Item {
    id: page

    property bool fullscreen: false
    property bool active: true      // true only while Live View is the shown page
    property bool audioSuppressed: false // the doorbell pop-up is up: grid stays silent
    signal fullscreenToggled()
    signal popOut(int deviceRow, string label)
    signal openBoschTourRequested()

    property int preset: 4
    property int maximizedIndex: -1
    property int selectedIndex: -1

    // A maximized pane is effectively the single ("1") view, so light that button
    // up in the layout toolbar rather than the underlying grid preset.
    readonly property int activePreset: maximizedIndex >= 0 ? 1 : tabOpen ? 0 : preset

    // ---- Mosaic ------------------------------------------------------------
    // A switch on the grid size (wayfinder/mosaic-live-view): the current grid page's
    // cameras, each at its own picture shape, packed by MosaicLayout to fill the area,
    // nothing cropped. Grouped by Site (alphabetically), in arrangement order within each;
    // empty cells are dropped. Paging and drags work as in the grid.
    // Named mosaics (Mosaics) are tabs after the switch: a tab shows all its cameras, in
    // its own order and grouping, with no paging; a number or the switch leaves it.
    // Activity mode is off while any mosaic is shown (its own setting is kept for the grid).
    property bool mosaicMode: false
    property string mosaicTab: ""        // the open named mosaic's id, "" = the grid page
    readonly property var tabMosaic: mosaicTab ? Mosaics.find(mosaicTab) : null
    readonly property bool tabOpen: tabMosaic !== null
    readonly property bool mosaicShown: mosaicMode || tabOpen
    readonly property string activeMosaic: tabOpen ? mosaicTab : "" // "" = the grid page
    readonly property bool activityOn: activityMode && !mosaicShown
    property bool mosaicAnimate: false   // a re-pack from new panes or shapes slides
    property var mosaicRectByRow: ({})   // device row -> rect while the mosaic is shown
    property var paneRatios: ({})        // "hostId:channel" -> drawn width/height

    onMosaicModeChanged: { layoutStore.mosaic = mosaicMode; repackMosaic(); }
    onMosaicTabChanged: { layoutStore.tab = mosaicTab; repackMosaic(); }
    onTabOpenChanged: if (!tabOpen) mosaicTab = ""   // its mosaic was deleted
    // A new set of cameras (page, grid size, view or the tab's mosaic changed), packed at once.
    function repackMosaic() {
        if (!mosaicShown)
            return;
        mosaicSettle.stop();
        mosaic.reset();
        rebuildMosaic();
    }
    Connections {
        target: Mosaics
        function onListChanged() {
            if (page.tabOpen) {
                mosaicSettle.stop();
                page.rebuildMosaic();
            }
        }
    }
    function openTab(id) {
        mosaicTab = id;
        maximizedIndex = -1;
    }

    function cameraKey(row) { return Mosaics.cameraKey(row); }
    function rowsByKey() {
        var byKey = ({});
        for (var r = 0; r < Devices.count; ++r)
            byKey[cameraKey(r)] = r;
        return byKey;
    }
    function mosaicItem(r, order) {
        return { row: r, site: Mosaics.siteOf(Devices.cameraInfo(r).hostName), order: order,
                 ratio: paneRatios[cameraKey(r)] || 16 / 9 };
    }
    // The rows shown, in mosaic order: the open tab's cameras, or the grid page's.
    function mosaicRows() {
        var list = [];
        if (tabOpen) {
            var byKey = rowsByKey();
            var cams = tabMosaic.cameras;
            for (var k = 0; k < cams.length; ++k)
                if (byKey[cams[k]] !== undefined)
                    list.push(mosaicItem(byKey[cams[k]], list.length));
            return Mosaics.arrange(list, tabMosaic.grouping, tabMosaic.siteOrder).map(x => x.row);
        }
        for (var i = pageOffset; i < pageOffset + preset && i < paneSlots.length; ++i) {
            var r = paneSlots[i];
            if (r >= 0 && r < Devices.count)
                list.push(mosaicItem(r, list.length));
        }
        return Mosaics.arrange(list, "site", []).map(x => x.row);
    }
    // Drops in a named mosaic: a camera not in it is added (before the pane it lands on);
    // one already in it swaps places, within its Site when grouped by Site. The mosaic keeps
    // its own order; the grid is never touched.
    function tabDropAllowed(targetRow, sourceRow) {
        if (!tabOpen || sourceRow < 0 || sourceRow >= Devices.count)
            return false;
        if (tabMosaic.cameras.indexOf(cameraKey(sourceRow)) < 0 || tabMosaic.grouping !== "site")
            return true;
        return Mosaics.siteOf(Devices.cameraInfo(sourceRow).hostName)
               === Mosaics.siteOf(Devices.cameraInfo(targetRow).hostName);
    }
    function tabDrop(targetRow, sourceRow) {
        if (!tabDropAllowed(targetRow, sourceRow) || targetRow === sourceRow)
            return;
        var cams = tabMosaic.cameras.slice();
        var ti = cams.indexOf(cameraKey(targetRow)), si = cams.indexOf(cameraKey(sourceRow));
        if (ti < 0)
            return;
        if (si < 0) {
            cams.splice(ti, 0, cameraKey(sourceRow));
        } else {
            cams[si] = cams[ti];
            cams[ti] = cameraKey(sourceRow);
        }
        Mosaics.update(mosaicTab, { cameras: cams });
    }

    // The open tab has cameras whose host has not answered yet.
    function tabMissing() {
        if (!tabOpen)
            return false;
        var byKey = rowsByKey();
        return tabMosaic.cameras.some(k => byKey[k] === undefined);
    }
    // At launch the NVRs' cameras arrive a few seconds after the window opens. The first
    // build waits for the saved arrangement's cameras (at most 8 s), so the mosaic opens
    // in its final layout rather than re-packing as each NVR answers.
    property bool mosaicWaitOver: false
    Timer {
        id: mosaicWait
        interval: 8000
        running: true
        onTriggered: { page.mosaicWaitOver = true; page.rebuildMosaic(); }
    }
    function rebuildMosaic() {
        if (!mosaicShown)
            return;
        if (!mosaicWaitOver && (!layoutRestored || hasPendingPanes() || tabMissing()))
            return;
        mosaic.panes = mosaicRows().map(r => ({ row: r,
                                                large: Mosaics.has(activeMosaic, "large", cameraKey(r)),
                                                ratio: paneRatios[cameraKey(r)] || 16 / 9 }));
    }
    // Shape changes, new cameras and rearranging are gathered for half a second, then
    // re-packed together in one move (cameras connecting at launch cause one, not 18).
    function scheduleMosaic() {
        if (mosaicShown && !mosaicSettle.running)
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
    function mapMosaicRects() {
        var m = ({});
        var p = mosaic.layoutPanes, r = mosaic.rects;
        for (var i = 0; i < p.length && i < r.length; ++i)
            m[p[i].row] = r[i];
        mosaicRectByRow = m;
    }

    // Sidebar click: show this camera alone, full-size (the maximized pane —
    // same as double-clicking it — which also defaults it to HD).
    function showCamera(row) {
        if (row < 0 || row >= Devices.count)
            return;
        maximizedIndex = row;
        selectedIndex = row;
    }
    // Column/row counts per preset. 6 (3x2) and 20 (5x4) are the non-square layouts,
    // both wider than tall to suit a landscape screen.
    readonly property var presets: [1, 4, 6, 9, 16, 20, 25]
    readonly property int cols: preset === 1 ? 1 : preset === 4 ? 2 : (preset === 6 || preset === 9) ? 3
                              : preset === 16 ? 4 : 5
    readonly property int rows: preset === 1 ? 1 : (preset === 4 || preset === 6) ? 2 : preset === 9 ? 3
                              : (preset === 16 || preset === 20) ? 4 : 5

    // ---- Paging ---------------------------------------------------------------
    // With more cameras than the grid has cells, the grid shows one page of `preset`
    // cameras at a time (in pane order). Paging never touches the saved arrangement.
    property int gridPage: 0
    readonly property int pageCount: Math.max(1, Math.ceil(Devices.count / preset))
    readonly property int curPage: Math.min(gridPage, pageCount - 1)
    readonly property int pageOffset: curPage * preset
    onPresetChanged: { gridPage = 0; syncActivityBaseline(); layoutStore.preset = preset; repackMosaic(); }

    // Previous/next: a maximized camera steps to its neighbour in pane order;
    // otherwise the grid moves a page. Both wrap around.
    function step(dir) {
        if (maximizedIndex >= 0) {
            var order = tabOpen ? mosaic.layoutPanes.map(p => p.row) : paneSlots.filter(r => r >= 0);
            var i = order.indexOf(maximizedIndex);
            if (order.length < 2 || i < 0)
                return;
            var row = order[(i + dir + order.length) % order.length];
            maximizedIndex = row;
            selectedIndex = row;
        } else if (pageCount > 1) {
            gridPage = (curPage + dir + pageCount) % pageCount;
        }
    }
    readonly property bool canStep: maximizedIndex >= 0 ? Devices.count > 1 : !tabOpen && pageCount > 1

    // ---- Which camera sits in which pane -----------------------------------
    // paneSlots[pane] = device row (-1 = empty). Without this the grid is a
    // straight 1:1 map of model order, so a camera can only ever appear in the
    // pane matching its position in the sidebar. Always kept at maxPanes long so
    // an arrangement made in the 16-grid survives a trip through the 4-grid. Longer than
    // the biggest grid so cameras beyond 25 can still be reached by paging.
    readonly property int maxPanes: 32
    property var paneSlots: []

    // ---- Activity mode -----------------------------------------------------
    // The user's arrangement (paneSlots) is the baseline and is never modified by
    // Activity mode: viewSlots is what is drawn, paneSlots with the activity cameras
    // laid over their tiles. Persistence and drag-and-drop only ever see paneSlots.
    property bool activityMode: false
    property var viewSlots: paneSlots
    property var userPinnedTiles: ({})

    function baselineRows() {
        var r = [];
        for (var i = 0; i < preset; ++i) {
            var row = pageOffset + i < paneSlots.length ? paneSlots[pageOffset + i] : -1;
            r.push(row);
        }
        return r;
    }
    function recomputeView() {
        if (!activityOn) {
            viewSlots = paneSlots;
            return;
        }
        var v = paneSlots.slice();
        var tiles = Activity.tiles;
        for (var t = 0; t < tiles.length; ++t) {
            var row = tiles[t].row;
            var idx = pageOffset + t;
            if (row < 0 || v[idx] === row)
                continue;
            var elsewhere = v.indexOf(row);
            if (elsewhere >= 0)
                v[elsewhere] = -1;
            v[idx] = row;
        }
        viewSlots = v;
    }
    function applyPins() {
        var tiles = Activity.tiles;
        for (var t = 0; t < tiles.length; ++t)
            Activity.setPinned(t, tiles[t].row === selectedIndex || userPinnedTiles[t] === true);
    }
    function syncActivityBaseline() {
        if (activityOn) {
            Activity.setBaseline(baselineRows());
            recomputeView();
            applyPins();
        }
    }
    // The activity detail for a camera row (null when it holds no tile for activity).
    function activityOf(row) {
        var tiles = Activity.tiles;
        for (var t = 0; t < tiles.length; ++t)
            if (activityOn && tiles[t].row === row && tiles[t].active)
                return tiles[t];
        return null;
    }
    function activityLabel(a, name) {
        if (!a)
            return "";
        var k = a.kind.charAt(0).toUpperCase() + a.kind.slice(1);
        var when = a.trigger > 0 ? Qt.formatTime(new Date(a.trigger * 1000), "HH:mm") : "";
        return k + " · " + name + (when ? " · " + when : "");
    }
    onActivityOnChanged: {
        Activity.enabled = activityOn;
        userPinnedTiles = ({});
        syncActivityBaseline();
        recomputeView();
    }
    onGridPageChanged: { syncActivityBaseline(); repackMosaic(); }
    onPaneSlotsChanged: { syncActivityBaseline(); if (!activityOn) viewSlots = paneSlots; scheduleMosaic(); }
    onSelectedIndexChanged: if (activityOn) applyPins()
    Connections {
        target: Activity
        function onTilesChanged() {
            page.recomputeView();
            page.applyPins();
        }
    }

    function rowAt(pane) {
        return pane >= 0 && pane < viewSlots.length ? viewSlots[pane] : -1;
    }
    function paneOfRow(row) { return row < 0 ? -1 : viewSlots.indexOf(row); }

    // Put `row` in `pane`. If it is already on the grid the two panes trade
    // places, so a drag can never quietly drop a camera off the layout.
    function assignPane(pane, row) {
        if (pane < 0 || pane >= maxPanes || row < 0 || row >= Devices.count)
            return;
        var s = paneSlots.slice();
        while (s.length < maxPanes) s.push(-1);
        var from = s.indexOf(row);
        if (from === pane)
            return;
        var displaced = s[pane];
        s[pane] = row;
        if (from >= 0)
            s[from] = displaced;   // swap
        paneSlots = s;
        saveLayout();
    }

    // Repair the mapping against the current model: drop rows that no longer
    // exist, remove duplicates, then fill gaps with cameras that aren't placed.
    // Runs whenever devices are added or removed.
    function syncSlots() {
        var s = [];
        var seen = ({});
        for (var i = 0; i < maxPanes; ++i) {
            var r = i < paneSlots.length ? paneSlots[i] : -1;
            if (r < 0 || r >= Devices.count || seen[r])
                r = -1;
            else
                seen[r] = true;
            s.push(r);
        }
        var free = [];
        for (var d = 0; d < Devices.count; ++d)
            if (!seen[d]) free.push(d);
        for (i = 0; i < s.length && free.length > 0; ++i)
            if (s[i] < 0) s[i] = free.shift();
        paneSlots = s;
    }

    // ---- Layout persistence -------------------------------------------------
    // Stored as "hostId:channel" per pane, because model rows shift whenever a
    // device is added or removed — a saved row index would point at the wrong
    // camera after the next change.
    Settings {
        id: layoutStore
        category: "liveGrid"
        property string arrangement: ""
        property int preset: 4   // last grid preset picked, reopened at next launch
        property bool mosaic: false // the Mosaic switch was on
        property string tab: ""     // the named mosaic last open ("" = the grid page)
    }
    Settings {
        id: shapeStore
        category: "mosaicShapes"       // "hostId:channel" = decoded width/height
    }
    property bool layoutRestored: false

    // Which detections take tiles in Activity mode (comma-separated), remembered.
    Settings {
        id: activityStore
        category: "activityMode"
        property string types: "person,vehicle,pet,visitor"
    }
    function setActivityType(type, on) {
        var cur = Activity.trackedTypes.filter(function (t) { return t !== type; });
        if (on)
            cur.push(type);
        Activity.trackedTypes = cur;
        activityStore.types = cur.join(",");
    }

    // Saved cameras that had no row when the layout was restored ("pane index" -> "hostId:channel").
    // A host whose camera list arrives late (its API is slow or down at start) must get its
    // cameras back in their saved panes, and the saved arrangement must not be overwritten with
    // stand-ins meanwhile.
    property var pendingPanes: ({})
    function hasPendingPanes() { return Object.keys(pendingPanes).length > 0; }
    Timer {
        id: pendingGiveUp
        interval: 120000
        onTriggered: { page.pendingPanes = ({}); page.saveLayout(); }
    }

    function saveLayout() {
        if (!layoutRestored || hasPendingPanes())
            return;
        var out = [];
        for (var i = 0; i < maxPanes; ++i) {
            var r = i < paneSlots.length ? paneSlots[i] : -1;
            if (r < 0) { out.push("-"); continue; }
            var c = Devices.cameraInfo(r);
            out.push(c && c.hostId !== undefined ? c.hostId + ":" + c.channel : "-");
        }
        layoutStore.arrangement = out.join(",");
    }

    function restoreLayout() {
        if (layoutRestored || Devices.count === 0)
            return;
        layoutRestored = true;
        var saved = layoutStore.arrangement;
        if (saved.length > 0) {
            // Build "hostId:channel" -> row once, then place each saved pane.
            var byKey = ({});
            for (var r = 0; r < Devices.count; ++r) {
                var c = Devices.cameraInfo(r);
                if (c && c.hostId !== undefined)
                    byKey[c.hostId + ":" + c.channel] = r;
            }
            var parts = saved.split(",");
            var s = [];
            var used = ({});
            for (var i = 0; i < maxPanes; ++i) {
                var key = i < parts.length ? parts[i] : "-";
                var row = (key !== "-" && byKey[key] !== undefined) ? byKey[key] : -1;
                if (row >= 0 && used[row]) row = -1;
                if (row >= 0) used[row] = true;
                s.push(row);
            }
            paneSlots = s;
            var pending = ({});
            for (var p = 0; p < maxPanes && p < parts.length; ++p)
                if (parts[p] !== "-" && byKey[parts[p]] === undefined)
                    pending[p] = parts[p];
            pendingPanes = pending;
            if (hasPendingPanes())
                pendingGiveUp.restart();
        }
        syncSlots();   // place any camera the saved layout didn't cover
    }

    // Rows arrived: put any saved camera that now exists back in its pane.
    function applyPendingPanes() {
        if (!hasPendingPanes())
            return;
        var byKey = ({});
        for (var r = 0; r < Devices.count; ++r) {
            var c = Devices.cameraInfo(r);
            if (c && c.hostId !== undefined)
                byKey[c.hostId + ":" + c.channel] = r;
        }
        var s = paneSlots.slice();
        var left = ({});
        for (var p in pendingPanes) {
            var row = byKey[pendingPanes[p]];
            if (row === undefined) { left[p] = pendingPanes[p]; continue; }
            for (var i = 0; i < s.length; ++i)
                if (s[i] === row) s[i] = -1;   // it was standing in elsewhere
            s[parseInt(p)] = row;
        }
        paneSlots = s;
        pendingPanes = left;
        if (!hasPendingPanes()) {
            pendingGiveUp.stop();
            syncSlots();
            saveLayout();
        }
    }

    // Removing a device shifts row indices; keep maximized/selected pointing at the
    // right pane (or clear them) so the grid never blanks or maximizes the wrong cam.
    Connections {
        target: Devices
        function onRowsRemoved(parent, first, last) {
            const removed = last - first + 1;
            if (page.maximizedIndex > last)
                page.maximizedIndex -= removed;
            else if (page.maximizedIndex >= first)
                page.maximizedIndex = -1;
            if (page.selectedIndex > last)
                page.selectedIndex -= removed;
            else if (page.selectedIndex >= first)
                page.selectedIndex = -1;
            // Slots hold row indices, which shift the same way.
            var s = page.paneSlots.slice();
            for (var i = 0; i < s.length; ++i) {
                if (s[i] > last) s[i] -= removed;
                else if (s[i] >= first) s[i] = -1;
            }
            page.paneSlots = s;
            page.syncSlots();
            page.saveLayout();
        }
        function onRowsInserted(parent, first, last) {
            const added = last - first + 1;
            var s = page.paneSlots.slice();
            for (var i = 0; i < s.length; ++i)
                if (s[i] >= first) s[i] += added;
            page.paneSlots = s;
            // First devices to arrive are the cue to apply the saved layout.
            if (!page.layoutRestored) {
                page.restoreLayout();
            } else {
                page.applyPendingPanes();
                page.syncSlots();
            }
        }
        function onModelReset() {
            page.paneSlots = [];
            page.layoutRestored = false;
            page.restoreLayout();
        }
    }

    Component.onCompleted: {
        if (presets.indexOf(layoutStore.preset) >= 0)
            preset = layoutStore.preset;
        mosaicMode = layoutStore.mosaic;
        if (Mosaics.find(layoutStore.tab))
            mosaicTab = layoutStore.tab;
        restoreLayout();
        Activity.trackedTypes = activityStore.types.length > 0 ? activityStore.types.split(",") : [];
        // RL_MOCK_ACTIVITY starts the page in Activity mode so a script can drive it.
        if (typeof mockActivity !== "undefined" && mockActivity)
            activityMode = true;
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: page.fullscreen ? 0 : Theme.spacing
        spacing: Theme.spacing

        RowLayout { // grid toolbar
            Layout.fillWidth: true
            spacing: Theme.spacing / 2
            visible: !page.fullscreen

            Repeater {
                model: page.presets
                Rectangle {
                    required property int modelData
                    width: 34
                    height: 26
                    radius: Theme.radius
                    color: page.activePreset === modelData ? Theme.accentDim
                         : presetArea.containsMouse ? Theme.surfaceAlt : Theme.surface
                    border.color: Theme.border
                    Text {
                        anchors.centerIn: parent
                        text: parent.modelData
                        color: page.activePreset === parent.modelData ? Theme.text : Theme.textMuted
                        font.pixelSize: 12
                    }
                    MouseArea {
                        id: presetArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            page.preset = parent.modelData;
                            page.mosaicTab = "";
                            page.maximizedIndex = -1;
                        }
                    }
                    ToolTip {
                        visible: presetArea.containsMouse
                        delay: 500
                        x: (parent.width - width) / 2
                        y: parent.height + 6
                        contentItem: Text {
                            text: modelData === 1 ? qsTr("Single pane")
                                : qsTr("%1-pane grid").arg(modelData)
                            color: Theme.text; font.pixelSize: 11
                        }
                        background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: 4 }
                    }
                }
            }

            Rectangle {
                width: mosaicLabel.implicitWidth + 20
                height: 26
                radius: Theme.radius
                color: page.mosaicMode && !page.tabOpen ? Theme.accentDim
                     : mosaicArea.containsMouse ? Theme.surfaceAlt : Theme.surface
                border.color: Theme.border
                Text {
                    id: mosaicLabel
                    anchors.centerIn: parent
                    text: qsTr("Mosaic")
                    color: page.mosaicMode && !page.tabOpen ? Theme.text : Theme.textMuted
                    font.pixelSize: 12
                }
                MouseArea {
                    id: mosaicArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        // From a tab, Mosaic goes back to the grid-page mosaic.
                        page.mosaicMode = page.tabOpen || !page.mosaicMode;
                        page.mosaicTab = "";
                        page.maximizedIndex = -1;
                    }
                }
                ToolTip {
                    visible: mosaicArea.containsMouse
                    delay: 500
                    x: (parent.width - width) / 2
                    y: parent.height + 6
                    contentItem: Text {
                        text: page.mosaicMode && !page.tabOpen ? qsTr("Mosaic on: back to the plain grid")
                                             : qsTr("Mosaic: this page's cameras at their own shapes, packed to fill the view")
                        color: Theme.text; font.pixelSize: 11
                    }
                    background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: 4 }
                }
            }

            // Named mosaics: one tab each, then +. Tabs that don't fit go into the » menu.
            Item {
                id: tabsArea
                Layout.fillWidth: true
                Layout.preferredHeight: 26
                clip: true

                readonly property int tabSpacing: Theme.spacing / 2
                function tabWidth(name) { return Math.ceil(tabMetrics.advanceWidth(name)) + 20; }
                // How many tabs fit, leaving room for + and, when needed, ».
                readonly property int fit: {
                    var l = Mosaics.list, w = width - 26 - tabSpacing, used = 0;
                    for (var i = 0; i < l.length; ++i) {
                        var tw = tabWidth(l[i].name) + tabSpacing;
                        var more = i < l.length - 1 ? 26 + tabSpacing : 0;
                        if (used + tw + more > w)
                            return i;
                        used += tw;
                    }
                    return l.length;
                }
                FontMetrics { id: tabMetrics; font.pixelSize: 12 }

                Row {
                    spacing: tabsArea.tabSpacing
                    Repeater {
                        model: Mosaics.list
                        Rectangle {
                            id: tab
                            required property var modelData
                            required property int index
                            readonly property bool open: page.mosaicTab === modelData.id
                            visible: index < tabsArea.fit
                            width: tabsArea.tabWidth(modelData.name)
                            height: 26
                            radius: Theme.radius
                            color: open ? Theme.accentDim : tabArea.containsMouse ? Theme.surfaceAlt : Theme.surface
                            border.color: Theme.border
                            Text {
                                anchors.centerIn: parent
                                text: tab.modelData.name
                                color: tab.open ? Theme.text : Theme.textMuted
                                font.pixelSize: 12
                            }
                            MouseArea {
                                id: tabArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                onClicked: (mouse) => {
                                    if (mouse.button === Qt.RightButton)
                                        tabMenu.popupFor(tab.modelData.id);
                                    else
                                        page.openTab(tab.modelData.id);
                                }
                            }
                            ToolTip {
                                visible: tabArea.containsMouse
                                delay: 500
                                x: (parent.width - width) / 2
                                y: parent.height + 6
                                contentItem: Text {
                                    text: qsTr("%1: right-click to edit, rename, move or delete")
                                          .arg(tab.modelData.name)
                                    color: Theme.text; font.pixelSize: 11
                                }
                                background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: 4 }
                            }
                        }
                    }
                    Rectangle {   // » the tabs that don't fit
                        visible: tabsArea.fit < Mosaics.list.length
                        readonly property bool holdsOpen: page.tabOpen && Mosaics.indexOf(page.mosaicTab) >= tabsArea.fit
                        width: 26
                        height: 26
                        radius: Theme.radius
                        color: holdsOpen ? Theme.accentDim
                             : moreArea.containsMouse || moreMenu.visible ? Theme.surfaceAlt : Theme.surface
                        border.color: Theme.border
                        Text { anchors.centerIn: parent; text: "»"; color: Theme.textMuted; font.pixelSize: 13 }
                        MouseArea {
                            id: moreArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: moreMenu.popup(0, parent.height + 4)
                        }
                        ThemedMenu {
                            id: moreMenu
                            Instantiator {
                                model: Mosaics.list.slice(tabsArea.fit)
                                delegate: ThemedMenuItem {
                                    required property var modelData
                                    text: modelData.name
                                    checkable: true
                                    checked: page.mosaicTab === modelData.id
                                    onTriggered: page.openTab(modelData.id)
                                }
                                onObjectAdded: (i, item) => moreMenu.insertItem(i, item)
                                onObjectRemoved: (i, item) => moreMenu.removeItem(item)
                            }
                        }
                    }
                    Rectangle {   // + a new mosaic, seeded with this grid page's cameras
                        width: 26
                        height: 26
                        radius: Theme.radius
                        color: addArea.containsMouse ? Theme.surfaceAlt : Theme.surface
                        border.color: Theme.border
                        Text { anchors.centerIn: parent; text: "+"; color: Theme.textMuted; font.pixelSize: 14 }
                        MouseArea {
                            id: addArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: mosaicEditor.openNew(page.tabOpen ? page.tabMosaic.cameras
                                : page.mosaicRows().map(r => page.cameraKey(r)))
                        }
                        ToolTip {
                            visible: addArea.containsMouse
                            delay: 500
                            x: (parent.width - width) / 2
                            y: parent.height + 6
                            contentItem: Text { text: qsTr("New mosaic"); color: Theme.text; font.pixelSize: 11 }
                            background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: 4 }
                        }
                    }
                }
            }

            // Activity mode: cameras with detections take over idle tiles. Your saved
            // arrangement is left alone and comes back when this is turned off.
            Rectangle {
                visible: !page.mosaicShown
                width: activityLabel.implicitWidth + 20
                height: 26
                radius: Theme.radius
                color: page.activityMode ? Theme.accentDim
                     : activityArea.containsMouse ? Theme.surfaceAlt : Theme.surface
                border.color: Theme.border
                Text {
                    id: activityLabel
                    anchors.centerIn: parent
                    text: page.activityMode && Activity.queued > 0
                          ? qsTr("Activity +%1").arg(Activity.queued) : qsTr("Activity")
                    color: page.activityMode ? Theme.text : Theme.textMuted
                    font.pixelSize: 12
                }
                MouseArea {
                    id: activityArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: page.activityMode = !page.activityMode
                }
                ToolTip {
                    visible: activityArea.containsMouse
                    delay: 500
                    x: (parent.width - width) / 2
                    y: parent.height + 6
                    contentItem: Text {
                        text: qsTr("Activity mode: cameras with people, vehicles, pets or motion take over idle tiles")
                        color: Theme.text; font.pixelSize: 11
                    }
                    background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: 4 }
                }
            }

            // Which detections count in Activity mode.
            Rectangle {
                visible: !page.mosaicShown
                width: 26
                height: 26
                radius: Theme.radius
                color: typesArea.containsMouse || typesPopup.visible ? Theme.surfaceAlt : Theme.surface
                border.color: Theme.border
                Text { anchors.centerIn: parent; text: "▾"; color: Theme.textMuted; font.pixelSize: 11 }
                MouseArea {
                    id: typesArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: typesPopup.visible ? typesPopup.close() : typesPopup.open()
                }
                Popup {
                    id: typesPopup
                    y: parent.height + 4
                    x: parent.width - width
                    padding: 8
                    background: Rectangle { color: Theme.surface; border.color: Theme.border; radius: Theme.radius }
                    contentItem: Column {
                        spacing: 2
                        Text {
                            text: qsTr("Activity mode tracks")
                            color: Theme.textMuted
                            font.pixelSize: 11
                            bottomPadding: 4
                        }
                        Repeater {
                            model: [
                                { key: "person", label: qsTr("Person") },
                                { key: "vehicle", label: qsTr("Vehicle") },
                                { key: "pet", label: qsTr("Pet") },
                                { key: "visitor", label: qsTr("Doorbell visitor") },
                                { key: "motion", label: qsTr("Motion") }
                            ]
                            Rectangle {
                                required property var modelData
                                readonly property bool on: Activity.trackedTypes.indexOf(modelData.key) >= 0
                                width: 190
                                height: 28
                                radius: 4
                                color: rowHover.hovered ? Theme.surfaceAlt : "transparent"
                                Row {
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.left: parent.left
                                    anchors.leftMargin: 6
                                    spacing: 8
                                    Rectangle {
                                        width: 16; height: 16; radius: 3
                                        anchors.verticalCenter: parent.verticalCenter
                                        color: parent.parent.on ? Theme.accent : Theme.surfaceAlt
                                        border.color: parent.parent.on ? Theme.accent : Theme.border
                                        Text {
                                            anchors.centerIn: parent
                                            visible: parent.parent.parent.on
                                            text: "\u2713"; color: Theme.window; font.pixelSize: 11; font.bold: true
                                        }
                                    }
                                    Text {
                                        text: parent.parent.modelData.label
                                        color: Theme.text
                                        font.pixelSize: 12
                                        anchors.verticalCenter: parent.verticalCenter
                                    }
                                }
                                HoverHandler { id: rowHover }
                                TapHandler { onTapped: page.setActivityType(parent.modelData.key, !parent.on) }
                            }
                        }
                    }
                }
            }

            // Previous / next camera (a maximized camera) or page of cameras.
            Repeater {
                model: [-1, 1]
                Rectangle {
                    required property int modelData
                    visible: page.canStep
                    width: 30
                    height: 26
                    radius: Theme.radius
                    color: stepArea.containsMouse ? Theme.surfaceAlt : Theme.surface
                    border.color: Theme.border
                    Text {
                        anchors.centerIn: parent
                        text: parent.modelData < 0 ? "◀" : "▶"
                        color: Theme.textMuted
                        font.pixelSize: 11
                    }
                    MouseArea {
                        id: stepArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: page.step(parent.modelData)
                    }
                    ToolTip {
                        visible: stepArea.containsMouse
                        delay: 500
                        x: (parent.width - width) / 2
                        y: parent.height + 6
                        contentItem: Text {
                            text: page.maximizedIndex >= 0
                                ? (modelData < 0 ? qsTr("Previous camera") : qsTr("Next camera"))
                                : (modelData < 0 ? qsTr("Previous page") : qsTr("Next page"))
                            color: Theme.text; font.pixelSize: 11
                        }
                        background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: 4 }
                    }
                }
            }
            Text {
                visible: page.canStep && page.maximizedIndex < 0
                text: qsTr("%1 / %2").arg(page.curPage + 1).arg(page.pageCount)
                color: Theme.textMuted
                font.pixelSize: 12
            }

            Rectangle {
                width: 34
                height: 26
                radius: Theme.radius
                color: fsArea.containsMouse ? Theme.surfaceAlt : Theme.surface
                border.color: Theme.border
                Text {
                    anchors.centerIn: parent
                    text: "⛶"
                    color: Theme.textMuted
                    font.pixelSize: 13
                }
                MouseArea {
                    id: fsArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: page.fullscreenToggled()
                }
                ToolTip {
                    visible: fsArea.containsMouse
                    delay: 500
                    x: (parent.width - width) / 2
                    y: parent.height + 6
                    contentItem: Text { text: qsTr("Fullscreen (F11)"); color: Theme.text; font.pixelSize: 11 }
                    background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: 4 }
                }
            }
        }

        Item {
            id: gridArea
            Layout.fillWidth: true
            Layout.fillHeight: true

            readonly property real cellWidth:
                (width - gap * (page.cols - 1)) / page.cols
            readonly property real cellHeight:
                (height - gap * (page.rows - 1)) / page.rows

            // Panes are placed by their assigned slot rather than by model order,
            // so a camera can sit in any cell. Positioning is explicit (not a
            // Grid) because the slot, not the child order, decides the cell.
            readonly property int gap: 4
            // `slot` is the pane's place in the arrangement; the current page's first
            // pane sits at the top-left.
            function slotX(slot) { return ((slot - page.pageOffset) % page.cols) * (cellWidth + gap); }
            function slotY(slot) { return Math.floor((slot - page.pageOffset) / page.cols) * (cellHeight + gap); }

            Text {
                anchors.centerIn: parent
                visible: page.mosaicShown && page.maximizedIndex < 0 && mosaic.layoutPanes.length === 0
                text: page.mosaicWaitOver && page.mosaicRows().length === 0
                      ? (page.tabOpen ? qsTr("None of this mosaic's cameras are here")
                                      : qsTr("No cameras on this page"))
                      : qsTr("Loading cameras…")
                color: Theme.textMuted
                font.pixelSize: 12
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

            // Empty slots: drawn under the camera panes so they are drop targets
            // for the cells no camera occupies.
            Repeater {
                model: page.preset
                LivePane {
                    required property int index
                    readonly property int slot: page.pageOffset + index
                    visible: page.maximizedIndex === -1 && !page.mosaicShown && page.rowAt(slot) < 0
                    width: gridArea.cellWidth
                    height: gridArea.cellHeight
                    x: gridArea.slotX(slot)
                    y: gridArea.slotY(slot)
                    paneIndex: slot
                    deviceRow: -1
                    onCameraDropped: (pane, row) => page.assignPane(pane, row)
                }
            }

            // Device-backed panes (model gives live name/status/caps updates).
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

                    readonly property int slot: page.paneOfRow(index)
                    readonly property bool isMaximized: page.maximizedIndex === index
                    readonly property var mrect: page.mosaicShown ? page.mosaicRectByRow[index] : undefined
                    readonly property bool inMosaic: mrect !== undefined

                    // A maximized pane shows even when its slot is beyond the grid
                    // preset (e.g. camera 5 clicked while in the 4-grid).
                    visible: isMaximized ||
                             (page.maximizedIndex === -1 && (page.mosaicShown ? inMosaic
                              : slot >= page.pageOffset && slot < page.pageOffset + page.preset))
                    width: isMaximized ? gridArea.width : inMosaic ? mrect.width : gridArea.cellWidth
                    height: isMaximized ? gridArea.height : inMosaic ? mrect.height : gridArea.cellHeight
                    x: isMaximized ? 0 : inMosaic ? mrect.x : slot < 0 ? 0 : gridArea.slotX(slot)
                    y: isMaximized ? 0 : inMosaic ? mrect.y : slot < 0 ? 0 : gridArea.slotY(slot)
                    offline: inMosaic && !online

                    // The shape this camera takes in the mosaic.
                    readonly property string cameraKey: hostId + ":" + channel
                    readonly property real mosaicRatio: page.drawnRatio(
                        frameRatio > 0 ? frameRatio : page.rememberedFrame(cameraKey),
                        rotationOverride, isFisheye, viewMode)
                    onFrameRatioChanged: page.rememberFrame(cameraKey, frameRatio)
                    onMosaicRatioChanged: page.noteRatio(cameraKey, mosaicRatio)
                    Component.onCompleted: page.noteRatio(cameraKey, mosaicRatio)
                    paneIndex: slot
                    deviceRow: index
                    cameraIdentity: ({ hostId: pane.hostId, channel: pane.channel })
                    label: name
                    viewRotation: rotationOverride
                    fisheyeLens: isFisheye
                    fisheyeMode: viewMode
                    selected: page.selectedIndex === index
                    audioAllowed: !page.audioSuppressed
                    pageActive: page.active && page.visible
                    // Sub-stream in the grid, main stream when maximized (DESIGN §5.7). In a
                    // mosaic a pane returns to the HD choice remembered for it there.
                    forceMain: isMaximized
                    restMain: page.mosaicShown && Mosaics.has(page.activeMosaic, "hd", cameraKey)
                    onQualityChosen: (main) => {
                        if (page.mosaicShown && !isMaximized)
                            Mosaics.setKey(page.activeMosaic, "hd", cameraKey, main);
                    }
                    showLarge: inMosaic && !isMaximized
                    large: page.mosaicShown && Mosaics.has(page.activeMosaic, "large", cameraKey)
                    onLargeToggled: {
                        Mosaics.toggle(page.activeMosaic, "large", cameraKey);
                        mosaicSettle.stop();
                        page.rebuildMosaic();
                    }
                    dropFilter: page.tabOpen ? (src => page.tabDropAllowed(index, src)) : null
                    capPtz: hasPtz
                    capZoom: hasZoom
                    capAudio: hasAudio
                    capSiren: hasSiren
                    capFloodlight: hasFloodlight
                    capTalk: hasTalk
                    // idx is the device row: maximizedIndex/selectedIndex track
                    // cameras, not cells, so they survive a rearrange.
                    onToggleMaximize: (idx) => {
                        page.maximizedIndex = page.maximizedIndex === idx ? -1 : idx;
                        // A double-click arrives as click + double-click; the click may
                        // just have deselected this tile. Maximizing keeps it selected.
                        page.selectedIndex = idx;
                    }
                    // Click selects (blue border + sound); clicking the selected tile
                    // again deselects it and mutes.
                    onClicked: (idx) => page.selectedIndex = page.selectedIndex === idx ? -1 : idx
                    onPopOut: (row, lbl) => page.popOut(row, lbl)
                    onOpenBoschTourRequested: page.openBoschTourRequested()
                    onCameraDropped: (targetPane, row) => {
                        if (page.tabOpen) {
                            page.tabDrop(index, row);
                            return;
                        }
                        page.assignPane(targetPane, row);
                        if (page.mosaicMode) {
                            mosaicSettle.stop();
                            page.rebuildMosaic();   // a swap moves at once
                        }
                        if (page.activityOn) {
                            var pinned = page.userPinnedTiles;
                            pinned[targetPane - page.pageOffset] = true;
                            page.userPinnedTiles = pinned;
                            page.applyPins();
                        }
                    }
                    readonly property var activity: page.activityOf(index)
                    activityKind: activity ? activity.kind : ""
                    activityText: page.activityLabel(activity, name)

                    // Slide to the new cell so a swap reads as movement rather
                    // than two panes blinking into each other's places. In the mosaic
                    // only a re-pack slides; a resize follows the window directly.
                    readonly property bool slides: !pane.isMaximized
                                                   && (!page.mosaicShown || page.mosaicAnimate)
                    readonly property int slideMs: page.mosaicShown ? 250 : 130
                    Behavior on x {
                        enabled: pane.slides
                        NumberAnimation { duration: pane.slideMs; easing.type: Easing.OutCubic }
                    }
                    Behavior on y {
                        enabled: pane.slides
                        NumberAnimation { duration: pane.slideMs; easing.type: Easing.OutCubic }
                    }
                    Behavior on width {
                        enabled: pane.slides && page.mosaicShown
                        NumberAnimation { duration: 250; easing.type: Easing.OutCubic }
                    }
                    Behavior on height {
                        enabled: pane.slides && page.mosaicShown
                        NumberAnimation { duration: 250; easing.type: Easing.OutCubic }
                    }
                }
            }
        }
    }

    // ---- Named mosaic menus and dialogs -----------------------------------------
    ThemedMenu {
        id: tabMenu
        property string mosaicId: ""
        readonly property int at: Mosaics.indexOf(mosaicId)
        function popupFor(id) { mosaicId = id; popup(); }
        ThemedMenuItem { text: qsTr("Edit…"); onTriggered: mosaicEditor.openEdit(tabMenu.mosaicId) }
        ThemedMenuItem { text: qsTr("Rename…"); onTriggered: renameDialog.openFor(tabMenu.mosaicId) }
        ThemedMenuItem {
            text: qsTr("Move left")
            enabled: tabMenu.at > 0
            onTriggered: Mosaics.move(tabMenu.mosaicId, -1)
        }
        ThemedMenuItem {
            text: qsTr("Move right")
            enabled: tabMenu.at >= 0 && tabMenu.at < Mosaics.list.length - 1
            onTriggered: Mosaics.move(tabMenu.mosaicId, 1)
        }
        ThemedMenuSeparator {}
        ThemedMenuItem {
            text: qsTr("Delete…")
            onTriggered: {
                var m = Mosaics.find(tabMenu.mosaicId);
                if (!m)
                    return;
                deleteConfirm.message = qsTr("Delete the mosaic \"%1\"?").arg(m.name);
                deleteConfirm.payload = m.id;
                deleteConfirm.open();
            }
        }
    }
    MosaicEditorDialog {
        id: mosaicEditor
        onSaved: (id) => page.openTab(id)
    }
    ConfirmDialog {
        id: deleteConfirm
        confirmLabel: qsTr("Delete")
        detail: qsTr("Your cameras and the grid are not affected.")
        onConfirmed: (id) => Mosaics.remove(id)
    }
    Dialog {
        id: renameDialog
        modal: true
        width: 360
        anchors.centerIn: Overlay.overlay
        title: qsTr("Rename mosaic")
        background: Rectangle { color: Theme.surface; border.color: Theme.border; radius: Theme.radius }
        header: Item {
            implicitHeight: hdrText.implicitHeight + 22
            Text {
                id: hdrText
                x: Theme.spacing; width: parent.width - Theme.spacing * 2
                anchors.verticalCenter: parent.verticalCenter
                text: renameDialog.title; color: Theme.text
                font.pixelSize: 16; font.bold: true; elide: Text.ElideRight
            }
        }
        property string mosaicId: ""
        function openFor(id) {
            var m = Mosaics.find(id);
            if (!m)
                return;
            mosaicId = id;
            renameField.text = m.name;
            open();
            renameField.selectAll();
            renameField.forceActiveFocus();
        }
        ColumnLayout {
            anchors.fill: parent
            spacing: Theme.spacing
            TextField {
                id: renameField
                Layout.fillWidth: true
                color: Theme.text
                background: Rectangle {
                    color: Theme.surfaceAlt
                    border.color: renameField.activeFocus ? Theme.accent : Theme.border
                    radius: 4
                }
                onAccepted: renameDialog.accept()
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Rectangle {
                    implicitWidth: rnCancel.implicitWidth + 24; implicitHeight: 32; radius: Theme.radius
                    color: rnCancelHover.hovered ? Theme.surfaceAlt : "transparent"
                    border.color: Theme.border
                    Text { id: rnCancel; anchors.centerIn: parent; text: qsTr("Cancel"); color: Theme.text; font.pixelSize: 12 }
                    HoverHandler { id: rnCancelHover }
                    TapHandler { onTapped: renameDialog.reject() }
                }
                Rectangle {
                    implicitWidth: rnOk.implicitWidth + 24; implicitHeight: 32; radius: Theme.radius
                    color: rnOkHover.hovered ? Theme.accent : Theme.accentDim
                    border.color: Theme.accent
                    Text { id: rnOk; anchors.centerIn: parent; text: qsTr("Rename"); color: Theme.text; font.pixelSize: 12 }
                    HoverHandler { id: rnOkHover }
                    TapHandler { onTapped: renameDialog.accept() }
                }
            }
        }
        onAccepted: {
            var name = renameField.text.trim();
            if (name)
                Mosaics.update(mosaicId, { name: name });
        }
    }
}
