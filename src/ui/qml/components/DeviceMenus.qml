import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ReolinkApp
import ReolinkApp.Core

// The camera and recorder menus that the Devices sidebar used to carry, now opened by a
// right-click on a camera card or a Site tab (wayfinder/site-tabs-redesign), and the
// credentials dialog they open. One instance, in Main.
Item {
    id: root

    signal openSettings(int row)       // open Device Settings for this row
    signal cameraProperties(int row)   // show the camera properties dialog
    signal nvrProperties(var host)     // show the recorder properties dialog (hostInfo map)

    function popupCamera(row) {
        var c = Devices.cameraInfo(row);
        if (!c || c.hostId === undefined)
            return;
        camMenu.row = row;
        camMenu.info = c;
        camMenu.popup();
    }
    // One block of actions per recorder at the Site, headed by its name when there are
    // several (Lascelles: the Frigate server and the NVR).
    function popupSite(site) {
        var ids = Sites.hostsOf(site), entries = [];
        for (var i = 0; i < ids.length; ++i) {
            var h = Devices.hostInfo(ids[i]);
            if (!h || h.firstRow === undefined)
                continue;
            if (ids.length > 1)
                entries.push({ kind: "head", host: h });
            entries.push({ kind: "properties", host: h }, { kind: "settings", host: h },
                         { kind: "reconnect", host: h }, { kind: "credentials", host: h },
                         { kind: "reboot", host: h }, { kind: "remove", host: h });
        }
        if (entries.length === 0)
            return;
        siteMenu.entries = entries;
        siteMenu.popup();
    }

    ThemedMenu {
        id: camMenu
        property int row: -1
        property var info: ({})
        readonly property bool underNvr: !!info.multiChannel
        readonly property int rotation: info.rotationOverride || 0
        ThemedMenuItem { text: qsTr("Properties"); onTriggered: root.cameraProperties(camMenu.row) }
        ThemedMenuItem { text: qsTr("Settings"); onTriggered: root.openSettings(camMenu.row) }
        ThemedMenuSeparator {}
        ThemedMenuItem { text: qsTr("Reconnect"); onTriggered: Devices.reconnect(camMenu.row) }
        ThemedMenuItem { text: qsTr("Update credentials…"); onTriggered: credsDialog.openFor(camMenu.row) }
        ThemedMenuSeparator {}
        // A ceiling-mounted 180° lens: enables the view modes (panorama, quad…) in the
        // pane's toolbar.
        ThemedMenuItem {
            text: camMenu.info.isFisheye ? qsTr("✓  Fisheye lens") : qsTr("     Fisheye lens")
            onTriggered: Devices.setFisheye(camMenu.row, !camMenu.info.isFisheye)
        }
        // For cameras the rotation heuristics can't know (issue #3): each click turns the
        // view another 90°.
        ThemedMenuItem {
            text: camMenu.rotation !== 0 ? qsTr("Rotate view 90° (now %1°)").arg(camMenu.rotation)
                                         : qsTr("Rotate view 90°")
            onTriggered: Devices.setRotationOverride(camMenu.row, camMenu.rotation + 90)
        }
        ThemedMenuItem {
            visible: camMenu.rotation !== 0
            height: visible ? implicitHeight : 0
            text: qsTr("Reset rotation")
            onTriggered: Devices.setRotationOverride(camMenu.row, 0)
        }
        ThemedMenuSeparator {}
        // Standalone cameras are their own host and can be removed here; an NVR's
        // channels are managed on the NVR (removed whole).
        ThemedMenuItem {
            text: camMenu.underNvr ? qsTr("Remove NVR…") : qsTr("Remove device")
            onTriggered: Devices.removeDevice(camMenu.row)
        }
    }

    ThemedMenu {
        id: siteMenu
        property var entries: []
        Instantiator {
            model: siteMenu.entries
            delegate: ThemedMenuItem {
                required property var modelData
                readonly property var host: modelData.host
                readonly property string kind: modelData.kind
                readonly property string what: host.frigateApi ? qsTr("server")
                                              : host.multiChannel ? qsTr("NVR") : qsTr("device")
                font.bold: kind === "head"
                enabled: kind === "head" ? false : kind === "reboot" ? !!host.isAdmin : true
                text: kind === "head" ? host.name
                    : kind === "properties" ? qsTr("Properties")
                    : kind === "settings" ? qsTr("Settings")
                    : kind === "reconnect" ? qsTr("Reconnect")
                    : kind === "credentials" ? qsTr("Update credentials…")
                    : kind === "reboot" ? qsTr("Reboot %1").arg(what)
                    : qsTr("Remove %1").arg(what)
                onTriggered: {
                    switch (kind) {
                    case "properties": root.nvrProperties(host); break;
                    case "settings": root.openSettings(host.firstRow); break;
                    case "reconnect": Devices.reconnect(host.firstRow); break;
                    case "credentials": credsDialog.openFor(host.firstRow); break;
                    case "reboot": Devices.settings.reboot(Devices.cameraKeyAt(host.firstRow)); break;
                    case "remove": Devices.removeDevice(host.firstRow); break;
                    }
                }
            }
            onObjectAdded: (i, item) => siteMenu.insertItem(i, item)
            onObjectRemoved: (i, item) => siteMenu.removeItem(item)
        }
    }

    // Fix a host's sign-in without removing and re-adding it — the recovery
    // path for the auth/locked states (which are never auto-retried, since
    // every rejected login burns the firmware's lockout counter).
    Dialog {
        id: credsDialog
        modal: true
        width: 360
        anchors.centerIn: Overlay.overlay
        property int targetRow: -1
        // A Frigate server: its live stream (go2rtc) can need a login of its own.
        property bool isFrigate: false
        property string initialUser: ""

        function openFor(row) {
            targetRow = row;
            var info = Devices.cameraInfo(row);
            isFrigate = !!(info && info.frigateApi === true);
            credsRestreamUser.text = isFrigate ? Devices.restreamUserAt(row) : "";
            credsRestreamPassword.text = "";
            credsUser.text = info && info.username !== undefined ? info.username : "";
            initialUser = credsUser.text;
            credsPassword.text = "";
            credsTitle.text = qsTr("Update credentials — %1")
                .arg(info && info.hostName ? info.hostName : (info && info.addr ? info.addr : ""));
            open();
        }

        background: Rectangle {
            color: Theme.surface
            border.color: Theme.border
            radius: Theme.radius
        }
        header: Item {
            implicitHeight: 44
            Text {
                id: credsTitle
                anchors.left: parent.left
                anchors.leftMargin: Theme.spacing * 2
                anchors.verticalCenter: parent.verticalCenter
                anchors.right: parent.right
                anchors.rightMargin: Theme.spacing
                elide: Text.ElideRight
                color: Theme.text
                font.pixelSize: 14
                font.bold: true
            }
        }

        contentItem: ColumnLayout {
            spacing: Theme.spacing
            TextField {
                id: credsUser
                Layout.fillWidth: true
                placeholderText: qsTr("Username")
                color: Theme.text
                placeholderTextColor: Theme.textMuted
                background: Rectangle {
                    color: Theme.surfaceAlt
                    border.color: parent.activeFocus ? Theme.accent : Theme.border
                    radius: 4
                }
            }
            TextField {
                id: credsPassword
                Layout.fillWidth: true
                placeholderText: qsTr("New password (blank keeps the current one)")
                echoMode: TextInput.Password
                color: Theme.text
                placeholderTextColor: Theme.textMuted
                background: Rectangle {
                    color: Theme.surfaceAlt
                    border.color: parent.activeFocus ? Theme.accent : Theme.border
                    radius: 4
                }
            }
            Text {
                Layout.fillWidth: true
                visible: credsDialog.isFrigate
                text: qsTr("Live stream login (go2rtc), if the stream asks for one. Not the Frigate login.")
                color: Theme.textMuted
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }
            TextField {
                id: credsRestreamUser
                Layout.fillWidth: true
                visible: credsDialog.isFrigate
                placeholderText: qsTr("Live stream username (blank for none)")
                color: Theme.text
                placeholderTextColor: Theme.textMuted
                background: Rectangle {
                    color: Theme.surfaceAlt
                    border.color: parent.activeFocus ? Theme.accent : Theme.border
                    radius: 4
                }
            }
            TextField {
                id: credsRestreamPassword
                Layout.fillWidth: true
                visible: credsDialog.isFrigate
                placeholderText: qsTr("Live stream password (blank keeps the current one)")
                echoMode: TextInput.Password
                color: Theme.text
                placeholderTextColor: Theme.textMuted
                background: Rectangle {
                    color: Theme.surfaceAlt
                    border.color: parent.activeFocus ? Theme.accent : Theme.border
                    radius: 4
                }
            }
        }

        footer: Item {
            implicitHeight: 52
            RowLayout {
                anchors.right: parent.right
                anchors.rightMargin: Theme.spacing * 2
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.spacing
                Rectangle {
                    implicitWidth: ccLbl.implicitWidth + 26; implicitHeight: 32; radius: Theme.radius
                    color: ccHov.hovered ? Theme.surfaceAlt : "transparent"
                    border.color: Theme.border
                    Text { id: ccLbl; anchors.centerIn: parent; text: qsTr("Cancel")
                           color: Theme.text; font.pixelSize: 13 }
                    HoverHandler { id: ccHov }
                    TapHandler { onTapped: credsDialog.close() }
                }
                Rectangle {
                    implicitWidth: csLbl.implicitWidth + 26; implicitHeight: 32; radius: Theme.radius
                    color: csHov.hovered ? Theme.accent : Theme.accentDim
                    border.color: Theme.accent
                    Text { id: csLbl; anchors.centerIn: parent; text: qsTr("Save & reconnect")
                           color: Theme.text; font.pixelSize: 13 }
                    HoverHandler { id: csHov }
                    TapHandler {
                        onTapped: {
                            // The restream login first: it is stored before the host revalidates.
                            if (credsDialog.isFrigate)
                                Devices.setRestreamLogin(credsDialog.targetRow,
                                                         credsRestreamUser.text, credsRestreamPassword.text);
                            if (!credsDialog.isFrigate || credsUser.text !== credsDialog.initialUser
                                    || credsPassword.text.length > 0)
                                Devices.updateCredentials(credsDialog.targetRow,
                                                          credsUser.text, credsPassword.text);
                            credsDialog.close();
                        }
                    }
                }
            }
        }
    }
}
