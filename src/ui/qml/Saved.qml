pragma Singleton
import QtCore
import QtQuick
import ReolinkApp.Core

// Saved selections (wayfinder/site-tabs-redesign): named sets of cameras, listed under
// ☆ Saved and recalled into a page's selection. Stored in [savedSelections]. The first
// launch takes them from the named mosaics ([mosaics], left in place for one release).
QtObject {
    id: root

    readonly property SavedSelections model: SavedSelections {
        onSaveRequested: (text) => store.setValue("list", text)
    }
    readonly property var list: model.list

    Component.onCompleted: {
        if (String(store.value("migrated", false)) !== "true") {
            var l = model.fromMosaics(String(mosaicStore.value("list", "")));
            store.setValue("list", JSON.stringify(l));
            store.setValue("migrated", true);
        }
        model.saved = String(store.value("list", "[]"));
    }

    property Settings store: Settings { id: store; category: "savedSelections" }   // list, migrated
    property Settings mosaicStore: Settings { id: mosaicStore; category: "mosaics" }
}
