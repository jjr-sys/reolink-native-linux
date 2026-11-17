# Architecture review

Each candidate appends its own section on its own branch.

## Candidate 6: backoff

**Module.** `src/core/Backoff.h` (header-only): `BackoffPolicy{start, cap, growth=2}` plus
`Backoff` (`fail()`, `current()`, `reset()`, `setDelay()`, `failAt(now)`). Units are the
caller's (ms, or seconds for DeviceManager). The module holds no clock: loops sleep and
schedulers schedule as before, and `failAt(nowMs)` takes "now" as an argument, so tests
need no fake-clock class. That is the same idea as `LiveStreamScheduler` (time comes from
outside) without a second interface for one function. Table test: `tests/test_backoff.cpp`.

**Delay sequences, recorded from the old code and asserted in the test.**

| Site | Sequence (every attempt fails) | Reset rule (kept) |
|---|---|---|
| StreamPlayer `runWorker` | 1, 2, 4, 8, 16, 30, 30 s | more than 100 frames decoded in the attempt |
| LiveStream non-Baichuan-main retry | 1, 2, 4, 8, 16, 30, 30 s | Streaming state, `resetPolicy()` |
| LiveStream Baichuan main | flat 1500 ms, three times, then RTSP fallback | `m_retries` as before |
| FrigateLive | 5, 10, 20, 40, 60, 60 s | connection made |
| BaichuanEventListener | 5, 10, 20, 40, 60, 60 s; refused: 600 s, sticky until a connect | subscribe succeeds |
| DeviceManager host probe | 15, 30, 60, 120, 240, 300, 300 s | success, Auth/Locked, user Reconnect |

**Decisions (each reversible with `git revert` of the named commit).**
- Growth rule is `delay < cap ? min(delay*growth, cap) : delay`. For in-range values it is
  identical to every old `min(d*2, cap)`; it also keeps BaichuanEventListener's refused
  600 s sticky, as the old `if (wait < kRefusedMs)` guard did.
- Flat 1500 ms stays a named constant in LiveStream (`kBaichuanMainRetryMs`), not a
  `growth=1` backoff: it is a count-limited retry, not a backoff.
- DeviceManager keeps `m_nextRetryAt` / `m_retryDelaySecs` (per host, header untouched) and
  uses only the stateless `BackoffPolicy::grow`. The edit replaces the three arithmetic
  sites in the host-validation result handler plus one constant; nothing else. The task said four
  loops, but only three advance the delay; the other two touches are `remove()` resets.
  The lockout rule (Auth and Locked never retried, `DeviceManager.h` Problem) is untouched.
- No jitter added: DESIGN.md section 5.8 says "with jitter" but the code never had any, and
  "keep behaviour identical" wins. Left for a separate change.

**Do LiveStream and StreamPlayer retries stack? No (verified by reading).**
LiveStream has two modes. In `Mode::Url` it never schedules a retry (comment at the
`Mode::Url` branch of `onState`); StreamPlayer's `runWorker` loop is the only retry. In
`Mode::BaichuanMain/Sub` LiveStream retries, and the transport feeds StreamPlayer through
`setPacketSource`; `runSession` returns "no retry" for a packet source (`!packetSource &&
...`), so StreamPlayer's loop exits after one session. One transport therefore has exactly
one loop, matching DESIGN.md ("no transport has two retry loops"). They are separate
`Backoff` instances; nothing is shared. The only interaction is the Baichuan-sub to URL
fallback on "Stream not found", which restarts a fresh attempt with fresh state.

**Reverting.** Revert the per-site commits (any one independently) and keep or drop the
module commit last; each site only depends on `core/Backoff.h`.

## Candidate 4: live arrangement

Branch `arch/linux-live-arrangement`. Each decision below is reversible on its own commit; revert the commit named.

**Problem.** `LiveViewPage.qml` held pure state logic keyed by device row: paging and wrap, the swap rule, slot repair, save and restore through "hostId:channel" strings with a pending-pane timer, and row-shift fix-ups for maximized and selected on every remove and insert. None of it could be tested without QML, and rows shifting made each rule fragile.

**Module.** `rl::LiveArrangement` (`src/ui/LiveArrangement.{h,cpp}`), registered in `main.cpp` next to `MosaicLayout`. Interface: `setSaved`, `setCameras(keys)`, `reset()`, `assign(pane, key)`, `turnPage`, `neighbour`, plus properties `paneKeys`, `pageKeys`, `preset`, `page`, `maximized`, `selected`, and the `saveRequested(text)` signal. Everything is keyed by camera key. The seam is the device model: QML tells the module which cameras exist and the module repairs itself, so no row arithmetic is left in QML. `test_livearrangement` exercises it through that interface only, with an old-format saved value among the cases.

