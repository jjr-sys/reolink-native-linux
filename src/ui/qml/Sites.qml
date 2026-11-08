pragma Singleton
import QtCore
import QtQuick
import ReolinkApp.Core

// The Sites (wayfinder/site-tabs-redesign): which recorders belong together, and the Site
// tab order. One instance, so the Site tabs on every page and Settings → Sites agree.
// Edited in Settings → Sites; stored in [sites].
QtObject {
    id: root

    readonly property SiteMap map: SiteMap {
        onSaveRequested: (text) => store.setValue("map", text)
    }
    readonly property var list: map.sites
    readonly property int revision: map.revision

    function siteOf(hostId) { return map.siteOf(hostId); }
    function siteOfKey(key) { return map.siteOfKey(key); }
    function hostsOf(site) { return map.hostsOf(site); }

    // The cameras of a Site in device list order: { row, key, name, online }.
    function camerasOf(site) {
        revision;
        var out = [];
        for (var r = 0; r < Devices.count; ++r) {
            var c = Devices.cameraInfo(r);
            if (c.hostId === undefined || map.siteOf(c.hostId) !== site)
                continue;
            out.push({ row: r, key: c.hostId + ":" + c.channel, name: c.name, online: c.online });
        }
        return out;
    }

    // Read straight from the store before the first hosts, so a stored map is never
    // mistaken for none (which would re-seed it from names).
    property bool loaded: false
    function syncHosts() {
        if (!loaded) {
            map.saved = String(store.value("map", ""));
            loaded = true;
        }
        var ids = Devices.hostIds(), hosts = [];
        for (var i = 0; i < ids.length; ++i)
            hosts.push({ id: ids[i], name: Devices.hostName(ids[i]) });
        map.hosts = hosts;
    }

    property Connections devices: Connections {
        target: Devices
        function onRowsInserted() { root.syncHosts(); }
        function onRowsRemoved() { root.syncHosts(); }
        function onModelReset() { root.syncHosts(); }
    }
    Component.onCompleted: syncHosts()

    property Settings store: Settings {
        id: store
        category: "sites"     // map: SiteMap's stored text
    }
}
