import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ReolinkApp
import ReolinkApp.Core

// Events on Site tabs (wayfinder/site-tabs-redesign, step 6): one column per selected camera,
// each listing one local day's events newest first, filtered by the camera's own event type
// row, with an "App not running" band where the app was not running. The day picker sits on
// the strip; with nothing selected the page shows the day's counts by Site. Clicking an event
// opens it in Playback.
Item {
    id: page
    signal jumpToPlayback(var hostId, var channel, var timestamp)
    property var menus: null        // DeviceMenus, for the right-click menus on the Site tabs

    readonly property int columnWidth: 300
    readonly property int columnGap: Theme.spacing

    onVisibleChanged: if (visible) { Events.markAllRead(); page.followToday(); page.countsRev++; }
    Component.onCompleted: {
        selection.saved = String(selStore.value("keys", ""));
        if (visible)
            Events.markAllRead();
    }

    function iconFor(type) {
        switch (type) {
        case "person": return "🚶";
        case "vehicle": return "🚗";
        case "pet": return "🐾";
        case "visitor": return "🔔";
        case "status": return "⚡";
        case "offline": return "⚠";
        case "online": return "✔";
        default: return "👁";
        }
    }
    function labelFor(type) {
        switch (type) {
        case "person": return qsTr("Person");
        case "vehicle": return qsTr("Vehicle");
        case "pet": return qsTr("Pet");
        case "visitor": return qsTr("Visitor");
        case "status": return qsTr("Status");
        case "offline": return qsTr("Went offline");
        case "online": return qsTr("Back online");
        default: return qsTr("Motion");
        }
    }
    function colorFor(type) {
        switch (type) {
        case "person": return Theme.accent;
        case "vehicle": return Theme.warning;
        case "pet": return Theme.online;
        case "visitor": return Theme.danger;
        case "offline": return Theme.danger;
        case "online": return Theme.online;
        default: return Theme.textMuted;
        }
    }
    function hhmm(epoch) { return Qt.formatTime(new Date(epoch * 1000), "HH:mm"); }

    // ---- The day -------------------------------------------------------------------------
    // Local days. The page follows today while it shows today, across midnight.
    property date day: new Date()
    readonly property real dayStart: Math.floor(new Date(day.getFullYear(), day.getMonth(), day.getDate()).getTime() / 1000)
    readonly property bool isToday: sameDay(day, new Date())
    property bool _onToday: true
    function sameDay(a, b) {
        return a.getFullYear() === b.getFullYear() && a.getMonth() === b.getMonth() && a.getDate() === b.getDate();
    }
    function setDay(y, m, d) {   // m: 1-based
        var next = new Date(y, m - 1, d);
        if (next > new Date())
            return;
        day = next;
        _onToday = isToday;
    }
    function stepDay(n) { setDay(day.getFullYear(), day.getMonth() + 1, day.getDate() + n); }
    function followToday() {
        if (_onToday && !isToday)
            day = new Date();
    }
    Timer { interval: 60000; repeat: true; running: page.visible; onTriggered: page.followToday() }

    // ---- The selection -------------------------------------------------------------------
    // Its own, stored in [eventsSelection]; it starts empty.
    SiteSelection {
        id: selection
        sites: Sites.map
        onSaveRequested: (text) => selStore.setValue("keys", text)
        onUndoChanged: if (!canUndo) undoToast.dismiss()
    }
    Settings { id: selStore; category: "eventsSelection" }
    // Keys of a recorder that has been removed are dropped once the device list settles.
    Timer {
        interval: 8000
        running: true
        onTriggered: if (Devices.count > 0) selection.pruneHosts(Devices.hostIds())
    }

    // Each camera's type row is remembered, per camera, in [eventTypes].
    Settings { id: typeStore; category: "eventTypes" }
    function typeKey(key) { return "c" + key.replace(":", "_"); }

    // Device facts change as recorders answer; columns re-read their types then.
    property int devRev: 0
    Connections {
        target: Devices
        function onDataChanged() { page.devRev++; }
        function onModelReset() { page.devRev++; }
        function onRowsInserted() { page.devRev++; }
    }
    function cameraName(key) {
        devRev;
        var p = key.split(":");
        var row = Devices.rowOfHostChannel(Number(p[0]), Number(p[1]));
        return row >= 0 ? Devices.nameAt(row) : key;
    }
    function typesOf(key) {
        devRev;
        var p = key.split(":");
        return Devices.eventTypesFor(Number(p[0]), Number(p[1]));
    }

    // A camera tapped again in the drawer: bring its column into view and flash it.
    property string flashKey: ""
    Timer { id: flashEnd; interval: 1600; onTriggered: page.flashKey = "" }
    function focusColumn(key) {
        var i = selection.paneOrder.indexOf(key);
        if (i < 0)
            return;
        var x = colRow.x + i * (columnWidth + columnGap) - (columns.width - columnWidth) / 2;
        columns.contentX = Math.max(0, Math.min(x, columns.contentWidth - columns.width));
        flashKey = key;
        flashEnd.restart();
    }

    // ---- Site tabs -----------------------------------------------------------------------
    SiteTabs {
        id: tabs
        z: 10
        width: parent.width
        height: implicitHeight
        selection: selection
        menus: page.menus
        tapSelectedToFocus: true
        onSelectedTapped: (key) => page.focusColumn(key)
        onCleared: (n) => undoToast.show(n === 1 ? qsTr("Cleared 1 camera ·") : qsTr("Cleared %1 cameras ·").arg(n))

        leftItems: [
            Rectangle {   // ⋯ the page's other actions
                width: 30; height: 26; radius: 13
                color: moreHover.hovered ? Theme.surfaceAlt : Theme.surface
                border.color: Theme.border
                Text { anchors.centerIn: parent; text: "⋯"; color: Theme.text; font.pixelSize: 14 }
                HoverHandler { id: moreHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: moreMenu.popup() }
                ThemedMenu {
                    id: moreMenu
                    ThemedMenuItem {
                        // The store holds only what the app saw live; this pulls each Frigate
                        // server's own history in.
                        text: qsTr("Sync Frigate history")
                        onTriggered: Events.syncAllFrigateHistory()
                    }
                    ThemedMenuItem {
                        text: Events.desktopNotifications ? qsTr("Desktop notifications: On")
                                                          : qsTr("Desktop notifications: Off")
                        onTriggered: Events.desktopNotifications = !Events.desktopNotifications
                    }
                    ThemedMenuSeparator {}
                    ThemedMenuItem {
                        text: qsTr("Delete all events…")
                        onTriggered: deleteAll.open()
                    }
                }
            }
        ]
        rightItems: [
            Row {   // ‹ day › and a calendar
                spacing: 2
                Rectangle {
                    width: 26; height: 26; radius: 13
                    color: prevHover.hovered ? Theme.surfaceAlt : Theme.surface
                    border.color: Theme.border
                    Text { anchors.centerIn: parent; text: "‹"; color: Theme.text; font.pixelSize: 15 }
                    HoverHandler { id: prevHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: page.stepDay(-1) }
                }
                Rectangle {
                    width: dayLabel.implicitWidth + 22; height: 26; radius: 13
                    color: dayHover.hovered || calPopup.opened ? Theme.surfaceAlt : Theme.surface
                    border.color: Theme.border
                    Text {
                        id: dayLabel
                        anchors.centerIn: parent
                        text: page.isToday ? qsTr("Today") : Qt.formatDate(page.day, "ddd d MMM")
                        color: Theme.text
                        font.pixelSize: 12
                    }
                    HoverHandler { id: dayHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: calPopup.opened ? calPopup.close() : calPopup.open() }
                    Popup {
                        id: calPopup
                        y: parent.height + 6
                        x: parent.width - width
                        padding: Theme.spacing
                        background: Rectangle { color: Theme.surface; border.color: Theme.border; radius: Theme.radius }
                        onAboutToShow: {
                            cal.year = page.day.getFullYear();
                            cal.month = page.day.getMonth() + 1;
                        }
                        MonthCalendar {
                            id: cal
                            width: 240
                            selDay: (year === page.day.getFullYear() && month === page.day.getMonth() + 1)
                                    ? page.day.getDate() : -1
                            onDateSelected: (y, m, d) => { page.setDay(y, m, d); calPopup.close(); }
                        }
                    }
                }
                Rectangle {
                    readonly property bool usable: !page.isToday
                    width: 26; height: 26; radius: 13
                    color: nextHover.hovered && usable ? Theme.surfaceAlt : Theme.surface
                    border.color: Theme.border
                    opacity: usable ? 1 : 0.4
                    Text { anchors.centerIn: parent; text: "›"; color: Theme.text; font.pixelSize: 15 }
                    HoverHandler { id: nextHover; cursorShape: parent.usable ? Qt.PointingHandCursor : Qt.ArrowCursor }
                    TapHandler { onTapped: if (parent.usable) page.stepDay(1) }
                }
            }
        ]
    }

    // ---- Nothing selected: a prompt and the day's counts by Site ---------------------------
    property int countsRev: 0
    Connections {
        target: Events
        enabled: selection.count === 0 && page.visible
        function onStoredChanged() { countsTimer.restart(); }
    }
    Timer { id: countsTimer; interval: 1000; onTriggered: page.countsRev++ }
    readonly property var siteCounts: {
        countsRev; Sites.revision;
        if (selection.count > 0)
            return [];
        var byHost = Events.dayCountsByHost(dayStart);
        return Sites.list.map(site => {
            var c = ({});
            Sites.hostsOf(site).forEach(h => {
                var t = byHost[String(h)] || ({});
                for (var k in t) {
                    var key = (k === "online" || k === "offline") ? "status" : k;
                    c[key] = (c[key] || 0) + t[k];
                }
            });
            return { site: site, counts: c };
        });
    }

    Column {
        anchors.top: tabs.bottom
        anchors.topMargin: Theme.spacing * 4
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(parent.width - Theme.spacing * 4, 900)
        visible: selection.count === 0
        spacing: Theme.spacing * 2

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: qsTr("Pick a camera from a Site tab to see its events")
            color: Theme.text
            font.pixelSize: 14
        }
        Flow {
            width: parent.width
            spacing: Theme.spacing
            Repeater {
                model: page.siteCounts
                Rectangle {
                    id: siteCard
                    required property var modelData
                    width: 210
                    height: siteCol.implicitHeight + 20
                    radius: Theme.radius
                    color: siteHover.hovered ? Theme.surfaceAlt : Theme.surface
                    border.color: Theme.border
                    Column {
                        id: siteCol
                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 6
                        Text {
                            text: siteCard.modelData.site
                            color: Theme.text
                            font.pixelSize: 13
                            font.bold: true
                        }
                        Text {
                            visible: Object.keys(siteCard.modelData.counts).length === 0
                            text: page.isToday ? qsTr("No events yet today") : qsTr("No events this day")
                            color: Theme.textMuted
                            font.pixelSize: 12
                        }
                        Flow {
                            width: parent.width
                            spacing: 8
                            Repeater {
                                model: ["person", "vehicle", "pet", "visitor", "motion", "status"]
                                        .filter(t => siteCard.modelData.counts[t] > 0)
                                Text {
                                    required property string modelData
                                    text: page.iconFor(modelData) + " " + siteCard.modelData.counts[modelData]
                                    color: Theme.text
                                    font.pixelSize: 12
                                    ToolTip.visible: tHover.hovered
                                    ToolTip.text: page.labelFor(modelData)
                                    HoverHandler { id: tHover }
                                }
                            }
                        }
                    }
                    HoverHandler { id: siteHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: tabs.openSite = modelData.site }
                }
            }
        }
    }

    // ---- One column per selected camera, scrolling sideways --------------------------------
    Flickable {
        id: columns
        anchors.fill: parent
        anchors.topMargin: tabs.height + Theme.spacing
        anchors.leftMargin: Theme.spacing
        anchors.rightMargin: Theme.spacing
        anchors.bottomMargin: Theme.spacing
        visible: selection.count > 0
        clip: true
        contentWidth: Math.max(colRow.width, width)
        contentHeight: height
        flickableDirection: Flickable.HorizontalFlick
        boundsBehavior: Flickable.StopAtBounds
        interactive: contentWidth > width
        ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
        // Tapping the columns closes an open drawer, as tapping the video does elsewhere.
        TapHandler { onTapped: tabs.close() }

        Row {
            id: colRow
            x: Math.max(0, (columns.width - width) / 2)
            height: columns.height - 10
            spacing: page.columnGap
            Repeater {
                model: selection.paneOrder
                Rectangle {
                    id: colItem
                    required property string modelData
                    readonly property var hc: modelData.split(":")
                    width: page.columnWidth
                    height: colRow.height
                    radius: Theme.radius
                    color: Theme.surface
                    border.color: page.flashKey === modelData ? Theme.accent : Theme.border
                    border.width: page.flashKey === modelData ? 2 : 1
                    Behavior on border.color { ColorAnimation { duration: 200 } }

                    EventColumn {
                        id: col
                        events: Events
                        cameraKey: colItem.modelData
                        dayStart: page.dayStart
                        supportedTypes: page.typesOf(colItem.modelData)
                        onTypesToggled: (types) => typeStore.setValue(page.typeKey(colItem.modelData), types.join(","))
                        Component.onCompleted: {
                            var v = typeStore.value(page.typeKey(colItem.modelData));
                            if (v !== undefined && v !== null)
                                restoreTypes(String(v) === "" ? [] : String(v).split(","));
                        }
                    }

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 6

                        // Head: name, Site, ✕
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            Text {
                                Layout.fillWidth: true
                                text: page.cameraName(colItem.modelData)
                                color: Theme.text
                                font.pixelSize: 13
                                font.bold: true
                                elide: Text.ElideRight
                            }
                            Text {
                                text: Sites.siteOfKey(colItem.modelData)
                                color: Theme.textMuted
                                font.pixelSize: 11
                            }
                            Rectangle {
                                width: 22; height: 22; radius: 11
                                color: xHover.hovered ? Theme.surfaceAlt : "transparent"
                                Text { anchors.centerIn: parent; text: "✕"; color: Theme.textMuted; font.pixelSize: 11 }
                                HoverHandler { id: xHover; cursorShape: Qt.PointingHandCursor }
                                TapHandler { onTapped: selection.toggle(colItem.modelData) }
                                ToolTip.visible: xHover.hovered
                                ToolTip.delay: 600
                                ToolTip.text: qsTr("Remove this column")
                            }
                        }

                        // The event type row: each type with its count for the day.
                        Flow {
                            Layout.fillWidth: true
                            visible: col.hasTypes
                            spacing: 4
                            Repeater {
                                model: col.supportedTypes
                                Rectangle {
                                    required property string modelData
                                    readonly property bool on: col.enabledTypes.indexOf(modelData) >= 0
                                    width: chip.implicitWidth + 14
                                    height: 24
                                    radius: 12
                                    color: on ? Theme.accentDim : (chipHover.hovered ? Theme.surfaceAlt : "transparent")
                                    border.color: on ? Theme.accent : Theme.border
                                    Text {
                                        id: chip
                                        anchors.centerIn: parent
                                        text: page.iconFor(modelData) + " " + (col.counts[modelData] || 0)
                                        color: parent.on ? Theme.text : Theme.textMuted
                                        font.pixelSize: 11
                                    }
                                    HoverHandler { id: chipHover; cursorShape: Qt.PointingHandCursor }
                                    TapHandler { onTapped: col.toggle(parent.modelData) }
                                    ToolTip.visible: chipHover.hovered
                                    ToolTip.delay: 500
                                    ToolTip.text: page.labelFor(modelData)
                                }
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: !col.hasTypes
                            text: qsTr("This camera reports no events.")
                            color: Theme.textMuted
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: col.keptNote !== ""
                            text: col.keptNote
                            color: Theme.textMuted
                            font.pixelSize: 11
                            font.italic: true
                            wrapMode: Text.WordWrap
                        }

                        ListView {
                            id: list
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            model: col
                            clip: true
                            spacing: 4
                            boundsBehavior: Flickable.StopAtBounds
                            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                            // One row per event, or an "App not running" band.
                            delegate: Rectangle {
                                id: rowItem
                                required property string kind
                                required property string type
                                required property var timestamp
                                required property string thumbnail
                                required property var from
                                required property var to
                                readonly property bool gap: kind === "gap"
                                width: ListView.view.width
                                height: gap ? 26 : 48
                                radius: 4
                                color: gap ? Theme.surfaceAlt : (rowHover.hovered ? Theme.surfaceAlt : "transparent")
                                opacity: gap ? 0.8 : 1

                                Text {
                                    anchors.centerIn: parent
                                    visible: rowItem.gap
                                    text: qsTr("App not running · %1–%2").arg(page.hhmm(rowItem.from)).arg(page.hhmm(rowItem.to))
                                    color: Theme.textMuted
                                    font.pixelSize: 11
                                }

                                Rectangle {   // type colour bar
                                    visible: !rowItem.gap
                                    width: 3; height: parent.height - 8; radius: 1.5
                                    anchors.verticalCenter: parent.verticalCenter
                                    color: page.colorFor(rowItem.type)
                                }
                                RowLayout {
                                    visible: !rowItem.gap
                                    anchors.fill: parent
                                    anchors.leftMargin: 8
                                    anchors.rightMargin: 6
                                    spacing: 8
                                    Rectangle {
                                        Layout.preferredWidth: 64
                                        Layout.preferredHeight: 36
                                        radius: 3
                                        color: Theme.paneBackground
                                        clip: true
                                        Image {
                                            anchors.fill: parent
                                            source: !rowItem.gap && rowItem.thumbnail.length > 0 ? "file://" + rowItem.thumbnail : ""
                                            fillMode: Image.PreserveAspectCrop
                                            asynchronous: true
                                            sourceSize.width: 128
                                            visible: rowItem.thumbnail.length > 0
                                        }
                                        Text {
                                            anchors.centerIn: parent
                                            visible: rowItem.thumbnail.length === 0
                                            text: page.iconFor(rowItem.type)
                                            font.pixelSize: 16
                                            opacity: 0.85
                                        }
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 1
                                        Text {
                                            Layout.fillWidth: true
                                            text: Qt.formatTime(new Date(rowItem.timestamp * 1000), "HH:mm:ss")
                                            color: Theme.text
                                            font.pixelSize: 12
                                        }
                                        Text {
                                            Layout.fillWidth: true
                                            text: page.labelFor(rowItem.type)
                                            color: page.colorFor(rowItem.type)
                                            font.pixelSize: 11
                                        }
                                    }
                                    Text {
                                        text: qsTr("View ›")
                                        color: rowHover.hovered ? Theme.accent : Theme.textMuted
                                        font.pixelSize: 11
                                    }
                                }
                                HoverHandler { id: rowHover; enabled: !rowItem.gap; cursorShape: Qt.PointingHandCursor }
                                TapHandler {
                                    enabled: !rowItem.gap
                                    onTapped: page.jumpToPlayback(Number(colItem.hc[0]), Number(colItem.hc[1]), rowItem.timestamp)
                                }
                            }

                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                y: 24
                                visible: col.hasTypes && col.count === 0
                                text: col.enabledTypes.length === 0 ? qsTr("Every type is off")
                                      : page.isToday ? qsTr("No events yet today") : qsTr("No events this day")
                                color: Theme.textMuted
                                font.pixelSize: 12
                            }
                        }
                    }
                }
            }
        }
    }

    ConfirmDialog {
        id: deleteAll
        message: qsTr("Delete every stored event?")
        detail: qsTr("Every recorder's event history and thumbnails go. Recordings are not affected.")
        confirmLabel: qsTr("Delete")
        onConfirmed: Events.clear()
    }

    UndoToast {
        id: undoToast
        onUndo: selection.undo()
        onExpired: selection.dropUndo()
    }
}