**Decisions.**
1. Keyed by camera key, not row. Removing, inserting or reordering devices needs no fix-up. Maximized and selected became keys too (`maximizedKey`, `selectedKey` in the page, aliases of the module). `showCamera(row)` still takes a row because `Main.qml` calls it.
2. Persistence text unchanged: comma-joined "hostId:channel", "-" for empty, QSettings category `liveGrid`, key `arrangement`. The `Settings` object stays in QML as a thin adapter (`saved: layoutStore.arrangement`, `onSaveRequested`). The module does not touch QSettings, so tests need no fixture.
3. Deliberate small change: the module asks to save whenever the arrangement differs from what was last saved (and nothing is waiting). The QML page saved only on remove, drop and pending resolved, so a newly added camera placed in a free pane was saved only on the next unrelated change. Now it is saved promptly. A saved value shorter than 32 entries is rewritten in full form once. Revert: make `settle()` in `LiveArrangement.cpp` emit only from `assign`, removal and `giveUp`/pending resolution.
4. `MosaicPacker` and `MosaicLayout` are unchanged. QML still builds the packer's pane list (rows, shapes, Sites) from `pageKeys` or the open tab's cameras; only the source of the page's cameras changed.
5. Duplicate rules found in the mosaic tabs. The drop rule in `tabDrop` (add before the target, or swap) is now `LiveArrangement.dropInto`, tested. `Mosaics.gridOrder` re-parsed the arrangement text; it now calls `savedKeys` on a private instance so the format lives in one place. Left alone: `tabDropAllowed` (Site rule, needs `Mosaics.siteOf`), `Mosaics.cameraKey`, and the several `hostId + ":" + channel` concatenations in `MosaicEditorDialog.qml` and the page's highlight lines. They are one-liners over model data, not rules. Revert: restore the previous bodies of `tabDrop` and `gridOrder`.
6. `LivePane.qml` is untouched. It still reports drops and double-clicks by device row; the page converts rows to keys at the handlers.

**Quality state: left where it is.** Stream quality has three owners but only one is arrangement state. In a mosaic the HD choice is per camera per mosaic and already lives in the `Mosaics` store (`hd` sets), which is the mosaic's own state, not the grid arrangement. In the plain grid it is a transient, unsaved toggle inside `LivePane` (`qualityMain` = maximized or the remembered HD), tied to the pane's stream lifecycle; `PoppedWindow` always plays the main stream, with no toggle. Moving it into `LiveArrangement` would add a quality map that is never saved for the grid and duplicates the `Mosaics` store, for no rule that could be tested. If quality is ever persisted for the grid, the module is the place for it, keyed the same way.

**Behaviour change.** Only item 3. Slot repair fills gaps in camera (model) order, as before.

**Known gaps.** The model-reset case follows the old code: the arrangement is restored from the last saved text. A camera that disappears and reappears (its host refreshing its channel list) loses its pane, as before. QML-level behaviour (drag, step, maximize) was verified by loading the page offscreen, not by automated tests.


## Candidate 7: Bosch seam

Branch `arch/linux-bosch-seam`. Verdict: the premise held in part.

**What I found.** `BoschTourController` (about 1350 lines) holds four state machines in one class: patrol, tour selection, scene mode and iris. They share one `m_generation` counter, one busy flag, one message, and each other's pending flags (selection tour drives patrol calls; iris and scene mode reset together on connect and disconnect). All Home Assistant traffic goes through a single private method, `request()`, which used a concrete `QNetworkAccessManager`. The token went through the concrete `CredentialStore`. So the network seam was missing, and cheap to add.

**What I did.**
- Added `HomeAssistantTransport` (`src/device/HomeAssistantTransport.{h,cpp}`): one `send(url, token, body, reply)` method, plus `NetworkHomeAssistantTransport`, the production adapter that holds the old QNAM code unchanged (same headers, same curl user-agent).
- `BoschTourController` takes an optional transport as its second constructor argument. Null selects the network adapter, so `main.cpp` and `test_livestreamqml.cpp` are untouched. The generation check (drop replies that arrive after connect or disconnect) stays in the controller's `request()`, so it is covered by tests.
- Made `CredentialStore::storeSecret`, `lookupSecret` and `removeSecret` virtual (and added a virtual destructor) so tests can supply a fake keyring. No behaviour change.
- Added `tests/test_boschtour.cpp` (28 tests) with a scripted transport and fake credentials. It covers connect, a stale reply after disconnect, patrol reads, start and stop, a read that overlaps a mutation, saving a tour, tour selection (start, restore, failure), scene mode (read, set, unconfirmed set, stale after disconnect) and iris (status text, unsupported, press, heartbeat, release before the start reply, failure, limit).
- Q_PROPERTY and Q_INVOKABLE names are unchanged. There is no user-visible change.

**Why I stopped there.** Splitting the four state machines into separate classes would be as wide as the code it replaces. They share the generation, busy and message state, and selection tour calls the patrol methods directly. Splitting them means designing a shared-context object, which is a larger and riskier change than this seam gives. With the seam in place, the state machines can now be tested as they are.

