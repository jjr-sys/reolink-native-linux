pragma Singleton
import QtCore
import QtQuick
import ReolinkApp.Core

// The user's named mosaics (wayfinder/mosaic-live-view), shown as tabs after the Mosaic
// switch in Live View and listed in Settings → Mosaics. One instance, so an edit in one
// place shows in the other at once. Each entry:
//   { id, name, cameras: ["hostId:channel", ...] in the mosaic's own order,
//     grouping: "site" | "order" | "shape", siteOrder: ["Site", ...] }
QtObject {
    id: root

    readonly property var list: parse(store.list)

    function parse(s) {
        try {
            var l = JSON.parse(s);
            return Array.isArray(l) ? l : [];
        } catch (e) {
            return [];
        }
    }
    function save(l) { store.list = JSON.stringify(l); }
    function indexOf(id) {
        for (var i = 0; i < list.length; ++i)
            if (list[i].id === id)
                return i;
        return -1;
    }
    function find(id) {
        var i = indexOf(id);
        return i >= 0 ? list[i] : null;
    }

    // Returns the new mosaic's id.
    function add(fields) {
        var m = { id: "m" + Date.now().toString(36), name: fields.name,
                  cameras: fields.cameras || [], grouping: fields.grouping || "site",
                  siteOrder: fields.siteOrder || [] };
        var l = list.slice();
        l.push(m);
        save(l);
        return m.id;
    }
    function update(id, fields) {
        var i = indexOf(id);
        if (i < 0)
            return;
        var l = list.slice();
        var m = Object.assign({}, l[i]);
        for (var k in fields)
            m[k] = fields[k];
        l[i] = m;
        save(l);
    }
    function move(id, dir) {
        var i = indexOf(id), j = i + dir;
        if (i < 0 || j < 0 || j >= list.length)
            return;
        var l = list.slice();
        var t = l[i]; l[i] = l[j]; l[j] = t;
        save(l);
    }
    function remove(id) {
        var i = indexOf(id);
        if (i < 0)
            return;
        var l = list.slice();
        l.splice(i, 1);
        save(l);
    }
    function nextName() {
        for (var n = 1; ; ++n) {
            var name = qsTr("Mosaic %1").arg(n);
            if (!list.some(m => m.name === name))
                return name;
        }
    }

    // ---- Sites --------------------------------------------------------------
    // A host named "<Site> <something>" (the Farm Hikvision's direct stream) belongs
    // to that Site.
    function siteOf(hostName) {
        var names = Devices.propertyNames();
        for (var i = 0; i < names.length; ++i)
            if (names[i] !== hostName && hostName.indexOf(names[i] + " ") === 0)
                return names[i];
        return hostName;
    }
    function cameraKey(row) {
        var c = Devices.cameraInfo(row);
        return c && c.hostId !== undefined ? c.hostId + ":" + c.channel : "";
    }
    // Every Site with a camera, in `order` first, then the rest alphabetically.
    function sites(order) {
        var all = [];
        for (var r = 0; r < Devices.count; ++r) {
            var s = siteOf(Devices.cameraInfo(r).hostName);
            if (all.indexOf(s) < 0)
                all.push(s);
        }
        var out = (order || []).filter(s => all.indexOf(s) >= 0);
        all.sort((a, b) => a.localeCompare(b));
        for (var i = 0; i < all.length; ++i)
            if (out.indexOf(all[i]) < 0)
                out.push(all[i]);
        return out;
    }
    // The grid arrangement's cameras in pane order: a new mosaic's order is seeded from it.
    function gridOrder() {
        return String(gridStore.value("arrangement", "")).split(",").filter(k => k && k !== "-");
    }
    // `items` are { site, order, ratio, ... }; sorted in place for the grouping and returned.
    function arrange(items, grouping, siteOrder) {
        if (grouping === "shape") {
            items.sort((a, b) => b.ratio - a.ratio || a.order - b.order);
        } else if (grouping === "site") {
            var s = sites(siteOrder);
            items.sort((a, b) => s.indexOf(a.site) - s.indexOf(b.site) || a.order - b.order);
        } else {
            items.sort((a, b) => a.order - b.order);
        }
        return items;
    }

    property Settings store: Settings {
        id: store
        category: "mosaics"
        property string list: "[]"
    }
    property Settings gridStore: Settings { category: "liveGrid" }
}
