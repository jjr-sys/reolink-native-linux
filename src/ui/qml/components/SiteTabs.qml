import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ReolinkApp
import ReolinkApp.Core

// The Site tabs strip (wayfinder/site-tabs-redesign, variant B): centred segmented Site
// tabs, each with a dot per selected camera; tapping one drops its camera row, a
// translucent drawer of camera cards, over the page below. Tapping a card selects or
// deselects that camera in `selection`. Right-clicks open the camera and recorder menus.
// The page puts its own controls in `leftItems` and `rightItems` (Clear all is built in).
// With a `budget`, a camera that would take its Site over the budget is greyed and can't
// be added, and the drawer's last line shows the Site's use of its budget.
Item {
    id: root

    property SiteSelection selection
    property SiteBudget budget: null                // greys cards over a Site's budget
    property var menus: null                       // DeviceMenus
    property var ratioOf: (key) => 16 / 9          // a camera's drawn shape, width/height
    property string openSite: ""
    readonly property bool drawerOpen: openSite !== "" && Sites.list.indexOf(openSite) >= 0
    property alias leftItems: leftRow.data
    property alias rightItems: rightRow.data
    readonly property bool hovered: stripHover.hovered || drawerHover.hovered
    signal cleared(int count)
    // ☆ Saved: save the selection under a name, recall one (skipping cameras over the budget).
    property bool showSaved: false
    signal recalled(string name, var skipped)   // skipped: keys left out over the budget

    function close() { openSite = ""; }
    // Show a saved selection: it replaces this page's selection, cameras over a Site's
    // budget left out (and named by the page).
    function recall(entry) {
        if (!selection || !entry)
            return;
        var skipped = budget ? budget.recall(entry.cameras) : [];
        if (!budget)
            selection.replace(entry.cameras);
        close();
        recalled(entry.name, skipped);
    }
    // Why this camera can't be added; "" when it can.
    function blockedWhy(key) {
        if (!budget || budget.canAdd(key))
            return "";
        var site = Sites.siteOfKey(key);
        return qsTr("%1 is at %2 / %3 Mbit/s; this camera needs %4.")
                .arg(site).arg(Sites.mbps(budget.used(site))).arg(Sites.mbps(budget.budgetOf(site)))
                .arg(Sites.mbps(budget.needed(key))) + " " + Sites.overBudgetAdvice;
    }

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
    // The open Site's use of its budget, e.g. "Farm · 2.1 / 5 Mbit/s". Kept up to date from
    // the budget's change signal rather than a binding: the budget's own inputs are still
    // being set while the page starts, which a binding would report as a loop.
    property string usage: ""
    function updateUsage() {
        usage = budget && openSite ? qsTr("%1 · %2 / %3 Mbit/s").arg(openSite)
                                         .arg(Sites.mbps(budget.used(openSite)))
                                         .arg(Sites.mbps(budget.budgetOf(openSite))) : "";
    }
    Connections {
        target: root.budget
        function onChanged() { root.updateUsage(); }
    }
    onBudgetChanged: updateUsage()

    // Last-seen frames are re-read each time a drawer opens.
    property int frameRev: 0
    onOpenSiteChanged: { frameRev++; updateUsage(); }

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
                                text: (tab.picked === 1 ? qsTr("%1: 1 camera selected · right-click for the recorder menu")
                                       : qsTr("%1: %2 cameras selected · right-click for the recorder menu").arg(tab.picked))
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
            Rectangle {   // ☆ Saved
                id: savedButton
                visible: root.showSaved
                width: savedLabel.implicitWidth + 22
                height: 26
                radius: 13
                color: savedHover.hovered || savedPopup.visible ? Theme.surfaceAlt : Theme.surface
                border.color: Theme.border
                Text {
                    id: savedLabel
                    anchors.centerIn: parent
                    text: qsTr("☆ Saved")
                    color: Theme.text
                    font.pixelSize: 12
                }
                HoverHandler { id: savedHover; cursorShape: Qt.PointingHandCursor }
                TapHandler {
                    onTapped: savedPopup.visible ? savedPopup.close() : savedPopup.open()
                }
                Popup {
                    id: savedPopup
                    x: savedButton.width - width
                    y: savedButton.height + 6
                    width: 260
                    padding: 6
                    background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: Theme.radius }
                    contentItem: Column {
                        spacing: 2
                        Repeater {
                            model: Saved.list
                            Rectangle {
                                id: savedRow
                                required property var modelData
                                width: 248
                                height: 32
                                radius: 4
                                color: rowHover.hovered ? Theme.accentDim : "transparent"
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 8
                                    anchors.rightMargin: 8
                                    Text {
                                        Layout.fillWidth: true
                                        text: savedRow.modelData.name
                                        color: Theme.text; font.pixelSize: 13
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        text: savedRow.modelData.cameras.length === 1 ? qsTr("1 camera")
                                              : qsTr("%1 cameras").arg(savedRow.modelData.cameras.length)
                                        color: Theme.textMuted; font.pixelSize: 11
                                    }
                                }
                                HoverHandler { id: rowHover; cursorShape: Qt.PointingHandCursor }
                                TapHandler {
                                    acceptedButtons: Qt.LeftButton
                                    onTapped: {
                                        savedPopup.close();
                                        root.recall(savedRow.modelData);
                                    }
                                }
                                TapHandler {
                                    acceptedButtons: Qt.RightButton
                                    onTapped: {
                                        savedRowMenu.entry = savedRow.modelData;
                                        savedRowMenu.popup();
                                    }
                                }
                                ToolTip {
                                    visible: rowHover.hovered
                                    delay: 800
                                    x: parent.width + 6
                                    contentItem: Text {
                                        text: qsTr("Click to show these cameras · right-click to rename or delete")
                                        color: Theme.text; font.pixelSize: 11
                                    }
                                    background: Rectangle { color: Theme.surfaceAlt; border.color: Theme.border; radius: 4 }
                                }
                            }
                        }
                        Text {
                            visible: Saved.list.length === 0
                            leftPadding: 8
                            topPadding: 4
                            bottomPadding: 4
                            text: qsTr("No saved selections yet")
                            color: Theme.textMuted; font.pixelSize: 12
                        }
                        Rectangle { width: 248; height: 1; color: Theme.border }
                        Rectangle {
                            readonly property bool usable: !!root.selection && root.selection.count > 0
                            width: 248
                            height: 32
                            radius: 4
                            opacity: usable ? 1 : 0.5
                            color: addHover.hovered && usable ? Theme.accentDim : "transparent"
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                x: 8
                                text: qsTr("＋ Save current selection…")
                                color: Theme.text; font.pixelSize: 13
                            }
                            HoverHandler { id: addHover; cursorShape: parent.usable ? Qt.PointingHandCursor : Qt.ArrowCursor }
                            TapHandler {
                                onTapped: {
                                    if (!parent.usable)
                                        return;
                                    savedPopup.close();
                                    nameDialog.openFor("", Saved.model.nextName(qsTr("Selection")));
                                }
                            }
                        }
                    }
                }
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
                            readonly property string blocked: { root.budget ? root.budget.revision : 0;
                                                                 return picked ? "" : root.blockedWhy(key); }
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
                            opacity: blocked ? 0.35 : 1

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
                            HoverHandler {
                                id: cardHover
                                cursorShape: card.blocked ? Qt.ForbiddenCursor : Qt.PointingHandCursor
                            }
                            TapHandler {
                                acceptedButtons: Qt.LeftButton
                                onTapped: if (!card.blocked) root.selection.toggle(card.key)
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
                                    text: (card.modelData.online ? qsTr("%1 · online") : qsTr("%1 · offline"))
                                              .arg(card.modelData.name) + "\n"
                                          + (card.blocked ? card.blocked
                                             : card.picked ? qsTr("Click to remove its pane") : qsTr("Click to add a pane"))
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
                    : root.budget ? root.usage
                    : qsTr("%1 · %2 of %3 selected").arg(root.openSite).arg(picked).arg(drawer.cameras.length)
                color: Theme.textMuted
                font.pixelSize: 11
            }
        }
    }

    // ---- Saved selections: the row menu and the name dialog ----------------------------
    ThemedMenu {
        id: savedRowMenu
        property var entry: null
        ThemedMenuItem {
            text: qsTr("Rename…")
            onTriggered: nameDialog.openFor(savedRowMenu.entry.id, savedRowMenu.entry.name)
        }
        ThemedMenuSeparator {}
        ThemedMenuItem {
            text: qsTr("Delete…")
            onTriggered: {
                deleteSaved.message = qsTr("Delete the saved selection \"%1\"?").arg(savedRowMenu.entry.name);
                deleteSaved.payload = savedRowMenu.entry.id;
                deleteSaved.open();
            }
        }
    }
    ConfirmDialog {
        id: deleteSaved
        confirmLabel: qsTr("Delete")
        detail: qsTr("Your cameras and what is on screen are not affected.")
        onConfirmed: (id) => Saved.model.remove(id)
    }
    // Names a new saved selection (`entryId` "") or renames one.
    Dialog {
        id: nameDialog
        property string entryId: ""
        function openFor(id, name) {
            entryId = id;
            nameField.text = name;
            open();
            nameField.selectAll();
            nameField.forceActiveFocus();
        }
        modal: true
        width: 360
        parent: Overlay.overlay
        anchors.centerIn: parent
        title: entryId ? qsTr("Rename saved selection") : qsTr("Save current selection")
        background: Rectangle { color: Theme.surface; border.color: Theme.border; radius: Theme.radius }
        header: Item {
            implicitHeight: nameTitle.implicitHeight + 22
            Text {
                id: nameTitle
                x: Theme.spacing; width: parent.width - Theme.spacing * 2
                anchors.verticalCenter: parent.verticalCenter
                text: nameDialog.title; color: Theme.text
                font.pixelSize: 16; font.bold: true; elide: Text.ElideRight
            }
        }
        ColumnLayout {
            anchors.fill: parent
            spacing: Theme.spacing
            TextField {
                id: nameField
                Layout.fillWidth: true
                color: Theme.text
                selectByMouse: true
                background: Rectangle {
                    color: Theme.surfaceAlt
                    border.color: nameField.activeFocus ? Theme.accent : Theme.border
                    radius: 4
                }
                onAccepted: if (text.trim() !== "") nameDialog.accept()
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Rectangle {
                    implicitWidth: ndCancel.implicitWidth + 24; implicitHeight: 32; radius: Theme.radius
                    color: ndCancelHover.hovered ? Theme.surfaceAlt : "transparent"
                    border.color: Theme.border
                    Text { id: ndCancel; anchors.centerIn: parent; text: qsTr("Cancel"); color: Theme.text; font.pixelSize: 12 }
                    HoverHandler { id: ndCancelHover }
                    TapHandler { onTapped: nameDialog.reject() }
                }
                Rectangle {
                    implicitWidth: ndOk.implicitWidth + 24; implicitHeight: 32; radius: Theme.radius
                    readonly property bool usable: nameField.text.trim() !== ""
                    opacity: usable ? 1 : 0.5
                    color: ndOkHover.hovered && usable ? Theme.accent : Theme.accentDim
                    border.color: Theme.accent
                    Text { id: ndOk; anchors.centerIn: parent; text: nameDialog.entryId ? qsTr("Rename") : qsTr("Save")
                           color: Theme.text; font.pixelSize: 12 }
                    HoverHandler { id: ndOkHover }
                    TapHandler { onTapped: if (parent.usable) nameDialog.accept() }
                }
            }
        }
        onAccepted: {
            var name = nameField.text.trim();
            if (!name)
                return;
            if (entryId)
                Saved.model.rename(entryId, name);
            else
                Saved.model.add(name, root.selection.keys);
        }
    }
}
