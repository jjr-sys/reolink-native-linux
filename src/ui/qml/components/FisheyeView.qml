import QtQuick
import ReolinkApp
import ReolinkApp.Core

// Shows a ceiling-mounted fisheye camera as one or more dewarped views. The camera's
// picture is captured once off-screen (`sourceItem`, the pane's VideoOutput, hidden while
// this is on) and every tile samples that same texture through dewarp.frag.
//
// viewMode:  "ptz"         one virtual camera you pan/tilt/zoom
//            "panorama"    the full 360 degrees as one strip
//            "dual"        two stacked 180-degree strips (front and back)
//            "quad"        four 120-degree virtual cameras facing 0/90/180/270 degrees
//            "fisheye_ptz" the raw circle beside one virtual camera
//
// The tiles and their aim come from FisheyeAims (C++, tested): one aim per camera and view,
// shared by every page (wayfinder/playback-controls ticket 13). The pane forwards its mouse to
// dragBy()/zoomBy(): drag turns the tile under the pointer, the wheel zooms it.
Item {
    id: root
    property Item sourceItem: null
    property string viewMode: "ptz"
    property string cameraKey: ""    // "hostId:channel"

    // The lens circle inside the square frame (as fisheye::kLensCx/Cy/Radius).
    readonly property real lensCx: 0.5
    readonly property real lensCy: 0.5
    readonly property real lensRadius: 0.476
    readonly property real panoDepth: 70 * Math.PI / 180

    readonly property var layout: {
        FisheyeAims.revision;   // re-read when any aim changes
        return FisheyeAims.tileList(cameraKey, viewMode, width, height);
    }

    function dragBy(x, y, dx, dy) { FisheyeAims.drag(cameraKey, viewMode, width, height, x, y, dx, dy); }
    function zoomBy(x, y, zoomIn) { FisheyeAims.zoom(cameraKey, viewMode, width, height, x, y, zoomIn); }

    // One texture for all the tiles. Square: these cameras send a square frame, and the
    // shader works in frame coordinates, not pane coordinates.
    ShaderEffectSource {
        id: capture
        sourceItem: root.sourceItem
        hideSource: true
        live: true
        visible: false
        textureSize: Qt.size(1024, 1024)
    }

    Rectangle { anchors.fill: parent; color: Theme.paneBackground; z: -1 }

    // By count, so an aim change updates the tiles in place instead of rebuilding them.
    Repeater {
        model: root.layout.length
        delegate: ShaderEffect {
            required property int index
            readonly property var t: root.layout[index] || ({})
            x: t.x || 0; y: t.y || 0; width: t.w || 0; height: t.h || 0

            // Shader inputs (names match dewarp.frag).
            property variant source: capture
            property real mode: t.kind === "raw" ? 0 : (t.kind === "ptz" ? 1 : 2)
            property real pan: t.pan || 0
            property real tilt: t.tilt || 0
            property real tanHalf: Math.tan((t.fov || 1.5) / 2)
            property real aspect: height / Math.max(1, width)
            property real phi0: t.phi0 || 0
            property real span: t.span || 0
            property real eTop: 0
            property real eBot: -root.panoDepth
            property real cx: root.lensCx
            property real cy: root.lensCy
            property real rad: root.lensRadius
            fragmentShader: "qrc:/qt/qml/ReolinkApp/shaders/dewarp.frag.qsb"
        }
    }
}
