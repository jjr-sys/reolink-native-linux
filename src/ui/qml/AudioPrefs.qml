pragma Singleton
import QtCore
import QtQuick

// Sound choices shared by live view and playback, remembered across sessions.
// One instance, so a change on one page is seen by the other at once.
QtObject {
    // Playback starts muted the first time, then keeps whatever was chosen.
    property alias playbackMuted: store.playbackMuted
    // 0..1 slider position; the same loudness in live view and playback.
    property alias volume: store.volume

    property Settings store: Settings {
        id: store
        category: "audio"
        property bool playbackMuted: true
        property real volume: 1.0
    }
}
