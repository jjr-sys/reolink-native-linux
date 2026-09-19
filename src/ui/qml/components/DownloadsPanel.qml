import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ReolinkApp
import ReolinkApp.Core

// The session's downloads: one row per camera and time range, with status, progress and
// the actions that fit its state. Files on disk are the lasting record; this list is
// cleared when the app closes.
Popup {
    id: panel
    width: 460
    height: Math.min(420, Math.max(120, list.contentHeight + header.height + footer.height + 2 * padding + 24))
    padding: Theme.spacing
    background: Rectangle { color: Theme.surface; border.color: Theme.border; radius: Theme.radius }

    function clock(secs) { return Qt.formatDateTime(new Date(secs * 1000), "hh:mm:ss"); }
    function day(secs) { return Qt.formatDateTime(new Date(secs * 1000), "ddd d MMM"); }

    component Action: Rectangle {
        property string label: ""
        signal activated()
        implicitWidth: actText.implicitWidth + 16
        implicitHeight: 24
        radius: 4
        color: actHover.hovered ? Theme.surfaceAlt : "transparent"
        border.color: Theme.border
        Text { id: actText; anchors.centerIn: parent; text: parent.label; color: Theme.text; font.pixelSize: 11 }
        HoverHandler { id: actHover }
        TapHandler { onTapped: parent.activated() }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spacing

        RowLayout {
            id: header
            Layout.fillWidth: true
            Text { text: qsTr("Downloads"); color: Theme.text; font.pixelSize: 14; font.bold: true }
            Item { Layout.fillWidth: true }
            Action { label: qsTr("Clear finished"); onActivated: Downloads.clearFinished() }
        }

        Text {
            visible: list.count === 0
            Layout.fillWidth: true
            text: qsTr("Nothing yet. Mark a start and end on the timeline, then choose Download range.")
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            font.pixelSize: 12
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: count > 0
            clip: true
            spacing: 6
            model: Downloads
            delegate: Rectangle {
                id: row
                required property int id
                required property string site
                required property string camera
                required property double start
                required property double end
                required property string state
                required property int progress
                required property string detail
                required property string path
                readonly property bool running: state === "preparing" || state === "downloading"
                readonly property bool finished: state === "done" || state === "failed" || state === "cancelled"
                width: list.width
                height: col.implicitHeight + 12
                radius: 4
                color: Theme.surfaceAlt
                border.color: state === "failed" ? Theme.danger : Theme.border

                ColumnLayout {
                    id: col
                    anchors.fill: parent
                    anchors.margins: 6
                    spacing: 4
                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            Layout.fillWidth: true
                            text: row.site + " · " + row.camera
                            color: Theme.text; font.pixelSize: 12; font.bold: true
                            elide: Text.ElideRight
                        }
                        Text {
                            text: panel.day(row.start) + "  " + panel.clock(row.start) + " – " + panel.clock(row.end)
                            color: Theme.textMuted; font.pixelSize: 11
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: row.state === "done" ? qsTr("Saved: %1").arg(row.path.split("/").pop()) : row.detail
                        color: row.state === "failed" ? Theme.danger : Theme.textMuted
                        font.pixelSize: 11
                        wrapMode: Text.WordWrap
                    }
                    ProgressBar {
                        Layout.fillWidth: true
                        visible: row.running
                        indeterminate: row.progress < 0
                        from: 0; to: 100
                        value: row.progress
                    }
                    RowLayout {
                        spacing: 6
                        Action { visible: row.state === "queued" || row.running; label: qsTr("Cancel")
                                 onActivated: Downloads.cancel(row.id) }
                        Action { visible: row.state === "failed" || row.state === "cancelled"; label: qsTr("Retry")
                                 onActivated: Downloads.retry(row.id) }
                        Action { visible: row.state === "done"; label: qsTr("Open folder")
                                 onActivated: Downloads.openFolder(row.id) }
                        Action { visible: row.finished; label: qsTr("Remove")
                                 onActivated: Downloads.remove(row.id) }
                    }
                }
            }
        }

        RowLayout {
            id: footer
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: qsTr("Saving to %1").arg(Downloads.folder)
                color: Theme.textMuted; font.pixelSize: 11
                elide: Text.ElideMiddle
            }
        }
    }
}
