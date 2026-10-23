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
   (the single-pane page already excluded direct rows). Revert: the QML commit.
2. Each pane (single page and each grid pane) owns its own session. The grid's speed change
   used to be fanned out by the page; each pane now reacts to its own route change, so only
   playing panes reopen.
3. Timeline coverage (`covered`) stays in QML and is passed in; the session does not know segments.
4. Event jump keeps its old behaviour (always HTTP sub, no coverage check) via `playHttpAt`.
5. Left in QML: the EOF auto-resume loop guard (page and pane), which is a candidate for a
   later deepening with a scheduler seam.

**Revert.** Revert the commits in reverse order: QML binding, session module, snap extraction.
The snap extraction is independent and may stay.
