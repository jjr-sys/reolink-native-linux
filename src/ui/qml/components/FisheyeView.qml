import QtQuick
import ReolinkApp

// Shows a ceiling-mounted fisheye camera as one or more dewarped views. The camera's
// picture is captured once off-screen (`sourceItem`, the pane's VideoOutput, hidden while
// this is on) and every tile samples that same texture through dewarp.frag.
//
// viewMode:  "ptz"         one virtual camera you pan/tilt/zoom
//            "panorama"    the full 360 degrees as one strip
//            "dual"        two stacked 180-degree strips (front and back)
//            "quad"        four virtual cameras facing 0/90/180/270 degrees
//            "fisheye_ptz" the raw circle beside one virtual camera
//
// The pane forwards its mouse to dragBy()/zoomBy(): drag pans a tile, the wheel zooms it.
Item {
    id: root
    property Item sourceItem: null
    property string viewMode: "ptz"

    // The lens circle inside the square frame, as fractions of its side. Measured on the
    // real Entrance and Kitchen cameras (a 2560x2560 still: 0.5, 0.5, 0.476).
    property real lensCx: 0.5
    property real lensCy: 0.5
    property real lensRadius: 0.476

    readonly property real deg: Math.PI / 180
    // Panorama strip shape: pixels wide per pixel high, and how far below the horizon
    // the strip reaches (looking down towards the floor under the camera).
    readonly property real pano360Aspect: 3.3
    readonly property real panoDualAspect: 2.1
    readonly property real panoDepth: 70 * deg

    // A 180-degree lens sees nothing above the horizon, so a virtual camera may not tilt
    // so far up that its top edge passes it. `aspect` is tile height / width.
    function maxTilt(fov, aspect) {
        return 88 * deg - Math.atan(Math.tan(fov / 2) * aspect);
    }

    // Where a click landed, so drag/zoom act on the tile under the pointer.
    function tileAt(x, y) {
        for (var i = 0; i < tiles.count; i++) {
            var t = tiles.itemAt(i);
            if (t && x >= t.x && x < t.x + t.width && y >= t.y && y < t.y + t.height)
                return t;
        }
        return null;
    }
    function dragBy(x, y, dx, dy) {
        var t = tileAt(x, y);
        if (!t)
            return;
        if (t.kind === "ptz") {
            // Grab the picture: dragging right turns the view left, dragging down looks up.
            t.pan -= dx * 0.006 * (t.fov / (90 * deg));
            t.tilt = Math.max(8 * deg, Math.min(maxTilt(t.fov, t.aspect), t.tilt + dy * 0.006 * (t.fov / (90 * deg))));
        } else if (t.kind === "pano") {
            t.phi0 -= dx / t.width * t.span;
        }
    }
    function zoomBy(x, y, zoomIn) {
        var t = tileAt(x, y);
        if (t && t.kind === "ptz") {
            t.fov = Math.max(30 * deg, Math.min(120 * deg, t.fov * (zoomIn ? 0.9 : 1.1)));
            t.tilt = Math.min(t.tilt, maxTilt(t.fov, t.aspect));   // widening must not pass the horizon
        }
    }

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

    // The tiles of the chosen mode, laid out in the pane.
    readonly property var layout: {
        var W = width, H = height, g = 2, out = [];
        var d = deg;
        if (viewMode === "ptz") {
            out.push({ kind: "ptz", x: 0, y: 0, w: W, h: H, pan: 0, tilt: Math.min(55 * d, maxTilt(90 * d, H / W)), fov: 90 * d });
        } else if (viewMode === "panorama") {
            var ph = Math.min(H, W / pano360Aspect), pw = ph * pano360Aspect;
            out.push({ kind: "pano", x: (W - pw) / 2, y: (H - ph) / 2, w: pw, h: ph, span: 2 * Math.PI, phi0: 0 });
        } else if (viewMode === "dual") {
            var th = Math.min((H - g) / 2, W / panoDualAspect), tw = th * panoDualAspect;
            var top = (H - (2 * th + g)) / 2;
            out.push({ kind: "pano", x: (W - tw) / 2, y: top, w: tw, h: th, span: Math.PI, phi0: 0 });
            out.push({ kind: "pano", x: (W - tw) / 2, y: top + th + g, w: tw, h: th, span: Math.PI, phi0: Math.PI });
        } else if (viewMode === "quad") {
            var qw = (W - g) / 2, qh = (H - g) / 2;
            for (var k = 0; k < 4; k++)
                out.push({ kind: "ptz", x: (k % 2) * (qw + g), y: Math.floor(k / 2) * (qh + g), w: qw, h: qh,
                           pan: k * 90 * d, tilt: Math.min(58 * d, maxTilt(85 * d, qh / qw)), fov: 85 * d });
        } else if (viewMode === "fisheye_ptz") {
            var s = Math.min(H, W * 0.4);
            out.push({ kind: "raw", x: 0, y: (H - s) / 2, w: s, h: s });
            out.push({ kind: "ptz", x: s + g, y: 0, w: W - s - g, h: H, pan: 0,
                       tilt: Math.min(55 * d, maxTilt(90 * d, H / (W - s - g))), fov: 90 * d });
        }
        return out;
    }

    Rectangle { anchors.fill: parent; color: "#05080b"; z: -1 }

    Repeater {
        id: tiles
        model: root.layout
        delegate: ShaderEffect {
            id: tile
            required property var modelData
            readonly property string kind: modelData.kind
            x: modelData.x; y: modelData.y; width: modelData.w; height: modelData.h

            // What the user changes by dragging or zooming.
            property real pan: modelData.pan !== undefined ? modelData.pan : 0
            property real tilt: modelData.tilt !== undefined ? modelData.tilt : 0
            property real fov: modelData.fov !== undefined ? modelData.fov : 1.5
            property real phi0: modelData.phi0 !== undefined ? modelData.phi0 : 0
            readonly property real span: modelData.span !== undefined ? modelData.span : 0

            // Shader inputs (names match dewarp.frag).
            property variant source: capture
            property real mode: kind === "raw" ? 0 : (kind === "ptz" ? 1 : 2)
            property real tanHalf: Math.tan(fov / 2)
            property real aspect: height / Math.max(1, width)
            property real eTop: 0
            property real eBot: -root.panoDepth
            property real cx: root.lensCx
            property real cy: root.lensCy
            property real rad: root.lensRadius
            fragmentShader: "qrc:/qt/qml/ReolinkApp/shaders/dewarp.frag.qsb"
        }
    }
}
