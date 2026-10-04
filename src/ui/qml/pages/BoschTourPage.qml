import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ReolinkApp
import ReolinkApp.Core

Item {
    id: page

    component ActionButton: Button {
        id: control
        property color fillColor: Theme.surfaceAlt
        implicitHeight: 32
        contentItem: Text {
            text: control.text
            color: control.enabled ? Theme.text : Theme.textMuted
            font.pixelSize: 12
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            radius: Theme.radius
            color: control.down ? Theme.accentDim
                 : control.hovered ? Theme.surface : control.fillColor
            border.color: control.hovered ? Theme.accent : Theme.border
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spacing * 2
        spacing: Theme.spacing * 2

        RowLayout {
            Layout.fillWidth: true
            Text {
                text: qsTr("Bosch Home Assistant connection")
                color: Theme.text
                font.pixelSize: 21
                font.bold: true
            }
            Item { Layout.fillWidth: true }
            Text {
                text: BoschTour.connectionStatus
                color: BoschTour.connected ? Theme.online : Theme.textMuted
                font.pixelSize: 12
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: connectionColumn.implicitHeight + Theme.spacing * 4
            color: Theme.surface
            border.color: Theme.border
            radius: Theme.radius

            ColumnLayout {
                id: connectionColumn
                anchors.fill: parent
                anchors.margins: Theme.spacing * 2
                spacing: Theme.spacing

                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        text: qsTr("Home Assistant")
                        color: Theme.text
                        font.pixelSize: 15
                        font.bold: true
                    }
                    Item { Layout.fillWidth: true }
                    ActionButton {
                        visible: BoschTour.configured && !connectionColumn.editConnection
                        text: qsTr("Change connection")
                        onClicked: connectionColumn.editConnection = true
                    }
                    ActionButton {
                        visible: BoschTour.configured && !connectionColumn.editConnection
                        text: qsTr("Disconnect")
                        onClicked: BoschTour.disconnectHomeAssistant()
                    }
                }

                Text {
                    visible: BoschTour.configured && !connectionColumn.editConnection
                    Layout.fillWidth: true
                    text: qsTr("Connected server: %1\nBosch entity: %2")
                            .arg(BoschTour.endpoint).arg(BoschTour.cameraEntity)
                    color: Theme.textMuted
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }

                property bool editConnection: !BoschTour.configured
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: connectionColumn.editConnection
                    spacing: Theme.spacing * 1.25

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Connect to the Home Assistant instance that owns the Bosch camera services. This app reads and updates the same positions and tour state as HA. The access token is saved in your system keyring.")
                        color: Theme.textMuted
                        font.pixelSize: 12
                        wrapMode: Text.WordWrap
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Text {
                            text: qsTr("Home Assistant URL")
                            color: Theme.text
                            font.pixelSize: 12
                            font.bold: true
                        }
                        TextField {
                            id: endpointInput
                            Layout.fillWidth: true
                            implicitHeight: 36
                            placeholderText: qsTr("https://ha.example")
                            placeholderTextColor: Theme.textMuted
                            text: BoschTour.endpoint || "https://ha-lascelles.ehiwa.dev"
                            color: Theme.text
                            selectByMouse: true
                            background: Rectangle {
                                color: Theme.surfaceAlt
                                border.color: endpointInput.activeFocus ? Theme.accent : Theme.border
                                radius: Theme.radius
                            }
                        }
                        Text {
                            text: qsTr("A dashboard URL is okay; the app connects to the site root.")
                            color: Theme.textMuted
                            font.pixelSize: 10
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Text {
                            text: qsTr("Long-lived access token")
                            color: Theme.text
                            font.pixelSize: 12
                            font.bold: true
                        }
                        TextField {
                            id: tokenInput
                            Layout.fillWidth: true
                            implicitHeight: 36
                            placeholderText: qsTr("Paste your Home Assistant token")
                            placeholderTextColor: Theme.textMuted
                            echoMode: TextInput.Password
                            passwordCharacter: "●"
                            color: Theme.text
                            selectByMouse: true
                            background: Rectangle {
                                color: Theme.surfaceAlt
                                border.color: tokenInput.activeFocus ? Theme.accent : Theme.border
                                radius: Theme.radius
                            }
                        }
                        Text {
                            text: qsTr("The token stays hidden while typing and is stored in the system keyring.")
                            color: Theme.textMuted
                            font.pixelSize: 10
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Text {
                            text: qsTr("Bosch camera entity")
                            color: Theme.text
                            font.pixelSize: 12
                            font.bold: true
                        }
                        TextField {
                            id: cameraEntityInput
                            Layout.fillWidth: true
                            implicitHeight: 36
                            placeholderText: qsTr("camera.example_bosch")
                            placeholderTextColor: Theme.textMuted
                            text: BoschTour.cameraEntity || "camera.lascelles_driveway_h26x_l1s1"
                            color: Theme.text
                            selectByMouse: true
                            background: Rectangle {
                                color: Theme.surfaceAlt
                                border.color: cameraEntityInput.activeFocus ? Theme.accent : Theme.border
                                radius: Theme.radius
                            }
                        }
                        Text {
                            text: qsTr("Used for manual PTZ. Presets and tours use HA's Bosch services.")
                            color: Theme.textMuted
                            font.pixelSize: 10
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: qsTr("On the live Bosch stream, select T and tap at least two named positions to run a temporary tour using Home Assistant's saved speed and dwell settings.")
                            color: Theme.textMuted
                            font.pixelSize: 11
                        }
                        Item { Layout.fillWidth: true }
                        ActionButton {
                            visible: BoschTour.configured && BoschTour.connected
                            text: qsTr("Cancel")
                            onClicked: connectionColumn.editConnection = false
                        }
                        ActionButton {
                            text: BoschTour.busy ? qsTr("Connecting…") : qsTr("Connect")
                            fillColor: Theme.accentDim
                            enabled: endpointInput.text.trim().length > 0
                                     && tokenInput.text.length > 0 && !BoschTour.busy
                            onClicked: {
                                BoschTour.connectHomeAssistant(endpointInput.text, tokenInput.text,
                                                               cameraEntityInput.text);
                                tokenInput.clear();
                            }
                        }
                    }
                }

                Text {
                    visible: BoschTour.message.length > 0
                    Layout.fillWidth: true
                    text: BoschTour.message
                    color: Theme.textMuted
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                }
            }
        }

        Item { Layout.fillWidth: true; Layout.fillHeight: true }
    }

    Connections {
        target: BoschTour
        function onConnectionChanged() {
            connectionColumn.editConnection = !BoschTour.connected;
        }
    }
}
