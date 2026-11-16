import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ReolinkApp
import ReolinkApp.Core

ApplicationWindow {
    id: window
    width: 1400
    height: 860
    visible: typeof startHidden === "undefined" || !startHidden
    title: qsTr("Reolink Client (jjr fork)")
    color: Theme.window

    // Video fullscreen (official client: chrome disappears, grid fills the
    // screen). F11 or the ⛶ buttons toggle; Esc always exits. The pre-fullscreen
    // visibility is saved and restored so leaving fullscreen doesn't clobber a
    // maximized window.
    property bool videoFullscreen: false
    property int _preFsVisibility: Window.AutomaticVisibility
    onVideoFullscreenChanged: {
        if (videoFullscreen) {
            if (visibility !== Window.FullScreen)
                _preFsVisibility = visibility;
            visibility = Window.FullScreen;
        } else {
            visibility = _preFsVisibility === Window.FullScreen
                       ? Window.Windowed : _preFsVisibility;
        }
    }
    // Closing hides to the tray when enabled — monitoring and notifications
    // keep running; Quit lives in the tray menu.
    onClosing: (close) => {
        if (Tray.available && Tray.closeToTray) {
            close.accepted = false;
            window.hide();
        }
    }

    // Keep the flag in sync if the compositor changes visibility out from under us.
    onVisibilityChanged: () => {
        if (window.visibility !== Window.FullScreen && videoFullscreen)
            videoFullscreen = false;
    }

    Shortcut {
        sequence: "F11"
        onActivated: window.videoFullscreen = !window.videoFullscreen
    }
    Shortcut {
        sequence: "Escape"
        onActivated: if (window.videoFullscreen) window.videoFullscreen = false
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        NavBar {
            id: nav
            Layout.fillWidth: true
            visible: !window.videoFullscreen
            onFullscreenRequested: window.videoFullscreen = true
        }

        Rectangle { // divider
            Layout.fillWidth: true
            height: 1
            color: Theme.border
            visible: !window.videoFullscreen
        }

        // Update banner — checked against GitHub Releases on startup. Shows a
        // one-click "Update now" when running as an AppImage, else a download link.
        Rectangle {
            id: updateBanner
            property bool dismissed: false
            Layout.fillWidth: true
            visible: !window.videoFullscreen && !dismissed
                     && (Updater.available || Updater.downloading || Updater.failed)
            implicitHeight: 40
            color: Updater.failed ? Theme.danger : Theme.accentDim

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spacing * 2
                anchors.rightMargin: Theme.spacing
                spacing: Theme.spacing

                Text { text: "\u2b06"; color: Theme.text; font.pixelSize: 14 }
                Text {
                    Layout.fillWidth: true
                    color: Theme.text; font.pixelSize: 12; elide: Text.ElideRight
                    text: Updater.downloading
                            ? qsTr("Downloading update\u2026 %1%").arg(Updater.progress)
                        : Updater.failed
                            ? qsTr("Update failed: %1").arg(Updater.error)
                        : qsTr("Version %1 is available \u2014 you have %2")
                            .arg(Updater.latestVersion).arg(Updater.currentVersion)
                }

                component BannerBtn: Rectangle {
                    property string label: ""
                    signal clicked()
                    implicitWidth: bl.implicitWidth + 22; implicitHeight: 26; radius: Theme.radius
                    color: bh.hovered ? Qt.rgba(1, 1, 1, 0.18) : Qt.rgba(1, 1, 1, 0.08)
                    Text { id: bl; anchors.centerIn: parent; text: parent.label; color: Theme.text; font.pixelSize: 12 }
                    HoverHandler { id: bh }
                    TapHandler { onTapped: parent.clicked() }
                }

                BannerBtn {
                    visible: !Updater.downloading
                    label: Updater.failed ? qsTr("Retry") : qsTr("What's new")
                    onClicked: Updater.failed ? Updater.applyUpdate() : Updater.openReleasePage()
                }
                BannerBtn {
                    visible: Updater.available && !Updater.downloading && !Updater.failed
                    label: Updater.canSelfUpdate ? qsTr("Update now") : qsTr("Download")
                    onClicked: Updater.applyUpdate()
                }
                Rectangle {
                    visible: !Updater.downloading
                    implicitWidth: 24; implicitHeight: 24; radius: 12
                    color: xh.hovered ? Qt.rgba(1, 1, 1, 0.18) : "transparent"
                    Text { anchors.centerIn: parent; text: "\u2715"; color: Theme.text; font.pixelSize: 12 }
                    HoverHandler { id: xh }
                    TapHandler { onTapped: updateBanner.dismissed = true }
                }
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: nav.currentIndex

            LiveViewPage {
                id: liveViewPage
                active: nav.currentIndex === 0
                menus: deviceMenus
                audioSuppressed: doorbell.active
                doorbellCamera: doorbell.active && doorbell.cameraIdentity.hostId !== undefined
                                ? doorbell.cameraIdentity.hostId + ":" + doorbell.cameraIdentity.channel : ""
                onEditHighlightRulesRequested: { settingsPage.category = "highlights"; nav.currentIndex = 3; }
                fullscreen: window.videoFullscreen
                onFullscreenToggled: window.videoFullscreen = !window.videoFullscreen
                onPopOut: (row, lbl) => window.openPopout(row, lbl)
                onOpenBoschTourRequested: nav.currentIndex = 4
            }
            PlaybackPage { id: playbackPage; active: nav.currentIndex === 1; menus: deviceMenus }
            EventsPage {
                menus: deviceMenus
                onJumpToPlayback: (hostId, channel, timestamp) =>
                    window.openEventPlayback(hostId, channel, timestamp)
            }
            DeviceSettingsPage {
                id: settingsPage
                onAddDeviceRequested: addDeviceDialog.open()
            }
            BoschTourPage { id: boschTourPage }
        }
    }

    // The camera and recorder menus (right-click a camera card or a Site tab).
    DeviceMenus {
        id: deviceMenus
        onOpenSettings: (row) => {
            nav.currentIndex = 3;
            settingsPage.showDevice(row);
        }
        onCameraProperties: (row) => window.showCameraProperties(row)
        onNvrProperties: (host) => window.showNvrProperties(host)
    }

    // Where a detection's recording plays: a camera whose footage is on a server of its
    // own (Frigate) opens the clip; everything else opens the Playback page at that moment.
    function openEventPlayback(hostId, channel, timestamp) {
        var clip = Devices.eventClipUrl(hostId, channel, timestamp);
        if (clip !== "") {
            var row = Devices.rowOfHostChannel(hostId, channel);
            clipDialog.openClip(Devices.nameAt(row),
                                Qt.formatDateTime(new Date(timestamp * 1000), "ddd d MMM, HH:mm:ss"), clip);
            return;
        }
        nav.currentIndex = 1;
        playbackPage.openAt(hostId, channel, timestamp);
    }
    ClipDialog { id: clipDialog }
    Timer {
        id: openClipTimer
        property int hostId: 0
        property int channel: 0
        interval: 4000
        onTriggered: window.openEventPlayback(hostId, channel, Math.floor(Date.now() / 1000) - 60)
    }

    AddDeviceDialog {
        id: addDeviceDialog
        anchors.centerIn: parent
    }

    Timer { id: mockDoorbellTimer; onTriggered: doorbell.active = true }   // RL_MOCK_DOORBELL "@secs"
    // Doorbell visitor-press surface (raised on a "visitor" detection).
    DoorbellOverlay {
        id: doorbell
        anchors.centerIn: parent
        z: 100
    }
    // Desktop-notification click: bring the window up and play the event back.
    Connections {
        target: Events
        function onEventActivated(hostId, channel, timestamp) {
            window.show();
            window.raise();
            window.requestActivate();
            window.openEventPlayback(hostId, channel, timestamp);
        }
    }

    // Detection sounds stay quiet while the doorbell pop-up is up.
    Binding { target: AlertSounds; property: "suppressed"; value: doorbell.active }

    Connections {
        target: Devices
        function onDetectionEvent(hostId, channel, type, camera) {
            if (type === "visitor") {
                doorbell.deviceRow = Devices.rowOfHostChannel(hostId, channel);
                doorbell.cameraIdentity = { hostId: hostId, channel: channel };
                doorbell.camera = camera;
                doorbell.active = true;
            }
        }
    }
    Component.onCompleted: {
        if (String(TestHooks.openClipTarget) !== "") {
            var ct = String(TestHooks.openClipTarget).split(":");
            openClipTimer.hostId = parseInt(ct[0]);
            openClipTimer.channel = parseInt(ct[1]);
            openClipTimer.start();
        }
        if (TestHooks.mockDoorbell) {
            // RL_MOCK_DOORBELL: aim the mock at a real doorbell (syntax in TestHooks.h).
            var at = String(TestHooks.mockDoorbellTarget).split("@");
            var t = at[0].split(":");
            doorbell.camera = "Front Door";
            if (t.length === 2) {
                doorbell.deviceRow = Devices.rowOfHostChannel(parseInt(t[0]), parseInt(t[1]));
                doorbell.cameraIdentity = { hostId: parseInt(t[0]), channel: parseInt(t[1]) };
            }
            if (at.length > 1 && Number(at[1]) > 0) {
                mockDoorbellTimer.interval = Number(at[1]) * 1000;
                mockDoorbellTimer.start();
            } else {
                doorbell.active = true;
            }
        }
        // First run: no devices yet → open Add Device and scan the network, like
        // the official client on install. Decide here (the DB count is stable at
        // startup); don't re-check in the timer, where an NVR mid-fan-out briefly
        // reports 0 as its placeholder row is swapped for channel rows.
        if (Devices.count === 0)
            firstRunTimer.start();
    }
    // The first switch-offs (wayfinder/events-feed): looked at once per launch, a few
    // seconds after the window is up with devices loaded (a launch hidden in the tray
    // waits until it's shown). Whether there is anything to offer is Events' call.
    FirstSwitchOffsDialog { id: firstSwitchOffsDialog }
    Timer {
        property bool done: false
        running: !done && Devices.count > 0
        interval: 3000
        onTriggered: {
            if (!window.visible) { restart(); return; }
            done = true;
            const offer = Events.offerFirstSwitchOffs();
            if (offer.pairs && offer.pairs.length > 0) {
                firstSwitchOffsDialog.pending = offer;
                firstSwitchOffsDialog.open();
            }
        }
    }
    // A short delay so the window is mapped before the modal + scan appear.
    Timer {
        id: firstRunTimer
        interval: 400
        onTriggered: addDeviceDialog.openAndScan()
    }

    // Device / camera "Properties" sheet, populated from the model.
    PropertiesDialog {
        id: propsDialog
        onOpenSettings: (row) => { nav.currentIndex = 3; settingsPage.showDevice(row); }
    }
    function showCameraProperties(row) {
        var c = Devices.cameraInfo(row);
        if (!c || c.name === undefined)
            return;
        var caps = [];
        if (c.capPtz) caps.push(qsTr("PTZ"));
        if (c.capZoom) caps.push(qsTr("Zoom"));
        if (c.capAudio) caps.push(qsTr("Audio"));
        if (c.capTalk) caps.push(qsTr("Two-way talk"));
        if (c.capSiren) caps.push(qsTr("Siren"));
        if (c.capFloodlight) caps.push(qsTr("Floodlight"));
        if (c.capBattery) caps.push(qsTr("Battery"));
        propsDialog.heading = c.name;
        propsDialog.subheading = c.multiChannel ? qsTr("Camera on %1").arg(c.hostName)
                                                  : qsTr("Camera");
        propsDialog.rows = [
            { label: qsTr("Status"), value: c.online ? qsTr("Online") : qsTr("Offline") },
            { label: qsTr("Channel"), value: String(c.channel) },
            { label: qsTr("Main stream"),
              value: String(c.codec).toUpperCase() + (c.mainSize ? " · " + c.mainSize : "") },
            { label: qsTr("Sub stream"), value: c.subSize || "—" },
            { label: qsTr("Features"), value: caps.length ? caps.join(", ") : "—" },
            { label: qsTr("UID"), value: c.uid || "—" }
        ];
        propsDialog.targetRow = row;
        propsDialog.isAdmin = c.isAdmin;
        propsDialog.showReboot = false;
        propsDialog.showRemove = !c.multiChannel; // NVR channels are removed as a whole
        propsDialog.removeLabel = qsTr("Remove device");
        propsDialog.open();
    }
    function showNvrProperties(host) {
        if (!host || host.name === undefined)
            return;
        propsDialog.heading = host.name;
        propsDialog.subheading = host.frigateApi ? qsTr("Frigate server") : host.multiChannel ? qsTr("Reolink NVR") : qsTr("Camera");
        propsDialog.rows = [
            { label: qsTr("Model"), value: host.model || "—" },
            { label: qsTr("Address"),
              value: host.addr + ":" + host.port + (host.https ? qsTr(" (HTTPS)") : "") },
            { label: qsTr("Account"), value: host.username + (host.isAdmin ? qsTr(" · admin") : "") },
            { label: qsTr("Cameras"),
              value: host.onlineCount + " / " + host.channelCount + qsTr(" online") }
        ];
        propsDialog.targetRow = host.firstRow;
        propsDialog.isAdmin = host.isAdmin;
        propsDialog.showReboot = true;
        propsDialog.showRemove = true;
        propsDialog.removeLabel = host.multiChannel ? qsTr("Remove server") : qsTr("Remove device");
        propsDialog.open();
    }

    // Detached camera windows for multi-monitor viewing.
    Component { id: popoutComponent; PoppedWindow {} }
    function openPopout(row, label) {
        const camera = Devices.cameraInfo(row);
        if (!camera || camera.hostId === undefined)
            return;
        popoutComponent.createObject(window, {
            cameraIdentity: { hostId: camera.hostId, channel: camera.channel }, label: label
        });
    }

    // Transient status toast for one-shot camera actions (snapshot, siren,
    // floodlight). These fire on the device but gave no on-screen confirmation,
    // so they felt like dead buttons; this surfaces success/failure.
    Rectangle {
        id: toast
        z: 300
        property bool isError: false
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 48
        radius: Theme.radius
        color: isError ? Theme.danger : Theme.surfaceAlt
        border.color: isError ? Theme.danger : Theme.border
        width: Math.min(toastLabel.implicitWidth + 28, window.width - 80)
        height: toastLabel.implicitHeight + 18
        opacity: 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 180 } }
        Text {
            id: toastLabel
            anchors.centerIn: parent
            width: Math.min(implicitWidth, window.width - 108)
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            color: Theme.text
            font.pixelSize: 13
        }
        Timer { id: toastTimer; interval: 2600; onTriggered: toast.opacity = 0 }
        function show(msg, err) {
            toastLabel.text = msg;
            toast.isError = err === true;
            toast.opacity = 1.0;
            toastTimer.restart();
        }
    }
    Connections {
        target: Devices
        function onSnapshotSaved(row, path) { toast.show(qsTr("Snapshot saved"), false); }
        function onSnapshotFailed(row, error) { toast.show(qsTr("Snapshot failed: %1").arg(error), true); }
    }
    Connections {
        target: Devices.settings
        function onSettingApplied(key, command, ok, error) {
            if (command === "AudioAlarmPlay")
                toast.show(ok ? qsTr("Siren triggered") : qsTr("Siren failed: %1").arg(error), !ok);
            else if (command === "SetWhiteLed")
                toast.show(ok ? qsTr("Floodlight toggled") : qsTr("Floodlight failed: %1").arg(error), !ok);
            else if (!ok)
                toast.show(qsTr("%1 failed: %2").arg(command).arg(error), true);
        }
    }
}
