import QtQuick
import ReolinkApp
import ReolinkApp.Core

// PTZ joystick: 8-way directional pad (press-and-hold to move, release to Stop)
// plus optional zoom rocker. Reolink cameras use Devices; Bosch uses HA ONVIF PTZ.
Rectangle {
    id: pad
    signal openBoschTourRequested()
    property int deviceRow: -1
    property bool showZoom: true
    property bool boschCamera: false
    property bool boschMoveRequested: false
    readonly property bool movementEnabled: !boschCamera
        || (BoschTour.connected && BoschTour.tourState === "stopped" && !BoschTour.busy)

    width: 120
    height: showZoom ? 168 : 120
    radius: Theme.radius
    color: "#e60d141b"
    border.color: Theme.border

    function move(op) {
        if (boschCamera) {
            if (!movementEnabled)
                return;
            boschMoveRequested = true;
            switch (op) {
            case "Up": BoschTour.moveCamera("", "UP", ""); break;
            case "Down": BoschTour.moveCamera("", "DOWN", ""); break;
            case "Left": BoschTour.moveCamera("LEFT", "", ""); break;
            case "Right": BoschTour.moveCamera("RIGHT", "", ""); break;
            case "LeftUp": BoschTour.moveCamera("LEFT", "UP", ""); break;
            case "RightUp": BoschTour.moveCamera("RIGHT", "UP", ""); break;
            case "LeftDown": BoschTour.moveCamera("LEFT", "DOWN", ""); break;
            case "RightDown": BoschTour.moveCamera("RIGHT", "DOWN", ""); break;
            case "ZoomDec": BoschTour.moveCamera("", "", "ZOOM_OUT"); break;
            case "ZoomInc": BoschTour.moveCamera("", "", "ZOOM_IN"); break;
            }
        } else if (deviceRow >= 0) {
            Devices.ptzMove(deviceRow, op, 32);
        }
    }
    function stop() {
        if (boschCamera) {
            if (boschMoveRequested) {
                BoschTour.stopCamera();
                boschMoveRequested = false;
            }
        } else if (deviceRow >= 0) {
            Devices.ptzStop(deviceRow);
        }
    }
    Component.onDestruction: {
        stop();
    }

    Column {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 6
        spacing: 6

        // Directional ring
        Item {
            width: 104; height: 104
            anchors.horizontalCenter: parent.horizontalCenter

            Rectangle { // hub
                anchors.centerIn: parent
                width: 34; height: 34; radius: 17
                color: Theme.surfaceAlt
                border.color: Theme.border
            }

            component Dir: Rectangle {
                property string op: ""
                property string glyph: ""
                width: 30; height: 30; radius: 6
                color: press.pressed ? Theme.accentDim : Theme.surface
                opacity: pad.movementEnabled ? 1 : 0.5
                border.color: Theme.border
                Text { anchors.centerIn: parent; text: parent.glyph; color: Theme.text; font.pixelSize: 13 }
                TapHandler {
                    id: press
                    onGrabChanged: (transition, point) => {}
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: pad.movementEnabled
                    onPressed: pad.move(parent.op)
                    onReleased: pad.stop()
                    onCanceled: pad.stop()
                }
            }

            Dir { op: "Up";        glyph: "▲"; anchors.top: parent.top; anchors.horizontalCenter: parent.horizontalCenter }
            Dir { op: "Down";      glyph: "▼"; anchors.bottom: parent.bottom; anchors.horizontalCenter: parent.horizontalCenter }
            Dir { op: "Left";      glyph: "◀"; anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter }
            Dir { op: "Right";     glyph: "▶"; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter }
            Dir { op: "LeftUp";    glyph: "↖"; anchors.top: parent.top; anchors.left: parent.left }
            Dir { op: "RightUp";   glyph: "↗"; anchors.top: parent.top; anchors.right: parent.right }
            Dir { op: "LeftDown";  glyph: "↙"; anchors.bottom: parent.bottom; anchors.left: parent.left }
            Dir { op: "RightDown"; glyph: "↘"; anchors.bottom: parent.bottom; anchors.right: parent.right }
        }

        // Zoom rocker
        Row {
            visible: pad.showZoom
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 8
            component Zoom: Rectangle {
                property string op: ""
                property string glyph: ""
                width: 46; height: 30; radius: 6
                color: zpress.pressed ? Theme.accentDim : Theme.surface
                opacity: pad.movementEnabled ? 1 : 0.5
                border.color: Theme.border
                Text { anchors.centerIn: parent; text: parent.glyph; color: Theme.text; font.pixelSize: 15 }
                MouseArea {
                    id: zpress
                    anchors.fill: parent
                    enabled: pad.movementEnabled
                    onPressed: pad.move(parent.op)
                    onReleased: pad.stop()
                    onCanceled: pad.stop()
                }
            }
            Zoom { op: "ZoomDec"; glyph: "−" }
            Zoom { op: "ZoomInc"; glyph: "+" }
        }

    }
}
