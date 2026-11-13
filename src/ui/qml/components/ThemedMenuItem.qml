import QtQuick
import QtQuick.Controls
import ReolinkApp

// A MenuItem matching the app theme; use inside ThemedMenu.
MenuItem {
    id: control
    implicitHeight: 32
    implicitWidth: 180
    contentItem: Text {
        text: control.text
        color: control.enabled ? Theme.text : Theme.textMuted
        font.pixelSize: 13
        verticalAlignment: Text.AlignVCenter
        leftPadding: 8
        rightPadding: 8
        elide: Text.ElideRight
    }
    arrow: Text {   // a submenu row
        x: control.width - width - 8
        y: (control.height - height) / 2
        visible: control.subMenu !== null
        text: "▸"
        color: Theme.textMuted
        font.pixelSize: 13
    }
    background: Rectangle {
        radius: 4
        color: control.highlighted ? Theme.accentDim : "transparent"
    }
}