**Not done.** Timers (poll, preset poll, iris heartbeat) are still real `QTimer`s. Tests never spin the event loop, so heartbeat resends and polling are not tested. `LivePane.qml` Bosch-feed logic is left alone, as agreed. The Bosch controller still reads and writes `QSettings` directly; tests isolate it with `QSettings::setPath`.

**How to revert.** Each step is its own commit and can be reverted alone with `git revert`. Reverting the "Add Home Assistant transport seam" commit also needs the test commit reverted first (the test depends on the seam). The `CredentialStore` virtuals are in their own commit.

## Candidate 3: playback session

Branch `arch/linux-playback-session`. Playback now has the module Live already had.

**Module.** `PlaybackSession` (`src/media/PlaybackSession.{h,cpp}`) owns transport choice
(HTTP-FLV sub / Baichuan HD / Baichuan fast sub), seek-in-place-or-reopen, the gap verdict
(stop, not an error) and "route changed while playing, so reopen". Interface: properties
`devices`, `player`, `deviceRow`, `hd`, `speed`, read-only `route`; `playAt(epoch, covered)`,
`playHttpAt(epoch)`, `stop()`; signal `reopenNeeded`. QML registers it as `PlaybackSession`.

**Seam.** `PlaybackTransport` (`src/media/PlaybackTransport.h`), shaped like
`LiveStreamTransport`: production adapter `DeviceTransport` (DeviceManager + StreamPlayer) and a
scripted adapter in `tests/test_playbacksession.cpp`. There is no scheduler seam: playback has
no timers in the module.

**File snapping.** `snapToRecordingFile()` (`src/device/RecordingFileSnap.{h,cpp}`) moved out of
`DeviceManager::playbackUrl`, which keeps a thin call. DeviceManager edits are limited to that
call and one include. The day-search code that builds `recordingFiles` and the playback offset
stayed in DeviceManager.

**Decisions (each reversible with `git revert` of the named commit).**
1. Reopen rule is one rule: reopen iff the route changes while a stream is up or connecting.
   This replaces the page's `crossed && !hdMode` and the HD tap handler's manual replay.
   Deliberate differences: toggling HD on a direct (Frigate) row no longer reopens a stream that
   would not change; and a grid pane on a Frigate row no longer tries Baichuan above 1x
   (the single-pane page already excluded direct rows); and on a direct row, speed crossing 1x no longer reopens the same HTTP URL (the old `crossed && !hdMode` rule did). Revert: the QML commit.
2. Each pane (single page and each grid pane) owns its own session. The grid's speed change
   used to be fanned out by the page; each pane now reacts to its own route change, so only
   playing panes reopen.
3. Timeline coverage (`covered`) stays in QML and is passed in; the session does not know segments.
4. Event jump keeps its old behaviour (always HTTP sub, no coverage check) via `playHttpAt`.
5. Left in QML: the EOF auto-resume loop guard (page and pane), which is a candidate for a
   later deepening with a scheduler seam.

**Revert.** Revert the commits in reverse order: QML binding, session module, snap extraction.
The snap extraction is independent and may stay.

## Candidates 2, 1, 5: device model

Three changes to `DeviceManager` and what hangs off it, made in this order on branch
`arch/linux-device-kind-session`. Each is one commit (or a short run of them) and
reverts with `git revert <sha>`; the later ones were written on top of the earlier
ones, so revert in reverse order (5, then 1, then 2).

### Candidate 2: device kind is a capability, not a string

**Module:** `src/device/DeviceKind.{h,cpp}`. `Kind` enum, `Capabilities` table
(`reolinkProtocol`, `multiChannel`, `alwaysOnline`, `frigateApi`, `detections`),
`liveUrl(LiveSource, mainStream)` (live URL per kind and quality), `directLiveUrl` (moved
here unchanged, still reachable as `rl::directLiveUrl`), and
`looksLikeBoschFeed()`. Hikvision is still only the path rule inside `directLiveUrl` (a `streamVendor()` helper was written and removed unused; making it a real capability is not done). Test: `tests/test_devicekind.cpp`.

**Decisions**
- The Database `kind` column is untouched: still the strings `camera|nvr|stream|frigate`.
  `kindFromString` maps anything unknown or blank to `Camera`, which is what an unknown
  row behaved like before (every compare was against `stream` or `frigate`).
- Capabilities are named after what they allow, not after the kind: `reolinkProtocol`
  replaces `isDirectKind(...)` (login, Baichuan, settings, PTZ, talk, playback search,
  HD), `multiChannel` replaces `kind == "nvr" || kind == "frigate"`, `frigateApi`
  replaces `kind == "frigate"` where it means "go through Frigate's API", `alwaysOnline`
  replaces `kind == "stream"`. `isDirectKind(..) || !primed` is `Entry::reolinkReady()`.
