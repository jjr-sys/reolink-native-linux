import QtQuick
import ReolinkApp

// Steering the Bosch by dragging its picture with the mouse (bosch-ptz-port ticket 04), as the
// Outpost app does by finger. Press anywhere: a ring marks the spot, and the further the cursor
// goes from it the faster the camera turns (full speed at `reach` px). The strip on the right
// edge zooms: drag its thumb up to zoom in, down to zoom out; it springs back on release.
// Releasing the button stops the camera. Shown while the pane's PTZ controls (⇅) are on.
Item {
    id: surface
    // The cursor distance for full speed (the phone's default joystick travel).
    property real reach: 192
    readonly property real railWidth: 42
    readonly property real thumbRadius: 16
    // Moves are allowed now: connected and the tour state known (a tour is stopped first).
    readonly property bool steerable: BoschTour.connected && !BoschTour.busy
        && (BoschTour.tourState === "stopped" || BoschTour.touring || BoschTour.selectionTourMode)

    // What the held button is doing: "", "steer" or "zoom".
    property string gesture: ""
    property point origin: Qt.point(0, 0)
    property point cursor: Qt.point(0, 0)
    property bool moved: false   // a move was asked for: release sends a stop
    property real ringOpacity: 0

    readonly property real stripHalfTravel: Math.max(1, height / 2 - thumbRadius)
    readonly property real stripCentreX: width - railWidth / 2

    function release() {
        if (moved)
            BoschTour.stopCamera();
        moved = false;
        gesture = "";
        fade.restart();
    }

    Component.onDestruction: release()

    // The zoom strip: a faint rail with end marks and a thumb at its middle.
    Rectangle {
        x: surface.width - surface.railWidth
        width: surface.railWidth
        height: surface.height
        color: "#2610212c"
        opacity: surface.steerable ? 1 : 0.4

        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            y: 24
            width: 1
            height: parent.height - 48
            color: "#80ffffff"
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            y: 2
            text: "+"
            color: Theme.text
            font.pixelSize: 14
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 2
            text: "−"
            color: Theme.text
            font.pixelSize: 14
        }
        Rectangle {
            id: zoomThumb
            width: surface.thumbRadius * 2
            height: width
            radius: width / 2
            anchors.horizontalCenter: parent.horizontalCenter
            y: surface.height / 2 - height / 2
               + (surface.gesture === "zoom"
                  ? Math.max(-surface.stripHalfTravel,
                             Math.min(surface.stripHalfTravel, surface.cursor.y - surface.origin.y))
                  : 0)
            color: "#4dffffff"
            border.color: "white"
            Behavior on y {
                enabled: surface.gesture !== "zoom"
                NumberAnimation { duration: 150; easing.type: Easing.OutBack }
            }
        }
    }

    // The steering ring at the press point, and the thumb under the cursor.
    Item {
        opacity: surface.gesture === "steer" ? 1 : surface.ringOpacity
        visible: opacity > 0

        Rectangle {
            x: surface.origin.x - 56; y: surface.origin.y - 56
            width: 112; height: 112; radius: 56
            color: "#14ffffff"
            border.color: "#b3ffffff"
        }
        Rectangle {
            x: surface.origin.x - 32; y: surface.origin.y - 32
            width: 64; height: 64; radius: 32
            color: "transparent"
            border.color: "#40ffffff"
        }
        Rectangle {
            readonly property point at: surface.gesture === "steer" ? surface.cursor : surface.origin
            x: at.x - 16; y: at.y - 16
            width: 32; height: 32; radius: 16
            color: "#4dffffff"
            border.color: "white"
        }
    }

    NumberAnimation {
        id: fade
        target: surface
        property: "ringOpacity"
        from: 1
        to: 0
        duration: 1000
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        enabled: surface.steerable || pressed   // held through a tour's stop
        cursorShape: surface.gesture !== "" ? Qt.ClosedHandCursor : Qt.OpenHandCursor
        preventStealing: true
        propagateComposedEvents: true   // a click selects the pane; a double-click maximises

        onPressed: (m) => {
            const onStrip = m.x >= surface.width - surface.railWidth;
            if (onStrip && Math.abs(m.y - surface.height / 2) > surface.thumbRadius) {
                m.accepted = true;   // the rail away from its thumb does nothing
                return;
            }
            fade.stop();
            surface.ringOpacity = 0;
            surface.gesture = onStrip ? "zoom" : "steer";
            surface.origin = Qt.point(m.x, m.y);
            surface.cursor = surface.origin;
        }
        onPositionChanged: (m) => {
            if (surface.gesture === "")
                return;
            surface.cursor = Qt.point(m.x, m.y);
            surface.moved = true;
            if (surface.gesture === "zoom")
                BoschTour.zoomDrag(m.y - surface.origin.y, surface.stripHalfTravel);
            else
                BoschTour.steerDrag(m.x - surface.origin.x, m.y - surface.origin.y, surface.reach);
        }
        onReleased: surface.release()
        onCanceled: surface.release()
    }

    // "Touring · <step>" or "Manual · tour stopped" (lascelles-bosch-tour ticket 03), as the
    // pad showed it.
    Rectangle {
        visible: BoschTour.tourStatus !== ""
        anchors.horizontalCenter: parent.horizontalCenter
        y: 8
        width: statusText.implicitWidth + 16
        height: statusText.implicitHeight + 6
        radius: Theme.radius
        color: "#e60d141b"
        border.color: Theme.border
        Text {
            id: statusText
            anchors.centerIn: parent
            text: BoschTour.tourStatus
            color: BoschTour.manualStopped ? Theme.accent : Theme.textMuted
            font.pixelSize: 11
        }
    }
}
