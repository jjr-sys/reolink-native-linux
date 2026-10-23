# Architecture review

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
