import QtQuick
import ReolinkApp

// "View": the six fisheye views (✓ on the current one) and Reset aim. A pane's right-click
// menu, or a submenu ("View ▸") of a bigger menu such as an Events card's
// (wayfinder/playback-controls ticket 13). Shared by Live, Playback and Events.
ThemedMenu {
    id: root
    title: qsTr("View")
    property string current: "fisheye"
    property bool aimed: false      // the current view has been turned: Reset aim does something
    signal picked(string view)
    signal resetAim()

    Repeater {
        model: FisheyeViews.views
        ThemedMenuItem {
            required property var modelData
            text: (root.current === modelData.view ? "✓  " : "     ") + modelData.name
            onTriggered: root.picked(modelData.view)
        }
    }
    ThemedMenuSeparator {}
    ThemedMenuItem {
        text: "     " + qsTr("↺ Reset aim")
        enabled: root.aimed
        onTriggered: root.resetAim()
    }
}
