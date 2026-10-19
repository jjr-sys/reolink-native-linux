import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ReolinkApp
import ReolinkApp.Core

// Settings → Detection highlights: one Highlight rule per detection type, each with its
// cameras, look, Alert sound and per-camera exceptions, plus the alert volume. Every edit
// goes straight to Highlights.rules (saved as JSON) and so to Live View at once.
ScrollView {
    id: root
    clip: true
    contentWidth: availableWidth

    readonly property var types: [
        { key: "person", label: qsTr("Person") },
        { key: "vehicle", label: qsTr("Vehicle") },
        { key: "pet", label: qsTr("Pet") },
        { key: "motion", label: qsTr("Motion") }
    ]
    readonly property var colours: [
        { key: "#f5c518", label: qsTr("Yellow") }, { key: "#f0a030", label: qsTr("Orange") },
        { key: "#e5484d", label: qsTr("Red") }, { key: "#d946ef", label: qsTr("Magenta") },
        { key: "#3d8bfd", label: qsTr("Blue") }, { key: "#22c1c3", label: qsTr("Teal") },
        { key: "#3cc46a", label: qsTr("Green") }, { key: "#9aa0a6", label: qsTr("Grey") },
        { key: "#ffffff", label: qsTr("White") }
    ]
    readonly property var styles: [
        { key: "flash-then-steady", label: qsTr("Flash 3 s, then steady") },
        { key: "steady", label: qsTr("Steady") },
        { key: "flashing", label: qsTr("Flashing") }
    ]
    readonly property var soundOptions: [{ key: "", label: qsTr("No sound") }].concat(
        AlertSounds.sounds.map(function (s) { return { key: s, label: AlertSounds.label(s) }; }))
    readonly property var exceptionSounds: [{ key: "", label: qsTr("Rule's sound") },
                                            { key: "none", label: qsTr("Silent") }].concat(soundOptions.slice(1))
    readonly property var exceptionColours: [{ key: "", label: qsTr("Rule's colour") }].concat(colours)

    property var rules: JSON.parse(Highlights.rules)
    Connections {
        target: Highlights
        function onRulesChanged() { root.rules = JSON.parse(Highlights.rules); }
    }
    // `change` edits a copy of one type's rule; the result is saved at once.
    function edit(type, change) {
        var r = JSON.parse(JSON.stringify(rules));
        change(r[type]);
        Highlights.rules = JSON.stringify(r);
    }
    function indexOf(list, key) {
        for (var i = 0; i < list.length; ++i)
            if (list[i].key === key)
                return i;
        return 0;
    }

    // Cameras a rule can cover: those with a detection source (not a direct stream).
    function cameras() {
        var out = [];
        for (var i = 0; i < Devices.count; ++i) {
            var c = Devices.cameraInfo(i);
            if (c.kind === "stream" || c.hostId === undefined)
                continue;
            out.push({ key: c.hostId + ":" + c.channel, label: c.name + " · " + c.hostName });
        }
        return out;
    }
    property var cameraList: cameras()
    Connections {
        target: Devices
        function onCountChanged() { root.cameraList = root.cameras(); }
    }
    function cameraName(key) {
        for (var i = 0; i < cameraList.length; ++i)
            if (cameraList[i].key === key)
                return cameraList[i].label;
        return key;
    }

    component FieldLabel: Text {
        color: Theme.textMuted
        font.pixelSize: 12
        Layout.preferredWidth: 150
    }
    component SmallBtn: Rectangle {
        property string text
        signal tapped()
        implicitWidth: Math.max(28, btnTxt.implicitWidth + 16); implicitHeight: 26; radius: 4
        color: btnHover.hovered && enabled ? Theme.surface : "transparent"
        border.color: Theme.border
        opacity: enabled ? 1 : 0.35
        Text { id: btnTxt; anchors.centerIn: parent; text: parent.text; color: Theme.text; font.pixelSize: 12 }
        HoverHandler { id: btnHover }
        TapHandler { onTapped: parent.tapped() }
    }
    component Picker: ComboBox {
        property var options: []
        property string value: ""
        signal picked(string key)
        Layout.preferredWidth: 220
        model: options
        textRole: "label"
        currentIndex: root.indexOf(options, value)
        onActivated: (i) => picked(options[i].key)
    }

    ColumnLayout {
        width: root.availableWidth
        spacing: Theme.spacing * 1.5

        Text {
            Layout.fillWidth: true
            text: qsTr("When a camera detects one of these, its panes in Live View get a coloured border for a while, and a sound can play. Detections arrive up to 10 seconds late. The Highlights switch in Live View turns it all off.")
            color: Theme.textMuted; font.pixelSize: 12; wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing
            FieldLabel { text: qsTr("Alert volume") }
            Slider {
                id: volume
                Layout.preferredWidth: 220
                from: 0; to: 100; stepSize: 1
                value: Math.round(AlertSounds.volume * 100)
                onMoved: AlertSounds.volume = value / 100
            }
            Text { text: Math.round(volume.value) + "%"; color: Theme.text; font.pixelSize: 12; Layout.preferredWidth: 40 }
            SmallBtn { text: "▶"; onTapped: AlertSounds.test("chime") }
            Item { Layout.fillWidth: true }
        }

        Repeater {
            model: root.types
            Rectangle {
                id: card
                required property var modelData
                readonly property string type: modelData.key
                readonly property var rule: root.rules[type]
                readonly property bool chosen: Array.isArray(rule.cameras)
                readonly property var exceptions: Object.keys(rule.overrides || {})
                Layout.fillWidth: true
                implicitHeight: body.implicitHeight + 24
                radius: Theme.radius
                color: Theme.surfaceAlt
                border.color: Theme.border

                ColumnLayout {
                    id: body
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 8

                    RowLayout {
                        spacing: Theme.spacing
                        Switch {
                            checked: card.rule.on
                            onToggled: { var on = checked; root.edit(card.type, function (r) { r.on = on; }); }
                        }
                        Rectangle {
                            width: 14; height: 14; radius: 3
                            color: "transparent"
                            border.color: card.rule.colour
                            border.width: 3
                        }
                        Text { text: card.modelData.label; color: Theme.text; font.pixelSize: 14; font.bold: true }
                        Text {
                            visible: !card.rule.on
                            text: qsTr("off")
                            color: Theme.textMuted; font.pixelSize: 12
                        }
                    }

                    ColumnLayout {
                        visible: card.rule.on
                        Layout.fillWidth: true
                        spacing: 8

                        RowLayout {
                            spacing: Theme.spacing
                            FieldLabel { text: qsTr("Cameras") }
                            Picker {
                                options: [{ key: "all", label: qsTr("All cameras") },
                                          { key: "chosen", label: qsTr("Chosen cameras") }]
                                value: card.chosen ? "chosen" : "all"
                                onPicked: (k) => root.edit(card.type, function (r) {
                                    r.cameras = k === "all" ? "all" : [];
                                })
                            }
                            SmallBtn {
                                visible: card.chosen
                                text: card.chosen && card.rule.cameras.length > 0
                                      ? qsTr("Choose… (%1)").arg(card.rule.cameras.length) : qsTr("Choose…")
                                onTapped: chooser.open()
                                Popup {
                                    id: chooser
                                    y: parent.height + 4
                                    width: 300
                                    height: Math.min(360, list.contentHeight + 16)
                                    padding: 8
                                    background: Rectangle { color: Theme.surface; border.color: Theme.border; radius: Theme.radius }
                                    contentItem: ListView {
                                        id: list
                                        clip: true
                                        model: root.cameraList
                                        delegate: CheckDelegate {
                                            id: camBox
                                            required property var modelData
                                            contentItem: Text {
                                                text: camBox.text
                                                color: Theme.text
                                                font.pixelSize: 12
                                                rightPadding: camBox.indicator.width + camBox.spacing
                                                elide: Text.ElideRight
                                                verticalAlignment: Text.AlignVCenter
                                            }
                                            width: ListView.view.width
                                            text: modelData.label
                                            checked: card.chosen && card.rule.cameras.indexOf(modelData.key) >= 0
                                            onToggled: {
                                                var key = modelData.key, on = checked;
                                                root.edit(card.type, function (r) {
                                                    r.cameras = r.cameras.filter(function (c) { return c !== key; });
                                                    if (on)
                                                        r.cameras.push(key);
                                                });
                                            }
                                        }
                                    }
                                }
                            }
                            Text {
                                visible: card.chosen && card.rule.cameras.length === 0
                                text: qsTr("None chosen yet")
                                color: Theme.warning; font.pixelSize: 12
                            }
                        }

                        RowLayout {
                            spacing: 6
                            FieldLabel { text: qsTr("Colour") }
                            Repeater {
                                model: root.colours
                                Rectangle {
                                    required property var modelData
                                    width: 22; height: 22; radius: 4
                                    color: modelData.key
                                    border.color: card.rule.colour === modelData.key ? Theme.text : Theme.border
                                    border.width: card.rule.colour === modelData.key ? 2 : 1
                                    ToolTip.visible: swatchHover.hovered
                                    ToolTip.text: modelData.label
                                    ToolTip.delay: 400
                                    HoverHandler { id: swatchHover }
                                    TapHandler {
                                        onTapped: {
                                            var c = parent.modelData.key;
                                            root.edit(card.type, function (r) { r.colour = c; });
                                        }
                                    }
                                }
                            }
                        }

                        RowLayout {
                            spacing: Theme.spacing
                            FieldLabel { text: qsTr("Style") }
                            Picker {
                                options: root.styles
                                value: card.rule.style
                                onPicked: (k) => root.edit(card.type, function (r) { r.style = k; })
                            }
                        }

                        RowLayout {
                            spacing: Theme.spacing
                            FieldLabel { text: qsTr("Highlight for") }
                            SpinBox {
                                from: 1; to: 600
                                value: card.rule.seconds
                                editable: true
                                onValueModified: { var v = value; root.edit(card.type, function (r) { r.seconds = v; }); }
                            }
                            Text { text: qsTr("seconds"); color: Theme.textMuted; font.pixelSize: 12 }
                        }

                        RowLayout {
                            spacing: Theme.spacing
                            FieldLabel { text: qsTr("Sound") }
                            Picker {
                                options: root.soundOptions
                                value: card.rule.sound
                                onPicked: (k) => {
                                    root.edit(card.type, function (r) { r.sound = k; });
                                    if (k)
                                        AlertSounds.test(k);
                                }
                            }
                            SmallBtn {
                                text: "▶"
                                enabled: card.rule.sound !== ""
                                onTapped: AlertSounds.test(card.rule.sound)
                            }
                        }

                        RowLayout {
                            visible: card.rule.sound !== "" || card.exceptions.length > 0
                            spacing: Theme.spacing
                            FieldLabel { text: qsTr("Repeat sound after") }
                            SpinBox {
                                from: 0; to: 3600
                                value: card.rule.cooldown
                                editable: true
                                onValueModified: { var v = value; root.edit(card.type, function (r) { r.cooldown = v; }); }
                            }
                            Text { text: qsTr("seconds, per camera"); color: Theme.textMuted; font.pixelSize: 12 }
                        }

                        // Per-camera exceptions: a camera's own colour or sound, or no highlight.
                        Text {
                            text: qsTr("Exceptions")
                            color: Theme.textMuted; font.pixelSize: 12
                            topPadding: 4
                        }
                        Repeater {
                            model: card.exceptions
                            RowLayout {
                                id: exRow
                                required property string modelData
                                readonly property var ex: card.rule.overrides[modelData]
                                spacing: Theme.spacing
                                Text {
                                    text: root.cameraName(exRow.modelData)
                                    color: Theme.text; font.pixelSize: 12
                                    elide: Text.ElideRight
                                    Layout.preferredWidth: 200
                                }
                                CheckBox {
                                    id: offBox
                                    text: qsTr("No highlight")
                                    contentItem: Text {
                                        text: offBox.text
                                        color: Theme.text
                                        font.pixelSize: 12
                                        leftPadding: offBox.indicator.width + offBox.spacing
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                    checked: exRow.ex.off === true
                                    onToggled: {
                                        var key = exRow.modelData, on = checked;
                                        root.edit(card.type, function (r) {
                                            if (on) r.overrides[key].off = true;
                                            else delete r.overrides[key].off;
                                        });
                                    }
                                }
                                Picker {
                                    visible: exRow.ex.off !== true
                                    Layout.preferredWidth: 150
                                    options: root.exceptionColours
                                    value: exRow.ex.colour || ""
                                    onPicked: (k) => {
                                        var key = exRow.modelData;
                                        root.edit(card.type, function (r) { r.overrides[key].colour = k; });
                                    }
                                }
                                Picker {
                                    visible: exRow.ex.off !== true
                                    Layout.preferredWidth: 160
                                    options: root.exceptionSounds
                                    value: exRow.ex.sound || ""
                                    onPicked: (k) => {
                                        var key = exRow.modelData;
                                        root.edit(card.type, function (r) { r.overrides[key].sound = k; });
                                    }
                                }
                                SmallBtn {
                                    text: "✕"
                                    onTapped: {
                                        var key = exRow.modelData;
                                        root.edit(card.type, function (r) { delete r.overrides[key]; });
                                    }
                                }
                            }
                        }
                        Picker {
                            Layout.preferredWidth: 300
                            options: [{ key: "", label: qsTr("Add an exception for…") }].concat(
                                root.cameraList.filter(function (c) {
                                    return card.exceptions.indexOf(c.key) < 0
                                        && (!card.chosen || card.rule.cameras.indexOf(c.key) >= 0);
                                }))
                            value: ""
                            onPicked: (k) => {
                                if (k)
                                    root.edit(card.type, function (r) { r.overrides[k] = {}; });
                                currentIndex = 0;
                            }
                        }
                    }
                }
            }
        }
        Item { Layout.preferredHeight: Theme.spacing }
    }
}