- QML reads booleans, not strings: the model gets a `multiChannel` role, and
  `hostInfo()` / `cameraInfo()` carry `direct`, `multiChannel`, `frigateApi`,
  `reolinkProtocol`, `alwaysOnline`, `hasDetections` next to the existing `kind`
  (`kind` stays for any external consumer; no QML reads it any more). `PlaybackPage`
  already used `Devices.isDirectAt(row)`; that is now answered from the table.
- The Bosch feed is still recognised by label, because that is how the feed reaches us
  (a Frigate camera named like the integration). The rule moved to one function,
  `looksLikeBoschFeed`, exposed as `Devices.isBoschFeed(label)`. Moving it to a real
  per-camera trait needs the Home Assistant entity mapping and is left for that work.
- The sidebar icon's own `kind: "nvr" | "cam"` property is an icon style chosen by its
  caller, not a device kind; it stays, minus an unreachable `"frigate"` branch.
- Deliberately not changed: `EventManager` still asks `hostInfo()` per call (now for
  `frigateApi`), it does not cache.
- No user-visible change.

**Revert:** `git revert` the commit "Make device kind a capability, not a string".

### Candidate 1: split DeviceManager (session and command set)

Done: (a) the per-host session, (b) the camera-command set, with a seam at both clients.
`DeviceManager` is still the list model and the QML-facing adapter; every `Q_INVOKABLE`
and signal QML uses is unchanged.

**Seams**
- `src/protocol/ReolinkTransport.h`: interface `call(batch)` + `lastFailKind()` (and the
  `callOne` convenience built on `call`). `ReolinkHttpClient` is the production adapter.
  `FailKind` moved into the interface; `ReolinkHttpClient::FailKind` still names it.
- `src/protocol/BaichuanLink.h`: interface for Baichuan config sessions (`open`, `close`,
  `get`, `readEnable`, `writeEnable`, `getConfigFlat`, `writeFields`, `transact`) and
  `BaichuanOpener` (factory). `BaichuanControl` is the production adapter
  (`realBaichuanOpener()`). Streaming and push (`readPushed`, `sendBinary`,
  `BaichuanClient`, `BaichuanEventListener`, talk) are not behind it; they keep the
  concrete classes.
- Scripted adapters for tests: `tests/ScriptedTransports.h` (`ScriptedHttp`,
  `ScriptedLink`, `scriptedOpener`).

**Modules**
- `src/device/HostSession.{h,cpp}`:
  - `HostSession` / `HostSessions`: one validation in flight, doubling backoff 15 s to
    300 s, the lockout rule (**Auth and Locked are never retried**, and clear a pending
    retry), poll overlap guard, the three-failed-polls rule, the API-only state and the
    liveness probe, as a state machine with an injectable clock.
  - `validateReolinkHost(ReolinkTransport&, rec, password, alreadyDegraded, opener)`:
    the login and discovery batches (GetDevInfo, GetEnc, GetAbility, GetBatteryInfo,
    GetChannelstatus, per-channel GetEnc) and the Baichuan camera-list fallback, moved
    out of the `validateAsync` lambda. A rejected login never reaches Baichuan.
  - `Problem`, `ChannelResult`, `Validation` moved here (aliased inside `DeviceManager`).
- `src/device/CameraCommands.{h,cpp}`: alert toggles (cmd 219/217/70 and set pairs), the
  recording-schedule XML (parse and single-table rewrite), generic Baichuan config
  read/write, floodlight toggle. Results are plain structs (`CommandResult`,
  `CommandFailure`); `DeviceManager` maps them to the same translated messages as before.
- Tests: `tests/test_hostsession.cpp` (backoff table, lockout rule, poll/probe rules, login
  classification through the HTTP seam, every command through the Baichuan seam).

**Decisions**
- Session state lives beside, not inside, the model rows: `Entry` keeps what QML shows,
  `HostSession` keeps what the retry/poll rules need. One flag per host (`beginPoll`) now
  guards both Reolink and Frigate polls (they never shared a host id, so no behaviour
  change; the old string-keyed `m_pollInFlight` is gone).
- Translation context: the moved status strings still use the `rl::DeviceManager`
  context via `QCoreApplication::translate`, so existing `.ts` entries keep matching.
- `removeDevice` does not yet `forget()` the host's session (as before, state for a
  removed host id was never cleaned; ids are not reused).

**Left (not done, not risky to do next)**
- `fetchSettings` / `applySetting` / quick replies / auto-reply / `pollDetections` batch
  building still live in `DeviceManager` and call `ReolinkHttpClient` directly. They could
  move into `CameraCommands` (HTTP half) now that `ReolinkTransport` exists.
- `Entry.client` and `Validation.client` stay `shared_ptr<ReolinkHttpClient>` because
  downloads, snapshots and playback search use its non-interface methods.
- `DeviceManager.cpp` is still about 2,400 lines; Frigate validation, recordings search and
  the HD clip pipeline are the next candidates.

**Revert:** `git revert` the commit "Split DeviceManager: host session and camera commands
behind protocol seams".

### Candidate 5: one detections module per host

