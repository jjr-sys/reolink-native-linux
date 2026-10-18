import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ReolinkApp
import ReolinkApp.Core

// Create or edit a named mosaic: its name, which cameras it shows, how they are grouped
// and the order of the Sites. A new mosaic's cameras keep the grid arrangement's order;
// cameras ticked later go at the end. `saved(id)` reports the mosaic written.
Dialog {
    id: dialog
    modal: true
    width: 560
    height: Math.min(680, (parent ? parent.height : 680) - 40)
    anchors.centerIn: Overlay.overlay

    property string mosaicId: ""     // "" = a new mosaic
    property var ticked: ({})        // "hostId:channel" -> true
    property var siteOrder: []
    property string grouping: "site"

    signal saved(string id)

    // `seed` ticks those cameras in a new mosaic.
    function openNew(seed) {
        mosaicId = "";
        nameField.text = Mosaics.nextName();
        var t = ({});
        for (var i = 0; i < (seed || []).length; ++i)
            t[seed[i]] = true;
        ticked = t;
        grouping = "site";
        siteOrder = Mosaics.sites([]);
        open();
        nameField.selectAll();
        nameField.forceActiveFocus();
    }
    function openEdit(id) {
        var m = Mosaics.find(id);
        if (!m)
            return;
        mosaicId = id;
        nameField.text = m.name;
        var t = ({});
        for (var i = 0; i < m.cameras.length; ++i)
            t[m.cameras[i]] = true;
        ticked = t;
        grouping = m.grouping;
        siteOrder = Mosaics.sites(m.siteOrder);
        open();
    }

    // The cameras of each Site, in Site order: [{ site, cams: [{ key, name }] }].
    readonly property var groups: {
        var by = ({});
        for (var r = 0; r < Devices.count; ++r) {
            var c = Devices.cameraInfo(r);
            var s = Mosaics.siteOf(c.hostName);
            (by[s] = by[s] || []).push({ key: c.hostId + ":" + c.channel, name: c.name });
        }
        return siteOrder.filter(s => by[s]).map(s => ({ site: s, cams: by[s] }));
    }
    function setTicked(key, on) {
        var t = Object.assign({}, ticked);
        if (on) t[key] = true; else delete t[key];
        ticked = t;
    }
    function moveSite(i, dir) {
        var j = i + dir;
        if (j < 0 || j >= siteOrder.length)
            return;
        var s = siteOrder.slice();
        var x = s[i]; s[i] = s[j]; s[j] = x;
        siteOrder = s;
    }
    readonly property int tickedCount: Object.keys(ticked).length

    function save() {
        var name = nameField.text.trim() || Mosaics.nextName();
        var old = mosaicId ? Mosaics.find(mosaicId) : null;
        // Keep the mosaic's own order, then add newly ticked cameras in grid order.
        var cams = (old ? old.cameras : []).filter(k => ticked[k]);
        var grid = Mosaics.gridOrder();
        var added = Object.keys(ticked).filter(k => cams.indexOf(k) < 0);
        added.sort((a, b) => {
            var ia = grid.indexOf(a), ib = grid.indexOf(b);
            return (ia < 0 ? 1e6 : ia) - (ib < 0 ? 1e6 : ib);
        });
        var fields = { name: name, cameras: cams.concat(added), grouping: grouping,
                       siteOrder: siteOrder };
        var id = mosaicId;
        if (id)
            Mosaics.update(id, fields);
        else
            id = Mosaics.add(fields);
        close();
        saved(id);
    }

    title: mosaicId ? qsTr("Edit mosaic") : qsTr("New mosaic")
    background: Rectangle { color: Theme.surface; border.color: Theme.border; radius: Theme.radius }
    header: Item {
        implicitHeight: hdrText.implicitHeight + 22
        Text {
            id: hdrText
            x: Theme.spacing; width: parent.width - Theme.spacing * 2
            anchors.verticalCenter: parent.verticalCenter
            text: dialog.title; color: Theme.text
            font.pixelSize: 16; font.bold: true; elide: Text.ElideRight
        }
    }

    component Btn: Rectangle {
        property string text
        property bool primary: false
        signal tapped()
        implicitWidth: btnTxt.implicitWidth + 24; implicitHeight: 32; radius: Theme.radius
        color: btnHover.hovered ? (primary ? Theme.accent : Theme.surfaceAlt)
                                : (primary ? Theme.accentDim : "transparent")
        border.color: primary ? Theme.accent : Theme.border
        opacity: enabled ? 1 : 0.5
        Text { id: btnTxt; anchors.centerIn: parent; text: parent.text; color: Theme.text; font.pixelSize: 12 }
        HoverHandler { id: btnHover }
        TapHandler { onTapped: parent.tapped() }
    }
    component SmallBtn: Rectangle {
        property string text
        signal tapped()
        width: 24; height: 22; radius: 4
        color: smHover.hovered && enabled ? Theme.surfaceAlt : "transparent"
        border.color: Theme.border
        opacity: enabled ? 1 : 0.35
        Text { anchors.centerIn: parent; text: parent.text; color: Theme.textMuted; font.pixelSize: 10 }
        HoverHandler { id: smHover }
        TapHandler { onTapped: parent.tapped() }
    }
    component Check: Rectangle {
        property bool on: false
        property bool partial: false
        width: 16; height: 16; radius: 3
        color: on ? Theme.accent : Theme.surfaceAlt
        border.color: on ? Theme.accent : Theme.border
        Text {
            anchors.centerIn: parent
            visible: parent.on || parent.partial
            text: parent.on ? "✓" : "–"
            color: parent.on ? Theme.window : Theme.textMuted
            font.pixelSize: 11; font.bold: true
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spacing

        TextField {
            id: nameField
            Layout.fillWidth: true
            placeholderText: qsTr("Name")
            color: Theme.text
            placeholderTextColor: Theme.textMuted
            background: Rectangle {
                color: Theme.surfaceAlt
                border.color: nameField.activeFocus ? Theme.accent : Theme.border
                radius: 4
            }
            onAccepted: dialog.save()
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing
            Text { text: qsTr("Group"); color: Theme.textMuted; font.pixelSize: 12 }
            Repeater {
                model: [
                    { key: "site", label: qsTr("By Site") },
                    { key: "order", label: qsTr("Your order") },
                    { key: "shape", label: qsTr("By shape") }
                ]
                Rectangle {
                    required property var modelData
                    implicitWidth: gTxt.implicitWidth + 20; implicitHeight: 26; radius: Theme.radius
                    color: dialog.grouping === modelData.key ? Theme.accentDim
                         : gHover.hovered ? Theme.surfaceAlt : Theme.surface
                    border.color: Theme.border
                    Text {
                        id: gTxt; anchors.centerIn: parent; text: parent.modelData.label; font.pixelSize: 12
                        color: dialog.grouping === parent.modelData.key ? Theme.text : Theme.textMuted
                    }
                    HoverHandler { id: gHover }
                    TapHandler { onTapped: dialog.grouping = parent.modelData.key }
                }
            }
            Item { Layout.fillWidth: true }
        }

        Text {
            Layout.fillWidth: true
            text: dialog.grouping === "site"
                  ? qsTr("Cameras are grouped by Site, Sites in the order below (▲ ▼ to move).")
                  : dialog.grouping === "order"
                  ? qsTr("Cameras keep the mosaic's own order, seeded from your grid arrangement.")
                  : qsTr("Cameras of the same picture shape are packed next to each other.")
            color: Theme.textMuted; font.pixelSize: 11; wrapMode: Text.WordWrap
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.surfaceAlt
            border.color: Theme.border
            radius: Theme.radius

            ScrollView {
                id: scroll
                anchors.fill: parent
                anchors.margins: 6
                clip: true
                contentWidth: availableWidth

                Column {
                    width: scroll.availableWidth
                    spacing: 2
                    Repeater {
                        model: dialog.groups
                        Column {
                            id: siteBlock
                            required property var modelData
                            required property int index
                            readonly property int on: modelData.cams.filter(c => dialog.ticked[c.key]).length
                            width: parent.width
                            spacing: 0

                            Rectangle {   // the Site: tick all / none, move up / down
                                width: parent.width
                                height: 30
                                radius: 4
                                color: siteHover.hovered ? Theme.surface : "transparent"
                                Row {
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.left: parent.left
                                    anchors.leftMargin: 6
                                    spacing: 8
                                    Check {
                                        anchors.verticalCenter: parent.verticalCenter
                                        on: siteBlock.on === siteBlock.modelData.cams.length
                                        partial: siteBlock.on > 0
                                    }
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: siteBlock.modelData.site
                                        color: Theme.text; font.pixelSize: 13; font.bold: true
                                    }
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: qsTr("%1 of %2").arg(siteBlock.on).arg(siteBlock.modelData.cams.length)
                                        color: Theme.textMuted; font.pixelSize: 11
                                    }
                                    // On the Site's name only, so ▲ ▼ don't tick it too.
                                    TapHandler {
                                        onTapped: {
                                            var all = siteBlock.on < siteBlock.modelData.cams.length;
                                            var t = Object.assign({}, dialog.ticked);
                                            siteBlock.modelData.cams.forEach(c => {
                                                if (all) t[c.key] = true; else delete t[c.key];
                                            });
                                            dialog.ticked = t;
                                        }
                                    }
                                }
                                HoverHandler { id: siteHover }
                                Row {
                                    visible: dialog.grouping === "site"
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.right: parent.right
                                    anchors.rightMargin: 6
                                    spacing: 4
                                    SmallBtn {
                                        text: "▲"
                                        enabled: siteBlock.index > 0
                                        onTapped: dialog.moveSite(siteBlock.index, -1)
                                    }
                                    SmallBtn {
                                        text: "▼"
                                        enabled: siteBlock.index < dialog.groups.length - 1
                                        onTapped: dialog.moveSite(siteBlock.index, 1)
                                    }
                                }
                            }
                            Repeater {
                                model: siteBlock.modelData.cams
                                Rectangle {
                                    required property var modelData
                                    width: siteBlock.width
                                    height: 28
                                    radius: 4
                                    color: camHover.hovered ? Theme.surface : "transparent"
                                    Row {
                                        anchors.verticalCenter: parent.verticalCenter
                                        anchors.left: parent.left
                                        anchors.leftMargin: 30
                                        spacing: 8
                                        Check {
                                            anchors.verticalCenter: parent.verticalCenter
                                            on: dialog.ticked[parent.parent.modelData.key] === true
                                        }
                                        Text {
                                            anchors.verticalCenter: parent.verticalCenter
                                            text: parent.parent.modelData.name
                                            color: Theme.text; font.pixelSize: 12
                                        }
                                    }
                                    HoverHandler { id: camHover }
                                    TapHandler {
                                        onTapped: dialog.setTicked(parent.modelData.key,
                                                                   !dialog.ticked[parent.modelData.key])
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Text {
                text: dialog.tickedCount === 1 ? qsTr("1 camera") : qsTr("%1 cameras").arg(dialog.tickedCount)
                color: Theme.textMuted; font.pixelSize: 12
            }
            Item { Layout.fillWidth: true }
            Btn { text: qsTr("Cancel"); onTapped: dialog.close() }
            Btn {
                text: qsTr("Save")
                primary: true
                enabled: dialog.tickedCount > 0
                onTapped: if (enabled) dialog.save()
            }
        }
    }
}
