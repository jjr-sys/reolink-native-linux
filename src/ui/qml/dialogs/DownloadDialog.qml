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
    title: qsTr("Download range")
    background: Rectangle { color: Theme.surface; border.color: Theme.border; radius: Theme.radius }

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

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spacing

        Text { text: dlg.siteName; color: Theme.textMuted; font.pixelSize: 12 }

        RowLayout {
            spacing: Theme.spacing
            Text { text: qsTr("From"); color: Theme.text; font.pixelSize: 12 }
            TextField { id: startField; implicitWidth: 92; inputMask: "99:99:99"; font.pixelSize: 12
                        color: dlg.s0 >= 0 ? Theme.text : Theme.danger }
            Text { text: qsTr("to"); color: Theme.text; font.pixelSize: 12 }
            TextField { id: endField; implicitWidth: 92; inputMask: "99:99:99"; font.pixelSize: 12
                        color: dlg.s1 > dlg.s0 ? Theme.text : Theme.danger }
            Text {
                text: dlg.rangeOk ? dlg.durationText(dlg.s1 - dlg.s0) : qsTr("End must be after start")
                color: dlg.rangeOk ? Theme.textMuted : Theme.danger
                font.pixelSize: 12
            }
        }

        Text { text: qsTr("Cameras"); color: Theme.text; font.pixelSize: 12; font.bold: true }
        ListView {
            id: cams
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(200, contentHeight)
            clip: true
            model: Devices
            delegate: CheckBox {
                required property int index
                required property string name
                visible: Devices.cameraInfo(index).hostId === dlg.hostId
                height: visible ? implicitHeight : 0
                text: name
                checked: !!dlg.picked[index]
                onToggled: {
                    var p = Object.assign({}, dlg.picked);
                    if (checked) p[index] = true; else delete p[index];
                    dlg.picked = p;
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
            Button { text: qsTr("Change…"); onClicked: folderDialog.open() }
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            Button { text: qsTr("Cancel"); onClicked: dlg.close() }
            Button {
                text: qsTr("Add to queue")
                enabled: dlg.rangeOk && dlg.pickedCount > 0
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
