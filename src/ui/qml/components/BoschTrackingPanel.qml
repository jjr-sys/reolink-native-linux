import QtQuick
import QtQuick.Controls
import ReolinkApp
import ReolinkApp.Core

// The Tracking ⌃ panel (bosch-ptz-port ticket 03), as the Outpost app's TrackingOptions:
// behaviour, seconds and return position are a draft until Save, editable only while
// tracking is off; the switch at the bottom starts it (stopping a tour) or stops it.
Column {
    id: panel
    spacing: 8

    readonly property bool known: BoschTour.trackingKnown
    readonly property string statusText: BoschTour.trackingSaid
    readonly property bool editable: BoschTour.trackingEditable
    readonly property string behaviour: BoschTour.trackingBehaviour
    readonly property int seconds: BoschTour.trackingSeconds
    readonly property string returnToken: BoschTour.trackingReturnToken

    function edit(behaviour, seconds, token) { BoschTour.editTracking(behaviour, seconds, token); }

    function presetIndex(token) {
        var presets = BoschTour.presets;
        for (var i = 0; i < presets.length; ++i)
            if (String(presets[i].token) === token)
                return i;
        return -1;
    }

    Text {
        visible: !panel.known
        width: panel.width
        text: qsTr("Tracking service unavailable")
        color: Theme.textMuted
        font.pixelSize: 12
        wrapMode: Text.Wrap
    }

    Column {
        visible: panel.known
        width: panel.width
        spacing: 8

        Text { text: qsTr("Behaviour"); color: Theme.textMuted; font.pixelSize: 11 }
        Row {
            spacing: 4
            Repeater {
                model: [{ key: "always", label: qsTr("Always on") },
                        { key: "fixed", label: qsTr("Fixed time") },
                        { key: "still", label: qsTr("Until still") }]
                delegate: Rectangle {
                    id: chip
                    required property var modelData
                    readonly property bool chosen: panel.behaviour === modelData.key
                    width: (panel.width - 8) / 3
                    height: 26
                    radius: 4
                    opacity: panel.editable ? 1 : 0.48
                    color: chosen ? Theme.accentDim : (chipHover.hovered && panel.editable ? Theme.surfaceAlt : "transparent")
                    border.color: chosen ? Theme.accent : Theme.border
                    Text {
                        anchors.centerIn: parent
                        text: chip.modelData.label
                        color: chip.chosen ? Theme.accent : Theme.text
                        font.pixelSize: 11
                    }
                    HoverHandler { id: chipHover }
                    TapHandler {
                        enabled: panel.editable
                        gesturePolicy: TapHandler.ReleaseWithinBounds
                        onTapped: panel.edit(chip.modelData.key, panel.seconds, panel.returnToken)
                    }
                }
            }
        }

        Row {
            visible: panel.behaviour !== "always"
            width: panel.width
            spacing: 4
            Text {
                width: panel.width - 4 * 2 - minus.width - secondsText.width - plus.width
                anchors.verticalCenter: parent.verticalCenter
                text: panel.behaviour === "fixed" ? qsTr("Track for") : qsTr("Still for")
                color: Theme.text
                font.pixelSize: 12
            }
            Button {
                id: minus
                width: 32; height: 26
                text: "−"
                enabled: panel.editable && panel.seconds > 5
                Accessible.name: qsTr("Decrease seconds")
                onClicked: panel.edit(panel.behaviour, panel.seconds - 5, panel.returnToken)
            }
            Text {
                id: secondsText
                width: 48
                anchors.verticalCenter: parent.verticalCenter
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("%1 s").arg(panel.seconds)
                color: Theme.text
                font.pixelSize: 12
            }
            Button {
                id: plus
                width: 32; height: 26
                text: "+"
                enabled: panel.editable && panel.seconds < 300
                Accessible.name: qsTr("Increase seconds")
                onClicked: panel.edit(panel.behaviour, panel.seconds + 5, panel.returnToken)
            }
        }

        Row {
            visible: panel.behaviour !== "always"
            width: panel.width
            spacing: 4
            Text {
                width: panel.width - 4 - returnPicker.width
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Return to")
                color: Theme.text
                font.pixelSize: 12
            }
            ComboBox {
                id: returnPicker
                width: 150
                height: 28
                model: BoschTour.presets
                textRole: "name"
                currentIndex: panel.presetIndex(panel.returnToken)
                displayText: currentIndex < 0 ? qsTr("Choose position") : currentText
                enabled: panel.editable && BoschTour.presets.length > 0
                Accessible.name: qsTr("Return position")
                onActivated: (i) => panel.edit(panel.behaviour, panel.seconds,
                                               String(BoschTour.presets[i].token))
            }
        }

        Text {
            width: panel.width
            text: panel.behaviour === "fixed" ? qsTr("Starts on the first target. Returns, then waits for another.")
                : panel.behaviour === "still" ? qsTr("After following, returns when the camera stays still.")
                : qsTr("Keeps following until you turn it off or steer.")
            color: Theme.textMuted
            font.pixelSize: 11
            wrapMode: Text.Wrap
        }

        Row {
            width: panel.width
            spacing: 8
            Text {
                width: panel.width - 8 - saveButton.width
                anchors.verticalCenter: parent.verticalCenter
                text: BoschTour.trackingNote
                color: Theme.textMuted
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }
            Button {
                id: saveButton
                height: 28
                text: qsTr("Save")
                enabled: panel.editable && BoschTour.trackingDirty
                onClicked: BoschTour.saveTracking()
            }
        }
    }

    Rectangle { width: panel.width; height: 1; color: Theme.border }

    Row {
        width: panel.width
        spacing: 8
        Column {
            width: panel.width - 8 - trackingSwitch.width
            anchors.verticalCenter: parent.verticalCenter
            Text { text: qsTr("Enable tracking"); color: Theme.text; font.pixelSize: 12; font.bold: true }
            Text {
                width: parent.width
                text: panel.statusText
                color: BoschTour.trackingStatus === "error" ? Theme.danger : Theme.textMuted
                font.pixelSize: 11
                elide: Text.ElideRight
            }
        }
        Switch {
            id: trackingSwitch
            checked: BoschTour.trackingOn
            enabled: BoschTour.trackingCanSwitch
            Accessible.name: qsTr("Enable tracking")
            onToggled: {
                BoschTour.switchTracking(checked);
                checked = Qt.binding(function () { return BoschTour.trackingOn; });
            }
        }
    }
}
