import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ReolinkApp
import ReolinkApp.Core

// Events (wayfinder/events-feed step 18, layout P2): Site tabs on top; on the left the
// Site's cameras, the most recently active first; in the middle one camera's feed over
// everything kept (30 days), newest first, a full-width snapshot under each caption line,
// sticky day headers; on the right the clip player (step 22).
// The camera's type chips filter its feed and are remembered per camera. Step 20 adds the
// unseen counts: bubbles per camera and Site, blue dots, "Seen up to here", a date-bar band.
Item {
    id: page
    signal jumpToPlayback(var hostId, var channel, var timestamp)
    property var menus: null        // DeviceMenus: right-click on a Site tab or a camera

    readonly property int listWidth: 260
    readonly property int feedWidth: 560
    readonly property int railWidth: 44     // the date bar beside the feed
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
    property bool feedTouched: false   // scrolled since the camera was shown
    // The camera was chosen for the person (nothing remembered): it follows the most active
    // camera as recorders answer, until they pick one.
    property bool autoChosen: true
    function pickSite(s) {
        if (s === page.site)
            return;
        page.camera = "";   // first, so the list refreshing for the new Site picks once
        store.site = s;
        page.chooseCamera();
    }
    function pickCamera(key) {
        page.autoChosen = false;
        page.camera = key;
        var m = {};
        try { m = JSON.parse(store.cameras); } catch (e) { m = {}; }
        m[page.site] = key;
        store.cameras = JSON.stringify(m);
    }
    // The remembered camera of this Site if it is listed, else the most active one.
    function chooseCamera() {
        if (page.camera !== "" && !page.autoChosen && cameraList.contains(page.camera))
            return;
        var m = {};
        try { m = JSON.parse(store.cameras); } catch (e) { m = {}; }
        const remembered = m[page.site] || "";
        // On screen the shown camera stays put (unless the remembered one has just been
        // listed): following the most active camera there would open, and mark seen, each
        // camera as it logs, and its bubbles would never show.
        if (page.camera !== "" && page.onScreen && cameraList.contains(page.camera)
                && !cameraList.contains(remembered))
            return;
        page.autoChosen = !cameraList.contains(remembered);
        const next = page.autoChosen ? cameraList.first() : remembered;
        if (next !== page.camera)
            page.camera = next;
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
    }

    readonly property var hc: page.camera.split(":")
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

    // Switch controls (step 21): noise cards and the types switched off on this camera.
    CameraNoise {
        id: noise
        events: Events
        cameraKey: page.camera
    }
    function number(n) { return Number(n).toLocaleString(Qt.locale(), "f", 0); }
    function size(bytes) {
        if (bytes >= 1e9) return qsTr("%1 GB").arg((bytes / 1e9).toFixed(1));
        if (bytes >= 1e6) return qsTr("%1 MB").arg(Math.round(bytes / 1e6));
        return qsTr("%1 KB").arg(Math.max(1, Math.round(bytes / 1e3)));
    }
    function every(secs) {
        return secs < 120 ? qsTr("%1 s").arg(secs) : qsTr("%1 min").arg(Math.round(secs / 60));
    }
    // Switch off: confirm with the daily rate and what it deletes, then switch.
    function askSwitchOff(type) {
        if (page.camera === "")
            return;
        const st = Events.switchStats(Number(page.hc[0]), Number(page.hc[1]), type);
        const rate = noise.perDay(type);
        switchOffDialog.payload = { key: page.camera, name: page.cameraName, type: type };
        switchOffDialog.message =
            qsTr("Switch off %1 on %2?").arg(page.labelFor(type)).arg(page.cameraName)
            + (rate >= 0 ? " " + qsTr("It logs about %1 a day.").arg(page.number(rate)) : "")
            + " " + qsTr("The app stops logging, notifying and highlighting it here.");
        switchOffDialog.detail =
            qsTr("Deletes %1 stored events (%2). This can't be undone. The recorder keeps recording.")
                .arg(page.number(st.count || 0)).arg(page.size(st.bytes || 0));
        switchOffDialog.open();
    }
    function turnOn(type) {
        if (page.camera !== "" && Events.switchOn(Number(page.hc[0]), Number(page.hc[1]), type))
            toast.show(qsTr("%1 logging again on %2").arg(page.labelFor(type)).arg(page.cameraName));
    }

    // Unseen counts (step 20): the camera on screen is the one opened; it stays seen while
    // it is open, and its feed marks what was new when it was opened.
    readonly property bool onScreen: page.visible && Window.visibility !== Window.Hidden
                                     && Window.visibility !== Window.Minimized
    Binding { target: Unseen; property: "openCamera"; value: page.onScreen ? page.camera : "" }

    EventFeed {
        id: feed
        events: Events
        seenLine: Unseen.openCamera === page.camera ? Unseen.seenLine : -1
        cameraKey: page.camera
        supportedTypes: page.cameraTypes
        onTypesToggled: (types) => typeStore.setValue(page.typeKey(page.camera), types.join(","))
        onCameraKeyChanged: {
            const v = page.camera === "" ? undefined : typeStore.value(page.typeKey(page.camera));
            if (v !== undefined && v !== null)
                restoreTypes(String(v) === "" ? [] : String(v).split(","));
            else
                useDefaultTypes();
            page.feedTouched = false;
            feedView.positionViewAtBeginning();
        }
    }
    Timer { interval: 60000; repeat: true; running: page.visible; onTriggered: feed.refresh() }

    // ---- Top bar: ⋯ and the Site tabs (the camera's type chips sit above its feed) --------
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
                    id: siteTab
                    required property string modelData
                    readonly property bool current: modelData === page.site
                    // The Site's unseen total (step 20), in a grey bubble after its name.
                    readonly property int unseen: {
                        Unseen.revision; Sites.revision;
                        return Unseen.totalForHosts(Sites.hostsOf(modelData));
                    }
                    width: siteLabel.implicitWidth + 28
                    height: 28
                    radius: 14
                    color: current ? Theme.accentDim : (siteHover.hovered ? Theme.surfaceAlt : Theme.surface)
                    border.color: current ? Theme.accent : Theme.border
                    Row {
                        id: siteLabel
                        anchors.centerIn: parent
                        spacing: 6
                        Text {
                            id: siteText
                            anchors.verticalCenter: parent.verticalCenter
                            text: siteTab.modelData
                            color: siteTab.current ? Theme.text : Theme.textMuted
                            font.pixelSize: 13
                        }
                        Rectangle {
                            visible: siteTab.unseen > 0
                            anchors.verticalCenter: parent.verticalCenter
                            width: Math.max(18, siteCount.implicitWidth + 10)
                            height: 18
                            radius: 9
                            color: Theme.textMuted
                            Text {
                                id: siteCount
                                anchors.centerIn: parent
                                text: siteTab.unseen > 99 ? "99+" : siteTab.unseen
                                color: Theme.surface
                                font.pixelSize: 11
                                font.bold: true
                            }
                        }
                    }
                    HoverHandler { id: siteHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: page.pickSite(siteTab.modelData) }
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: if (page.menus) page.menus.popupSite(siteTab.modelData)
                    }
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
                // { type: n } of its unseen events (step 20), in bubble order.
                readonly property var unseen: { Unseen.revision; return Unseen.countsFor(key); }
                readonly property var unseenTypes: Unseen.countedTypes().filter(t => (unseen[t] || 0) > 0)
                width: ListView.view.width
                height: 48
                radius: Theme.radius
                color: current ? Theme.accentDim : (camHover.hovered ? Theme.surfaceAlt : "transparent")
                border.color: current ? Theme.accent : "transparent"
                ColumnLayout {
                    anchors.left: parent.left
                    anchors.right: bubbles.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.leftMargin: 10
                    anchors.rightMargin: 6
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
                // Coloured bubbles of unseen events per type (99+), or ✓ when nothing is new.
                Row {
                    id: bubbles
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 3
                    Repeater {
                        model: camRow.unseenTypes
                        Rectangle {
                            required property string modelData
                            readonly property int n: camRow.unseen[modelData] || 0
                            width: Math.max(20, bubbleText.implicitWidth + 10)
                            height: 20
                            radius: 10
                            color: page.colorFor(modelData)
                            Text {
                                id: bubbleText
                                anchors.centerIn: parent
                                text: parent.n > 99 ? "99+" : parent.n
                                color: "white"
                                font.pixelSize: 11
                                font.bold: true
                            }
                            ToolTip.visible: bubbleHover.hovered
                            ToolTip.delay: 400
                            ToolTip.text: qsTr("%1 new: %2").arg(page.labelFor(modelData)).arg(n)
                            HoverHandler { id: bubbleHover }
                        }
                    }
                    Text {
                        visible: camRow.unseenTypes.length === 0 && !camRow.statusOnly
                        text: "✓"
                        color: Theme.textMuted
                        font.pixelSize: 13
                    }
                }
                HoverHandler { id: camHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: page.pickCamera(camRow.key) }
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: {
                        const p = camRow.key.split(":");
                        const row = Devices.rowOfHostChannel(Number(p[0]), Number(p[1]));
                        if (page.menus && row >= 0)
                            page.menus.popupCamera(row);
                    }
                }
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
        width: page.feedWidth + page.railWidth + Theme.spacing * 4

        ColumnLayout {
            id: feedColumn
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
                Layout.rightMargin: page.railWidth   // the date bar sits there
                model: feed
                clip: true
                spacing: 10
                // What is switched off here, then a noise card per noisy type (step 21); they
                // scroll away with the feed.
                onMovementStarted: page.feedTouched = true
                // ↑/↓ step the clip player while a clip is up (the feed takes focus when one
                // starts); the list's own key navigation is off.
                keyNavigationEnabled: false
                Keys.onUpPressed: (e) => { if (clipPlayer.active) page.stepClip(-1); else e.accepted = false; }
                Keys.onDownPressed: (e) => { if (clipPlayer.active) page.stepClip(1); else e.accepted = false; }
                header: Column {
                    width: feedView.width
                    // The header fills in after the feed is placed: keep its top in view
                    // until the person scrolls.
                    onHeightChanged: if (!page.feedTouched && feedView.contentY <= 1)
                                         feedView.positionViewAtBeginning()
                    spacing: 10
                    bottomPadding: (noise.offTypes.length > 0 || noise.cards.length > 0) ? 10 : 0

                    // "Switched off here — not logged: Motion · Person", each with Turn on.
                    Item {
                        visible: noise.offTypes.length > 0
                        width: parent.width
                        height: visible ? offFlow.implicitHeight + 16 : 0
                        Repeater {   // the dashed outline: top and bottom rules
                            model: 2
                            Row {
                                required property int index
                                y: index === 0 ? 0 : parent.height - 1
                                spacing: 4
                                clip: true
                                width: parent.width
                                Repeater {
                                    model: Math.ceil(feedView.width / 8)
                                    Rectangle { width: 4; height: 1; color: Theme.textMuted }
                                }
                            }
                        }
                        Flow {
                            id: offFlow
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: 6
                            spacing: 8
                            Text {
                                text: qsTr("Switched off here — not logged:")
                                color: Theme.textMuted
                                font.pixelSize: 12
                            }
                            Repeater {
                                model: noise.offTypes
                                Row {
                                    required property string modelData
                                    required property int index
                                    spacing: 6
                                    Text {
                                        text: (index > 0 ? "· " : "") + page.labelFor(modelData)
                                        color: Theme.text
                                        font.pixelSize: 12
                                    }
                                    Text {
                                        text: qsTr("Turn on")
                                        color: Theme.accent
                                        font.pixelSize: 12
                                        font.underline: turnOnHover.hovered
                                        HoverHandler { id: turnOnHover; cursorShape: Qt.PointingHandCursor }
                                        TapHandler { onTapped: page.turnOn(modelData) }
                                    }
                                }
                            }
                        }
                    }

                    // A noise card: four recent snapshots, the rate, Switch off… / Keep logging.
                    Repeater {
                        model: noise.cards
                        Rectangle {
                            id: card
                            required property var modelData
                            width: feedView.width
                            height: cardColumn.implicitHeight + 20
                            radius: Theme.radius
                            color: Theme.surface
                            border.color: page.colorFor(modelData.type)
                            Column {
                                id: cardColumn
                                x: 10; y: 10
                                width: parent.width - 20
                                spacing: 8
                                Text {
                                    width: parent.width
                                    text: qsTr("%1: %2 a day on %3 — about one every %4. Mostly the same scene again?")
                                              .arg(page.labelFor(card.modelData.type))
                                              .arg(page.number(card.modelData.perDay))
                                              .arg(page.cameraName)
                                              .arg(page.every(card.modelData.everySecs))
                                    color: Theme.text
                                    font.pixelSize: 13
                                    wrapMode: Text.WordWrap
                                }
                                Row {
                                    spacing: 4
                                    Repeater {
                                        model: card.modelData.thumbnails
                                        Rectangle {
                                            required property string modelData
                                            width: (cardColumn.width - 4 * (noise.cardSnapshots - 1)) / noise.cardSnapshots
                                            height: Math.round(width * 9 / 16)
                                            radius: 3
                                            color: Theme.paneBackground
                                            clip: true
                                            Image {
                                                anchors.fill: parent
                                                source: "file://" + parent.modelData
                                                fillMode: Image.PreserveAspectCrop
                                                asynchronous: true
                                                sourceSize.width: 160
                                            }
                                        }
                                    }
                                }
                                Row {
                                    spacing: 8
                                    Repeater {
                                        model: [{ label: qsTr("Switch off…"), off: true },
                                                { label: qsTr("Keep logging"), off: false }]
                                        Rectangle {
                                            required property var modelData
                                            width: btnText.implicitWidth + 24
                                            height: 30
                                            radius: Theme.radius
                                            color: modelData.off ? (btnHover.hovered ? Theme.accent : Theme.accentDim)
                                                                 : (btnHover.hovered ? Theme.surfaceAlt : "transparent")
                                            border.color: modelData.off ? Theme.accent : Theme.border
                                            Text {
                                                id: btnText
                                                anchors.centerIn: parent
                                                text: parent.modelData.label
                                                color: Theme.text
                                                font.pixelSize: 12
                                            }
                                            HoverHandler { id: btnHover; cursorShape: Qt.PointingHandCursor }
                                            TapHandler {
                                                onTapped: parent.modelData.off ? page.askSwitchOff(card.modelData.type)
                                                                               : noise.keepLogging(card.modelData.type)
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                reuseItems: true
                cacheBuffer: 1600
                boundsBehavior: Flickable.StopAtBounds

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
                    required property int index
                    required property var eventId
                    readonly property bool playing: clipPlayer.active && eventId === clipPlayer.eventId
                    required property bool isNew          // after the seen line: a blue dot
                    required property bool seenLineAbove  // "Seen up to here" sits above it
                    readonly property bool gap: kind === "gap"
                    readonly property bool status: type === "online" || type === "offline"
                    readonly property bool slim: gap || status
                    width: ListView.view.width
                    height: slim ? 28 : caption.height + 6 + snap.height
                    z: seenLineAbove ? 1 : 0   // the line is drawn in the spacing above

                    // "Seen up to here" (step 20): where the person had looked up to when they
                    // opened the camera, drawn across the spacing above this row so the rows
                    // keep the heights the date bar lays out.
                    Item {
                        visible: ev.seenLineAbove
                        width: parent.width
                        height: feedView.spacing
                        // Above the day header when the row starts a day, not under it.
                        y: -feedView.spacing
                           - (ev.ListView.section !== ev.ListView.previousSection ? dateBar.headerHeight : 0)
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width
                            height: 1
                            color: Theme.accent
                        }
                        Rectangle {
                            anchors.centerIn: parent
                            width: seenText.implicitWidth + 16
                            height: 16
                            radius: 8
                            color: Theme.accent
                            Text {
                                id: seenText
                                anchors.centerIn: parent
                                text: qsTr("Seen up to here")
                                color: "white"
                                font.pixelSize: 10
                                font.bold: true
                            }
                        }
                    }

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
                        Rectangle {   // new since the camera was last opened
                            visible: ev.isNew
                            Layout.leftMargin: 2
                            width: 8; height: 8; radius: 4
                            color: Theme.accent
                        }
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
                                    text: qsTr("Switch off %1 on %2…").arg(page.labelFor(ev.type)).arg(page.cameraName)
                                    onTriggered: page.askSwitchOff(ev.type)
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
                        Rectangle {   // the event playing in the clip player
                            anchors.fill: parent
                            visible: ev.playing
                            color: "transparent"
                            radius: 4
                            border.width: 2
                            border.color: Theme.accent
                        }
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        TapHandler {   // plays it in the clip player
                            enabled: !ev.slim
                            onTapped: page.playRow(ev.index)
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

    // ---- The date bar beside the feed (wayfinder/events-feed step 19) ----------------------
    // Day marks where each day starts in the feed, labels stacked so they never overlap, a
    // thumb for the visible part; press or drag to jump; a bubble with the top event's date
    // and time while the feed moves. Geometry comes from EventDateBar (the feed's own rows).
    readonly property string todayKey: Qt.formatDate(page.now, "yyyy-MM-dd")
    readonly property string yesterdayKey:
        Qt.formatDate(new Date(page.now.getFullYear(), page.now.getMonth(), page.now.getDate() - 1), "yyyy-MM-dd")
    EventDateBar {
        id: dateBar
        feed: feed
        eventHeight: 28 + 6 + Math.round(feedView.width * 9 / 16)
        slimHeight: 28
        headerHeight: 30
        spacing: feedView.spacing
        barHeight: rail.height
        minGap: 30   // a two-line label is about 27 px
        keep: [page.todayKey, page.yesterdayKey]
    }
    // The row at the top of the view and how far into it the view is scrolled; follows
    // scrolling, rows arriving and the layout changing.
    readonly property var viewTop: {
        feedView.contentY; dateBar.total;
        return page.topOfView();
    }
    function topOfView() {
        for (const dy of [1, 31, 61]) {
            const i = feedView.indexAt(20, feedView.contentY + dy);
            if (i >= 0) {
                const item = feedView.itemAtIndex(i);
                return { row: i, offset: item ? Math.max(0, feedView.contentY - item.y) : 0 };
            }
        }
        return { row: 0, offset: 0 };
    }
    property int lastJump: -1
    function jumpTo(y) {
        const row = dateBar.rowAtBar(y, feedView.height);
        if (row >= 0 && row !== page.lastJump)
            feedView.positionViewAtIndex(row, ListView.Beginning);
        page.lastJump = row;
    }
    // A mark's label: Today, Yest., or the weekday over the day number, with the month on
    // the 1st and where the month changes from the label above.
    function markLabel(day, above) {
        if (day === page.todayKey) return { top: qsTr("Today"), bottom: "" };
        if (day === page.yesterdayKey) return { top: qsTr("Yest."), bottom: "" };
        const p = day.split("-");
        const d = new Date(Number(p[0]), Number(p[1]) - 1, Number(p[2]));
        const newMonth = d.getDate() === 1 || (above !== "" && above.substring(0, 7) !== day.substring(0, 7));
        return { top: Qt.formatDate(d, "ddd"), bottom: d.getDate() + (newMonth ? " " + Qt.formatDate(d, "MMM") : "") };
    }
    Rectangle {
        id: rail
        x: feedPane.x + feedColumn.x + feedView.x + feedView.width
        y: feedPane.y + feedColumn.y + feedView.y
        width: page.railWidth
        height: feedView.height
        visible: feedView.count > 0
        color: Theme.surface
        border.color: Theme.border
        radius: 4

        Repeater {
            model: dateBar.marks
            Item {
                required property var modelData
                required property int index
                anchors.fill: parent
                Rectangle {   // the tick where the day starts
                    x: modelData.labelled ? 3 : parent.width - 10
                    width: modelData.labelled ? parent.width - 6 : 7
                    y: modelData.tickY
                    height: 1
                    color: Theme.border
                }
                Column {
                    visible: modelData.labelled
                    x: 3
                    width: parent.width - 6
                    y: modelData.y + 1
                    readonly property var label: {
                        var above = "";
                        for (var j = index - 1; j >= 0; --j)
                            if (dateBar.marks[j].labelled) { above = dateBar.marks[j].day; break; }
                        return page.markLabel(modelData.day, above);
                    }
                    Text {
                        width: parent.width
                        horizontalAlignment: Text.AlignRight
                        text: parent.label.top
                        color: Theme.text
                        font.pixelSize: 11
                        font.bold: true
                    }
                    Text {
                        width: parent.width
                        visible: text !== ""
                        horizontalAlignment: Text.AlignRight
                        text: parent.label.bottom
                        color: Theme.textMuted
                        font.pixelSize: 10
                    }
                }
            }
        }

        Rectangle {   // the band over the new part of the feed (step 20)
            visible: feed.newUntilRow > 0
            x: parent.width - 4
            width: 4
            radius: 2
            color: Theme.accent
            opacity: 0.45
            y: 0
            height: Math.max(4, dateBar.total > 0 ? dateBar.barY(feed.newUntilRow, 0) : 0)
        }
        Rectangle {   // the thumb: the visible part of the feed
            readonly property var viewTop: page.viewTop
            x: 0
            width: 4
            radius: 2
            color: Theme.accent
            y: Math.min(parent.height - height, dateBar.barY(viewTop.row, viewTop.offset))
            height: Math.max(16, dateBar.total > 0 ? feedView.height / dateBar.total * parent.height : parent.height)
        }

        MouseArea {
            id: railArea
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onPressed: (mouse) => { page.feedTouched = true; page.lastJump = -1; page.jumpTo(mouse.y); }
            onPositionChanged: (mouse) => { if (pressed) page.jumpTo(mouse.y); }
            onReleased: bubbleHide.restart()
        }
    }
    // The bubble beside the bar: the top event's date and time while the person scrolls
    // or drags, gone a second after. It sits over the feed's edge and takes no input.
    Rectangle {
        id: bubble
        readonly property var viewTop: page.viewTop
        readonly property bool active: feedView.moving || railArea.pressed || bubbleHide.running
        x: rail.x - width - 6
        y: rail.y + Math.max(0, Math.min(rail.height - height, dateBar.barY(viewTop.row, viewTop.offset) - height / 2))
        width: bubbleText.implicitWidth + 16
        height: 22
        radius: 11
        color: Theme.accent
        opacity: active && feedView.count > 0 ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 250 } }
        Text {
            id: bubbleText
            anchors.centerIn: parent
            text: {
                const ts = feed.timestampAt(bubble.viewTop.row);
                return ts > 0 ? Qt.formatDateTime(new Date(ts * 1000), "ddd d MMM · HH:mm") : "";
            }
            color: Theme.window
            font.pixelSize: 11
            font.bold: true
        }
    }
    Timer { id: bubbleHide; interval: 1000 }
    Connections {
        target: feedView
        function onMovementEnded() { bubbleHide.restart(); }
    }

    // ---- Right: the clip player (step 22) --------------------------------------------------
    // Beside the feed when there is room; on a narrow window it opens over the feed while
    // an event plays, and closing it goes back to the feed.
    EventClipPlayer {
        id: clipPlayer
        anchors.top: bar.bottom
        anchors.bottom: parent.bottom
        x: page.showPlayer ? feedPane.x + feedPane.width : feedPane.x
        width: page.width - x
        visible: page.showPlayer || active
        z: page.showPlayer ? 0 : 5
        onOpenInPlayback: (h, c, t) => page.jumpToPlayback(h, c, t)
        onStepRequested: (d) => page.stepClip(d)
        Rectangle { anchors.left: parent.left; width: 1; height: parent.height; color: Theme.border; visible: page.showPlayer }
    }
    onOnScreenChanged: if (!onScreen) clipPlayer.stop()
    // RL_EVENTS_PLAY test hook: play the newest event once, after the feed loads.
    Timer {
        running: typeof eventsAutoPlay !== "undefined" && eventsAutoPlay && feed.count > 0
                 && !clipPlayer.active && !played
        interval: 1500
        property bool played: false
        onTriggered: { played = true; page.playRow(feed.stepEvent(-1, 1)); }
    }
    // Play a feed row in the player, and keep it in view.
    function playRow(row) {
        const r = feed.rowAt(row);
        if (r.eventId === undefined || page.camera === "")
            return;
        clipPlayer.play({ hostId: Number(page.hc[0]), channel: Number(page.hc[1]),
                          timestamp: r.timestamp, thumbnail: r.thumbnail, eventId: r.eventId,
                          title: page.labelFor(r.type) + " · " + page.cameraName + " · "
                                 + Qt.formatDateTime(new Date(r.timestamp * 1000), "ddd d MMM, HH:mm:ss") });
        page.feedTouched = true;
        feedView.positionViewAtIndex(row, ListView.Contain);
        feedView.forceActiveFocus();   // ↑/↓ go to the feed
    }
    // ↑/↓: the newer/older event in the feed shown. Nothing when the playing event isn't
    // in it (another camera's, or a type filtered out).
    function stepClip(direction) {
        const row = feed.rowOfId(clipPlayer.eventId);
        const next = row < 0 ? -1 : feed.stepEvent(row, direction);
        if (next >= 0)
            page.playRow(next);
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
        id: switchOffDialog
        title: qsTr("Switch off an event type")
        confirmLabel: qsTr("Switch off")
        onConfirmed: (p) => {
            const hc = p.key.split(":");
            toast.show(Events.switchOff(Number(hc[0]), Number(hc[1]), p.type)
                       ? qsTr("%1 switched off on %2").arg(page.labelFor(p.type)).arg(p.name)
                       : qsTr("Couldn't switch %1 off").arg(page.labelFor(p.type)));
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
