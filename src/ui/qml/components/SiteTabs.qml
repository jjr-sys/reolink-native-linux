import QtQuick
import QtQuick.Controls
import ReolinkApp
import ReolinkApp.Core

// The Site tabs strip (wayfinder/site-tabs-redesign, variant B): centred segmented Site
// tabs, each with a dot per selected camera; tapping one drops its camera row, a
// translucent drawer of camera cards, over the page below. Tapping a card selects or
// deselects that camera in `selection`. Right-clicks open the camera and recorder menus.
// The page puts its own controls in `leftItems` and `rightItems` (Clear all is built in).
Item {
    id: root

    property SiteSelection selection
    property var menus: null                       // DeviceMenus
    property var ratioOf: (key) => 16 / 9          // a camera's drawn shape, width/height
    property string openSite: ""
    readonly property bool drawerOpen: openSite !== "" && Sites.list.indexOf(openSite) >= 0
    property alias leftItems: leftRow.data
    property alias rightItems: rightRow.data
    readonly property bool hovered: stripHover.hovered || drawerHover.hovered
    signal cleared(int count)

    function close() { openSite = ""; }

    implicitHeight: 44

    // Status, names and last-seen frames are plain calls on the device model, so the cards
    // re-read them on every model change.
    property int rev: 0
    Connections {
        target: Devices
        function onDataChanged() { root.rev++; }
        function onRowsInserted() { root.rev++; }
        function onRowsRemoved() { root.rev++; }
        function onModelReset() { root.rev++; }
    }
    // Last-seen frames are re-read each time a drawer opens.
    property int frameRev: 0
    onOpenSiteChanged: frameRev++

    Rectangle {
        id: strip
        anchors.fill: parent
        color: Theme.window
        z: 2
        HoverHandler { id: stripHover }
        Rectangle { // bottom rule only
            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
            height: 1
            color: Theme.border
        }

        Row {
            id: leftRow
            anchors.left: parent.left
            anchors.leftMargin: Theme.spacing
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spacing / 2
        }

        Rectangle {   // the segmented Site tabs
            id: seg
            anchors.centerIn: parent
            height: 32
            width: segRow.implicitWidth + 6
            radius: height / 2
            color: Theme.surface
            border.color: Theme.border
            Row {
                id: segRow
                anchors.centerIn: parent
                spacing: 0
                Repeater {
                    model: Sites.list
                    Rectangle {
                        id: tab
                        required property string modelData
                        readonly property bool open: root.openSite === modelData
                        readonly property int picked: { root.selection ? root.selection.count : 0;
                                                         Sites.revision;
                                                         return root.selection ? root.selection.countIn(modelData) : 0; }
                        width: tabRow.implicitWidth + 30
                        height: 26
                        radius: 13
                        color: open ? Theme.accentDim : tabHover.hovered ? Theme.surfaceAlt : "transparent"
                        Row {
                            id: tabRow
                            anchors.centerIn: parent
                            spacing: 6
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: tab.modelData
                                color: tab.open ? Theme.text : Theme.textMuted
                                font.pixelSize: 13
                            }
                            Row {
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 2
                                Repeater {
                                    model: Math.min(tab.picked, 8)
                                    Rectangle { width: 5; height: 5; radius: 2.5; color: Theme.accent }
                                }
                            }
                        }
                        HoverHandler { id: tabHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler {
                            acceptedButtons: Qt.LeftButton
                            onTapped: root.openSite = tab.open ? "" : tab.modelData
                        }
                        TapHandler {
                            acceptedButtons: Qt.RightButton
                            onTapped: if (root.menus) root.menus.popupSite(tab.modelData)
                        }
                        ToolTip {
                            visible: tabHover.hovered
                            delay: 600
                            y: parent.height + 6
                            contentItem: Text {
                                text: qsTr("%1: %n camera(s) selected · right-click for the recorder menu", "", tab.picked)
                                      .arg(tab.modelData)
                                color: Theme.text; font.pixelSize: 11
                            }
                            background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: 4 }
                        }
                    }
                }
            }
        }

        Row {
            anchors.right: parent.right
            anchors.rightMargin: Theme.spacing
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spacing / 2
            Row {
                id: rightRow
                spacing: Theme.spacing / 2
            }
            Rectangle {   // Clear all
                readonly property bool usable: !!root.selection && root.selection.count > 0
                width: clearLabel.implicitWidth + 22
                height: 26
                radius: 13
                color: clearHover.hovered && usable ? Theme.surfaceAlt : Theme.surface
                border.color: Theme.border
                opacity: usable ? 1 : 0.5
                Text {
                    id: clearLabel
                    anchors.centerIn: parent
                    text: qsTr("Clear all")
                    color: Theme.danger
                    font.pixelSize: 12
                }
                HoverHandler { id: clearHover; cursorShape: parent.usable ? Qt.PointingHandCursor : Qt.ArrowCursor }
                TapHandler {
                    onTapped: {
                        if (!parent.usable)
                            return;
                        var n = root.selection.clear();
                        root.close();
                        if (n > 0)
                            root.cleared(n);
                    }
                }
            }
        }
    }

    // ---- The drawer: this Site's cameras as cards, over the page ---------------------
    Rectangle {
        id: drawer
        visible: root.drawerOpen
        z: 1
        y: root.height - 1
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(cardsFlick.contentWidth + 24, root.width - 24)
        height: drawerCol.implicitHeight + 20
        color: Qt.alpha(Theme.surface, 0.94)
        border.color: Theme.border
        radius: 12
        HoverHandler { id: drawerHover }
        // Clicks in the drawer's gaps stay in the drawer rather than reaching the video.
        TapHandler {}

        readonly property var cameras: { root.rev; Sites.revision;
                                         return root.drawerOpen ? Sites.camerasOf(root.openSite) : []; }

        Column {
            id: drawerCol
            x: 12
            y: 10
            width: parent.width - 24
            spacing: 6

            Flickable {
                id: cardsFlick
                width: parent.width
                height: 96
                contentWidth: cardsRow.implicitWidth
                contentHeight: height
                flickableDirection: Flickable.HorizontalFlick
                boundsBehavior: Flickable.StopAtBounds
                clip: true
                ScrollBar.horizontal: ScrollBar { policy: cardsFlick.contentWidth > cardsFlick.width ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff }
                WheelHandler {
                    onWheel: (e) => cardsFlick.contentX = Math.max(0, Math.min(cardsFlick.contentWidth - cardsFlick.width,
                                                                   cardsFlick.contentX - (e.angleDelta.y || e.angleDelta.x)))
                }

                Row {
                    id: cardsRow
                    spacing: 10
                    Repeater {
                        model: drawer.cameras
                        Rectangle {
                            id: card
                            required property var modelData
                            readonly property string key: modelData.key
                            readonly property bool picked: { root.selection ? root.selection.count : 0;
                                                             return !!root.selection && root.selection.contains(key); }
                            readonly property real ratio: root.ratioOf(key)
                            readonly property string frameUrl: { root.frameRev;
                                var p = key.split(":");
                                var u = Devices.lastFrameUrl(parseInt(p[0]), parseInt(p[1]));
                                return u ? u + "?r=" + root.frameRev : ""; }
                            width: 118
                            height: 92
                            radius: 8
                            color: picked ? Qt.alpha(Theme.accent, 0.12) : cardHover.hovered ? Theme.surfaceAlt : "transparent"
                            border.width: 2
                            border.color: picked ? Theme.accent : "transparent"

                            Rectangle {   // the thumbnail box, the picture at its shape inside
                                id: thumb
                                x: 5; y: 5
                                width: 108; height: 61
                                radius: 5
                                color: Theme.paneBackground
                                clip: true
                                Rectangle {
                                    id: shape
                                    anchors.centerIn: parent
                                    width: card.ratio >= thumb.width / thumb.height ? thumb.width : thumb.height * card.ratio
                                    height: card.ratio >= thumb.width / thumb.height ? thumb.width / card.ratio : thumb.height
                                    color: Theme.surfaceAlt
                                    opacity: card.modelData.online ? 1 : 0.4
                                    Image {
                                        anchors.centerIn: parent
                                        visible: card.frameUrl !== ""
                                        source: card.frameUrl
                                        cache: false
                                        asynchronous: true
                                        fillMode: Image.Stretch
                                        sourceSize.width: 216
                                        readonly property int turn: Devices.cameraInfo(card.modelData.row).rotationOverride || 0
                                        rotation: turn
                                        width: turn % 180 !== 0 ? shape.height : shape.width
                                        height: turn % 180 !== 0 ? shape.width : shape.height
                                    }
                                }
                                Rectangle {   // ✓
                                    visible: card.picked
                                    anchors.right: parent.right; anchors.top: parent.top
                                    anchors.margins: 3
                                    width: 16; height: 16; radius: 8
                                    color: Theme.accent
                                    Text { anchors.centerIn: parent; text: "✓"; color: Theme.window; font.pixelSize: 11; font.bold: true }
                                }
                            }
                            Row {
                                anchors.horizontalCenter: parent.horizontalCenter
                                y: thumb.y + thumb.height + 5
                                spacing: 4
                                Rectangle {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 7; height: 7; radius: 3.5
                                    color: card.modelData.online ? Theme.online : Theme.danger
                                }
                                Text {
                                    id: cardName
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: Math.min(implicitWidth, 98)
                                    text: card.modelData.name
                                    elide: Text.ElideRight
                                    color: card.picked ? Theme.accent : Theme.text
                                    font.pixelSize: 11
                                    font.bold: card.picked
                                }
                            }
                            HoverHandler { id: cardHover; cursorShape: Qt.PointingHandCursor }
                            TapHandler {
                                acceptedButtons: Qt.LeftButton
                                onTapped: root.selection.toggle(card.key)
                            }
                            TapHandler {
                                acceptedButtons: Qt.RightButton
                                onTapped: if (root.menus) root.menus.popupCamera(card.modelData.row)
                            }
                            ToolTip {
                                visible: cardHover.hovered
                                delay: 600
                                y: parent.height + 4
                                contentItem: Text {
                                    text: card.modelData.name + " · "
                                          + (card.modelData.online ? qsTr("online") : qsTr("offline"))
                                          + "\n" + (card.picked ? qsTr("Click to remove its pane") : qsTr("Click to add a pane"))
                                    color: Theme.text; font.pixelSize: 11
                                }
                                background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: 4 }
                            }
                        }
                    }
                }
            }

            Text {
                readonly property int picked: { if (!root.selection) return 0;
                                                root.selection.count;
                                                return root.selection.countIn(root.openSite); }
                text: drawer.cameras.length === 0 ? qsTr("No cameras at %1 yet").arg(root.openSite)
                    : qsTr("%1 · %2 of %3 selected").arg(root.openSite).arg(picked).arg(drawer.cameras.length)
                color: Theme.textMuted
                font.pixelSize: 11
            }
        }
    }
}
