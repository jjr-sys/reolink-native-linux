pragma Singleton
import QtQuick

// The six fisheye views in menu order, with their names (one list for the icons and menus).
QtObject {
    readonly property var views: [
        { view: "fisheye",     name: qsTr("Fisheye (original)") },
        { view: "quad",        name: qsTr("Quad view (4 directions)") },
        { view: "ptz",         name: qsTr("Virtual PTZ") },
        { view: "panorama",    name: qsTr("Panorama 360°") },
        { view: "dual",        name: qsTr("Dual panorama (2 × 180°)") },
        { view: "fisheye_ptz", name: qsTr("Fisheye + virtual PTZ") }
    ]
}
