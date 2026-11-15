import QtQuick
import QtQuick.Controls
import ReolinkApp
import ReolinkApp.Core

// Live replay on a Live View pane (wayfinder/live-replay ticket 07, layout C): a side rail with
// Playback's transport, a "time behind live" slider along the bottom, and while replaying the
// camera's recording played by Playback's pane over the live picture (the live stream stops),
// an amber border, the recording's date and time with "N behind live", and "● Live" to return.
// LiveReplay (one per Live View page) decides what plays where; this only shows and asks.
Item {
    id: root

    property LiveReplay replay: null
    property string cameraKey: ""
    property int deviceRow: -1
    property bool replayable: false   // a Reolink recorder camera (not Frigate, not a bare stream)
    property bool hovered: false      // the pane is hovered
    property bool maximized: false
    property bool selected: false
    property bool audioAllowed: true
    property bool hd: false
    property int viewRotation: 0
    property bool fisheyeLens: false
    property string fisheyeMode: "fisheye"
    property bool hideRail: false     // the PTZ pad has the right edge
    property color highlightColour: "transparent"

    signal clicked()
    signal doubleClicked()

    readonly property bool replaying: !!replay && replay.cameraKey === cameraKey && cameraKey !== ""
    readonly property PlaybackGroup group: replay ? replay.group : null
    readonly property bool shown: replayable && (replaying || maximized || hovered || slider.dragging)
    readonly property bool playing: replaying && !replay.paused

    // ---- Actions (the rail, the slider and the page's keys) -----------------------------
    function user() { if (pane.item) pane.item.clearLinkSlow(); }
    function begin() {
        if (!replaying)
            replay.start(cameraKey);
        replay.paused = false;
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
        replay.playFrom(Math.min(replay.now, replay.playhead + secs), secs > 0 ? 1 : group.direction);
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
        if (k === "J") {
            if (!replaying) back(() => group.playBackward());
            else speed(() => group.stepFast(-1));
        } else if (k === "K") {
            togglePause();
        } else if (k === "L") {
            speed(() => group.stepFast(1));
        } else if (k === "Left") {
            skip(-10);
        } else if (k === "Right") {
            if (!replaying) return false;
            skip(10);
        }
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
            audioMuted: !root.selected || !root.audioAllowed
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
            text: qsTr("● Live")
            color: "#ff5c5c"
            font.pixelSize: 12; font.bold: true
        }
        HoverHandler { id: liveHover }
        TapHandler { onTapped: root.goLive() }
    }

    // When and how far behind live.
    Rectangle {
        z: 4
        visible: root.replaying
        anchors.left: parent.left
        anchors.bottom: slider.top
        anchors.margins: 8
        width: whenText.implicitWidth + 14
        height: whenText.implicitHeight + 8
        radius: 4
        color: "#cc0d141b"
        Text {
            id: whenText
            anchors.centerIn: parent
            color: Theme.warning
            font.pixelSize: 12
            text: {
                if (!root.replay) return "";
                var t = new Date(root.replay.playhead * 1000);
                return Qt.formatDateTime(t, "ddd d MMM  hh:mm:ss") + "  ·  "
                    + root.behindText(root.replay.behind) + " " + qsTr("behind live")
                    + (root.replay.paused ? "  ·  " + qsTr("paused") : "");
            }
        }
    }
    function behindText(s) {
        s = Math.max(0, Math.round(s));
        if (s < 60) return qsTr("%1 s").arg(s);
        if (s < 3600) return qsTr("%1 min %2 s").arg(Math.floor(s / 60)).arg(s % 60);
        if (s < 86400) return qsTr("%1 h %2 min").arg(Math.floor(s / 3600)).arg(Math.floor(s % 3600 / 60));
        return qsTr("%1 d %2 h").arg(Math.floor(s / 86400)).arg(Math.floor(s % 86400 / 3600));
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

    // ---- The side rail ----------------------------------------------------------------
    component RailButton: Rectangle {
        id: b
        property string glyph: ""
        property string tip: ""
        property bool lit: false
        property bool dim: false
        signal activated()
        width: 34; height: 28; radius: Theme.radius
        color: lit ? Theme.accent : bHover.hovered && !dim ? Theme.surfaceAlt : Theme.surface
        border.color: lit ? Theme.accent : Theme.border
        opacity: dim ? 0.35 : 1
        Text {
            anchors.centerIn: parent
            text: b.glyph
            color: b.lit ? Theme.window : Theme.text
            font.pixelSize: 13
        }
        HoverHandler { id: bHover }
        ToolTip {
            visible: bHover.hovered && b.tip !== ""
            delay: 500
            x: -width - 8
            y: (parent.height - height) / 2
            contentItem: Text { text: b.tip; color: Theme.text; font.pixelSize: 11 }
            background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: 4 }
        }
        TapHandler { enabled: !b.dim; onTapped: b.activated() }
    }
    Rectangle {
        id: rail
        z: 5
        visible: root.shown && !root.hideRail && root.height > 200
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.rightMargin: 8
        width: railCol.implicitWidth + 10
        height: railCol.implicitHeight + 10
        radius: Theme.radius
        color: "#cc0d141b"
        border.color: Theme.border
        readonly property bool g: !!root.group
        Column {
            id: railCol
            anchors.centerIn: parent
            spacing: 4
            RailButton {
                glyph: "◀"; tip: root.replaying ? qsTr("Backwards at 1× (J)") : qsTr("Replay backwards from now (J)")
                lit: root.playing && rail.g && root.group.direction === -1 && root.group.speed === 1
                onActivated: root.back(() => root.group.playBackward())
            }
            RailButton {
                glyph: "«"; tip: qsTr("Fast backwards: 2× → 4× → 8× → 16× → 1×")
                lit: root.playing && rail.g && root.group.direction === -1 && root.group.speed >= 2
                onActivated: root.back(() => root.group.stepFast(-1))
            }
            RailButton {
                glyph: "◀½"; tip: qsTr("Slow backwards: ½× → ¼× → 1×")
                lit: root.playing && rail.g && root.group.direction === -1 && root.group.speed < 1
                onActivated: root.back(() => root.group.stepSlow(-1))
            }
            RailButton {
                glyph: root.replaying && root.replay.paused ? "⏵" : "⏸"
                tip: qsTr("Pause / play on (K)"); dim: !root.replaying
                onActivated: root.togglePause()
            }
            RailButton {
                glyph: "▶½"; tip: qsTr("Slow forwards: ½× → ¼× → 1×"); dim: !root.replaying
                lit: root.playing && rail.g && root.group.direction === 1 && root.group.speed < 1
                onActivated: root.speed(() => root.group.stepSlow(1))
            }
            RailButton {
                glyph: "»"; tip: qsTr("Fast forwards: 2× → 4× → 8× → 16× → 1× (L)"); dim: !root.replaying
                lit: root.playing && rail.g && root.group.direction === 1 && root.group.speed >= 2
                onActivated: root.speed(() => root.group.stepFast(1))
            }
            RailButton {
                glyph: "▶"; tip: qsTr("Forwards at 1×"); dim: !root.replaying
                lit: root.playing && rail.g && root.group.direction === 1 && root.group.speed === 1
                onActivated: root.speed(() => root.group.playForward())
            }
            RailButton {
                glyph: "↺"; tip: qsTr("Back 10 seconds (←)")
                onActivated: root.skip(-10)
            }
            RailButton {
                glyph: "↻"; tip: qsTr("Forward 10 seconds (→)"); dim: !root.replaying
                onActivated: root.skip(10)
            }
        }
    }

    // ---- The "time behind live" slider --------------------------------------------------
    Item {
        id: slider
        z: 5
        visible: root.shown
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: 12
        anchors.rightMargin: rail.visible ? rail.width + 20 : 12
        anchors.bottomMargin: root.replaying ? 8 : 56   // above the live toolbar when live
        height: 34
        property bool dragging: false
        property real dragPos: 0
        readonly property real oldestBehind: root.replay && root.replay.oldest > 0
                                             ? root.replay.now - root.replay.oldest : 0
        readonly property real headPos: dragging ? dragPos
            : root.replaying ? root.replay.posFor(root.replay.playhead) : 0

        Rectangle {
            anchors.fill: parent
            anchors.topMargin: -4
            radius: 4
            color: "#aa0d141b"
        }
        Rectangle {
            id: track
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.topMargin: 8
            height: 6
            radius: 3
            color: Theme.surfaceAlt
            // Where the recordings are (known so far).
            Repeater {
                model: root.replay && root.replaying ? root.replay.strip : []
                Rectangle {
                    required property var modelData
                    x: modelData.from * track.width
                    width: Math.max(1, (modelData.to - modelData.from) * track.width)
                    height: track.height
                    radius: 2
                    color: Theme.accentDim
                }
            }
            Rectangle {
                width: 12; height: 12; radius: 6
                x: slider.headPos * track.width - width / 2
                anchors.verticalCenter: parent.verticalCenter
                color: root.replaying || slider.dragging ? Theme.warning : "#ff5c5c"
                border.color: Theme.window
            }
        }
        // The marks: now · 1 min · 10 min · 1 h · 1 day · 1 week · oldest.
        Repeater {
            model: [{ s: 0, t: qsTr("now") }, { s: 60, t: qsTr("1 min") }, { s: 600, t: qsTr("10 min") },
                    { s: 3600, t: qsTr("1 h") }, { s: 86400, t: qsTr("1 day") }, { s: 604800, t: qsTr("1 wk") }]
            Text {
                required property var modelData
                visible: !!root.replay && slider.oldestBehind > modelData.s && track.width > 240
                x: (root.replay ? root.replay.posForBehind(modelData.s, slider.oldestBehind) : 0) * track.width - (modelData.s === 0 ? 0 : width / 2)
                y: track.y + track.height + 3
                text: modelData.t
                color: Theme.textMuted
                font.pixelSize: 9
            }
        }
        Text {
            visible: slider.oldestBehind > 0
            anchors.right: parent.right
            y: track.y + track.height + 3
            text: root.replay && root.replay.oldestFound ? qsTr("oldest") : qsTr("oldest so far")
            color: Theme.textMuted
            font.pixelSize: 9
        }
        // Dragging: the moment under the pointer; the pane opens there on release.
        Rectangle {
            visible: slider.dragging
            x: Math.max(0, Math.min(slider.width - width, slider.dragPos * track.width - width / 2))
            y: -height - 6
            width: dragText.implicitWidth + 12
            height: dragText.implicitHeight + 6
            radius: 4
            color: Theme.surfaceAlt
            border.color: Theme.border
            Text {
                id: dragText
                anchors.centerIn: parent
                color: Theme.text
                font.pixelSize: 11
                text: {
                    if (!root.replay || !slider.dragging) return "";
                    var e = root.replay.epochForPos(slider.dragPos);
                    return Qt.formatDateTime(new Date(e * 1000), "ddd d MMM  hh:mm:ss") + "  ·  "
                        + root.behindText(root.replay.now - e) + " " + qsTr("behind live");
                }
            }
        }
        MouseArea {
            anchors.fill: parent
            enabled: slider.oldestBehind > 0 || !root.replaying
            preventStealing: true
            function posAt(x) { return Math.max(0, Math.min(1, x / track.width)); }
            onPressed: (m) => {
                // From live, the first press starts the search for recordings.
                if (!root.replaying) root.begin();
                slider.dragPos = posAt(m.x);
                slider.dragging = true;
            }
            onPositionChanged: (m) => { if (slider.dragging) slider.dragPos = posAt(m.x); }
            onReleased: {
                slider.dragging = false;
                if (slider.dragPos <= 0.0005) root.goLive();
                else root.seekToPos(slider.dragPos);
            }
            onCanceled: slider.dragging = false
        }
    }
}