**Module:** `src/device/Detections.{h,cpp}`. `HostDetections` (one per host, kept in
`DeviceManager::m_detections`) takes every source's message and returns `Detection`
values (`channel`, `type`, `camera`) for `DeviceManager` to emit as `detectionEvent`.
It owns:
- 0-to-1 edge detection per channel (`api::risingDetections`) for poll and push alike, so
  the two share one "last state";
- push-over-poll takeover: `setPushLive`, `pollQueriesStates(channelCount)`, and `onPoll`
  dropping a poll that was sent while push was live or is answered while it is, and
  carrying over the pushed `visitor` flag (a poll cannot see a doorbell press);
- Frigate's watermark and its first-poll rule (first poll raises nothing, only sets the
  watermark; `RL_FRIGATE_BACKFILL` seconds supported), and the burst gate shared by the
  Frigate poll and websocket, so an event seen on both is raised once.
`BurstGate` and `sameBurstWindow` are also used by `EventManager::applyFrigateHistory`
(its private `lastRaised` map and its inline `abs(ts - ts) < kBurstGapSecs` became those),
so the live path and the history catch-up cannot drift apart.
Tests: `tests/test_detections.cpp` (table tests for takeover, burst boundary at 30 s,
watermark, poll/socket dedup in both orders, history window).

**Decisions**
- Logic only: the sockets and timers (`BaichuanEventListener`, `FrigateLive`, the poll
  tick) stay in `DeviceManager`, which feeds the module. A `DetectionHub` QObject owning
  the listeners too would be the next step; it needs a camera-name resolver from the
  model, so it was left.
- Burst key is now `camera/type` inside a per-host gate; it was `host/camera/type` in a
  global map. Same behaviour, since a gate belongs to one host.
- Reolink poll with push live on a single-channel host still queries states, as before
  (the poll is that camera's health check).
- Rest of `EventManager` (532 lines, no test): its other rules are persistence concerns
  (retention: motion 24 h, the rest 7 days under 25,000 per recorder, notification gating by `pushEnabledFor`,
  thumbnail capture) and do not duplicate the detection rules, so they were not moved.
  The one duplicate (burst gate and history window) is shared as above. A table test for
  retention and the history de-duplication needs an in-memory `Database`; left.
- No user-visible change.

**Revert:** `git revert` the commit "One detections module per host".

### Build and test notes

`ctest` runs offscreen without a display or keyring. `test_mosaicpacker` has a wall-clock
assertion (`tieBreakBuildTime`, under 10 ms) that fails when the machine is loaded (seen
at load average 29, passes at load 4); it does not touch this code.

## Review #2 (2026-11-16): transport across the video tabs

Branch `arch/transport-deepening`, one commit per item below; each is reversible with
`git revert` of the named commit (revert in reverse order: 2e9a711 pane bar fixes, 948a0b3 review fixes, 788d34c,
f87457c, f4df1b9, 2f42811, d95c2ab). Scope came from the hot spots of the last 80 commits. Not built, with reasons, at the
end.

### 3a: seek in place per stream (`d95c2ab`)

**Problem.** DeviceManager kept one weak `m_playbackClient` + `m_playbackRow` for in-place
seek. Playback (and several Live replay panes) start every pane in turn, so each pane found the
slot held by the pane before it and reconnected on every scrub.

**Decision.** The seek lives on the stream: `StreamPlayer::setPacketSource(…, onSeek)` and
`seekSource(epoch)`. The handle moves with the source's stop handle (pending → active on start,
cleared on stop/setSource), so a stopped client is never seeked. Live, live buffer and URL
sources have none and reopen. Test: `test_livestream::seekGoesToThisPlayersOwnSource`.

### 3b: unreachable recorder HTTP-FLV playback (`2f42811`)

`PlaybackSession::chooseRoute` returns `Http` only for direct rows; the Reolink branch of
`playbackUrl` needed `reolinkReady()` (not direct). Removed that branch, `RecordingFileSnap`
and its tests, `Entry::playbackOffsetSecs` / `recordingFiles`. **Supersedes** candidate 3's
"the snap extraction may stay". Kept: `api::playbackFlvUrl` (an endpoint checked on hardware)
and the `forReplay` split of the day search (Live replay uses it).

### 4a: event filing fixes (`f4df1b9`)

`Database::eventNear` matches type (bursts are per camera and type); `device::Detection::at`
carries Frigate's start through `reportDetection` / `detectionEvent` (filed at
`min(at, now)`); unread, snapshot and notification only for a stored row. Not done: the
history-then-live duplicate (narrow; ticket 02, fixed), the Inbox module (built: see
"4b: the Inbox"), deduplicating the switch-off check (DON'T: one implementation, four consumers; the
source gate protects Highlights).

### Candidate 2: EventClip is the clip player's state machine (`f87457c`)

