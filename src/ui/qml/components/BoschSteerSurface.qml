import QtQuick
import ReolinkApp
import ReolinkApp.Core

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
    readonly property real thumbGrab: 24   // the thumb can be taken this far from its middle
    readonly property real ringRadius: 56
    // Moves are allowed now: connected and the tour state known (a tour is stopped first).
    readonly property bool steerable: BoschTour.connected && !BoschTour.busy
        && (BoschTour.tourState === "stopped" || BoschTour.touring || BoschTour.selectionTourMode)

    // What the held button is doing: "", "steer" or "zoom".
    property string gesture: ""
    property point origin: Qt.point(0, 0)
    property point cursor: Qt.point(0, 0)
    property real ringOpacity: 0

    readonly property real stripHalfTravel: Math.max(1, height / 2 - thumbGrab)

    function release() {
        const steered = gesture === "steer";
        if (gesture !== "")
            BoschTour.stopCamera();   // stops only if the drag moved the camera
        gesture = "";
        if (steered)   // the ring fades after steering only
            fade.restart();
    }

    Component.onDestruction: release()

    // The zoom strip: a faint rail with end marks and a thumb at its middle.
    Rectangle {
        x: surface.width - surface.railWidth
        width: surface.railWidth
        height: surface.height
        color: Theme.steerRail
        opacity: surface.steerable ? 1 : 0.4

        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            y: 24
            width: 1
            height: parent.height - 48
            color: Theme.steerMarkSoft
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
            color: Theme.steerFill
            border.color: Theme.steerMark
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

        component Mark: Rectangle {
            property point centre
            property real size
            x: centre.x - size / 2
            y: centre.y - size / 2
            width: size
            height: size
            radius: size / 2
        }
        Mark {   // full speed is further out than this; the ring only marks the spot
            centre: surface.origin
            size: surface.ringRadius * 2
            color: Theme.steerRingFill
            border.color: Theme.steerMarkSoft
        }
        Mark {
            centre: surface.origin
            size: surface.ringRadius * 8 / 7
            color: "transparent"
            border.color: Theme.steerMarkFaint
        }
        Mark {
            centre: surface.gesture === "steer" ? surface.cursor : surface.origin
            size: surface.thumbRadius * 2
            color: Theme.steerFill
            border.color: Theme.steerMark
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
        // Always takes the press while ⇅ is on, so a drag never picks the pane up instead;
        // it only steers once Home Assistant is ready.
        cursorShape: surface.gesture !== "" ? Qt.ClosedHandCursor : Qt.OpenHandCursor
        preventStealing: true
        propagateComposedEvents: true   // a click selects the pane; a double-click maximises

        onPressed: (m) => {
            const onStrip = m.x >= surface.width - surface.railWidth;
            if (!surface.steerable
                || (onStrip && Math.abs(m.y - surface.height / 2) > surface.thumbGrab))
                return;   // taken, but nothing to do: not ready, or the rail away from its thumb
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
        color: Theme.paneBar
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
