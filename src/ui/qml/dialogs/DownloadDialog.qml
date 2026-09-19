import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import ReolinkApp
import ReolinkApp.Core

// Choose which cameras to download a time range from. Cameras of one site only: they
// share an NVR, so they are fetched one after another, not all at once.
Dialog {
    id: dlg
    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(460, (Overlay.overlay ? Overlay.overlay.width : 460) - 80)
    padding: Theme.spacing * 2
    background: Rectangle { color: Theme.surface; border.color: Theme.border; radius: Theme.radius }

    header: Item {
        implicitHeight: hdr.implicitHeight + 22
        ColumnLayout {
            id: hdr
            x: Theme.spacing * 2; width: parent.width - Theme.spacing * 4
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2
            Text { text: qsTr("Download range"); color: Theme.text; font.pixelSize: 16; font.bold: true }
            Text { text: dlg.siteName; color: Theme.textMuted; font.pixelSize: 12 }
        }
    }

    component ThemedField: TextField {
        color: Theme.text
        selectionColor: Theme.accentDim
        selectedTextColor: Theme.text
        font.pixelSize: 13
        horizontalAlignment: TextInput.AlignHCenter
        background: Rectangle {
            color: Theme.surfaceAlt
            border.color: parent.activeFocus ? Theme.accent : Theme.border
            radius: 4
        }
    }
    component ActionButton: Rectangle {
        property string label: ""
        property bool primary: false
        property bool enabledBtn: true
        signal clicked()
        implicitWidth: abTxt.implicitWidth + 24
        implicitHeight: 30
        radius: Theme.radius
        opacity: enabledBtn ? 1 : 0.45
        color: primary ? (abHover.hovered && enabledBtn ? Theme.accent : Theme.accentDim)
                       : (abHover.hovered && enabledBtn ? Theme.surfaceAlt : Theme.surface)
        border.color: primary ? Theme.accent : Theme.border
        Text { id: abTxt; anchors.centerIn: parent; text: parent.label; color: Theme.text; font.pixelSize: 12 }
        HoverHandler { id: abHover; enabled: parent.enabledBtn }
        TapHandler { enabled: parent.enabledBtn; onTapped: parent.clicked() }
    }

    property int hostId: -1         // the site whose cameras are offered
    property string siteName: ""
    property real dayEpoch: 0       // Unix seconds at 00:00 of the day being watched
    property int startSecs: 0       // seconds into the day
    property int endSecs: 0
    property int firstRow: -1       // camera ticked when the dialog opens
    property var picked: ({})       // device row -> true

    function openFor(row, dayEpochSecs, start, end) {
        var info = Devices.cameraInfo(row);
        hostId = info.hostId;
        siteName = info.hostName;
        dayEpoch = dayEpochSecs;
        startSecs = Math.floor(start);
        endSecs = Math.floor(end);
        firstRow = row;
        var p = ({});
        p[row] = true;
        picked = p;
        startField.text = clock(startSecs);
        endField.text = clock(endSecs);
        open();
    }

    function pad(n) { return (n < 10 ? "0" : "") + n; }
    function clock(s) { return pad(Math.floor(s / 3600)) + ":" + pad(Math.floor(s / 60) % 60) + ":" + pad(s % 60); }
    // "HH:MM:SS" -> seconds into the day, or -1.
    function parseClock(t) {
        var m = /^(\d{1,2}):(\d{2})(?::(\d{2}))?$/.exec(t.trim());
        if (!m) return -1;
        var s = parseInt(m[1]) * 3600 + parseInt(m[2]) * 60 + (m[3] ? parseInt(m[3]) : 0);
        return (parseInt(m[1]) < 24 && parseInt(m[2]) < 60 && (!m[3] || parseInt(m[3]) < 60)) ? s : -1;
    }
    readonly property int s0: parseClock(startField.text)
    readonly property int s1: parseClock(endField.text)
    readonly property bool rangeOk: s0 >= 0 && s1 > s0 + 1
    readonly property int pickedCount: Object.keys(picked).length
    function durationText(secs) {
        var m = Math.floor(secs / 60), s = secs % 60;
        return m > 0 ? qsTr("%1 min %2 s").arg(m).arg(s) : qsTr("%1 s").arg(s);
    }

    // The cameras at this site, in the order the sidebar lists them.
    readonly property var siteRows: {
        var out = [];
        for (var r = 0; r < Devices.count; ++r)
            if (Devices.cameraInfo(r).hostId === dlg.hostId)
                out.push(r);
        return out;
    }
    function setAll(on) {
        var p = ({});
        if (on)
            for (var i = 0; i < siteRows.length; ++i)
                p[siteRows[i]] = true;
        picked = p;
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spacing

        RowLayout {
            spacing: Theme.spacing
            Text { text: qsTr("From"); color: Theme.text; font.pixelSize: 13 }
            ThemedField { id: startField; implicitWidth: 92; inputMask: "99:99:99;0"
                          color: dlg.s0 >= 0 ? Theme.text : Theme.danger }
            Text { text: qsTr("to"); color: Theme.text; font.pixelSize: 13 }
            ThemedField { id: endField; implicitWidth: 92; inputMask: "99:99:99;0"
                          color: dlg.s1 > dlg.s0 ? Theme.text : Theme.danger }
            Text {
                text: dlg.rangeOk ? dlg.durationText(dlg.s1 - dlg.s0) : qsTr("End must be after start")
                color: dlg.rangeOk ? Theme.textMuted : Theme.danger
                font.pixelSize: 12
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: qsTr("Cameras at %1 (%2 of %3)").arg(dlg.siteName).arg(dlg.pickedCount).arg(dlg.siteRows.length)
                color: Theme.text; font.pixelSize: 13; font.bold: true
                elide: Text.ElideRight
            }
            ActionButton { label: qsTr("All"); onClicked: dlg.setAll(true) }
            ActionButton { label: qsTr("None"); onClicked: dlg.setAll(false) }
        }
        ListView {
            id: cams
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(280, contentHeight)
            clip: true
            spacing: 2
            model: dlg.siteRows
            delegate: CheckBox {
                id: cb
                required property int modelData   // the camera's device row
                width: cams.width
                height: 30
                checked: !!dlg.picked[modelData]
                onToggled: {
                    var p = Object.assign({}, dlg.picked);
                    if (checked) p[modelData] = true; else delete p[modelData];
                    dlg.picked = p;
                }
                indicator: Rectangle {
                    x: 6
                    implicitWidth: 18; implicitHeight: 18
                    anchors.verticalCenter: parent.verticalCenter
                    radius: 4
                    color: cb.checked ? Theme.accent : Theme.surfaceAlt
                    border.color: cb.checked ? Theme.accent : Theme.textMuted
                    Text {
                        anchors.centerIn: parent
                        text: "\u2713"; visible: cb.checked
                        color: Theme.window; font.pixelSize: 12; font.bold: true
                    }
                }
                contentItem: Text {
                    text: Devices.cameraInfo(cb.modelData).name
                    color: Theme.text
                    font.pixelSize: 13
                    leftPadding: 6 + cb.indicator.width + 8
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }
                background: Rectangle {
                    radius: 4
                    color: cb.hovered ? Theme.surfaceAlt : "transparent"
                }
            }
        }
        Text {
            visible: dlg.pickedCount > 1
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("These download one after another: the NVR serves one clip at a time.")
            color: Theme.textMuted; font.pixelSize: 11
        }

        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: qsTr("Save to %1").arg(Downloads.folder)
                color: Theme.textMuted; font.pixelSize: 11
                elide: Text.ElideMiddle
            }
            ActionButton { label: qsTr("Change…"); onClicked: folderDialog.open() }
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            ActionButton { label: qsTr("Cancel"); onClicked: dlg.close() }
            ActionButton {
                label: qsTr("Add to queue")
                primary: true
                enabledBtn: dlg.rangeOk && dlg.pickedCount > 0
                onClicked: {
                    var rows = Object.keys(dlg.picked).map(Number).sort(function (a, b) { return a - b; });
                    for (var i = 0; i < rows.length; ++i)
                        Downloads.enqueue(rows[i], dlg.dayEpoch + dlg.s0, dlg.dayEpoch + dlg.s1);
                    dlg.accepted();
                    dlg.close();
                }
            }
        }
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("Choose where to save clips")
        currentFolder: "file://" + Downloads.folder
        onAccepted: Downloads.folder = selectedFolder.toString().replace(/^file:\/\//, "")
    }
}
