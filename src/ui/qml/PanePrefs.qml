pragma Singleton
import QtCore
import QtQuick
import ReolinkApp.Core

// Per-camera pane choices (wayfinder/site-tabs-redesign): which cameras show in HD, and
// which are Large in a mosaic. Stored in [cameraView] as "hostId:channel,..." lists.
// The first launch takes them from the mosaics: a camera that was HD or Large in the
// grid-page mosaic ([mosaicGrid]) or any named mosaic ([mosaics]) stays so. Those old
// sections are left in place, unused, for one release.
QtObject {
    id: root

    property var hd: []
    property var large: []

    function cameraKey(row) {
        var c = Devices.cameraInfo(row);
        return c && c.hostId !== undefined ? c.hostId + ":" + c.channel : "";
    }
    function keys(field) { return field === "hd" ? hd : large; }
    function has(field, key) { return keys(field).indexOf(key) >= 0; }
    function set(field, key, on) {
        var l = keys(field).filter(k => k !== key);
        if (on)
            l.push(key);
        if (field === "hd")
            hd = l;
        else
            large = l;
        store.setValue(field, l.join(","));
    }
    function toggle(field, key) { set(field, key, !has(field, key)); }

    function load() {
        if (String(store.value("migrated", false)) !== "true") {
            var mosaics = String(mosaicStore.value("list", ""));
            for (var f of ["hd", "large"])
                store.setValue(f, migration.mosaicKeys(mosaics, f, String(gridSets.value(f, ""))).join(","));
            store.setValue("migrated", true);
        }
        var split = (v) => String(v).split(",").filter(k => k.indexOf(":") > 0);
        hd = split(store.value("hd", ""));
        large = split(store.value("large", ""));
    }
    Component.onCompleted: load()

    // Only for its static readers of the old mosaic format.
    readonly property SavedSelections migration: SavedSelections {}
    property Settings store: Settings { id: store; category: "cameraView" }   // hd, large, migrated
    property Settings gridSets: Settings { id: gridSets; category: "mosaicGrid" }
    property Settings mosaicStore: Settings { id: mosaicStore; category: "mosaics" }
}
