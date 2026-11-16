.pragma library

// The pane bar's transport (PaneBarRules ids) and the page keys, one mapping for every video
// pane (Live replay, Playback, the Events clip player; wayfinder/live-replay tickets 11, 12).
// `target` is the pane's handler: skip(secs), togglePause() and speedAction(step[, dir]), which
// applies `step` (a PlaybackGroup change) its own way — `dir` is -1 for the backwards buttons
// (Live replay starts from live on those; Playback and Events don't need it).
// Each page's key(k) keeps its own guards (J on Frigate clips, → at live) around keyId().

// The speed buttons: the PlaybackGroup change each makes and its direction.
const speeds = {
    back:        { dir: -1, step: (g) => g.playBackward() },
    fastBack:    { dir: -1, step: (g) => g.stepFast(-1) },
    slowBack:    { dir: -1, step: (g) => g.stepSlow(-1) },
    forward:     { dir: 1,  step: (g) => g.playForward() },
    slowForward: { dir: 1,  step: (g) => g.stepSlow(1) },
    fastForward: { dir: 1,  step: (g) => g.stepFast(1) },
};

// Do what the button `id` does on `target`; false for an id that isn't the transport's.
function apply(id, group, target) {
    const sp = speeds[id];
    if (sp) {
        target.speedAction(() => sp.step(group), sp.dir);
        return true;
    }
    switch (id) {
    case "skipBack": target.skip(-10); return true;
    case "skipForward": target.skip(10); return true;
    case "pause": target.togglePause(); return true;
    }
    return false;
}

// The page keys as buttons: J « · K ❚❚ · L » · ← ↺ · → ↻ ("" for any other key). One exception
// is the caller's: on a Live pane at live, J starts ◀ at 1× (LiveReplayControls.key), as the
// pane's ◀ does, rather than fast backwards.
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
