pragma Singleton
import QtCore
import QtQuick
import ReolinkApp.Core

// The Sites (wayfinder/site-tabs-redesign): which recorders belong together, and the Site
// tab order, each Site's budget in Mbit/s and what each camera's pane costs. One instance,
// so the Site tabs on every page and Settings → Sites agree. Edited in Settings → Sites;
// stored in [sites].
QtObject {
    id: root

    readonly property SiteMap map: SiteMap {
        onSaveRequested: (text) => store.setValue("map", text)
    }
    readonly property var list: map.sites
    readonly property int revision: map.revision

    // ---- Site budgets (Mbit/s) --------------------------------------------------------
    // Measured links over the relay: Woorabinda ~7, Farm ~5, Lascelles ~3.3; any other Site
    // starts at 5. Seeded once by name, then edited in Settings → Sites.
    readonly property real defaultBudget: 5
    property var budgets: ({})
    // Mbit/s as people write it: one decimal, none when whole ("4.6", "5").
    function mbps(v) {
        var r = Math.round(Number(v) * 10) / 10;
        return r.toLocaleString(Qt.locale(), "f", r % 1 === 0 ? 0 : 1);
    }
    // The advice that ends every refusal over a budget.
    readonly property string overBudgetAdvice: qsTr("Deselect a camera or switch one out of HD.")
    function budgetOf(site) { return budgets[site] > 0 ? budgets[site] : defaultBudget; }
    function setBudget(site, mbps) {
        var v = Number(mbps);
        if (!site || !(v > 0) || budgets[site] === v)
            return;
        var b = Object.assign({}, budgets);
        b[site] = Math.round(v * 10) / 10;
        budgets = b;
        store.setValue("budgets", JSON.stringify(b));
    }
    function loadBudgets() {
        var text = String(store.value("budgets", ""));
        if (text === "") {
            budgets = { "Woorabinda": 7, "Farm": 5, "Lascelles": 3.3 };
            store.setValue("budgets", JSON.stringify(budgets));
            return;
        }
        try { budgets = JSON.parse(text) || {}; } catch (e) { budgets = {}; }
    }

    // ---- Pane costs (Mbit/s) ----------------------------------------------------------
    // From each camera's GetEnc bitrates when its recorder connects. The last known figures
    // are kept ([sites] costs), so a camera whose NVR is down still counts at its own cost.
    property var costs: ({})
    property var knownCosts: ({})
    function refreshCosts() {
        var live = Devices.streamCosts(), known = Object.assign({}, knownCosts), dirty = false;
        for (var k in live) {
            var c = live[k];
            if (c.known && (!known[k] || known[k].sub !== c.sub || known[k].main !== c.main)) {
                known[k] = { sub: c.sub, main: c.main };
                dirty = true;
            }
        }
        if (dirty) {
            knownCosts = known;
            store.setValue("costs", JSON.stringify(known));
        }
        var out = Object.assign({}, known);
        for (var j in live)
            if (live[j].known || !out[j])
                out[j] = { sub: live[j].sub, main: live[j].main };
        costs = out;
    }
    property Timer costTimer: Timer { interval: 500; onTriggered: root.refreshCosts() }

    // ---- Playback ----------------------------------------------------------------------
    // A bare stream has no recordings of its own. Where a recorder records the same camera,
    // Playback offers that camera instead: "hostId:channel" of the stream -> of the recorder.
    // The Farm Hikvision stream (host 4) is Farm NVR channel 8.
    readonly property var playbackInstead: ({ "4:0": "2:8" })
    // The camera to play for `key`: itself, or the recorder's copy of a bare stream ("" for
    // a bare stream with none).
    function playbackKey(key) {
        return !isBareStream(key) ? key : playbackInstead[key] || "";
    }
    function isBareStream(key) {
        var info = Devices.hostInfo(parseInt(String(key).split(":")[0]));
        return !!info && info.hasRecordings === false;
    }

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
            loadBudgets();
            try { knownCosts = JSON.parse(String(store.value("costs", "{}"))) || {}; }
            catch (e) { knownCosts = {}; }
            loaded = true;
        }
        costTimer.restart();
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
        function onDataChanged() { root.costTimer.restart(); }
    }
    Component.onCompleted: syncHosts()

    property Settings store: Settings {
        id: store
        category: "sites"     // map: SiteMap's stored text; budgets, costs: JSON
    }
}
