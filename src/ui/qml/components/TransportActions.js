.pragma library

// The pane bar's transport (PaneBarRules ids) and the page keys, one mapping for every video
// pane (Live replay, Playback, the Events clip player; wayfinder/live-replay tickets 11, 12).
// `target` is the pane's handler: skip(secs), togglePause() and speedAction(step, dir), which
// applies `step` (a PlaybackGroup change) its own way — `dir` is -1 for the backwards buttons.

// The PlaybackGroup change a speed button makes, or null for the others.
function speedStep(id, group) {
    switch (id) {
    case "back": return () => group.playBackward();
    case "fastBack": return () => group.stepFast(-1);
    case "slowBack": return () => group.stepSlow(-1);
    case "forward": return () => group.playForward();
    case "slowForward": return () => group.stepSlow(1);
    case "fastForward": return () => group.stepFast(1);
    }
    return null;
}

// Do what the button `id` does on `target`; false for an id that isn't the transport's.
function apply(id, group, target) {
    const step = speedStep(id, group);
    if (step) {
        target.speedAction(step, id.endsWith("Back") || id === "back" ? -1 : 1);
        return true;
    }
    switch (id) {
    case "skipBack": target.skip(-10); return true;
    case "skipForward": target.skip(10); return true;
    case "pause": target.togglePause(); return true;
    }
    return false;
}

// The page keys as buttons: J « · K ❚❚ · L » · ← ↺ · → ↻ ("" for any other key).
function keyId(k) {
    switch (k) {
    case "J": return "fastBack";
    case "K": return "pause";
    case "L": return "fastForward";
    case "Left": return "skipBack";
    case "Right": return "skipForward";
    }
    return "";
}
