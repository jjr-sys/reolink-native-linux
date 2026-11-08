import QtQuick
import ReolinkApp

// "Cleared N cameras · Undo" for 5 s at the foot of a page. show(text); `undo` when the
// button is used, `expired` when the toast goes without it.
Rectangle {
    id: root
    signal undo()
    signal expired()

    function show(msg) {
        label.text = msg;
        opacity = 1;
        timer.restart();
    }
    function dismiss() {
        timer.stop();
        opacity = 0;
    }

    anchors.horizontalCenter: parent.horizontalCenter
    anchors.bottom: parent.bottom
    anchors.bottomMargin: 32
    z: 200
    width: row.implicitWidth + 28
    height: 38
    radius: Theme.radius
    color: Theme.surfaceAlt
    border.color: Theme.border
    opacity: 0
    visible: opacity > 0
    Behavior on opacity { NumberAnimation { duration: 180 } }

    Timer {
        id: timer
        interval: 5000
        onTriggered: { root.opacity = 0; root.expired(); }
    }
    Row {
        id: row
        anchors.centerIn: parent
        spacing: 14
        Text {
            id: label
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.text
            font.pixelSize: 13
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Undo")
            color: undoHover.hovered ? Theme.text : Theme.accent
            font.pixelSize: 13
            font.bold: true
            HoverHandler { id: undoHover; cursorShape: Qt.PointingHandCursor }
            TapHandler {
                onTapped: {
                    root.dismiss();
                    root.undo();
                }
            }
        }
    }
}
