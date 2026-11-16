import QtQuick
import QtQuick.Controls
import ReolinkApp
import ReolinkApp.Core

// One bottom bar on every video pane (wayfinder/live-replay tickets 10 and 11, layout A): a
// floating pill at the bottom centre — a thin time line on top; ↺ « ◀½ ◀ ❚❚ ▶ ▶½ » ↻, HD,
// sound, the page's common tools and ⋯ for the rest — with the time bottom-left above it. The
// same on Live (live and replaying), Playback panes and the Events clip player. What shows,
// greys, lights and fits is PaneBarRules' (tested C++); the pane's host fills this in and
// answers its signals. Fills its pane; only the pill and the time take pointer input.
Item {
    id: bar

    // ---- What the host tells the bar ------------------------------------------------------
    property string pageKind: "live"          // "live" | "replay" | "playback" | "events"
    property string camera: "recorder"    // "recorder" | "frigate" | "stream"
    property PlaybackGroup speedGroup: null    // speed and direction (null: 1× forward)
    property bool paused: false
    property bool shown: false            // the host's hover (the bar adds its own menus)
    // Tools: [{ id, glyph | label, tip, active, wide }] in the pill's order; only those the
    // camera has. `label` makes a wide text picker (Bosch). The menu reads `text`, else tip.
    property var toolList: []
    property bool hdShown: true
    property bool hdOn: false
    property string hdText: hdOn ? "HD" : "SD"
    property string hdTip: ""
    property bool soundShown: false
    property bool soundOn: false
    property string soundTip: ""
    // The time line: segments [{from, to}] and head as 0..1 of its width (head < 0: none).
    property var segments: []
    property real head: -1
    property color headColour: Theme.warning
    property var ticks: []                // 0..1: marks on the line (the event's moment)
    property var marks: []                // [{pos, text}] under the line, on hover
    property var labelAt: null            // function(pos) → the moment under the pointer
    property bool seekable: true
    property string timeText: ""          // bottom-left, above the pill
    property color timeColour: Theme.text
    property bool timeAlways: false       // shown without hover (a replaying Live pane)

    signal action(string id)              // a transport id (PaneBarRules), "hd" or "sound"
    signal tool(string id, Item anchor)   // a tool, from the pill or the ⋯ menu
    signal seekPressed(real pos)
    signal seekReleased(real pos)
    signal seekCanceled()

    // ---- What the bar tells the host ------------------------------------------------------
    readonly property bool busy: moreMenu.opened || line.dragging || pillHover.hovered
    readonly property bool visibleNow: pill.visible
    readonly property real pillTop: pill.visible ? pill.y : height
    readonly property bool hasTransport: PaneBarRules.hasTransport(rules)

    // ---- Rules ----------------------------------------------------------------------------
    readonly property var rules: ({
        page: bar.pageKind, camera: bar.camera,
        direction: bar.speedGroup ? bar.speedGroup.direction : 1,
        speed: bar.speedGroup ? bar.speedGroup.speed : 1,
        paused: bar.paused,
        speedLocked: bar.speedGroup ? bar.speedGroup.speedLocked : false,
        reverseAvailable: bar.speedGroup ? bar.speedGroup.reverseAvailable : true
    })
    readonly property var toolIds: toolList.map(t => t.id)
    function toolWidth(t) { return t.wide ? (t.wideWidth || 88) : kButton; }
    function widthAt(s) {
        var w = 2 * kPad, n = 0;
        if (hasTransport) {
            var t = PaneBarRules.transport(rules, s);
            for (var id in t) {
                if (!t[id].shown) continue;
                w += id === "pause" ? kBig : t[id].caption !== "" ? kCaptioned : kButton;
                n++;
            }
            w += kSep; n++;
        }
        if (hdShown) { w += kHd; n++; }
        if (soundShown) { w += kButton; n++; }
        var atStep = PaneBarRules.tools(pageKind, toolIds, s);
        for (var i = 0; i < toolList.length; i++)
            if (atStep.shown.indexOf(toolList[i].id) >= 0) { w += toolWidth(toolList[i]); n++; }
        if (atStep.menu.length > 0) { w += kButton; n++; }
        return w + Math.max(0, n - 1) * kSpacing;
    }
    readonly property int step: {
        bar.rules; bar.toolList; bar.hdShown; bar.soundShown;
        var ws = [];
        for (var s = 0; s < PaneBarRules.stepCount; s++) ws.push(widthAt(s));
        return PaneBarRules.fitStep(bar.width - 16, ws);
    }
    readonly property var buttons: hasTransport ? PaneBarRules.transport(rules, step) : ({})
    readonly property var split: PaneBarRules.tools(pageKind, toolIds, step)

    readonly property int kButton: 28
    readonly property int kCaptioned: 34
    readonly property int kBig: 32
    readonly property int kHd: 32
    readonly property int kSep: 9
    readonly property int kSpacing: 2
    readonly property int kPad: 6

    // ---- Pieces ---------------------------------------------------------------------------
    component Tip: ToolTip {
        id: tipBox
        property string tipText: ""
        delay: 500
        y: -height - 8
        contentItem: Text { text: tipBox.tipText; color: Theme.text; font.pixelSize: 11 }
        background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: 4 }
    }
    component Btn: Rectangle {
        id: b
        property string glyph: ""
        property string caption: ""
        property string tip: ""
        property bool lit: false
        property bool active: false
        property bool enabledBtn: true
        property bool big: false
        signal activated()
        width: big ? bar.kBig : caption !== "" ? bar.kCaptioned : bar.kButton
        height: 28
        radius: 4
        color: lit ? Theme.accent : active ? Theme.accentDim
             : bHover.hovered && enabledBtn ? Theme.surfaceAlt : "transparent"
        opacity: enabledBtn ? 1 : 0.35
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: b.caption !== "" ? -4 : 0
            text: b.glyph
            color: b.lit ? Theme.window : Theme.text
            font.pixelSize: b.big ? 15 : 13
        }
        Text {
            visible: b.caption !== ""
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 1
            text: b.caption
            color: b.lit ? Theme.window : Theme.textMuted
            font.pixelSize: 8
            font.bold: b.lit
        }
        HoverHandler { id: bHover }
        Tip { visible: bHover.hovered && b.tip !== ""; tipText: b.tip; x: (b.width - width) / 2 }
        TapHandler {
            enabled: b.enabledBtn
            gesturePolicy: TapHandler.ReleaseWithinBounds
            onTapped: b.activated()
        }
    }
    component Sep: Item {
        width: bar.kSep - 2 * bar.kSpacing; height: 20
        Rectangle { anchors.centerIn: parent; width: 1; height: 18; color: Theme.border }
    }
    component Transport: Btn {
        property string tid: ""
        readonly property var r: bar.buttons[tid] || ({})
        visible: !!r.shown
        enabledBtn: !!r.enabled
        lit: !!r.lit
        caption: r.caption || ""
        property string actTip: ""
        tip: r.enabled ? actTip : (r.reason || actTip)
        onActivated: bar.action(tid)
    }

    // ---- The time, bottom-left above the pill ---------------------------------------------
    Rectangle {
        visible: bar.timeText !== "" && (bar.timeAlways || pill.visible) && bar.width > 220
        anchors.left: parent.left
        anchors.leftMargin: 8
        y: (pill.visible ? pill.y : bar.height) - height - 6
        width: Math.min(timeLabel.implicitWidth + 14, bar.width - 16)
        height: timeLabel.implicitHeight + 8
        radius: 4
        color: Theme.paneBar
        Text {
            id: timeLabel
            anchors.centerIn: parent
            width: Math.min(implicitWidth, bar.width - 30)
            elide: Text.ElideRight
            color: bar.timeColour
            font.pixelSize: 12
            textFormat: Text.StyledText
            text: bar.timeText
        }
    }

    // ---- The pill -------------------------------------------------------------------------
    Rectangle {
        id: pill
        // paneBarsShown: the RL_PANE_BARS screenshot hook (main.cpp).
        visible: bar.shown || bar.busy || (typeof paneBarsShown !== "undefined" && paneBarsShown)
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 8
        width: Math.min(row.implicitWidth + 2 * bar.kPad, bar.width - 16)
        height: row.implicitHeight + 2 * bar.kPad + (line.visible ? line.height + 2 : 0)
        radius: Theme.radius
        color: "#e00d141b"
        border.color: Theme.border
        HoverHandler { id: pillHover }

        // The thin time line along the pill's top.
        Item {
            id: line
            visible: bar.hasTransport
            x: bar.kPad + 4
            y: 5
            width: pill.width - 2 * x
            height: 10
            property bool dragging: false
            property real dragPos: 0
            readonly property real headPos: dragging ? dragPos : bar.head
            Rectangle {
                id: track
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                height: lineHover.hovered || line.dragging ? 6 : 4
                radius: height / 2
                color: Theme.surfaceAlt
                Repeater {
                    model: bar.segments
                    Rectangle {
                        required property var modelData
                        x: Math.max(0, modelData.from) * track.width
                        width: Math.max(1, (Math.min(1, modelData.to) - Math.max(0, modelData.from)) * track.width)
                        height: track.height
                        radius: 2
                        color: Theme.accentDim
                    }
                }
                Repeater {
                    model: bar.ticks
                    Rectangle {
                        required property var modelData
                        x: modelData * track.width - 1
                        y: -3
                        width: 2; height: track.height + 6
                        color: Theme.text
                    }
                }
                Rectangle {
                    visible: line.headPos >= 0
                    width: 10; height: 10; radius: 5
                    x: line.headPos * track.width - width / 2
                    anchors.verticalCenter: parent.verticalCenter
                    color: line.dragging ? Theme.warning : bar.headColour
                    border.color: Theme.window
                }
            }
            HoverHandler { id: lineHover }
            // The marks, above the line on hover (the pill's own row stays put).
            Repeater {
                model: lineHover.hovered || line.dragging ? bar.marks : []
                Text {
                    required property var modelData
                    visible: track.width > 200
                    x: Math.max(0, Math.min(line.width - width, modelData.pos * track.width - width / 2))
                    y: -height - 4
                    text: modelData.text
                    color: Theme.text
                    style: Text.Outline; styleColor: "#000000"
                    font.pixelSize: 9
                }
            }
            Rectangle {   // dragging: the moment under the pointer
                visible: line.dragging && bar.labelAt !== null
                x: Math.max(-line.x, Math.min(line.width - width, line.dragPos * track.width - width / 2))
                y: -height - 18
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
                    text: line.dragging && bar.labelAt ? bar.labelAt(line.dragPos) : ""
                }
            }
            MouseArea {
                anchors.fill: parent
                anchors.topMargin: -4
                anchors.bottomMargin: -2
                enabled: bar.seekable
                preventStealing: true
                cursorShape: Qt.PointingHandCursor
                function posAt(x) { return Math.max(0, Math.min(1, x / track.width)); }
                onPressed: (m) => {
                    line.dragPos = posAt(m.x);
                    line.dragging = true;
                    bar.seekPressed(line.dragPos);
                }
                onPositionChanged: (m) => { if (line.dragging) line.dragPos = posAt(m.x); }
                onReleased: { line.dragging = false; bar.seekReleased(line.dragPos); }
                onCanceled: { line.dragging = false; bar.seekCanceled(); }
            }
        }

        Flickable {
            id: scroller
            x: bar.kPad
            y: (line.visible ? line.y + line.height + 2 : 0) + bar.kPad
            width: pill.width - 2 * bar.kPad
            height: row.implicitHeight
            clip: true
            contentWidth: Math.max(width, row.implicitWidth)
            interactive: contentWidth > width
            flickableDirection: Flickable.HorizontalFlick
            boundsBehavior: Flickable.StopAtBounds

            Row {
                id: row
                x: Math.max(0, (scroller.width - implicitWidth) / 2)
                spacing: bar.kSpacing

                Transport { tid: "skipBack"; glyph: "↺"; actTip: bar.pageKind === "live" ? qsTr("Replay from 10 seconds ago (←)") : qsTr("Back 10 seconds (←)") }
                Transport { tid: "fastBack"; glyph: "«"; actTip: qsTr("Fast backwards: 2× → 4× → 8× → 16× → 1× (J)") }
                Transport { tid: "slowBack"; glyph: "◀½"; actTip: qsTr("Slow backwards: ½× → ¼× → 1×") }
                Transport { tid: "back"; glyph: "◀"; actTip: bar.pageKind === "live" ? qsTr("Replay backwards from now (J)") : qsTr("Backwards at 1×") }
                Transport {
                    tid: "pause"; big: true
                    glyph: bar.paused || bar.pageKind === "live" ? "▷" : "❚❚"
                    actTip: bar.paused ? qsTr("Play on (K)") : qsTr("Pause (K)")
                }
                Transport { tid: "forward"; glyph: "▶"; actTip: qsTr("Forwards at 1×") }
                Transport { tid: "slowForward"; glyph: "▶½"; actTip: qsTr("Slow forwards: ½× → ¼× → 1×") }
                Transport { tid: "fastForward"; glyph: "»"; actTip: qsTr("Fast forwards: 2× → 4× → 8× → 16× → 1× (L)") }
                Transport { tid: "skipForward"; glyph: "↻"; actTip: qsTr("Forward 10 seconds (→)") }
                Sep { visible: bar.hasTransport }

                Btn {
                    visible: bar.hdShown
                    width: bar.kHd
                    glyph: bar.hdText
                    active: bar.hdOn
                    tip: bar.hdTip
                    onActivated: bar.action("hd")
                }
                Btn {
                    visible: bar.soundShown
                    glyph: bar.soundOn ? "🔊" : "🔇"
                    tip: bar.soundTip
                    onActivated: bar.action("sound")
                }
                Repeater {
                    model: bar.toolList
                    Item {
                        id: toolSlot
                        required property var modelData
                        visible: bar.split.shown.indexOf(modelData.id) >= 0
                        width: visible ? (modelData.wide ? wideTool.width : glyphTool.width) : 0
                        height: 28
                        Btn {
                            id: glyphTool
                            visible: !toolSlot.modelData.wide
                            glyph: toolSlot.modelData.glyph || ""
                            tip: toolSlot.modelData.tip || ""
                            active: !!toolSlot.modelData.active
                            onActivated: bar.tool(toolSlot.modelData.id, toolSlot)
                        }
                        Rectangle {   // a text picker (Bosch positions / marked range)
                            id: wideTool
                            visible: !!toolSlot.modelData.wide
                            width: bar.toolWidth(toolSlot.modelData)
                            height: 28
                            radius: 4
                            readonly property bool usable: toolSlot.modelData.usable !== false
                            opacity: usable ? 1 : 0.48
                            color: toolSlot.modelData.active ? Theme.accentDim : (wideHover.hovered ? Theme.surfaceAlt : "transparent")
                            Text {
                                anchors.left: parent.left; anchors.leftMargin: 8
                                anchors.right: parent.right; anchors.rightMargin: 18
                                anchors.verticalCenter: parent.verticalCenter
                                text: toolSlot.modelData.label || ""
                                color: Theme.textMuted
                                font.pixelSize: 10; font.bold: true
                                elide: Text.ElideRight
                            }
                            Text {
                                anchors.right: parent.right; anchors.rightMargin: 7
                                anchors.verticalCenter: parent.verticalCenter
                                text: "⌃"; color: Theme.textMuted; font.pixelSize: 13
                            }
                            HoverHandler { id: wideHover }
                            Tip { visible: wideHover.hovered && (toolSlot.modelData.tip || "") !== ""; tipText: toolSlot.modelData.tip || "" }
                            TapHandler {
                                enabled: wideTool.usable
                                gesturePolicy: TapHandler.ReleaseWithinBounds
                                onTapped: bar.tool(toolSlot.modelData.id, toolSlot)
                            }
                        }
                    }
                }
                Btn {
                    id: moreBtn
                    visible: bar.split.menu.length > 0
                    glyph: "⋯"
                    tip: qsTr("More")
                    active: moreMenu.opened
                    onActivated: moreMenu.opened ? moreMenu.close() : moreMenu.popup(moreBtn, 0, -moreMenu.implicitHeight - 4)
                }
            }
        }
    }

    ThemedMenu {
        id: moreMenu
        Instantiator {
            model: bar.toolList.filter(t => bar.split.menu.indexOf(t.id) >= 0)
            delegate: ThemedMenuItem {
                required property var modelData
                text: (modelData.glyph ? modelData.glyph + "  " : "") + (modelData.text || modelData.tip || modelData.label || "")
                enabled: modelData.usable !== false
                onTriggered: bar.tool(modelData.id, moreBtn)
            }
            onObjectAdded: (index, object) => moreMenu.insertItem(index, object)
            onObjectRemoved: (index, object) => moreMenu.removeItem(object)
        }
    }
}
