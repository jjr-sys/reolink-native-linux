import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ReolinkApp
import ReolinkApp.Core

// Events (wayfinder/events-feed step 18, layout P2): Site tabs on top; on the left the
// Site's cameras, the most recently active first; in the middle one camera's feed over
// everything kept (30 days), newest first, a full-width snapshot under each caption line,
// sticky day headers; on the right the player panel (the clip player comes in step 22).
// The camera's type chips filter its feed and are remembered per camera.
Item {
    id: page
    signal jumpToPlayback(var hostId, var channel, var timestamp)
    property var menus: null        // DeviceMenus (unused here since the Site drawer went)

    readonly property int listWidth: 260
    readonly property int feedWidth: 560
    readonly property bool showPlayer: width >= 1150

    onVisibleChanged: if (visible) { Events.markAllRead(); page.now = new Date(); }
    Component.onCompleted: if (visible) Events.markAllRead()

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

    // The clock, ticked each minute: `new Date()` alone is not watched by bindings.
    property date now: new Date()
    Timer { interval: 60000; repeat: true; running: page.visible; onTriggered: page.now = new Date() }
    function ago(epoch) {
        if (epoch <= 0)
            return qsTr("nothing in 30 days");
        const s = Math.max(0, Math.floor(page.now.getTime() / 1000) - epoch);
        if (s < 60) return qsTr("last just now");
        if (s < 3600) return qsTr("last %1 min ago").arg(Math.floor(s / 60));
        if (s < 86400) return qsTr("last %1 h ago").arg(Math.floor(s / 3600));
        return qsTr("last %1 d ago").arg(Math.floor(s / 86400));
    }
    // A sticky day header: Today, Yesterday, or "Tue 10 Nov".
    function dayTitle(isoDay) {
        const p = isoDay.split("-");
        if (p.length !== 3)
            return isoDay;
        const d = new Date(Number(p[0]), Number(p[1]) - 1, Number(p[2]));
        const today = new Date(page.now.getFullYear(), page.now.getMonth(), page.now.getDate());
        const days = Math.round((today - d) / 86400000);
        if (days === 0) return qsTr("Today");
        if (days === 1) return qsTr("Yesterday");
        return Qt.formatDate(d, "ddd d MMM");
    }

    // ---- What is shown: a Site and one of its cameras, remembered ------------------------
    Settings {
        id: store
        category: "eventsFeed"
        property string site: ""
        property string cameras: "{}"    // JSON: site -> the camera last opened there
    }
    property string site: Sites.list.indexOf(store.site) >= 0 ? store.site
                          : (Sites.list.length > 0 ? Sites.list[0] : "")
    property string camera: ""
    function pickSite(s) {
        store.site = s;
        page.camera = "";
        page.chooseCamera();
    }
    function pickCamera(key) {
        page.camera = key;
        var m = {};
        try { m = JSON.parse(store.cameras); } catch (e) { m = {}; }
        m[page.site] = key;
        store.cameras = JSON.stringify(m);
    }
    // The remembered camera of this Site if it is still listed, else the most active one.
    function chooseCamera() {
        if (page.camera !== "" && cameraList.has(page.camera))
            return;
        var m = {};
        try { m = JSON.parse(store.cameras); } catch (e) { m = {}; }
        const remembered = m[page.site] || "";
        page.camera = cameraList.has(remembered) ? remembered : cameraList.first();
    }

    // Device facts change as recorders answer; the camera list re-reads them then.
    property int devRev: 0
    Connections {
        target: Devices
        function onDataChanged() { devRefresh.restart(); }
        function onModelReset() { devRefresh.restart(); }
        function onRowsInserted() { devRefresh.restart(); }
    }
    Timer { id: devRefresh; interval: 300; onTriggered: page.devRev++ }

    // The Site's cameras, for the list: { key, name, types }.
    readonly property var siteCameras: {
        devRev; Sites.revision;
        var out = [];
        Sites.hostsOf(page.site).forEach(h => {
            Devices.channelsForHost(h).forEach(c => {
                const row = Devices.rowOfHostChannel(h, c.channel);
                if (row < 0)
                    return;
                out.push({ key: h + ":" + c.channel, name: Devices.nameAt(row),
                           types: Devices.eventTypesFor(h, c.channel) });
            });
        });
        return out;
    }
    EventCameraList {
        id: cameraList
        events: Events
        cameras: page.siteCameras
        onRefreshed: page.chooseCamera()
        function has(key) {
            for (var i = 0; i < page.siteCameras.length; ++i)
                if (page.siteCameras[i].key === key)
                    return true;
            return false;
        }
    }

    readonly property var hc: camera.split(":")
    readonly property string cameraName: {
        devRev;
        const row = camera === "" ? -1 : Devices.rowOfHostChannel(Number(hc[0]), Number(hc[1]));
        return row >= 0 ? Devices.nameAt(row) : "";
    }
    // The camera's type chips: what it reports, less what is switched off (Status always).
    readonly property var cameraTypes: {
        devRev; switchRev;
        if (camera === "")
            return [];
        return Devices.eventTypesFor(Number(hc[0]), Number(hc[1]))
            .filter(t => t === "status" || !Events.isSwitchedOff(Number(hc[0]), Number(hc[1]), t));
    }
    property int switchRev: 0
    Connections {
        target: Events
        function onSwitchesChanged() { page.switchRev++; }
    }

    // Each camera's chosen chips, remembered per camera, in [eventTypes].
    Settings { id: typeStore; category: "eventTypes" }
    function typeKey(key) { return "c" + key.replace(":", "_"); }

    EventFeed {
        id: feed
        events: Events
        cameraKey: page.camera
        supportedTypes: page.cameraTypes
        onTypesToggled: (types) => typeStore.setValue(page.typeKey(page.camera), types.join(","))
        onCameraKeyChanged: {
            const v = page.camera === "" ? undefined : typeStore.value(page.typeKey(page.camera));
            if (v !== undefined && v !== null)
                restoreTypes(String(v) === "" ? [] : String(v).split(","));
            else
                useDefaultTypes();
            feedView.positionViewAtBeginning();
        }
    }
    Timer { interval: 60000; repeat: true; running: page.visible; onTriggered: feed.refresh() }

    // ---- Top bar: ⋯, the Site tabs, the camera's type chips -------------------------------
    Rectangle {
        id: bar
        width: parent.width
        height: 44
        color: Theme.window
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }

        Rectangle {   // ⋯ the page's other actions
            id: moreButton
            anchors.left: parent.left
            anchors.leftMargin: Theme.spacing
            anchors.verticalCenter: parent.verticalCenter
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

        Row {   // the Site tabs: a tap picks the Site
            anchors.centerIn: parent
            spacing: 2
            Repeater {
                model: Sites.list
                Rectangle {
                    required property string modelData
                    readonly property bool current: modelData === page.site
                    width: siteText.implicitWidth + 28
                    height: 28
                    radius: 14
                    color: current ? Theme.accentDim : (siteHover.hovered ? Theme.surfaceAlt : Theme.surface)
                    border.color: current ? Theme.accent : Theme.border
                    Text {
                        id: siteText
                        anchors.centerIn: parent
                        text: parent.modelData
                        color: parent.current ? Theme.text : Theme.textMuted
                        font.pixelSize: 13
                    }
                    HoverHandler { id: siteHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: page.pickSite(parent.modelData) }
                }
            }
        }
    }

    // ---- Left: the Site's cameras ----------------------------------------------------------
    Rectangle {
        id: listPane
        anchors.top: bar.bottom
        anchors.bottom: parent.bottom
        width: page.listWidth
        color: Theme.surface
        Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.border }

        ListView {
            id: cameraView
            anchors.fill: parent
            anchors.margins: 6
            model: cameraList
            clip: true
            spacing: 2
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            delegate: Rectangle {
                id: camRow
                required property string key
                required property string name
                required property var last
                required property bool statusOnly
                readonly property bool current: key === page.camera
                width: ListView.view.width
                height: 48
                radius: Theme.radius
                color: current ? Theme.accentDim : (camHover.hovered ? Theme.surfaceAlt : "transparent")
                border.color: current ? Theme.accent : "transparent"
                ColumnLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: 1
                    Item { Layout.fillHeight: true }
                    Text {
                        Layout.fillWidth: true
                        text: camRow.name
                        color: camRow.statusOnly && !camRow.current ? Theme.textMuted : Theme.text
                        font.pixelSize: 13
                        font.bold: camRow.current
                        elide: Text.ElideRight
                    }
                    Text {
                        Layout.fillWidth: true
                        text: camRow.statusOnly ? qsTr("Status only") : page.ago(Number(camRow.last))
                        color: Theme.textMuted
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }
                    Item { Layout.fillHeight: true }
                }
                HoverHandler { id: camHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: page.pickCamera(camRow.key) }
            }
            Text {
                anchors.centerIn: parent
                visible: cameraView.count === 0
                text: Sites.list.length === 0 ? qsTr("No Sites yet") : qsTr("No cameras on this Site")
                color: Theme.textMuted
                font.pixelSize: 12
            }
        }
    }

    // ---- Middle: the camera's feed ---------------------------------------------------------
    Item {
        id: feedPane
        anchors.top: bar.bottom
        anchors.bottom: parent.bottom
        anchors.left: listPane.right
        width: page.feedWidth + Theme.spacing * 4

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: Theme.spacing * 2
            anchors.topMargin: Theme.spacing
            spacing: 6

            // The camera's name and its type chips (counts over 30 days).
            RowLayout {
                Layout.fillWidth: true
                visible: page.camera !== ""
                spacing: 8
                Text {
                    text: page.cameraName
                    color: Theme.text
                    font.pixelSize: 15
                    font.bold: true
                    elide: Text.ElideRight
                    Layout.maximumWidth: 200
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 4
                    Repeater {
                        model: feed.supportedTypes
                        Rectangle {
                            required property string modelData
                            readonly property bool on: feed.enabledTypes.indexOf(modelData) >= 0
                            width: chip.implicitWidth + 16
                            height: 26
                            radius: 13
                            color: on ? Theme.accentDim : (chipHover.hovered ? Theme.surfaceAlt : "transparent")
                            border.color: on ? Theme.accent : Theme.border
                            Text {
                                id: chip
                                anchors.centerIn: parent
                                text: page.iconFor(parent.modelData) + " " + page.labelFor(parent.modelData)
                                      + " " + (feed.counts[parent.modelData] || 0)
                                color: parent.on ? Theme.text : Theme.textMuted
                                font.pixelSize: 12
                            }
                            HoverHandler { id: chipHover; cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: feed.toggle(parent.modelData) }
                        }
                    }
                }
            }

            ListView {
                id: feedView
                Layout.fillWidth: true
                Layout.fillHeight: true
                model: feed
                clip: true
                spacing: 10
                reuseItems: true
                cacheBuffer: 1600
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                section.property: "day"
                section.criteria: ViewSection.FullString
                section.labelPositioning: ViewSection.InlineLabels | ViewSection.CurrentLabelAtStart
                section.delegate: Rectangle {
                    required property string section
                    width: ListView.view.width
                    height: 30
                    color: Theme.window
                    z: 2
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: page.dayTitle(parent.section)
                        color: Theme.text
                        font.pixelSize: 13
                        font.bold: true
                    }
                }

                delegate: Item {
                    id: ev
                    required property string kind
                    required property string type
                    required property var timestamp
                    required property string thumbnail
                    required property var from
                    required property var to
                    readonly property bool gap: kind === "gap"
                    readonly property bool status: type === "online" || type === "offline"
                    readonly property bool slim: gap || status
                    width: ListView.view.width
                    height: slim ? 28 : caption.height + 6 + snap.height

                    // "App not running" band, or a Status line.
                    Rectangle {
                        anchors.fill: parent
                        visible: ev.slim
                        radius: 4
                        color: Theme.surfaceAlt
                        opacity: 0.85
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: 10
                            text: ev.gap
                                  ? qsTr("App not running · %1–%2").arg(page.hhmm(ev.from)).arg(page.hhmm(ev.to))
                                  : page.iconFor(ev.type) + " " + page.labelFor(ev.type) + " · "
                                    + Qt.formatTime(new Date(ev.timestamp * 1000), "HH:mm:ss")
                            color: ev.status && ev.type === "offline" ? Theme.danger : Theme.textMuted
                            font.pixelSize: 12
                        }
                    }

                    // The caption: type pill, date and time, ⋯.
                    RowLayout {
                        id: caption
                        visible: !ev.slim
                        width: parent.width
                        height: 28
                        spacing: 8
                        Rectangle {
                            width: pill.implicitWidth + 16
                            height: 22
                            radius: 11
                            color: "transparent"
                            border.color: page.colorFor(ev.type)
                            Text {
                                id: pill
                                anchors.centerIn: parent
                                text: page.iconFor(ev.type) + " " + page.labelFor(ev.type)
                                color: page.colorFor(ev.type)
                                font.pixelSize: 12
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: Qt.formatDateTime(new Date(ev.timestamp * 1000), "ddd d MMM · HH:mm:ss")
                            color: Theme.text
                            font.pixelSize: 12
                        }
                        Rectangle {
                            width: 28; height: 24; radius: 12
                            color: dotsHover.hovered ? Theme.surfaceAlt : "transparent"
                            Text { anchors.centerIn: parent; text: "⋯"; color: Theme.text; font.pixelSize: 14 }
                            HoverHandler { id: dotsHover; cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: eventMenu.popup() }
                            ThemedMenu {
                                id: eventMenu
                                ThemedMenuItem {
                                    text: qsTr("Open in Playback")
                                    onTriggered: page.jumpToPlayback(Number(page.hc[0]), Number(page.hc[1]), ev.timestamp)
                                }
                                ThemedMenuItem {
                                    text: qsTr("Save snapshot")
                                    enabled: ev.thumbnail.length > 0
                                    onTriggered: {
                                        const saved = Events.saveSnapshot(ev.thumbnail, page.cameraName, ev.timestamp);
                                        toast.show(saved !== "" ? qsTr("Saved to %1").arg(saved)
                                                                : qsTr("Couldn't save the snapshot"));
                                    }
                                }
                            }
                        }
                    }

                    // The snapshot at full width (16:9), tap to open in Playback.
                    Rectangle {
                        id: snap
                        visible: !ev.slim
                        anchors.top: caption.bottom
                        anchors.topMargin: 6
                        width: parent.width
                        height: Math.round(width * 9 / 16)
                        radius: 4
                        color: Theme.paneBackground
                        clip: true
                        Image {
                            anchors.fill: parent
                            source: !ev.slim && ev.thumbnail.length > 0 ? "file://" + ev.thumbnail : ""
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            sourceSize.width: page.feedWidth
                            visible: ev.thumbnail.length > 0
                        }
                        Text {
                            anchors.centerIn: parent
                            visible: ev.thumbnail.length === 0
                            text: page.iconFor(ev.type)
                            font.pixelSize: 40
                            opacity: 0.6
                        }
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        TapHandler {
                            enabled: !ev.slim
                            onTapped: page.jumpToPlayback(Number(page.hc[0]), Number(page.hc[1]), ev.timestamp)
                        }
                    }
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: 40
                    visible: feedView.count === 0
                    text: page.camera === "" ? qsTr("Pick a camera")
                          : !feed.hasTypes ? qsTr("This camera reports no events.")
                          : feed.enabledTypes.length === 0 ? qsTr("Every type is off")
                          : qsTr("Nothing in the last 30 days")
                    color: Theme.textMuted
                    font.pixelSize: 13
                }
            }
        }
    }

    // ---- Right: the player panel (the clip player comes in step 22) -----------------------
    Rectangle {
        anchors.top: bar.bottom
        anchors.bottom: parent.bottom
        anchors.left: feedPane.right
        anchors.right: parent.right
        visible: page.showPlayer
        color: Theme.surface
        Rectangle { anchors.left: parent.left; width: 1; height: parent.height; color: Theme.border }
        Text {
            anchors.centerIn: parent
            text: qsTr("Pick an event…")
            color: Theme.textMuted
            font.pixelSize: 13
        }
    }

    // A short message at the bottom (a saved snapshot).
    Rectangle {
        id: toast
        function show(text) { toastText.text = text; opacity = 1; toastTimer.restart(); }
        anchors.horizontalCenter: feedPane.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.spacing * 3
        width: toastText.implicitWidth + 28
        height: 34
        radius: 17
        color: Theme.surfaceAlt
        border.color: Theme.border
        opacity: 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 200 } }
        Text { id: toastText; anchors.centerIn: parent; color: Theme.text; font.pixelSize: 12 }
        Timer { id: toastTimer; interval: 3000; onTriggered: toast.opacity = 0 }
    }

    // TEST HOOK: RL_EVENTS_AUTOSCROLL (see main.cpp).
    Timer {
        id: autoScroll
        property real last: 0
        property real worst: 0
        property real started: 0
        property int over33: 0
        property int over100: 0
        interval: 16
        repeat: true
        running: typeof eventsAutoScroll !== "undefined" && eventsAutoScroll && page.visible
                 && feedView.count > 0
        onTriggered: {
            const t = Date.now();
            if (started === 0) started = t;
            if (last > 0) {
                const gap = t - last;
                worst = Math.max(worst, gap);
                if (gap > 33) over33++;
                if (gap > 100) {
                    over100++;
                    console.log("autoscroll: slow step", gap, "ms at", ((t - started) / 1000).toFixed(1), "s");
                }
            }
            last = t;
            if (feedView.atYEnd && feedView.contentY > 0) {
                console.log("autoscroll: rows", feedView.count, "height", Math.round(feedView.contentHeight),
                            "seconds", ((t - started) / 1000).toFixed(1), "worst step ms", worst,
                            "steps over 33 ms", over33, "over 100 ms", over100);
                Qt.quit();
                return;
            }
            feedView.contentY += 60;
        }
    }

    ConfirmDialog {
        id: deleteAll
        message: qsTr("Delete every stored event?")
        detail: qsTr("Every recorder's event history and thumbnails go. Recordings are not affected.")
        confirmLabel: qsTr("Delete")
        onConfirmed: Events.clear()
    }
}
