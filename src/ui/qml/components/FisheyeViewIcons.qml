import QtQuick
import QtQuick.Controls
import ReolinkApp

// The six fisheye view icons in a column at a picture's right edge, shown while the pointer
// is over it (wayfinder/playback-controls ticket 13; approved layout design in
// fisheye-and-framing prototypes/fisheye-layout). One click picks a view. On a short pane the
// icons shrink so all six fit. Shared by Live, Playback, the Events feed cards and the clip player.
//
// Anchor it to the picture: `anchors.right` + `anchors.top` of the parent; set `shown`.
Column {
    id: root
    property string current: "fisheye"
    property bool shown: false
    // The height the icons must fit in (usually the parent's).
    property real available: parent ? parent.height : 300
    signal picked(string view)
    // Whether a point (in `item`'s coordinates) is on the icons, so a click there isn't also
    // the picture's own click (a parent's TapHandler sees it too).
    function covers(item, x, y) {
        return visible && contains(mapFromItem(item, x, y));
    }

    readonly property real topGap: available < 300 ? 6 : 30
    readonly property real chip: Math.max(18, Math.min(30, (available - topGap - 8) / 6 - 4))
    anchors.topMargin: topGap
    anchors.rightMargin: 6
    spacing: 4
    visible: shown
    z: 20

    Repeater {
        model: FisheyeViews.views
        delegate: Rectangle {
            id: chipItem
            required property var modelData
            readonly property bool isCurrent: root.current === modelData.view
            width: root.chip; height: root.chip; radius: 4
            color: isCurrent ? Theme.accentDim : (hover.hovered ? Theme.surfaceAlt : Theme.overlay)
            border.color: isCurrent ? Theme.accent : Theme.border
            FisheyeViewGlyph {
                anchors.centerIn: parent
                width: parent.width * 0.7; height: width
                view: chipItem.modelData.view
                color: chipItem.isCurrent || hover.hovered ? Theme.text : Theme.textMuted
            }
            HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: root.picked(chipItem.modelData.view) }
            ToolTip {
                visible: hover.hovered
                delay: 300
                x: -width - 8
                y: (parent.height - height) / 2
                contentItem: Text { text: chipItem.modelData.name; color: Theme.text; font.pixelSize: 11 }
                background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: 4 }
            }
        }
    }
}
