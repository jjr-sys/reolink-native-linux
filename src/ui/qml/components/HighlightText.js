.pragma library

// A Highlight's kind as shown ("Person", "Doorbell"): one place for the pane label, "● Live ·
// Person" and the replaying pane's chips (live-replay ticket 14). Kinds as HighlightPolicy
// names them, plus the doorbell pop-up's fixed "doorbell".
function kindText(kind) {
    switch (kind) {
    case "person": return qsTr("Person");
    case "vehicle": return qsTr("Vehicle");
    case "pet": return qsTr("Pet");
    case "motion": return qsTr("Motion");
    case "doorbell": return qsTr("Doorbell");
    }
    return kind ? kind.charAt(0).toUpperCase() + kind.slice(1) : "";
}
