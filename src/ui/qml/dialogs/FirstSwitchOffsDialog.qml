import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ReolinkApp
import ReolinkApp.Core

// The first switch-offs (wayfinder/events-feed): shown once after the build that brings
// event switches, listing the agreed (camera, type) pairs from settings with one
// confirmation of what it deletes. Switch off applies them all; Cancel leaves everything
// on. Either way it isn't shown again (the noise card and ⋯ take over). Only the two
// buttons close it, so a stray Esc doesn't count as an answer.
Dialog {
    id: dialog
    modal: true
    width: 480
    anchors.centerIn: Overlay.overlay
    closePolicy: Popup.NoAutoClose

    property var pending: ({})   // Events.firstSwitchOffs()

    title: qsTr("Switch off the noisy event types?")
    background: Rectangle { color: Theme.surface; border.color: Theme.border; radius: Theme.radius }
    header: Item {
        implicitHeight: 46
        Text {
            anchors.left: parent.left
            anchors.leftMargin: Theme.spacing * 2
            anchors.verticalCenter: parent.verticalCenter
            text: dialog.title
            color: Theme.text
            font.pixelSize: 15
            font.bold: true
        }
    }

    function typeName(t) {
        return { person: qsTr("Person"), vehicle: qsTr("Vehicle"), pet: qsTr("Pet"),
                 motion: qsTr("Motion"), visitor: qsTr("Visitor") }[t] || t;
    }
    function number(n) { return Number(n).toLocaleString(Qt.locale(), "f", 0); }
    function size(bytes) {
        if (bytes >= 1e9) return qsTr("%1 GB").arg((bytes / 1e9).toFixed(1));
        if (bytes >= 1e6) return qsTr("%1 MB").arg(Math.round(bytes / 1e6));
        return qsTr("%1 KB").arg(Math.max(1, Math.round(bytes / 1e3)));
    }
    // One line per type: its cameras grouped by recorder (or, past six, just how many
    // and where) and the events it deletes.
    function lines() {
        var byType = {}, order = [];
        var pairs = dialog.pending.pairs || [];
        for (var i = 0; i < pairs.length; ++i) {
            var p = pairs[i];
            if (!byType[p.type]) { byType[p.type] = { cams: [], count: 0 }; order.push(p.type); }
            byType[p.type].cams.push(p);
            byType[p.type].count += p.count;
        }
        var out = [];
        for (var j = 0; j < order.length; ++j) {
            var g = byType[order[j]], recorders = [], camsOf = {};
            for (var k = 0; k < g.cams.length; ++k) {
                var c = g.cams[k];
                if (!camsOf[c.recorder]) { camsOf[c.recorder] = []; recorders.push(c.recorder); }
                camsOf[c.recorder].push(c.camera);
            }
            var where = g.cams.length > 6
                ? qsTr("%1 cameras on %2").arg(g.cams.length).arg(recorders.join(", "))
                : recorders.map(function (r) { return r + " " + camsOf[r].join(", "); }).join("; ");
            out.push({ type: dialog.typeName(order[j]), where: where,
                       count: qsTr("%1 events").arg(dialog.number(g.count)) });
        }
        return out;
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spacing
        Text {
            Layout.fillWidth: true
            text: qsTr("These stop being logged here: no events, snapshots, notifications or highlights.")
            color: Theme.text; font.pixelSize: 13; wrapMode: Text.WordWrap
        }
        Repeater {
            model: dialog.visible ? dialog.lines() : []
            delegate: ColumnLayout {
                required property var modelData
                Layout.fillWidth: true
                spacing: 1
                Text {
                    Layout.fillWidth: true
                    text: "<b>" + modelData.type + "</b> · " + modelData.count
                    textFormat: Text.StyledText
                    color: Theme.text; font.pixelSize: 13
                }
                Text {
                    Layout.fillWidth: true
                    text: modelData.where
                    color: Theme.textMuted; font.pixelSize: 12; wrapMode: Text.WordWrap
                }
            }
        }
        Text {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spacing / 2
            text: qsTr("Deletes %1 stored events (%2). This can't be undone. The recorders keep recording.")
                      .arg(dialog.number(dialog.pending.count || 0)).arg(dialog.size(dialog.pending.bytes || 0))
            color: Theme.danger; font.pixelSize: 12; wrapMode: Text.WordWrap
        }
        Text {
            Layout.fillWidth: true
            text: qsTr("Cancel keeps logging everything. Either way this isn't asked again.")
            color: Theme.textMuted; font.pixelSize: 12; wrapMode: Text.WordWrap
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spacing
            Item { Layout.fillWidth: true }
            Rectangle {
                implicitWidth: cxTxt.implicitWidth + 24; implicitHeight: 32; radius: Theme.radius
                color: cxHover.hovered ? Theme.surfaceAlt : "transparent"
                border.color: Theme.border
                Text { id: cxTxt; anchors.centerIn: parent; text: qsTr("Cancel"); color: Theme.text; font.pixelSize: 12 }
                HoverHandler { id: cxHover }
                TapHandler { onTapped: { Events.declineFirstSwitchOffs(); dialog.close(); } }
            }
            Rectangle {
                implicitWidth: okTxt.implicitWidth + 24; implicitHeight: 32; radius: Theme.radius
                color: okHover.hovered ? Theme.danger : Theme.surfaceAlt
                border.color: Theme.danger
                Text { id: okTxt; anchors.centerIn: parent; text: qsTr("Switch off"); color: Theme.text; font.pixelSize: 12 }
                HoverHandler { id: okHover }
                TapHandler { onTapped: { Events.confirmFirstSwitchOffs(); dialog.close(); } }
            }
        }
    }
}
