import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
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
    // Leaving a maximised pane: the panes it covered stopped while hidden; those with
    // footage start again at the playhead once shown.
    onMaximizedKeyChanged: {
        if (maximizedKey !== "")
            return;
        if (!isPlaying()) {   // stopped: the panes shown again get their picture at the playhead
            Qt.callLater(stillsWhereMissing);
            return;
        }
        selection.keys.forEach(k => { _resume[k] = "ifStopped"; });
        Qt.callLater(flushResume);
    }
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
    // Keys of a recorder that has been removed, or of a camera removed from a Frigate server,
    // are dropped once the device list settles. A bare stream (migrated, or picked before) is swapped for the recorder's copy, or dropped.
    Timer {
        id: pruneTimer
        interval: 8000
        onTriggered: {
            if (Devices.count === 0)
                return;
            selection.pruneHosts(Devices.hostIds());
            selection.pruneCameras(Devices.listedCameras());
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
        reverseAvailable: true   // ticket 12
        // Frigate panes play at 1× forward only (wayfinder/playback-controls ticket 11).
        directKeys: { Devices.count; selection.count;
                      return selection.keys.filter(k => page.isDirectKey(k)); }
        // Back at 1× forward, the frozen Frigate panes catch up at the playhead.
        onSpeedChanged: {
            if (offNormal || !page.isPlaying())
                return;
            selection.keys.filter(k => page.isDirectKey(k))
                     .forEach(k => { page._resume[k] = "ifStopped"; });
            Qt.callLater(page.flushResume);
        }
    }
    function isDirectKey(key) {
        var r = rowsByKey()[key];
        return r !== undefined && Devices.isDirectAt(r);
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
        statusText.text = selection.count === 0 ? ""
                        : qsTr("Synced playback — %n camera(s)", "", selection.count);
        recomputeTimeline();
        scheduleMosaic();
        flushResume();
    }

    // ---- Playing ---------------------------------------------------------------------
    property int streamingPanes: 0   // playing, not fetching a still
    function countStreaming() {
        var n = 0;
        for (var i = 0; i < paneRepeater.count; i++) {
            var p = paneRepeater.itemAt(i);
            if (p && p.visible && p.streaming && !p.stillMode) n++;
        }
        streamingPanes = n;
    }
    // Opening or playing (a still being fetched doesn't count).
    function isPlaying() {
        if (Object.keys(_resume).length > 0)
            return true;
        for (var i = 0; i < paneRepeater.count; i++) {
            var p = paneRepeater.itemAt(i);
            if (p && p.visible && p.busy && !p.stillMode) return true;
        }
        return false;
    }

    // ---- Stills (wayfinder/fisheye-and-framing step 08) ---------------------------------
    // Stopped, each pane shows a picture from the playhead's moment: when a camera's day
    // arrives (page opened, day changed, camera added), when a maximised pane is left, and
    // when a timeline drag rests. At most 4 at once per recorder; playing cancels them.
    StillQueue {
        id: stills
        perRecorder: 4
        onStartStill: (key) => {
            var p = page.paneOf(key);
            if (p && p.visible)
                p.showStillAt(page.playheadSecs, page.epochAt(page.playheadSecs));
            else
                Qt.callLater(stills.done, key);
        }
    }
    function stillFor(key) {
        var p = paneOf(key);
        if (!p || !p.visible || segsByKey[key] === undefined)
            return;
        if (!p.covered(playheadSecs)) {
            stills.cancel(key);
            p.showNoFootage();
            return;
        }
        stills.request(key, parseInt(key.split(":")[0]));
    }
    // Every pane, at the playhead (a drag came to rest).
    function stillsAtPlayhead() { selection.keys.forEach(k => stillFor(k)); }
    // Panes with nothing on screen yet (page opened, panes shown, a camera's day arrived).
    function stillsWhereMissing() {
        if (!active || isPlaying())
            return;
        selection.keys.forEach(k => {
            var p = paneOf(k);
            if (p && p.visible && !p.showsPicture && !p.stillMode)
                stillFor(k);
        });
    }
    Timer { id: stillRest; interval: 250; onTriggered: if (!page.isPlaying()) page.stillsAtPlayhead() }

    function stopAllPanes() {
        stills.cancelAll();
        stillRest.stop();
        _resume = ({});
        for (var i = 0; i < paneRepeater.count; i++) {
            var p = paneRepeater.itemAt(i);
            if (p) p.stopPlayback();
        }
    }
    // Panes waiting to start at the playhead: their day isn't known yet, or their pane
    // isn't on screen yet. "ifStopped": only if it isn't already opening or playing (a
    // pane hidden for a moment by a re-pack or a maximise).
    property var _resume: ({})
    function flushResume() {
        for (var k in _resume) {
            if (!selection.contains(k)) { delete _resume[k]; continue; }
            var p = paneOf(k);
            if (!p || !p.visible || segsByKey[k] === undefined)
                continue;
            var onlyIfStopped = _resume[k] === "ifStopped";
            delete _resume[k];
            if (onlyIfStopped && p.busy)
                continue;
            p.playAtSecs(playheadSecs, epochAt(playheadSecs));
        }
    }
    // Move the playhead and play every pane from that moment; each answers from its own
    // footage (play, or "no footage").
    function playAt(sec) {
        // Backwards from where the camera that leads has nothing: from the end of its
        // previous recording, or paused when there is none that day (ticket 12).
        if (group.direction < 0) {
            var lead = reverseLeadKey();
            if (lead !== "" && !coveredAt(lead, sec)) {
                var prev = previousEnd(lead, sec);
                if (prev < 0) {
                    playheadSecs = sec;
                    stopAllPanes();
                    statusText.text = qsTr("Nothing earlier that day");
                    return;
                }
                sec = prev;
            }
        }
        stills.cancelAll();
        stillRest.stop();
        playheadSecs = sec;
        _resume = ({});
        selection.keys.forEach(k => { _resume[k] = true; });
        flushResume();
    }
    // Advance the playhead in real time while streaming, from the first picture on (the
    // panes keep in step with it). Measured, not counted, so timer jitter doesn't add up.
    // Off 1× it follows the recording time of the frames on screen instead (the recorder's
    // 16× runs nearer 20×, and a slow link holds it back): the selected pane's, or the first
    // Reolink pane's when the selected one is Frigate.
    property real _tickWall: 0
    Timer {
        interval: 250; repeat: true
        running: page.streamingPanes > 0
        onRunningChanged: page._tickWall = Date.now()
        onTriggered: {
            var now = Date.now();
            var dt = (now - page._tickWall) / 1000;
            page._tickWall = now;
            var f = group.offNormal ? page.frameDriverSecs() : -1;
            if (group.direction < 0)
                page.keepReverseInStep();
            if (f >= 0) {
                page.playheadSecs = Math.max(0, Math.min(86399, f));
                return;
            }
            if (page.playheadSecs < 86399)
                page.playheadSecs = Math.max(0, Math.min(86399, page.playheadSecs + dt * group.speed * group.direction));
        }
    }
    // Backwards, the selected camera's recordings decide where to jump (the first Reolink
    // pane's when the selected one is Frigate): ticket 12.
    function reverseLeadKey() {
        if (selectedKey !== "" && !isDirectKey(selectedKey))
            return selectedKey;
        var order = selection.paneOrder;
        for (var i = 0; i < order.length; i++)
            if (!isDirectKey(order[i]))
                return order[i];
        return "";
    }
    function coveredAt(key, sec) {
        var segs = segsByKey[key] || [];
        for (var i = 0; i < segs.length; i++)
            if (sec >= segs[i].start && sec <= segs[i].end)
                return true;
        return false;
    }
    // The end of the last recording finishing before `sec` (-1: none that day).
    function previousEnd(key, sec) {
        var segs = segsByKey[key] || [];
        var best = -1;
        for (var i = 0; i < segs.length; i++)
            if (segs[i].end < sec - 0.5 && segs[i].end > best)
                best = segs[i].end;
        return best;
    }
    // A pane's reverse run reached the start of its recording. The leading camera's takes
    // every pane to the end of its previous recording, or pauses when there is none.
    function onReverseEnded(key) {
        if (group.direction >= 0 || key !== reverseLeadKey())
            return;
        var prev = previousEnd(key, playheadSecs);
        if (prev < 0) {
            stopAllPanes();
            statusText.text = qsTr("Nothing earlier that day");
            return;
        }
        for (var i = 0; i < paneRepeater.count; i++) {
            var p = paneRepeater.itemAt(i);
            if (p) p.freeze();
        }
        playAt(prev);
    }
    // Backwards, panes that aren't leading play at their own pace: one that stopped at the
    // start of its recording starts again when the playhead reaches earlier footage of it,
    // and one more than 5 s from the playhead starts again at the playhead.
    function keepReverseInStep() {
        var lead = paneOf(reverseLeadKey());
        for (var i = 0; i < paneRepeater.count; i++) {
            var p = paneRepeater.itemAt(i);
            if (!p || p === lead || !p.visible || p.frozen || p.stillMode
                || !p.covered(playheadSecs) || Date.now() - p._reverseRestartWall < 3000)
                continue;
            var off = p.frameSecs >= 0 && Math.abs(p.frameSecs - playheadSecs) > 5;
            if (!p.busy || off) {
                p._reverseRestartWall = Date.now();
                p.playAtSecs(playheadSecs, epochAt(playheadSecs));
            }
        }
    }
    function frameDriverSecs() {
        var sel = paneOf(selectedKey);
        if (sel && sel.visible && !sel.frozen && sel.frameSecs >= 0)
            return sel.frameSecs;
        var order = selection.paneOrder;
        for (var i = 0; i < order.length; i++) {
            var p = paneOf(order[i]);
            if (p && p.visible && !p.frozen && p.frameSecs >= 0)
                return p.frameSecs;
        }
        return -1;
    }
    onActiveChanged: active ? Qt.callLater(stillsWhereMissing) : stopAllPanes()

    function epochAt(sec) {
        var d = new Date(selYear, selMonth - 1, selDay, 0, 0, 0);
        return Math.floor(d.getTime() / 1000) + Math.floor(sec);
    }
    readonly property real dayStartEpoch: new Date(selYear, selMonth - 1, selDay).getTime() / 1000
    // The speed buttons (layout B4): change the group's speed, and start playing if stopped.
    function speedAction(fn) {
        fn();
        if (!isPlaying())
            playAt(playheadSecs);
    }
    function togglePause() {
        if (isPlaying())
            stopAllPanes();
        else
            playAt(playheadSecs);
    }
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
    // A new day (calendar or an event's jump) also goes back to ▶ 1×.
    onDayStartEpochChanged: { markStart = -1; markEnd = -1; rangeArmed = false; group.newDay(); }
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
            selection.keys.forEach(k => { if (!_resume[k]) _resume[k] = "ifStopped"; });
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
        stillsWhereMissing();
        // Test hook: play the first recording once to check the video path.
        if (typeof playbackAutoplay !== "undefined" && playbackAutoplay && segments.length > 0 && !_autoplayed) {
            _autoplayed = true;
            if (typeof playbackHd !== "undefined" && playbackHd)
                toggleHd(selectedKey);
            if (typeof playbackBackSteps !== "undefined" && playbackBackSteps > 0) {
                group.playBackward();
                for (var b = 1; b < playbackBackSteps; b++)
                    group.stepFast(-1);
            }
            playAt(typeof playbackAtSecs !== "undefined" && playbackAtSecs >= 0 ? playbackAtSecs
                                                                              : segments[0].start);
            if (typeof playbackFastSteps !== "undefined")
                for (var n = 0; n < playbackFastSteps; n++)
                    group.stepFast(1);
            if (typeof playbackStopMs !== "undefined" && playbackStopMs > 0)
                autoStop.start();
        }
    }
    Timer {   // test hook: RL_PLAYBACK_STOP_MS
        id: autoStop
        interval: typeof playbackStopMs !== "undefined" ? playbackStopMs : 0
        onTriggered: page.stopAllPanes()
    }

    // ---- Mosaic ------------------------------------------------------------------------
    property bool mosaicAnimate: false
    property var mosaicRectByRow: ({})
    // Each camera's decoded shape, shared with Live View ("hostId:channel" = width/height).
    // A pane measures its picture and saves it here, so a camera Live never measured (a
    // dewarped fisheye) gets its own shape too (wayfinder/fisheye-and-framing step 07).
    Settings { id: shapeStore; category: "mosaicShapes" }
    function rememberedFrame(key) {
        var v = parseFloat(shapeStore.value(key, 0));
        return v > 0 ? v : 0;
    }
    function rememberFrame(key, r) {
        if (!key || !FrameShape.shapeChanged(rememberedFrame(key), r))
            return;
        shapeStore.setValue(key, r.toFixed(4));
        scheduleMosaic();
    }
    function paneRatio(row) {
        var c = Devices.cameraInfo(row);
        if (c && c.isFisheye && c.viewMode)
            return FrameShape.viewRatio(c.viewMode);   // the view's shape (frameshape::viewRatio)
        return FrameShape.drawnRatio(rememberedFrame(cameraKey(row)), c ? (c.rotationOverride || 0) : 0);
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
        Qt.callLater(stillsWhereMissing);   // ... or, stopped, show their picture
    }

    Component.onCompleted: {
        var now = new Date();
        selYear = now.getFullYear();
        selMonth = now.getMonth() + 1;
        selDay = now.getDate();
        group.setMonth(selYear, selMonth);
        restoreSelection();
        pruneTimer.start();   // the device list may be complete already (no rowsInserted)
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
                        required property bool isFisheye
                        required property string viewMode
                        cameraKey: hostId + ":" + channel
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
                        fisheyeLens: isFisheye
                        fisheyeMode: viewMode
                        // A fisheye pane takes its view's shape: re-pack when it changes.
                        onFisheyeLensChanged: page.scheduleMosaic()
                        onFisheyeModeChanged: page.scheduleMosaic()
                        selected: page.selectedKey === cameraKey
                        // Sound plays at 1× forward only.
                        audioMuted: AudioPrefs.playbackMuted || group.offNormal
                        audioActive: page.active && selected
                        frozen: { Devices.count; return Devices.isDirectAt(index) && group.offNormal; }
                        speed: frozen ? 1 : group.speed
                        direction: group.direction
                        speedText: { group.speed; group.direction; return group.label(false); }
                        hdActive: page.hdKeys.indexOf(cameraKey) >= 0
                        segments: page.segsByKey[cameraKey] || []
                        onFrameRatioChanged: page.rememberFrame(cameraKey, frameRatio)
                        onStillDone: (picture) => stills.done(cameraKey, picture)
                        onStillModeChanged: page.countStreaming()
                        playheadSecs: page.playheadSecs
                        dayEpoch: page.dayStartEpoch
                        onStreamingChanged: page.countStreaming()
                        onVisibleChanged: page.countStreaming()
                        onClicked: group.select(cameraKey)
                        onReverseEnded: page.onReverseEnded(cameraKey)
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
                onSeek: (seconds) => { page.playheadSecs = seconds; stillRest.restart(); } // stopped: stills when it rests
                onCommit: (seconds) => page.playAt(seconds)       // play from there on release
            }

            // Layout B4 (wayfinder/playback-controls ticket 11): tools on the left, the centred
            // « · slow back · ◀ · ⏸ · ▶ · slow forward · » cluster with the speed as text
            // above it, sound / status / quality on the right.
            Item {
                id: bar
                Layout.fillWidth: true
                Layout.preferredHeight: Math.max(leftTools.height, centre.height, rightTools.height)
                readonly property bool playing: page.streamingPanes > 0
                readonly property string frigateTip: qsTr("Frigate plays at 1× forward only")
                readonly property string reverseTip: qsTr("Coming in reverse play")
                component Ctl: Rectangle {
                    id: ctl
                    property string glyph: ""
                    property string tip: ""
                    property string caption: ""   // small label along the bottom edge
                    property color glyphColor: Theme.text
                    property bool big: false      // ◀ ⏸ ▶
                    property bool lit: false      // the speed playing now
                    property bool dim: false      // greyed: the tooltip says why
                    signal activated()
                    width: big ? 44 : 34; height: big ? 40 : 30; radius: Theme.radius
                    color: lit ? Theme.accent : cHover.hovered && !dim ? Theme.surfaceAlt : Theme.surface
                    border.color: lit ? Theme.accent : Theme.border
                    opacity: dim ? 0.35 : 1
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: parent.caption !== "" ? 2 : (parent.height - height) / 2
                        text: parent.glyph; color: ctl.lit ? Theme.window : parent.glyphColor
                        font.pixelSize: ctl.big ? 17 : parent.caption !== "" ? 13 : 14
                    }
                    Text {
                        visible: parent.caption !== ""
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: parent.height - height - 2
                        text: parent.caption; color: ctl.lit ? Theme.window : Theme.textMuted; font.pixelSize: 9
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
                    TapHandler { enabled: !ctl.dim; onTapped: ctl.activated() }
                }
                component Sep: Rectangle {
                    width: 1; height: 24; color: Theme.border
                    anchors.verticalCenter: parent.verticalCenter
                }

                Row {
                    id: leftTools
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.spacing
                    // Jump 10 s, keeping the speed.
                    Ctl { glyph: "↺"; caption: qsTr("10s"); tip: qsTr("Back 10 seconds (←)"); onActivated: page.skip(-10) }
                    Ctl { glyph: "↻"; caption: qsTr("10s"); tip: qsTr("Forward 10 seconds (→)"); onActivated: page.skip(10) }
                    Sep {}
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
                        anchors.verticalCenter: parent.verticalCenter
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
                }

                Column {
                    id: centre
                    // Centred in the bar, pushed aside rather than over the tools when narrow.
                    x: Math.max(leftTools.width + 12,
                                Math.min((bar.width - width) / 2, bar.width - rightTools.width - 12 - width))
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2
                    readonly property bool locked: group.speedLocked
                    readonly property bool noReverse: !group.reverseAvailable || locked
                    readonly property string backTip: locked ? bar.frigateTip : bar.reverseTip
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: { group.speed; group.direction; return group.label(!bar.playing); }
                        color: group.offNormal ? Theme.accent : Theme.textMuted
                        font.pixelSize: 12; font.bold: group.offNormal
                    }
                    Row {
                        spacing: 5
                        Ctl {
                            anchors.verticalCenter: parent.verticalCenter
                            glyph: "«"; caption: qsTr("fast"); dim: centre.noReverse
                            lit: bar.playing && group.direction === -1 && group.speed >= 2
                            tip: dim ? centre.backTip : qsTr("Fast backwards: 2× → 4× → back to 1× (J)")
                            onActivated: page.speedAction(() => group.stepFast(-1))
                        }
                        Ctl {
                            anchors.verticalCenter: parent.verticalCenter
                            glyph: "◀½"; caption: qsTr("slow"); dim: centre.noReverse
                            lit: bar.playing && group.direction === -1 && group.speed < 1
                            tip: dim ? centre.backTip : qsTr("Slow backwards: ½× → ¼× → back to 1×")
                            onActivated: page.speedAction(() => group.stepSlow(-1))
                        }
                        Ctl {
                            big: true; glyph: "◀"; dim: centre.noReverse
                            lit: bar.playing && group.direction === -1 && group.speed === 1
                            tip: dim ? centre.backTip : qsTr("Play backwards at 1×")
                            onActivated: page.speedAction(() => group.playBackward())
                        }
                        Ctl {
                            big: true; glyph: "⏸"
                            glyphColor: bar.playing ? Theme.text : Theme.accent
                            tip: bar.playing ? qsTr("Pause (K)") : qsTr("Paused: K plays on at this speed")
                            onActivated: if (bar.playing) page.stopAllPanes()
                        }
                        Ctl {
                            big: true; glyph: "▶"
                            lit: bar.playing && !group.offNormal
                            tip: qsTr("Play at 1×")
                            onActivated: page.speedAction(() => group.playForward())
                        }
                        Ctl {
                            anchors.verticalCenter: parent.verticalCenter
                            glyph: "▶½"; caption: qsTr("slow"); dim: centre.locked
                            lit: bar.playing && group.direction === 1 && group.speed < 1
                            tip: dim ? bar.frigateTip : qsTr("Slow forwards: ½× → ¼× → back to 1×")
                            onActivated: page.speedAction(() => group.stepSlow(1))
                        }
                        Ctl {
                            anchors.verticalCenter: parent.verticalCenter
                            glyph: "»"; caption: qsTr("fast"); dim: centre.locked
                            lit: bar.playing && group.direction === 1 && group.speed >= 2
                            tip: dim ? bar.frigateTip : qsTr("Fast forwards: 2× → 4× → 8× → 16× → back to 1× (L)")
                            onActivated: page.speedAction(() => group.stepFast(1))
                        }
                    }
                }

                Row {
                    id: rightTools
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.spacing
                    // Sound: the selected pane's, once its stream has an audio track; 1× forward only.
                    Ctl {
                        visible: page.audioAvailable
                        glyph: AudioPrefs.playbackMuted || group.offNormal ? "🔇" : "🔊"
                        tip: group.offNormal ? qsTr("Sound plays at 1× forward only")
                           : AudioPrefs.playbackMuted ? qsTr("Unmute the selected pane") : qsTr("Mute")
                        onActivated: AudioPrefs.playbackMuted = !AudioPrefs.playbackMuted
                    }
                    Text {
                        id: statusText
                        anchors.verticalCenter: parent.verticalCenter
                        text: ""
                        color: Theme.textMuted
                        font.pixelSize: 12
                        elide: Text.ElideRight
                        width: Math.min(implicitWidth, 320)
                    }
                    // Quality of the selected pane: low by default; HD only within the Site budget.
                    Rectangle {
                        id: hdBtn
                        readonly property bool on: page.hdKeys.indexOf(page.selectedKey) >= 0
                        visible: page.selectedRow >= 0 && !page.selectedDirect
                        width: 52; height: 30; radius: Theme.radius
                        color: on && !sdNow ? Theme.accent : (hdHover.hovered ? Theme.surfaceAlt : Theme.surface)
                        border.color: on ? Theme.accent : Theme.border
                        // HD plays the sub stream above 1× (PlaybackSession::chooseRoute): "SD at speed".
                        // Backwards stays HD while the link keeps up (live-replay ticket 06).
                    readonly property bool atSpeed: on && group.direction > 0 && group.speed > 1
                    readonly property bool linkSlow: { paneRepeater.count;
                        var p = page.selectedRow >= 0 ? paneRepeater.itemAt(page.selectedRow) : null;
                        return on && !!p && p.linkSlow; }
                    readonly property bool sdNow: atSpeed || linkSlow
                        Column {
                            anchors.centerIn: parent
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: hdBtn.on && !hdBtn.sdNow ? qsTr("HD") : qsTr("SD")
                                color: hdBtn.on && !hdBtn.sdNow ? Theme.window : hdBtn.sdNow ? Theme.accent : Theme.text
                                font.pixelSize: 12; font.bold: true
                            }
                            Text {
                                visible: hdBtn.sdNow
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: hdBtn.linkSlow ? qsTr("link slow") : qsTr("at speed")
                                color: Theme.accent; font.pixelSize: 8
                            }
                        }
                        HoverHandler { id: hdHover }
                        ToolTip {
                            visible: hdHover.hovered
                            delay: 500
                            x: (parent.width - width) / 2
                            y: -height - 8
                            contentItem: Text {
                                text: hdBtn.linkSlow ? qsTr("SD: the link is too slow for HD backwards; HD is tried again on the next play or jump")
                                : hdBtn.atSpeed ? qsTr("HD comes back at 1× forward")
                                    : hdBtn.on ? qsTr("Switch the selected pane to SD")
                                                       : qsTr("Switch the selected pane to HD (full resolution)")
                                color: Theme.text; font.pixelSize: 11
                            }
                            background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: 4 }
                        }
                        TapHandler { onTapped: page.toggleHd(page.selectedKey) }
                    }
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

    // Keys (wayfinder/playback-controls ticket 11): J = «, K = pause / play on, L = »,
    // ←/→ = 10 s back / forward. Only while Playback is on screen; a text field that has
    // focus keeps its own keys.
    readonly property bool keysOn: active && !(Window.activeFocusItem
                                               && Window.activeFocusItem.cursorPosition !== undefined)
    Shortcut {
        sequence: "J"
        enabled: page.keysOn && group.reverseAvailable && !group.speedLocked
        onActivated: page.speedAction(() => group.stepFast(-1))
    }
    Shortcut { sequence: "K"; enabled: page.keysOn; onActivated: page.togglePause() }
    Shortcut {
        sequence: "L"
        enabled: page.keysOn && !group.speedLocked
        onActivated: page.speedAction(() => group.stepFast(1))
    }
    Shortcut { sequence: "Left"; enabled: page.keysOn; onActivated: page.skip(-10) }
    Shortcut { sequence: "Right"; enabled: page.keysOn; onActivated: page.skip(10) }

    UndoToast {
        id: undoToast
        onUndo: selection.undo()
        onExpired: selection.dropUndo()
    }
}
