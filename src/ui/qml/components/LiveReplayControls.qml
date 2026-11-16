import QtQuick
import QtQuick.Controls
import ReolinkApp
import ReolinkApp.Core
import "TransportActions.js" as TransportActions

// Live replay on a Live View pane (wayfinder/live-replay ticket 07): while replaying, the
// camera's recording played by Playback's pane over the live picture (the live stream stops),
// an amber border and "● Live" to return. The transport and the "time behind live" line are
// the pane's bottom bar (PaneBar, ticket 11), which calls the actions here (act, track*) and
// reads the line and the time from here. LiveReplay (one per Live View page) decides what
// plays where; this only shows and asks.
Item {
    id: root

    property LiveReplay replay: null
    property string cameraKey: ""
    property int deviceRow: -1
    property bool replayable: false   // a Reolink recorder camera (not Frigate, not a bare stream)
    property bool selected: false
    property bool audioAllowed: true
    property bool hd: false
    property int viewRotation: 0
    property bool fisheyeLens: false
    property string fisheyeMode: "fisheye"
    property color highlightColour: "transparent"
    // Live events while replaying (live-replay ticket 14): what this camera sees now, on
    // "● Live" ("● Live · Person"); and, maximised, the other shown cameras highlighted now
    // ([{ key, text, colour }]), each a chip that goes to that camera live.
    property string liveNote: ""
    property var othersLit: []
    signal showCamera(string key)

    signal clicked()
    signal doubleClicked()

    readonly property bool replaying: !!replay && replay.cameraKey === cameraKey && cameraKey !== ""
    // This pane's camera was replaying (read when the pane's camera changes).
    property bool wasReplaying: false
    onReplayingChanged: Qt.callLater(function () { root.wasReplaying = root.replaying; })
    readonly property PlaybackGroup group: replay ? replay.group : null
    readonly property bool playing: replaying && !replay.paused

    // ---- Actions (the pane bar and the page's keys) ---------------------------------------
    function user() { if (pane.item) pane.item.clearLinkSlow(); }
    function begin() {
        if (!replaying) {
            replay.start(cameraKey);
            refreshBuffer();   // before the first play: ◀ from now starts in the buffer
        }
        replay.paused = false;
    }
    // The camera's live buffer as footage for the replay (live-replay ticket 08b): at the
    // start and each second while replaying (it keeps filling: live stays connected).
    function refreshBuffer() { if (replay && replaying) replay.setBufferSpans(Devices.liveBufferSpans(deviceRow)); }
    Timer {
        interval: 1000
        repeat: true
        running: root.replaying
        onTriggered: root.refreshBuffer()
    }
    // From live: backwards from now at the speed `step` sets.
    // A change of direction while playing reopens by itself (PlaybackSession::reopenNeeded).
    function back(step) {
        var live = !replaying, wasPaused = !live && replay.paused;
        begin();
        step();
        user();
        if (live)
            replay.playFrom(replay.now, -1);
        else if (wasPaused)
            replay.playFrom(replay.playhead, group.direction);
    }
    function speed(step) {
        if (!replaying)
            return;
        step();
        if (replay.paused) {
            replay.paused = false;
            user();
            replay.playFrom(replay.playhead, group.direction);
        }
    }
    function togglePause() {
        if (!replaying)
            return;
        if (replay.paused) {
            replay.paused = false;
            user();
            replay.playFrom(replay.playhead, group.direction);
        } else {
            replay.paused = true;
            if (pane.item) pane.item.stopPlayback();
        }
    }
    function skip(secs) {
        if (!replaying) {
            if (secs > 0)
                return;              // nothing ahead of live
            begin();
            group.playForward();
            user();
            replay.playFrom(replay.now + secs, 1);
            return;
        }
        if (pane.item) pane.item.freeze();
        replay.paused = false;
        user();
        replay.skipBy(secs);
    }
    function seekToPos(pos) {
        begin();
        if (group.direction < 0 && !replaying)
            group.playForward();
        user();
        if (pane.item) pane.item.freeze();
        replay.playFrom(replay.epochForPos(pos), group.direction);
    }
    function goLive() { if (replaying) replay.stop(); }
    function pause() { if (replaying && !replay.paused) togglePause(); }
    // A speed button: backwards ones start replay from live (back), forwards ones only change
    // a running replay (speed).
    function speedAction(step, dir) { if (dir < 0) back(step); else speed(step); }
    // The pane bar's transport (PaneBarRules ids; TransportActions).
    function act(id) {
        if (!replayable || !group)
            return;
        TransportActions.apply(id, group, root);
    }
    // The pane bar's time line: a press from live starts the search for recordings; a release
    // at "now" goes back to live, anywhere else plays from there.
    property bool _trackFromLive: false
    function trackPressed() {
        _trackFromLive = !replaying;
        if (_trackFromLive) begin();
    }
    function trackReleased(pos) {
        if (pos <= 0.0005) goLive();
        else seekToPos(pos);
    }
    function trackCanceled() { if (_trackFromLive) goLive(); }
    // What the line shows: where the recordings are (known so far), the playhead (0 = now) and
    // the marks now · 1 min · 10 min · 1 h · 1 day · 1 week · oldest (LiveReplay::sliderMarks).
    readonly property real oldestBehind: replay && replay.oldest > 0 ? replay.now - replay.oldest : 0
    readonly property var strip: replay && replaying ? replay.strip : []
    // posFor reads now and the oldest too: re-read when either moves.
    readonly property real headPos: replaying ? (replay.now, replay.oldest, replay.posFor(replay.playhead)) : 0
    readonly property var markNames: [qsTr("now"), qsTr("1 min"), qsTr("10 min"), qsTr("1 h"),
                                      qsTr("1 day"), qsTr("1 wk")]
    readonly property var marks: {
        if (!replay || oldestBehind <= 0) return [];
        var out = [], m = replay.sliderMarks();
        for (var i = 0; i < m.length && i < markNames.length; i++)
            if (oldestBehind > m[i]) out.push({ pos: replay.posForBehind(m[i], oldestBehind), text: markNames[i] });
        out.push({ pos: 1, text: replay.oldestFound ? qsTr("oldest") : qsTr("oldest so far") });
        return out;
    }
    function labelAt(pos) {
        if (!replay) return "";
        var e = replay.epochForPos(pos);
        return Qt.formatDateTime(new Date(e * 1000), "ddd d MMM  hh:mm:ss") + "  ·  "
            + replay.behindText(replay.now - e) + " " + qsTr("behind live");
    }
    // When and how far behind live (the bar's time, bottom-left).
    readonly property string whenText: {
        if (!replaying) return "";
        var t = new Date(replay.playhead * 1000);
        return Qt.formatDateTime(t, "ddd d MMM  hh:mm:ss") + "  ·  "
            + replay.behindText(replay.behind) + " " + qsTr("behind live")
            + (replay.paused ? "  ·  " + qsTr("paused") : "");
    }
    // The replaying picture's tools (the bar's ◉ and ⊕ while replaying).
    readonly property bool hasAudio: !!pane.item && pane.item.hasAudio
    readonly property bool zoomed: !!pane.item && pane.item.zoom > 1.01
    function snapshot() {
        return pane.item ? pane.item.snapshot(Math.floor(replay.playhead)) : "";
    }
    function toggleZoom() {
        if (!pane.item) return;
        if (pane.item.zoom > 1.01) { pane.item.zoom = 1; pane.item.panX = 0; pane.item.panY = 0; }
        else pane.item.zoom = 2;
    }
    function showNotice(t) { notice.show(t); }
    // Play from `secs` behind now going `dir`, after `fast` presses of » (the test hook).
    function playBehind(secs, dir, fast) {
        begin();
        if (dir < 0) group.playBackward(); else group.playForward();
        for (var i = 0; i < fast; i++) group.stepFast(dir);
        user();
        replay.playFrom(replay.now - secs, dir);
    }
    // The page's keys: J, K, L, Left, Right.
    function key(k) {
        if (!replayable)
            return false;
        // J from live starts ◀ (as the bar's ◀); while replaying it steps « like a forwards
        // speed (no restart of a paused replay's direction first).
        if (k === "J") {
            if (!replaying) back(() => group.playBackward());
            else speed(() => group.stepFast(-1));
            return true;
        }
        if (k === "Right" && !replaying)
            return false;
        const id = TransportActions.keyId(k);
        if (id !== "")
            TransportActions.apply(id, group, root);
        return true;
    }

    // ---- The recording, over the live picture ---------------------------------------
    Loader {
        id: pane
        anchors.fill: parent
        active: root.replaying
        z: 1
        sourceComponent: PlaybackPane {
            deviceRow: root.deviceRow
            cameraKey: root.cameraKey
            label: ""
            viewRotation: root.viewRotation
            fisheyeLens: root.fisheyeLens
            fisheyeMode: root.fisheyeMode
            hdActive: root.hd
            selected: root.selected
            // As Playback: muted off 1× forward or when Playback's sound is off (live-replay 07).
            audioMuted: AudioPrefs.playbackMuted || root.group.offNormal || !root.selected
                        || !root.audioAllowed
            audioActive: true
            segments: root.replay.daySegments
            dayEpoch: root.replay.dayEpoch
            playheadSecs: root.replay.playhead - root.replay.dayEpoch
            speed: root.group.speed
            direction: root.group.direction
            speedText: root.group.speed !== 1 || root.group.direction !== 1
                       ? root.group.label(root.replay.paused) : ""
            onFrameSecsChanged: if (frameSecs >= 0) root.replay.frameAt(dayEpoch + frameSecs, Date.now())
            reportForwardEnd: true
            useLiveBuffer: true
            onReverseEnded: root.replay.carryOn(-1)
            onForwardEnded: root.replay.carryOn(1)
            onClicked: root.clicked()
            onDoubleClicked: root.doubleClicked()
        }
    }
    Connections {
        target: root.replay
        enabled: root.replaying
        function onPlayWanted(epoch) {
            if (pane.item)
                pane.item.playAtSecs(epoch - root.replay.dayEpoch, epoch);
        }
        function onNothingEarlier() {
            root.replay.paused = true;
            if (pane.item) pane.item.stopPlayback();
            notice.show(qsTr("Nothing earlier: this is the oldest recording"));
        }
    }
    // The playWanted for a play that started before the pane existed (a first ◀).
    Connections {
        target: pane
        function onLoaded() {
            if (root.replay.daySegments.length > 0 && root.replay.covered(root.replay.playhead))
                pane.item.playAtSecs(root.replay.playhead - root.replay.dayEpoch, root.replay.playhead);
        }
    }

    // Amber border while replaying; a highlight still flashes (it doesn't return to live).
    Rectangle {
        anchors.fill: parent
        z: 2
        visible: root.replaying
        color: "transparent"
        border.color: root.highlightColour.a > 0 ? root.highlightColour : Theme.warning
        border.width: 3
    }

    // "● Live": back to live.
    Rectangle {
        z: 4
        visible: root.replaying
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 8
        width: liveText.implicitWidth + 18
        height: 26
        radius: 13
        color: liveHover.hovered ? Theme.surfaceAlt : "#cc0d141b"
        border.color: Theme.border
        Text {
            id: liveText
            anchors.centerIn: parent
            text: root.liveNote !== "" ? qsTr("● Live · %1").arg(root.liveNote) : qsTr("● Live")
            color: Theme.danger
            font.pixelSize: 12; font.bold: true
        }
        HoverHandler { id: liveHover }
        TapHandler { onTapped: root.goLive() }
    }
    // The other cameras highlighted now (maximised): a tap goes to that camera, live.
    Column {
        z: 4
        visible: root.replaying && root.othersLit.length > 0
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: 8 + 26 + 6
        anchors.rightMargin: 8
        spacing: 4
        Repeater {
            model: root.replaying ? root.othersLit : []
            delegate: Rectangle {
                required property var modelData
                x: parent ? parent.width - width : 0   // right-aligned (a Column sets only y)
                width: chipText.implicitWidth + 18
                height: 24
                radius: 12
                color: chipHover.hovered ? Theme.surfaceAlt : "#cc0d141b"
                border.color: modelData.colour
                border.width: 2
                Text {
                    id: chipText
                    anchors.centerIn: parent
                    text: modelData.text
                    color: Theme.text
                    font.pixelSize: 12
                }
                HoverHandler { id: chipHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: root.showCamera(modelData.key) }
                ToolTip.visible: chipHover.hovered
                ToolTip.text: qsTr("Back to live on this camera")
            }
        }
    }

    // A short message on the pane.
    Rectangle {
        id: notice
        z: 6
        visible: opacity > 0
        opacity: 0
        anchors.centerIn: parent
        width: noticeText.implicitWidth + 20
        height: noticeText.implicitHeight + 12
        radius: 4
        color: "#dd0d141b"
        function show(t) { noticeText.text = t; opacity = 1; noticeTimer.restart(); }
        Text { id: noticeText; anchors.centerIn: parent; color: Theme.text; font.pixelSize: 12 }
        Timer { id: noticeTimer; interval: 4000; onTriggered: notice.opacity = 0 }
        Behavior on opacity { NumberAnimation { duration: 200 } }
    }
}
