import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ReolinkApp
import ReolinkApp.Core

// The live grid: 1/4/9/16 presets, double-click a pane to maximize/restore,
// fullscreen hides all chrome (handled by Main).
Item {
    id: page

    property bool fullscreen: false
    property bool active: true      // true only while Live View is the shown page
    signal fullscreenToggled()
    signal popOut(int deviceRow, string label)

    property int preset: 4
    property int maximizedIndex: -1
    property int selectedIndex: -1

    // A maximized pane is effectively the single ("1") view, so light that button
    // up in the layout toolbar rather than the underlying grid preset.
    readonly property int activePreset: maximizedIndex >= 0 ? 1 : preset

    // Sidebar click: show this camera alone, full-size (the maximized pane —
    // same as double-clicking it — which also defaults it to HD).
    function showCamera(row) {
        if (row < 0 || row >= Devices.count)
            return;
        maximizedIndex = row;
        selectedIndex = row;
    }
    // Column/row counts per preset. 6 is the only non-square layout (3x2).
    readonly property int cols: preset === 1 ? 1 : preset === 4 ? 2 : (preset === 6 || preset === 9) ? 3 : 4
    readonly property int rows: preset === 1 ? 1 : (preset === 4 || preset === 6) ? 2 : preset === 9 ? 3 : 4

    // ---- Paging ---------------------------------------------------------------
    // With more cameras than the grid has cells, the grid shows one page of `preset`
    // cameras at a time (in pane order). Paging never touches the saved arrangement.
    property int gridPage: 0
    readonly property int pageCount: Math.max(1, Math.ceil(Devices.count / preset))
    readonly property int curPage: Math.min(gridPage, pageCount - 1)
    readonly property int pageOffset: curPage * preset
    onPresetChanged: gridPage = 0

    // Previous/next: a maximized camera steps to its neighbour in pane order;
    // otherwise the grid moves a page. Both wrap around.
    function step(dir) {
        if (maximizedIndex >= 0) {
            var order = paneSlots.filter(r => r >= 0);
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
    readonly property bool canStep: maximizedIndex >= 0 ? Devices.count > 1 : pageCount > 1

    // ---- Which camera sits in which pane -----------------------------------
    // paneSlots[pane] = device row (-1 = empty). Without this the grid is a
    // straight 1:1 map of model order, so a camera can only ever appear in the
    // pane matching its position in the sidebar. Always kept at maxPanes long so
    // an arrangement made in the 16-grid survives a trip through the 4-grid. Longer than
    // the biggest grid so cameras beyond 16 can still be reached by paging.
    readonly property int maxPanes: 32
    property var paneSlots: []

    function rowAt(pane) {
        return pane >= 0 && pane < paneSlots.length ? paneSlots[pane] : -1;
    }
    function paneOfRow(row) { return row < 0 ? -1 : paneSlots.indexOf(row); }

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
    }
    property bool layoutRestored: false

    function saveLayout() {
        if (!layoutRestored)
            return;
        var out = [];
        for (var i = 0; i < maxPanes; ++i) {
            var r = rowAt(i);
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
        }
        syncSlots();   // place any camera the saved layout didn't cover
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
            if (!page.layoutRestored)
                page.restoreLayout();
            else
                page.syncSlots();
        }
        function onModelReset() {
            page.paneSlots = [];
            page.layoutRestored = false;
            page.restoreLayout();
        }
    }

    Component.onCompleted: restoreLayout()

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: page.fullscreen ? 0 : Theme.spacing
        spacing: Theme.spacing

        RowLayout { // grid toolbar
            Layout.fillWidth: true
            spacing: Theme.spacing / 2
            visible: !page.fullscreen

            Repeater {
                model: [1, 4, 6, 9, 16]
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

            Item { Layout.fillWidth: true }

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

            // Loudness of the camera you are hearing (the selected tile); the same
            // setting playback uses.
            Text { text: "🔉"; color: Theme.textMuted; font.pixelSize: 13 }
            Slider {
                Layout.preferredWidth: 100
                Layout.preferredHeight: 26
                from: 0; to: 1
                value: AudioPrefs.volume
                onMoved: AudioPrefs.volume = value
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: qsTr("Volume")
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

            // Empty slots: drawn under the camera panes so they are drop targets
            // for the cells no camera occupies.
            Repeater {
                model: page.preset
                LivePane {
                    required property int index
                    readonly property int slot: page.pageOffset + index
                    visible: page.maximizedIndex === -1 && page.rowAt(slot) < 0
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
                    required property string name
                    required property bool hasPtz
                    required property bool hasZoom
                    required property bool hasAudio
                    required property bool hasSiren
                    required property bool hasFloodlight
                    required property bool hasTalk
                    required property int rotationOverride

                    readonly property int slot: page.paneOfRow(index)
                    readonly property bool isMaximized: page.maximizedIndex === index

                    // A maximized pane shows even when its slot is beyond the grid
                    // preset (e.g. camera 5 clicked while in the 4-grid).
                    visible: isMaximized ||
                             (page.maximizedIndex === -1 && slot >= page.pageOffset
                              && slot < page.pageOffset + page.preset)
                    width: isMaximized ? gridArea.width : gridArea.cellWidth
                    height: isMaximized ? gridArea.height : gridArea.cellHeight
                    x: isMaximized || slot < 0 ? 0 : gridArea.slotX(slot)
                    y: isMaximized || slot < 0 ? 0 : gridArea.slotY(slot)
                    paneIndex: slot
                    deviceRow: index
                    label: name
                    viewRotation: rotationOverride
                    selected: page.selectedIndex === index
                    pageActive: page.active && page.visible
                    // Sub-stream in the grid, main stream when maximized (DESIGN §5.7).
                    forceMain: isMaximized
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
                    onCameraDropped: (targetPane, row) => page.assignPane(targetPane, row)

                    // Slide to the new cell so a swap reads as movement rather
                    // than two panes blinking into each other's places.
                    Behavior on x {
                        enabled: !pane.isMaximized
                        NumberAnimation { duration: 130; easing.type: Easing.OutCubic }
                    }
                    Behavior on y {
                        enabled: !pane.isMaximized
                        NumberAnimation { duration: 130; easing.type: Easing.OutCubic }
                    }
                }
            }
        }
    }
}
