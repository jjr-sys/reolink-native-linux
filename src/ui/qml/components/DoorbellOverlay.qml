import QtQuick
import QtQuick.Layouts
import ReolinkApp
import ReolinkApp.Core

// High-priority visitor-press surface for Reolink doorbells: live view of the
// doorbell, Answer (two-way talk), Dismiss, and one button per quick-reply clip
// the device reports. Raised on a "visitor" event; dismisses after a timeout
// while nobody has touched it.
Rectangle {
    id: root
    property int deviceRow: -1     // the doorbell's own camera row (its NVR channel)
    property string camera: ""
    property bool active: false

    signal dismissed()

    // Quick-reply list state: "loading" | "ready" | "error". "ready" with no clips
    // is the explicit "none" state.
    property string replyState: "loading"
    property var clips: []
    property string replyError: ""
    property string status: ""     // last play result / talk problem, shown under the video
    property bool statusIsError: false
    property int playingId: -1

    readonly property bool talking: talk.active

    visible: active
    color: "#e60a0f14"
    radius: Theme.radius
    border.color: Theme.accent
    border.width: 2
    width: 480
    height: 400

    function close() {
        talk.stop();
        root.active = false;
        root.dismissed();
    }

    function loadReplies() {
        clips = [];
        replyError = "";
        playingId = -1;
        status = "";
        if (deviceRow < 0) {
            replyState = "error";
            replyError = qsTr("doorbell not found");
            return;
        }
        replyState = "loading";
        Devices.fetchQuickReplies(deviceRow);
    }

    onActiveChanged: {
        if (active) {
            autoDismiss.restart();
            loadReplies();
        } else {
            autoDismiss.stop();
            talk.stop();
        }
    }
    onDeviceRowChanged: if (active) loadReplies()

    // Left alone for 30 s and nobody answered: go away. Any interaction cancels it.
    Timer { id: autoDismiss; interval: 30000; onTriggered: root.close() }

    TalkSession { id: talk }

    Connections {
        target: talk
        function onStateChanged() {
            if (talk.state === TalkSession.Error) {
                root.status = qsTr("Talk: %1").arg(talk.errorString);
                root.statusIsError = true;
            }
        }
    }

    Connections {
        target: Devices
        function onQuickRepliesLoaded(row, list, maxFiles, error) {
            if (row !== root.deviceRow)
                return;
            root.clips = list;
            root.replyError = error;
            root.replyState = error !== "" ? "error" : "ready";
        }
        function onQuickReplyPlayed(row, clipId, ok, error) {
            if (row !== root.deviceRow || clipId !== root.playingId)
                return;
            root.playingId = -1;
            root.statusIsError = !ok;
            root.status = ok ? qsTr("Reply played") : qsTr("Reply failed: %1").arg(error);
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spacing * 2
        spacing: Theme.spacing

        RowLayout {
            Layout.fillWidth: true
            Text { text: "🔔"; font.pixelSize: 22 }
            ColumnLayout {
                spacing: 0
                Text { text: qsTr("Someone's at the door"); color: Theme.text; font.pixelSize: 15; font.bold: true }
                Text { text: root.camera; color: Theme.textMuted; font.pixelSize: 12 }
            }
            Item { Layout.fillWidth: true }
        }

        // Live view of the doorbell. Selected = its audio plays, except while we
        // talk (the doorbell's speaker would feed back into the microphone).
        LivePane {
            Layout.fillWidth: true
            Layout.fillHeight: true
            deviceRow: root.active ? root.deviceRow : -1
            label: root.camera
            selected: !root.talking
            forceMain: false
            pageActive: root.active
        }

        Text {
            Layout.fillWidth: true
            visible: root.status !== ""
            text: root.status
            color: root.statusIsError ? Theme.danger : Theme.textMuted
            font.pixelSize: 12
            elide: Text.ElideRight
        }

        component ActionBtn: Rectangle {
            property string label: ""
            property color tint: Theme.surfaceAlt
            property bool busy: false
            signal clicked()
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            radius: Theme.radius
            color: abHover.hovered ? Qt.lighter(tint, 1.2) : tint
            opacity: busy ? 0.6 : 1
            border.color: Theme.border
            Text {
                anchors.centerIn: parent
                width: parent.width - 8
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                text: parent.label; color: Theme.text; font.pixelSize: 13; font.bold: true
            }
            HoverHandler { id: abHover }
            TapHandler { onTapped: parent.clicked() }
        }

        // Quick replies: what the device reports, or an explicit reason there are none.
        Text {
            visible: root.replyState !== "ready" || root.clips.length === 0
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.pixelSize: 12
            color: root.replyState === "error" ? Theme.danger : Theme.textMuted
            text: root.replyState === "loading" ? qsTr("Loading quick replies…")
                : root.replyState === "error" ? qsTr("Quick replies unavailable: %1").arg(root.replyError)
                : qsTr("No quick replies on this doorbell")
        }
        Flow {
            Layout.fillWidth: true
            spacing: Theme.spacing
            visible: root.replyState === "ready" && root.clips.length > 0
            Repeater {
                model: root.clips
                delegate: Rectangle {
                    required property var modelData
                    width: Math.max(90, replyLabel.implicitWidth + 24)
                    height: 32
                    radius: Theme.radius
                    color: replyHover.hovered ? Qt.lighter(Theme.surfaceAlt, 1.2) : Theme.surfaceAlt
                    border.color: Theme.border
                    opacity: root.playingId >= 0 ? 0.6 : 1
                    Text {
                        id: replyLabel
                        anchors.centerIn: parent
                        text: parent.modelData.name !== "" ? parent.modelData.name
                                                           : qsTr("Reply %1").arg(parent.modelData.id)
                        color: Theme.text; font.pixelSize: 12
                    }
                    HoverHandler { id: replyHover }
                    TapHandler {
                        onTapped: {
                            if (root.playingId >= 0)
                                return;
                            autoDismiss.stop();
                            root.playingId = parent.modelData.id;
                            root.status = qsTr("Playing “%1”…").arg(replyLabel.text);
                            root.statusIsError = false;
                            Devices.playQuickReply(root.deviceRow, parent.modelData.id);
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing

            ActionBtn {
                label: talk.state === TalkSession.Connecting ? qsTr("Connecting…")
                     : root.talking ? qsTr("Hang up") : qsTr("Answer")
                tint: root.talking ? Theme.danger : Theme.online
                onClicked: {
                    autoDismiss.stop();
                    if (root.talking) {
                        talk.stop();
                    } else {
                        root.status = "";
                        Devices.startTalk(root.deviceRow, talk);
                    }
                }
            }
            ActionBtn {
                label: qsTr("Dismiss")
                onClicked: root.close()
            }
        }
    }
}