**Module.** `EventClip` owns the phase (`Idle/Starting/Playing/Paused/Ended/Failed`), the paused
moment, the give-up times (10 s at 1×, 30 s off 1×, 20 s Frigate), the reopen-once-per-place
rule, the clock and its `PlaybackGroup`. **Seam.** Events in (`play`, `togglePause`, `seekTo`,
`skip`, `speedChanged`, `hdChanged`, `keepPlaying`, `firstPicture`, `streamReconnecting`,
`streamStopped`, `mediaEnded`, `loadFailed`, `openFailed`, `reopenNeeded`, `tick(wallMs)`; bound
`frameEpochMs`, `mediaPositionMs`), commands out (`openWanted`, `stopAllWanted`,
`stopStreamWanted`, `pauseMediaWanted`, `seekMediaWanted`, `retryHdWanted`, `notice`).

**Decisions.**
1. Not driven through `PlaybackTransport`: the Frigate clip plays through `ClipLoader` /
   `MediaPlayer`, outside that seam, so commands are signals the QML binds to its engines.
2. The clock is passed in (`tick(wallMs)`), as Backoff decided: no timer or fake clock in C++;
   the give-up count starts at the first tick of each start.
3. `edgeReached` / `reopenAfterEnd` became private; their table tests became sequences through
   the interface. `progress` / `timeAt` / `skipFrom` stay public (the pane bar binds them).
4. Behaviour unchanged: before/after smoke logs of a recorder and a Frigate event clip match.

### Candidate 1: PlaybackDay and PaneSync (`788d34c`)

**Module.** `PlaybackDay` (`src/ui/PlaybackDay.{h,cpp}`): one day's footage per camera, the lead
camera, `previousEnd` / `nextStart`, `landing` (backwards), `carryOn` (the lead's stream ran
out), `wantSearch` (3 tries, 10 s), `advance` (the clock), and the panes' frame driver and
restart verdicts from plain pane state. `PaneSync` (`src/ui/PaneSync.{h,cpp}`) holds the two
rules Playback and `LiveReplay` share.

**Decisions.**
1. A sibling of `LiveReplay`, not a mode of it: `LiveReplay` is built around now, cross-day
   books, pending plays, the oldest scan and the live buffer; a one-day flag would make its
   interface shallow.
2. In step: one rule, Live replay's (both ways, 5 s × speed, 3 s gap). **Visible change** for
   Playback forwards (was backwards only, 5 s flat). Frigate panes are locked off 1×. To go
   back, change `panesync::needsRestart` (reverting the commit would take PlaybackDay with it).
   On-screen checks (real display, three recorders; `5d7c459`) settled the open question:
   - a pane catching up by itself (`aligning`: catch-up ≠ 1×) is never also restarted, and the
     3 s gap counts from the pane's own last open (`lastOpenMs`), not only this rule's restarts;
   - a driver that isn't playing is started like any pane. The driver is exempt only from
     drift restarts. Before, a lead joined by J was never opened and Live replay froze (seen on
     the old build too: the joined pane said "Ready", the first pane was seeked back every few
     seconds).
   - Both pages now pass panes as `PaneSync::paneFrom` maps (one shape; the standards review's
     data clump).
6. Small differences noted by review: the wall-clock playhead now moves backwards from 23:59:59
   (it stuck there); EventClip's give-up count starts at the first 200 ms tick of a start.

### 3c: RecorderDays, one camera's day of recordings (`9a508dc`, `1eb48de`, review fixes)

