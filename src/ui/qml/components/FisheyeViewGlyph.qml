import QtQuick
import ReolinkApp

// The icon of one fisheye view (as in the approved layout design,
// wayfinder/fisheye-and-framing prototypes/fisheye-layout): a circle, four squares, a camera
// frame, a strip, two strips, a circle beside a frame. Drawn on a 20 x 20 grid.
Canvas {
    id: glyph
    property string view: "fisheye"
    property color color: Theme.text
    implicitWidth: 20
    implicitHeight: 20
    onViewChanged: requestPaint()
    onColorChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()

    function roundRect(ctx, x, y, w, h, r) {
        ctx.beginPath();
        ctx.moveTo(x + r, y);
        ctx.arcTo(x + w, y, x + w, y + h, r);
        ctx.arcTo(x + w, y + h, x, y + h, r);
        ctx.arcTo(x, y + h, x, y, r);
        ctx.arcTo(x, y, x + w, y, r);
        ctx.closePath();
        ctx.stroke();
    }
    onPaint: {
        var ctx = getContext("2d");
        ctx.reset();
        var s = Math.min(width, height) / 20;
        ctx.translate((width - 20 * s) / 2, (height - 20 * s) / 2);
        ctx.scale(s, s);
        ctx.strokeStyle = glyph.color;
        ctx.fillStyle = glyph.color;
        ctx.lineWidth = 1.6;
        switch (glyph.view) {
        case "fisheye":
            ctx.beginPath(); ctx.arc(10, 10, 7, 0, 2 * Math.PI); ctx.stroke();
            ctx.beginPath(); ctx.arc(10, 10, 2.5, 0, 2 * Math.PI); ctx.fill();
            break;
        case "quad":
            ctx.strokeRect(2.5, 2.5, 6.5, 6.5); ctx.strokeRect(11, 2.5, 6.5, 6.5);
            ctx.strokeRect(2.5, 11, 6.5, 6.5); ctx.strokeRect(11, 11, 6.5, 6.5);
            break;
        case "ptz":
            roundRect(ctx, 2.5, 4.5, 15, 11, 1.5);
            ctx.beginPath(); ctx.moveTo(10, 7.5); ctx.lineTo(10, 12.5);
            ctx.moveTo(7.5, 10); ctx.lineTo(12.5, 10); ctx.stroke();
            break;
        case "panorama":
            roundRect(ctx, 1.5, 7, 17, 6, 1);
            break;
        case "dual":
            roundRect(ctx, 2, 4, 16, 5, 1); roundRect(ctx, 2, 11, 16, 5, 1);
            break;
        case "fisheye_ptz":
            ctx.beginPath(); ctx.arc(6, 10, 4.5, 0, 2 * Math.PI); ctx.stroke();
            roundRect(ctx, 11.5, 5, 7, 10, 1);
            break;
        }
    }
}
