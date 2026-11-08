import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ReolinkApp
import ReolinkApp.Core

// Playback on Site tabs (wayfinder/site-tabs-redesign, step 4): the panes are exactly the
// selected cameras, from any Sites, packed as a mosaic. One calendar and one playhead drive
// every pane; the calendar marks the days any selected camera recorded. Panes play low
// quality; the selected pane (clicked last, outlined) is the one HD, sound, Export and
// Download range act on. Speed applies to all panes. The Site budget applies, counted on
// its own (separately from Live View).
Item {
    id: page

    property bool active: true      // false when Playback isn't on screen: every pane stops
    property var menus: null        // DeviceMenus, for the right-click menus on the Site tabs

    property int selYear: 2026
    property int selMonth: 7
    property int selDay: 9
    property real playheadSecs: 0   // seconds into the selected day
    property string maximizedKey: ""
    property bool _autoplayed: false // test hook guard

    // ---- The selection ---------------------------------------------------------------
    // Stored in [playbackSelection]. On the first launch it is taken from the old 4-pane
    // grid ([playback] gridArrangement, left in place for one release).
    SiteSelection {
        id: selection
        sites: Sites.map
        onSaveRequested: (text) => selStore.setValue("keys", text)
        onChanged: page.followSelection()
        onUndoChanged: if (!canUndo) undoToast.dismiss()
    }
    Settings { id: selStore; category: "playbackSelection" }   // keys, migrated
    Settings { id: gridStore; category: "playback" }           // read once, for the migration
    function restoreSelection() {
        if (String(selStore.value("migrated", false)) === "true") {
            selection.saved = String(selStore.value("keys", ""));
            return;
        }
        var keys = selection.fromGrid(String(gridStore.value("gridArrangement", "")), 4);
        selection.saved = keys.join(",");
        selStore.setValue("keys", keys.join(","));
        selStore.setValue("migrated", true);
    }
    // Keys of a recorder that has been removed are dropped once the device list settles.
    // A bare stream (migrated, or picked before) is swapped for the recorder's copy, or dropped.
    Timer {
        id: pruneTimer
        interval: 8000
        onTriggered: {
            if (Devices.count === 0)
                return;
            selection.pruneHosts(Devices.hostIds());
            selection.keys.filter(k => Sites.isBareStream(k)).forEach(k => {
                selection.toggle(k);
                var instead = Sites.playbackKey(k);
                if (instead && page.rowsByKey()[instead] !== undefined)
                    page.addCamera(instead);
            });
        }
    }

    // The calendar union, the selected pane and the speed.
    PlaybackGroup {
        id: group
        selection: selection
    }
    readonly property string selectedKey: group.selectedKey
    readonly property int selectedRow: { Devices.count; selection.count;
                                         var r = rowsByKey()[selectedKey];
                                         return r === undefined ? -1 : r; }

    // ---- The Site budget ---------------------------------------------------------------
    // Playback's own count. HD is chosen per pane on this page only and not kept: Playback
    // opens low quality.
    property var hdKeys: []
    SiteBudget {
        id: budget
        sites: Sites.map
        selection: selection
        budgets: Sites.budgets
        defaultBudget: Sites.defaultBudget
        costs: Sites.costs
        hd: page.hdKeys
    }
    function hdRefusal(key) {
        if (budget.canHd(key))
            return "";
        var site = Sites.siteOfKey(key);
        return qsTr("%1 is at %2 / %3 Mbit/s; HD needs %4 more.")
                .arg(site).arg(Sites.mbps(budget.used(site))).arg(Sites.mbps(budget.budgetOf(site)))
                .arg(Sites.mbps(budget.hdExtra(key))) + " " + Sites.overBudgetAdvice;
    }
    function toggleHd(key) {
        if (!key)
            return;
        var i = hdKeys.indexOf(key);
        if (i >= 0) {
            hdKeys = hdKeys.filter(k => k !== key);
            return;
        }
        var why = hdRefusal(key);
        if (why !== "") {
            undoToast.show(why, false);
            return;
        }
        hdKeys = hdKeys.concat([key]);
    }

    // A bare stream has no recordings; its card is greyed, naming the recorder's copy.
    function unavailable(key) {
        if (!Sites.isBareStream(key))
            return "";
        var instead = Sites.playbackKey(key);
        var row = instead ? rowsByKey()[instead] : undefined;
        if (row === undefined)
            return qsTr("A bare stream has no recordings of its own.");
        return qsTr("No recordings of its own. Play %1 › %2 instead (click).")
                .arg(Sites.siteOfKey(instead)).arg(Devices.nameAt(row));
    }
    // Add a camera to the selection if the budget allows; else say why. Returns whether
    // it is selected now.
    function addCamera(key) {
        if (selection.contains(key))
            return true;
        if (!budget.canAdd(key)) {
            var site = Sites.siteOfKey(key);
            undoToast.show(qsTr("%1 is at %2 / %3 Mbit/s; this camera needs %4.")
                .arg(site).arg(Sites.mbps(budget.used(site))).arg(Sites.mbps(budget.budgetOf(site)))
                .arg(Sites.mbps(budget.needed(key))) + " " + Sites.overBudgetAdvice, false);
            return false;
        }
        selection.toggle(key);
        return true;
    }

    // ---- Cameras and rows ------------------------------------------------------------
    function cameraKey(row) { return PanePrefs.cameraKey(row); }
    function rowsByKey() {
        var byKey = ({});
        for (var r = 0; r < Devices.count; ++r)
            byKey[cameraKey(r)] = r;
        return byKey;
    }
    function paneOf(key) {
        var r = rowsByKey()[key];
        return r === undefined ? null : paneRepeater.itemAt(r);
    }

    // ---- Recordings ------------------------------------------------------------------
    // Each selected camera's segments for the selected day ("hostId:channel" -> list), set
    // as its recorder answers. A camera not answered yet has no entry.
    property var segsByKey: ({})
    property var _asked: ({})       // key -> { at: ms of the last search, n: searches } for this day
    // A recorder that has just connected may answer its first search empty: an empty day is
    // asked again (at most 3 times, 10 s apart) as the device list changes.
    function search(key, force) {
        var row = rowsByKey()[key];
        if (row === undefined)
            return;
        var now = Date.now(), a = _asked[key] || { at: 0, n: 0 };
        var known = segsByKey[key] !== undefined;
        if (!force && ((known && (segsByKey[key].length > 0 || a.n >= 3)) || now - a.at < 10000))
            return;
        _asked[key] = { at: now, n: a.n + 1 };
        if (typeof mockRecordings !== "undefined" && mockRecordings) {
            Qt.callLater(() => page.mockAnswer(key, row));
            return;
        }
        Devices.searchRecordings(row, selYear, selMonth, selDay);
    }
    // Ask for every selected camera's day (the searches queue per recorder).
    function refresh() {
        segsByKey = ({});
        _asked = ({});
        selection.keys.forEach(k => search(k, true));
        recomputeTimeline();
    }
    function searchMissing() { selection.keys.forEach(k => search(k, false)); }
    // The timeline: one lane per pane (who has footage where), and their union to scrub.
    function recomputeTimeline() {
        var lanes = [], union = [];
        var byKey = rowsByKey();
        selection.paneOrder.forEach(k => {
            var segs = segsByKey[k] || [];
            var r = byKey[k];
            lanes.push({ name: r !== undefined ? Devices.nameAt(r) : k, segments: segs });
            union = union.concat(segs);
        });
        timeline.lanes = lanes.length > 1 ? lanes : [];
        timeline.segments = union;
        var row = page.selectedRow;
        var info = row >= 0 ? Devices.cameraInfo(row) : null;
        timeline.alarmTicks = info && info.hostId !== undefined
            ? Events.eventTimesFor(info.hostId, info.channel, page.dayStartEpoch) : [];
    }
    onSelectedRowChanged: recomputeTimeline()
    function mockAnswer(key, row) {
        var i = selection.paneOrder.indexOf(key);
        var base = [{ start: 3600, end: 12600, type: "timer" }, { start: 14400, end: 15000, type: "alarm" },
                    { start: 28800, end: 43200, type: "timer" }, { start: 45000, end: 45600, type: "alarm" },
                    { start: 61200, end: 79200, type: "timer" }];
        var segs = base.map(s => ({ start: Math.min(86399, s.start + i * 1800),
                                    end: Math.min(86399, s.end + i * 1800), type: s.type }));
        gotSegments(row, segs);
        group.setDays(key, selYear, selMonth, [1, 3, 4, 8 + i, selDay]);
    }

    // ---- The selection changes -------------------------------------------------------
    property var _known: []      // keys selected at the last change
    function followSelection() {
        if (maximizedKey !== "" && !selection.contains(maximizedKey))
            maximizedKey = "";
        hdKeys = hdKeys.filter(k => selection.contains(k));
        var added = selection.keys.filter(k => _known.indexOf(k) < 0);
        _known = selection.keys.slice();
        // A camera added while others play starts at the playhead once its day is known.
        if (streamingPanes > 0)
            added.forEach(k => { _resume[k] = true; });
        added.forEach(k => search(k, false));
        recomputeTimeline();
        scheduleMosaic();
        flushResume();
    }

    // ---- Playing ---------------------------------------------------------------------
    property int streamingPanes: 0
    function countStreaming() {
        var n = 0;
        for (var i = 0; i < paneRepeater.count; i++) {
            var p = paneRepeater.itemAt(i);
            if (p && p.visible && p.streaming) n++;
        }
        streamingPanes = n;
    }
    function stopAllPanes() {
        _resume = ({});
        for (var i = 0; i < paneRepeater.count; i++) {
            var p = paneRepeater.itemAt(i);
            if (p) p.stopPlayback();
        }
    }
    // Panes waiting to start at the playhead: their day isn't known yet, or their pane
    // isn't on screen yet.
    property var _resume: ({})
    function flushResume() {
        for (var k in _resume) {
            if (!selection.contains(k)) { delete _resume[k]; continue; }
            var p = paneOf(k);
            if (!p || !p.visible || segsByKey[k] === undefined)
                continue;
            delete _resume[k];
            p.playAtSecs(playheadSecs, epochAt(playheadSecs));
        }
    }
    // Move the playhead and play every pane from that moment; each answers from its own
    // footage (play, or "no footage").
    function playAt(sec) {
        playheadSecs = sec;
        _resume = ({});
        selection.keys.forEach(k => { _resume[k] = true; });
        flushResume();
    }
    // Advance the playhead in real time while streaming.
    Timer {
        interval: 1000; repeat: true
        running: page.streamingPanes > 0
        onTriggered: if (page.playheadSecs < 86399) page.playheadSecs = Math.min(86399, page.playheadSecs + group.speed)
    }
    onActiveChanged: if (!active) stopAllPanes()

    function epochAt(sec) {
        var d = new Date(selYear, selMonth - 1, selDay, 0, 0, 0);
        return Math.floor(d.getTime() / 1000) + Math.floor(sec);
    }
    readonly property real dayStartEpoch: new Date(selYear, selMonth - 1, selDay).getTime() / 1000
    function speedText(v) { return (v === 0.25 ? "0.25" : v === 0.5 ? "0.5" : String(v)) + "×"; }
    // Jump the playhead by `secs`, keeping each pane's last picture until its new one.
    function skip(secs) {
        var to = Math.max(0, Math.min(86399, playheadSecs + secs));
        for (var i = 0; i < paneRepeater.count; i++) {
            var p = paneRepeater.itemAt(i);
            if (p) p.freeze();
        }
        playAt(to);
    }
    // Save a JPEG of every playing pane.
    function snapshot() {
        var epoch = dayStartEpoch + Math.floor(playheadSecs);
        var saved = [];
        for (var i = 0; i < paneRepeater.count; i++) {
            var p = paneRepeater.itemAt(i);
            var f = p && p.visible ? p.snapshot(epoch) : "";
            if (f !== "")
                saved.push(f);
        }
        statusText.text = saved.length === 0 ? qsTr("Nothing to save: no picture yet")
            : saved.length === 1 ? qsTr("Snapshot saved: %1").arg(saved[0].split("/").pop())
            : qsTr("%1 snapshots saved to %2").arg(saved.length).arg(Downloads.folder);
    }

    // ---- The selected pane's tools ---------------------------------------------------
    readonly property bool selectedDirect: selectedRow >= 0 && Devices.isDirectAt(selectedRow)
    readonly property bool anyReolink: { Devices.count;
        return selection.keys.some(k => { var r = rowsByKey()[k]; return r !== undefined && !Devices.isDirectAt(r); }); }
    readonly property bool audioAvailable: { paneRepeater.count;
                                             var p = selectedRow >= 0 ? paneRepeater.itemAt(selectedRow) : null;
                                             return !!p && p.hasAudio; }
    // Export a clip of the selected pane starting at the playhead.
    function exportClip(secs) {
        if (selectedRow < 0)
            return;
        var epoch = dayStartEpoch + Math.floor(playheadSecs);
        if (Downloads.enqueue(selectedRow, epoch, epoch + secs) >= 0)
            statusText.text = qsTr("Clip queued — see Downloads");
    }
    // Range for "Download range": marked on the timeline, seconds into the day (-1 = unset).
    property real markStart: -1
    property real markEnd: -1
    property bool rangeArmed: false
    onDayStartEpochChanged: { markStart = -1; markEnd = -1; rangeArmed = false; }
    function toggleRange() {
        var now = Math.floor(playheadSecs);
        if (!rangeArmed) {
            markStart = now;
            markEnd = -1;
            rangeArmed = true;
            statusText.text = qsTr("Range started at %1 — press stop to mark the end").arg(clockText(now));
            return;
        }
        rangeArmed = false;
        var s = markStart, e = now;
        if (e < s) { var t = s; s = e; e = t; }
        if (e - s < 2) {
            markStart = -1; markEnd = -1;
            statusText.text = qsTr("Range too short: nothing marked");
            return;
        }
        markStart = s;
        markEnd = e;
        openDownloadDialog();
    }
    function openDownloadDialog() {
        if (selectedRow < 0)
            return;
        var s = markStart >= 0 ? markStart : Math.floor(playheadSecs);
        var e = markEnd > s ? markEnd : s + 120;
        dlDialog.openFor(selectedRow, dayStartEpoch, s, e);
    }
    function clockText(s) {
        var v = Math.floor(s), pad = function (n) { return (n < 10 ? "0" : "") + n; };
        return pad(Math.floor(v / 3600)) + ":" + pad(Math.floor(v / 60) % 60) + ":" + pad(v % 60);
    }
    DownloadDialog { id: dlDialog; onAccepted: dlPanel.open() }
    Connections {
        target: Downloads
        function onItemFinished(id, state, message) {
            statusText.text = state === "done" ? qsTr("Clip saved: %1").arg(message)
                            : state === "failed" ? qsTr("Download failed: %1").arg(message)
                            : qsTr("Download cancelled");
        }
    }

    // ---- From Events: "View ›" -------------------------------------------------------
    // Adds the camera to the selection (if the budget allows) and plays every pane from a
    // few seconds before the event: detection timestamps mark when the alarm fired.
    property string _eventKey: ""
    function openAt(hostId, channel, timestamp) {
        var key = Sites.playbackKey(hostId + ":" + channel);
        var d = new Date((timestamp - 5) * 1000);
        var dayChanged = d.getFullYear() !== selYear || d.getMonth() + 1 !== selMonth || d.getDate() !== selDay;
        stopAllPanes();
        selYear = d.getFullYear();
        selMonth = d.getMonth() + 1;
        selDay = d.getDate();
        playheadSecs = d.getHours() * 3600 + d.getMinutes() * 60 + d.getSeconds();
        if (key === "")
            undoToast.show(qsTr("A bare stream has no recordings of its own."), false);
        else if (addCamera(key)) {
            group.select(key);
            _eventKey = key;
        }
        if (dayChanged)
            refresh();
        else
            searchMissing();
        playAt(playheadSecs);
    }

    Connections {
        target: Devices
        function onRecordingsFound(row, segments) { page.gotSegments(row, segments); }
        function onRecordingsFailed(row, error) {
            var key = page.cameraKey(row);
            if (!selection.contains(key))
                return;
            // An empty lane is the honest render of a failed search; the status says why.
            page.gotSegments(row, []);
            statusText.text = error;
        }
        function onRecordingDaysFound(row, year, month, days) {
            var key = page.cameraKey(row);
            if (key)
                group.setDays(key, year, month, days);
        }
        // A selected camera's recorder connected (its first search may have come back
        // empty): ask again for the cameras still unanswered.
        function onDataChanged() { page.searchMissing(); }
        function onRowsInserted() { page.rowsShifted(); page.searchMissing(); }
        function onRowsRemoved() { page.rowsShifted(); }
        function onModelReset() { page.scheduleMosaic(); }
    }
    // Panes follow device rows: when rows shift, panes after them hide (and stop) until the
    // mosaic is rebuilt. Those playing start again at the playhead once shown.
    function rowsShifted() {
        if (streamingPanes > 0)
            selection.keys.forEach(k => { _resume[k] = true; });
        scheduleMosaic();
        pruneTimer.restart();
    }
    function gotSegments(row, segments) {
        var key = cameraKey(row);
        if (!key || !selection.contains(key))
            return;
        var s = Object.assign({}, segsByKey);
        s[key] = segments;
        segsByKey = s;   // reassigned so pane bindings re-read it
        recomputeTimeline();
        statusText.text = selection.count === 1 ? qsTr("%n recording(s)", "", segments.length)
                                                : qsTr("Synced playback — %n camera(s)", "", selection.count);
        if (key === _eventKey) {
            _eventKey = "";
            statusText.text = qsTr("Event at %1").arg(clockText(playheadSecs + 5));
        }
        flushResume();
        // Test hook: play the first recording once to check the video path.
        if (typeof playbackAutoplay !== "undefined" && playbackAutoplay && segments.length > 0 && !_autoplayed) {
            _autoplayed = true;
            playAt(segments[0].start);
        }
    }

    // ---- Mosaic ------------------------------------------------------------------------
    property bool mosaicAnimate: false
    property var mosaicRectByRow: ({})
    Settings { id: shapeStore; category: "mosaicShapes" }   // Live View keeps each camera's shape
    function paneRatio(row) {
        var c = Devices.cameraInfo(row);
        var f = parseFloat(shapeStore.value(cameraKey(row), 0));
        var r = f > 0 ? f : 16 / 9;
        return c && (c.rotationOverride || 0) % 180 !== 0 ? 1 / r : r;
    }
    function cardRatio(key) {
        var row = rowsByKey()[key];
        return row === undefined ? 16 / 9 : paneRatio(row);
    }
    function rebuildMosaic() {
        mosaicSettle.stop();
        var byKey = rowsByKey(), panes = [];
        selection.paneOrder.forEach(k => {
            if (byKey[k] !== undefined)
                panes.push({ row: byKey[k], large: false, ratio: paneRatio(byKey[k]) });
        });
        mosaic.panes = panes;
    }
    function scheduleMosaic() { if (!mosaicSettle.running) mosaicSettle.start(); }
    Timer { id: mosaicSettle; interval: 300; onTriggered: page.rebuildMosaic() }
    Timer { id: mosaicAnimateEnd; interval: 300; onTriggered: page.mosaicAnimate = false }
    function mapMosaicRects() {
        var m = ({});
        var p = mosaic.layoutPanes, r = mosaic.rects;
        for (var i = 0; i < p.length && i < r.length; ++i)
            m[p[i].row] = r[i];
        mosaicRectByRow = m;
        Qt.callLater(flushResume);   // panes just shown can start
    }

    Component.onCompleted: {
        var now = new Date();
        selYear = now.getFullYear();
        selMonth = now.getMonth() + 1;
        selDay = now.getDate();
        group.setMonth(selYear, selMonth);
        restoreSelection();
        if (typeof mockRecordings !== "undefined" && mockRecordings)
            playheadSecs = 32400;
    }
    // The calendar pages through months itself (which ends its bindings); a date set here
    // (from Events) brings it back. Its dots are the month it shows.
    function showMonth() {
        cal.year = selYear;
        cal.month = selMonth;
        group.setMonth(selYear, selMonth);
    }
    onSelMonthChanged: showMonth()
    onSelYearChanged: showMonth()

    // ---- Site tabs ---------------------------------------------------------------------
    SiteTabs {
        id: tabs
        z: 10
        width: parent.width
        height: implicitHeight
        selection: selection
        budget: budget
        menus: page.menus
        ratioOf: (key) => page.cardRatio(key)
        showSaved: true
        unavailable: (key) => page.unavailable(key)
        onUnavailableTapped: (key) => {
            var instead = Sites.playbackKey(key);
            if (instead && page.rowsByKey()[instead] !== undefined && page.addCamera(instead))
                group.select(instead);
        }
        onCleared: (n) => undoToast.show(n === 1 ? qsTr("Cleared 1 camera ·") : qsTr("Cleared %1 cameras ·").arg(n))
        onRecalled: (name, skipped) => {
            if (skipped.length === 0)
                return;
            var byKey = page.rowsByKey();
            var names = skipped.map(k => byKey[k] !== undefined ? Devices.nameAt(byKey[k]) : k);
            undoToast.show(qsTr("%1: left out %2 (over the Site budget or no recordings)").arg(name).arg(names.join(", ")), false);
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.topMargin: tabs.height + Theme.spacing
        anchors.leftMargin: Theme.spacing
        anchors.rightMargin: Theme.spacing
        anchors.bottomMargin: Theme.spacing
        spacing: Theme.spacing

        // ---- Main: panes + timeline + controls ----
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.spacing

            Item {
                id: mosaicArea
                Layout.fillWidth: true
                Layout.fillHeight: true

                Text {
                    anchors.centerIn: parent
                    visible: page.maximizedKey === "" && mosaic.layoutPanes.length === 0
                    horizontalAlignment: Text.AlignHCenter
                    text: selection.count === 0 ? qsTr("Pick cameras from a Site tab above to play them back here.")
                                                : qsTr("Loading cameras…")
                    color: Theme.textMuted
                    font.pixelSize: 13
                }

                MosaicLayout {
                    id: mosaic
                    width: mosaicArea.width
                    height: mosaicArea.height
                    gap: 4
                    onLayoutPanesChanged: {
                        page.mosaicAnimate = true;
                        mosaicAnimateEnd.restart();
                        page.mapMosaicRects();
                    }
                    onRectsChanged: page.mapMosaicRects()
                    onSettled: {
                        page.mosaicAnimate = true;
                        mosaicAnimateEnd.restart();
                    }
                }

                // A pane per camera (model: Devices); only the selected ones are shown, and
                // only shown panes stream.
                Repeater {
                    id: paneRepeater
                    model: Devices
                    PlaybackPane {
                        id: pbPane
                        required property int index
                        required property var hostId
                        required property int channel
                        required property string name
                        required property int rotationOverride
                        readonly property string cameraKey: hostId + ":" + channel
                        readonly property bool isMax: page.maximizedKey === cameraKey
                        readonly property var mrect: page.mosaicRectByRow[index]
                        readonly property bool inMosaic: mrect !== undefined && selection.contains(cameraKey)
                        visible: page.active && (isMax || (page.maximizedKey === "" && inMosaic))
                        width: isMax ? mosaicArea.width : inMosaic ? mrect.width : 0
                        height: isMax ? mosaicArea.height : inMosaic ? mrect.height : 0
                        x: isMax || !inMosaic ? 0 : mrect.x
                        y: isMax || !inMosaic ? 0 : mrect.y
                        deviceRow: index
                        paneIndex: selection.paneOrder.indexOf(cameraKey)
                        label: name
                        viewRotation: rotationOverride
                        selected: page.selectedKey === cameraKey
                        audioMuted: AudioPrefs.playbackMuted
                        audioActive: page.active && selected
                        speed: group.speed
                        hdActive: page.hdKeys.indexOf(cameraKey) >= 0
                        segments: page.segsByKey[cameraKey] || []
                        playheadSecs: page.playheadSecs
                        dayEpoch: page.dayStartEpoch
                        onStreamingChanged: page.countStreaming()
                        onVisibleChanged: page.countStreaming()
                        onClicked: group.select(cameraKey)
                        onDoubleClicked: {
                            page.maximizedKey = isMax ? "" : cameraKey;
                            group.select(cameraKey);
                        }
                        // Drag a pane onto another of the same Site to swap their places.
                        dropFilter: (src => selection.sameSite(cameraKey, page.cameraKey(src)))
                        onCameraDropped: (target, row) => selection.swap(cameraKey, page.cameraKey(row))
                        readonly property bool slides: !isMax && page.mosaicAnimate
                        Behavior on x { enabled: pbPane.slides; NumberAnimation { duration: 250; easing.type: Easing.OutCubic } }
                        Behavior on y { enabled: pbPane.slides; NumberAnimation { duration: 250; easing.type: Easing.OutCubic } }
                        Behavior on width { enabled: pbPane.slides; NumberAnimation { duration: 250; easing.type: Easing.OutCubic } }
                        Behavior on height { enabled: pbPane.slides; NumberAnimation { duration: 250; easing.type: Easing.OutCubic } }
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

            Timeline {
                id: timeline
                Layout.fillWidth: true
                // Its lanes add height below the track; a layout reads preferredHeight, not height.
                Layout.preferredHeight: trackHeight + lanesHeight
                position: page.playheadSecs
                markStart: page.markStart
                markEnd: page.rangeArmed ? page.playheadSecs : page.markEnd
                onSeek: (seconds) => page.playheadSecs = seconds  // move the playhead only
                onCommit: (seconds) => page.playAt(seconds)       // play from there on release
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacing
                component Ctl: Rectangle {
                    property string glyph: ""
                    property string tip: ""
                    property string caption: ""   // small label along the bottom edge
                    property color glyphColor: Theme.text
                    signal activated()
                    width: 34; height: 30; radius: Theme.radius
                    color: cHover.hovered ? Theme.surfaceAlt : Theme.surface
                    border.color: Theme.border
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: parent.caption !== "" ? 2 : (parent.height - height) / 2
                        text: parent.glyph; color: parent.glyphColor
                        font.pixelSize: parent.caption !== "" ? 13 : 14
                    }
                    Text {
                        visible: parent.caption !== ""
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: parent.height - height - 2
                        text: parent.caption; color: Theme.textMuted; font.pixelSize: 9
                    }
                    HoverHandler { id: cHover }
                    ToolTip {
                        visible: cHover.hovered && tip !== ""
                        delay: 500
                        x: (parent.width - width) / 2
                        y: -height - 8
                        contentItem: Text { text: tip; color: Theme.text; font.pixelSize: 11 }
                        background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: 4 }
                    }
                    TapHandler { onTapped: parent.activated() }
                }
                // Play/pause drives every pane (a stream is one-shot, so play re-opens it).
                Ctl {
                    readonly property bool playing: page.streamingPanes > 0
                    glyph: playing ? "⏸" : "▶"
                    tip: playing ? qsTr("Pause") : qsTr("Play")
                    onActivated: playing ? page.stopAllPanes() : page.playAt(page.playheadSecs)
                }
                // Sound: the selected pane's, once its stream has an audio track.
                Ctl {
                    visible: page.audioAvailable
                    glyph: AudioPrefs.playbackMuted ? "🔇" : "🔊"
                    tip: AudioPrefs.playbackMuted ? qsTr("Unmute the selected pane") : qsTr("Mute")
                    onActivated: AudioPrefs.playbackMuted = !AudioPrefs.playbackMuted
                }
                // Skip back/forward 10 s. There is no reverse play: back means jumping back.
                Ctl { glyph: "⏪"; caption: qsTr("-10s"); tip: qsTr("Back 10 seconds"); onActivated: page.skip(-10) }
                Ctl { glyph: "⏩"; caption: qsTr("+10s"); tip: qsTr("Forward 10 seconds"); onActivated: page.skip(10) }
                // Speed, for every pane (Frigate footage plays at 1×).
                Rectangle {
                    visible: page.anyReolink
                    width: 52; height: 30; radius: Theme.radius
                    color: spdHover.hovered ? Theme.surfaceAlt : Theme.surface
                    border.color: group.speed !== 1 ? Theme.accent : Theme.border
                    Text { anchors.centerIn: parent; text: page.speedText(group.speed)
                           color: group.speed !== 1 ? Theme.accent : Theme.text; font.pixelSize: 12 }
                    HoverHandler { id: spdHover }
                    ToolTip {
                        visible: spdHover.hovered
                        delay: 500
                        x: (parent.width - width) / 2
                        y: -height - 8
                        contentItem: Text { text: qsTr("Playback speed for every pane (sound only plays at 1×)")
                                            color: Theme.text; font.pixelSize: 11 }
                        background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: 4 }
                    }
                    TapHandler { onTapped: speedMenu.popup() }
                    ThemedMenu {
                        id: speedMenu
                        ThemedMenuItem { text: page.speedText(0.25); onTriggered: group.speed = 0.25 }
                        ThemedMenuItem { text: page.speedText(0.5);  onTriggered: group.speed = 0.5 }
                        ThemedMenuItem { text: page.speedText(1);    onTriggered: group.speed = 1 }
                        ThemedMenuItem { text: page.speedText(2);    onTriggered: group.speed = 2 }
                        ThemedMenuItem { text: page.speedText(4);    onTriggered: group.speed = 4 }
                        ThemedMenuItem { text: page.speedText(8);    onTriggered: group.speed = 8 }
                    }
                }
                Ctl { glyph: "📷"; tip: qsTr("Save a snapshot of every pane")
                      onActivated: page.snapshot() }
                Ctl { glyph: "⏹"; tip: qsTr("Stop"); onActivated: page.stopAllPanes() }
                // Export: a clip of the selected pane from the playhead.
                Rectangle {
                    id: exportBtn
                    visible: page.selectedRow >= 0 && !page.selectedDirect
                    width: expRow.implicitWidth + 18; height: 30; radius: Theme.radius
                    color: expHover.hovered ? Theme.surfaceAlt : Theme.surface
                    border.color: Theme.border
                    Row {
                        id: expRow
                        anchors.centerIn: parent
                        spacing: 5
                        Text { text: qsTr("Export clip"); color: Theme.text; font.pixelSize: 12
                               anchors.verticalCenter: parent.verticalCenter }
                        Text { text: "▾"; color: Theme.textMuted
                               font.pixelSize: 10; anchors.verticalCenter: parent.verticalCenter }
                    }
                    HoverHandler { id: expHover }
                    ToolTip {
                        visible: expHover.hovered
                        delay: 500
                        y: -height - 8
                        contentItem: Text { text: qsTr("A clip of the selected pane from the playhead")
                                            color: Theme.text; font.pixelSize: 11 }
                        background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: 4 }
                    }
                    TapHandler { onTapped: exportMenu.popup() }
                    ThemedMenu {
                        id: exportMenu
                        ThemedMenuItem { text: qsTr("15 seconds"); onTriggered: page.exportClip(15) }
                        ThemedMenuItem { text: qsTr("30 seconds"); onTriggered: page.exportClip(30) }
                        ThemedMenuItem { text: qsTr("1 minute");  onTriggered: page.exportClip(60) }
                        ThemedMenuItem { text: qsTr("2 minutes"); onTriggered: page.exportClip(120) }
                    }
                }
                // Range: press to mark the start, again to mark the end (then the download
                // dialog opens for the selected pane's camera).
                Ctl {
                    visible: page.selectedRow >= 0 && !page.selectedDirect
                    glyph: page.rangeArmed ? "■" : "●"
                    glyphColor: "#ff4d4d"
                    tip: page.rangeArmed ? qsTr("Stop: mark the end of the range here")
                                         : qsTr("Record: mark the start of a range here")
                    onActivated: page.toggleRange()
                }
                Text {
                    visible: page.markStart >= 0
                    text: page.rangeArmed ? qsTr("● from %1").arg(page.clockText(page.markStart))
                        : page.markEnd > page.markStart
                        ? page.clockText(page.markStart) + " – " + page.clockText(page.markEnd)
                        : qsTr("from %1").arg(page.clockText(page.markStart))
                    color: page.rangeArmed ? "#ff4d4d" : Theme.accent; font.pixelSize: 12
                    Layout.alignment: Qt.AlignVCenter
                    TapHandler { onTapped: { page.markStart = -1; page.markEnd = -1; page.rangeArmed = false; } }
                }
                Rectangle {
                    visible: page.selectedRow >= 0 && !page.selectedDirect
                    width: dlRow.implicitWidth + 18; height: 30; radius: Theme.radius
                    color: dlHover.hovered ? Theme.surfaceAlt : Theme.surface
                    border.color: Theme.border
                    Text { id: dlRow; anchors.centerIn: parent; text: qsTr("Download range…")
                           color: Theme.text; font.pixelSize: 12 }
                    HoverHandler { id: dlHover }
                    TapHandler { onTapped: page.openDownloadDialog() }
                }
                Rectangle {
                    id: dlBtn
                    visible: page.anyReolink
                    width: dlBtnText.implicitWidth + 18; height: 30; radius: Theme.radius
                    color: dlBtnHover.hovered ? Theme.surfaceAlt : Theme.surface
                    border.color: Downloads.activeCount > 0 ? Theme.accent : Theme.border
                    Text { id: dlBtnText; anchors.centerIn: parent
                           text: Downloads.activeCount > 0 ? qsTr("⬇ Downloads (%1)").arg(Downloads.activeCount)
                                                           : qsTr("⬇ Downloads")
                           color: Theme.text; font.pixelSize: 12 }
                    HoverHandler { id: dlBtnHover }
                    TapHandler { onTapped: dlPanel.opened ? dlPanel.close() : dlPanel.open() }
                    DownloadsPanel { id: dlPanel; x: 0; y: -height - 8 }
                }
                Item { Layout.fillWidth: true }
                Text {
                    id: statusText
                    text: ""
                    color: Theme.textMuted
                    font.pixelSize: 12
                    elide: Text.ElideRight
                    Layout.maximumWidth: 320
                }
                // Quality of the selected pane: low by default; HD only within the Site budget.
                Rectangle {
                    id: hdBtn
                    readonly property bool on: page.hdKeys.indexOf(page.selectedKey) >= 0
                    visible: page.selectedRow >= 0 && !page.selectedDirect
                    width: 52; height: 30; radius: Theme.radius
                    color: on ? Theme.accent : (hdHover.hovered ? Theme.surfaceAlt : Theme.surface)
                    border.color: on ? Theme.accent : Theme.border
                    Text {
                        anchors.centerIn: parent
                        text: parent.on ? qsTr("HD") : qsTr("SD")
                        color: parent.on ? Theme.window : Theme.text
                        font.pixelSize: 12; font.bold: true
                    }
                    HoverHandler { id: hdHover }
                    ToolTip {
                        visible: hdHover.hovered
                        delay: 500
                        x: (parent.width - width) / 2
                        y: -height - 8
                        contentItem: Text {
                            text: hdBtn.on ? qsTr("Switch the selected pane to SD")
                                                   : qsTr("Switch the selected pane to HD (full resolution)")
                            color: Theme.text; font.pixelSize: 11
                        }
                        background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: 4 }
                    }
                    TapHandler { onTapped: page.toggleHd(page.selectedKey) }
                }
            }
        }

        // ---- Right: calendar ----
        Rectangle {
            Layout.preferredWidth: 240
            Layout.fillHeight: true
            color: Theme.surface
            border.color: Theme.border
            radius: Theme.radius

            MonthCalendar {
                id: cal
                onMonthChanged: group.setMonth(year, month)
                onYearChanged: group.setMonth(year, month)
                anchors.top: parent.top
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.topMargin: Theme.spacing
                year: page.selYear
                month: page.selMonth
                selDay: page.selDay
                markedDays: group.recordingDays
                onDateSelected: (y, m, d) => {
                    page.stopAllPanes();
                    page.selYear = y; page.selMonth = m; page.selDay = d;
                    page.refresh();
                }
            }
        }
    }

    UndoToast {
        id: undoToast
        onUndo: selection.undo()
        onExpired: selection.dropUndo()
    }
}
