pragma Singleton
import QtCore
import QtQuick

// Sound choice for playback, remembered across sessions.
// One instance, so a change on one page is seen by the other at once.
QtObject {
    // Playback starts muted the first time, then keeps whatever was chosen.
    property alias playbackMuted: store.playbackMuted

    property Settings store: Settings {
        id: store
        category: "audio"
        property bool playbackMuted: true
    }
}