**Module.** `RecorderDays` (`src/device/RecorderDays.{h,cpp}`): `search(key, y, m, d)` answers
`dayFound(key, y, m, d, segments, monthDays)` / `dayFailed(key, y, m, d, error)` /
`dayNotReady(key, y, m, d)` (the camera can't be asked yet; added for ticket 07). Vendors are
adapters behind a `DaySearch` (`reolinkDaySearch`: the HTTP `Search` over `ReolinkTransport`;
`frigateDaySearch`: recordings + summary, with a cancel flag). `daySegments(files, date)` is
the one segment rule. `DeviceManager` hands it key → search (`rowOfCameraKey`, exact) and
owns it (`Devices.days`); the four old searches and five old signals are gone.

**Decisions** (wayfinder/architecture ticket 01, with its three amendments).
1. Keyed by camera key, not row: rows shift while a search is out. Live replay already spoke
   keys; Playback moved to them.
2. Always dated, and no `purpose` argument: one answer serves both pages. Playback takes it
   only for a pane's camera and the day shown (`PlaybackDay::takes`, tested), which closes the
   undated answer that could land on a newer day; a Live replay search can fill Playback's
   same camera and day.
3. The Reolink search is HTTP (the review said Baichuan): `ScriptedHttp` is its test double.
   Frigate's adapter is tested against a local stand-in server, including cancel.
4. Its own pool (4 workers); going cancels and waits at most 2 s (as `ClipLoader`, ticket 05).
5. Stream opening stays in `DeviceManager` (DON'T, as decided).
6. Merging touching recordings stays in the Frigate adapter (`frigate::mergeRecordings`), as
   before: a Reolink recorder's files are its own and are drawn as they come.
7. Both adapters take the cancel flag: Frigate's requests stop within ~50 ms; a Reolink search
   passes it to `ReolinkTransport::call(commands, cancel)` (new overload, default ignores it),
   which the HTTP client honours, though one queued behind another request to the same
   recorder waits for it (up to 15 s; the tray Quit's 3 s exit covers that).
8. Live replay never asks for a Frigate camera's day (the page answers `dayFailed`), as the
   old `searchReplayDay` refused it.
9. Behaviour unchanged on screen: Playback (two recorders + Frigate) and Live replay (from
   now, joined) log and draw as before. Found while checking, older than this work: a Frigate
   camera's Playback day stayed empty when Playback opened at start (ticket 07, fixed:
   "not ready" is its own answer and not a try; Playback asks for unanswered days every 10 s
   while it shows).

Revert: the review-fix commit, then `1eb48de`, then `9a508dc` (each builds and passes alone).

### Candidate 5: one press rule (`3e64cda`, `6e78432`, review fixes)

**Module.** `PaneBarRules::press(state, idOrKey)` → `{ id, refusal }`
(`src/ui/PaneBarRules.{h,cpp}`; QML `PaneBarRules.press(map, idOrKey)` and `PaneBar.press(k)`).
`id` is the button to apply (`TransportActions.apply`, unchanged); `refusal` is the greyed
button's reason. Built on `transport()` at the widest step, so a key and its button can't
disagree, and a key still acts when its button has collapsed off a narrow pill.

**Decisions** (wayfinder/architecture ticket 04, plus the agreed plan).
1. Keys and button ids share one string space ("J", "Left", "fastBack"…); `keyId` moved in.
   J at live is `back` (◀ at 1×). J while a Live replay runs still steps « without restarting
   a paused replay's direction (`LiveReplayControls.key`, the one key rule left in QML).
2. Every key path asks it: Playback's five shortcuts, Live (`LivePane.replayKey` →
   `paneBar.press`), Live replay, the Events clip player. Pane-bar buttons keep calling
   `apply`: a greyed one can't be tapped and shows the reason as its tooltip.
3. Playback's bottom bar takes dim / lit / reason from `transport()` with a group state
   (`barRules`: a recorder, speedLocked when every pane is Frigate). Enabled buttons keep
   their own how-to tips. New on screen: its back buttons also grey when reverse isn't
   available (never today; ticket 12 made reverse universal).
4. A refused key shows its reason for 2 s: the pane's notice (Live, Events; other notices
   keep 4 s) or Playback's speed line, cleared by the next press that acts. K, L and → at
   live now say "Nothing ahead of live…" (their shortcuts stay on); J on a Frigate clip is
   now taken by the clip player, not passed on.
5. Not modelled: "ended" (an ended clip is paused, `EventClip::paused`), so keys act as when
   paused. The QML wiring has no tests (the page key paths stay screen-checked).
6. Left as judgement calls from review: the "refusal → show, else apply" shape is repeated in
   three callers, and `barRules` repeats `PaneBar.rules`' map by hand.

Revert: the review-fix commit, then `6e78432`, then `3e64cda` (each builds and passes alone).

### 4b: the Inbox, one filing path (`490a4a6`, `ee7ac4d`, review fixes)

**Module.** `Inbox` (`src/device/Inbox.{h,cpp}`), knowing only `Database` and
`EventSwitches`, with the clock and the cap passed in. `file(candidate, Live|History)` →
`Filing { verdict, row, announce, needsThumbnail }`; `fileHistory(batch)` (one per camera,
type and burst, oldest first; filings in the batch's order); `fileStatus(...)` (connectivity);
`enforceRetention()`, `recount(host)`, signal `trimmed`. `EventManager` keeps the list model,
unread count, desktop notices and thumbnail fetching; its doors call the Inbox and put stored
rows in the list at their place by time (`take`, `insertSorted`).

**Decisions** (wayfinder/architecture ticket 03, plus the agreed plan).
1. Reolink recorder history is treated as coming (the user), so the Inbox is built now.
2. Verdicts: stored, switched off, too old, same event (a stored row covers it), same burst
   (a history batch already filed one), not stored. "Same event" and "same burst" are kept
   apart: one met the store, the other only the batch's gate.
3. Refusals (switched off, too old) come before a batch's burst gate, so an event that isn't
   filed doesn't take its burst (found in review; `main` had it right).
4. The live burst gate stays in `HostDetections`; `DeviceManager::reportDetection`'s early
   switch-off drop stays (the Inbox checks again: one implementation, `EventSwitches::isOff`).
5. Rows come back as return values, not a `filed` signal as planned: a history batch can be
   hundreds of rows and the caller emits `storedChanged` once per batch (now the host's id,
   not -1; `EventFeed` handles both). A cap or retention trim during a filing reloads once.
6. The gap ticket 02 left: a live report of a stored row without a thumbnail grabs a camera
   frame for it, unless Frigate's snapshot for that row is still on its way
   (`m_snapshotsDue`, so a frame can't overwrite the right picture).
7. New and harmless: a live event older than its type's retention would be refused (none are).
   The history batch's gate is keyed by host and channel, not Frigate's camera name.
8. Checked on screen: with the Frigate host's events removed from a scratch copy of the
   store, the start-up catch-up filed the same 268 rows over the same span as the build
   before (pictures fetched; the feed showed them).

Revert: the review-fix commit, then `ee7ac4d`, then `490a4a6`.

### Leftover: test hooks in one object (`a004f67`, `6a8591c`)

`TestHooks` (`src/ui/TestHooks.{h,cpp}`, the `TestHooks` QML singleton) reads the 17 `RL_*`
variables QML used once, with their documentation; it replaces 17 context properties in
`main.cpp` and the `typeof x !== "undefined"` guards. `startHidden` (a real option) and the
hooks C++ reads stay where they are. Behaviour unchanged (each variable read the same way;
checked on screen with the Events keys, Playback autoplay at a time, and Live replay). Rest of
wayfinder/architecture ticket 06: settings commands → ticket 08 (below); DetectionHub not yet.

Revert: `6a8591c`, then `a004f67`.

### Leftover: DeviceSettings, settings commands by camera key (`ccf87fc`, `d70cdb9`, review fixes)

**Module.** `DeviceSettings` (`src/device/DeviceSettings.{h,cpp}`; QML `Devices.settings`):
the HTTP settings commands (fetch with retries and V20 fallbacks, apply), quick replies and
auto-reply, alert switches, recording schedule, Baichuan config, floodlight, reboot, the
motion-zone codec, and the push switch for desktop notices. `DeviceManager` hands it one
lookup, camera key → `CameraLink { ready, admin, kind, channel, http, baichuan }`, and a
Baichuan opener; `CameraCommands` stays its protocol layer. `DeviceManager.cpp` went from
2,469 to 1,970 lines.

**Decisions** (wayfinder/architecture ticket 08).
1. Keyed by camera key: calls name the camera, answers carry the key, and QML handlers
   match it (`Devices.cameraKeyAt(row)`; the Settings page and doorbell keep the key they
   asked with).
2. The push switch is kept by camera key. It was by row, so a notice could follow another
   camera's switch after the list changed (fixed). An event on a camera with no row of its
   own is now allowed (it took the host's first row's switch).
3. `setRotationOverride` stays in `DeviceManager` (a local view setting).
4. `kind` is carried for a second command set (Bosch); nothing reads it yet.
5. New, stricter: Baichuan writes check the camera is ready (as reads did), and an unknown
   alert kind answers instead of staying silent.
6. Left as found: the worker threads are waited for without a time cap at teardown (as
   before; `RecorderDays` cancels and caps at 2 s), and `tr()` runs on workers for some
   wordings (static, as before).
7. Checked on screen: Settings → System (HTTP) and Detection (Baichuan) read from the RLN36
   through the new path. Writes weren't tried: the scratch run's login isn't an administrator.

Revert: the review-fix commit, then `d70cdb9`, then `ccf87fc`.

### Feature: OfflineAlerts, desktop offline alerts (`e363a33`, `f2577fb`, `b549b94`)

**Module.** `OfflineAlerts` (`src/device/OfflineAlerts.{h,cpp}`, `test_offlinealerts`) decides
when a connectivity change becomes a desktop alert. It is pure (the clock is passed in) and sits
beside the Inbox: the Inbox files Status rows, OfflineAlerts decides pop-ups. `EventManager`
hands it each change, shows what comes back through `Notifier::alert` (one bubble per camera
key, critical with `sound-name`, a click → `liveActivated`) and arms one timer at `nextDue()`.

**Decisions** (wayfinder/urgent-offline-alerts 05, 08).
1. A camera waits 30 s; a whole host alerts at once, since `HostSession`'s 3 polls + Baichuan
   probe already waited. Found down at the first connect (`atStart`), a host waits 30 s too.
2. `DeviceManager::connectivityKnown` is alert-only (nothing filed): what this run's first
   listing of a host finds down, and cameras still down when a host recovers. Changes stay
   `connectivityChanged`.
3. Recorder wording asks `capabilities().multiChannel`. At start-up a host's cameras aren't
   listed yet, so that alert has no camera count.
4. The old 7 s offline notice under `desktopNotifications` is removed (no double pop-ups).
5. Left as found / declined (review judgement calls): EventManager now owns the alert timer and
   switch (a small controller could take them if more alert kinds come); the
   `(hostId, channel, name, online, cameras)` clump stays as the existing signal's shape.

Revert: `b549b94`, then `f2577fb`, then `e363a33`.

### Not built

- **3c recorder media module:** day search built (above); DON'T for stream opening (one
  adapter; shared with HD live).
- **Candidate 5 one press module:** built (above).
