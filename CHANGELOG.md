# Changelog

All notable changes to this project are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and
versions follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
While the project is pre-1.0, minor versions may still change behaviour.

## [0.7.1-jjr] — 2026-09-20

### Changed

- **Activity mode: tiles stop hopping.** A tile keeps the camera it picked up
  after its hold ends instead of snapping back to its own camera; when new
  activity needs a tile it takes the one that has been inactive longest (a tile
  never used for activity counts as longest). A camera that is already on screen
  is highlighted where it is, never moved to a second tile. Leaving Activity mode
  still restores your layout exactly.
- **Choose what Activity mode tracks.** The ▾ beside the Activity button lists
  Person, Vehicle, Pet, Doorbell visitor and Motion, remembered between runs.
  Motion is off by default, since it fires the most.
- The Live View grid is silent while the doorbell pop-up is showing.

## [0.7.0-jjr] — 2026-09-20

### Added

- **Frigate servers as a site.** Add Device has a new **Frigate** tab: enter
  the server's address (port 5000, no login) and every enabled camera on it
  appears in the sidebar under its own server heading, like an NVR's cameras.
  Live view plays through Frigate's own video proxy, so no extra port needs to be
  open.
- **Frigate detections everywhere.** People, vehicles (car, truck, bus,
  motorcycle, bicycle) and pets (dog, cat) that Frigate detects show in the
  events inbox with a thumbnail, raise desktop notifications, and take tiles in
  Activity mode. Detections are checked every 10 s; one person crossing the yard
  is one event, not one per tracked object.
- Frigate cameras have no talk, PTZ, settings, playback or download, and always
  play their single stream.
- `RL_FRIGATE_BACKFILL=<seconds>` treats the last N seconds of Frigate events as
  new, for testing.

## [0.6.0-jjr] — 2026-09-20

### Added

- **Activity mode** (Live View toolbar). Cameras with detections take over idle
  tiles: person, vehicle, pet and visitor take a tile straight away; plain
  motion only fills a free one. A camera keeps its tile at least 30 s after its
  last detection (and at least 15 s once shown), then the tile goes back to its
  own camera. Your saved arrangement is never changed and comes back when you
  switch Activity mode off.
- **Why it changed:** an amber border (inside the blue selection border, so both
  show) and a badge such as "Person · Driveway · 14:02". The tile cuts straight
  to the camera's live picture. The selected tile and any camera you drag into a
  tile stay put. When every tile is busy, new activity waits (the button shows
  `+N`); nothing is dropped.
- At most two new streams start at once per NVR.
- `RL_MOCK_ACTIVITY="person:1:5@6;motion:1:7@8"` (`type:hostId:channel@seconds`)
  starts Live View in Activity mode and plays scripted detections, for testing.

### Known limits

- Detections are seen up to about 10 s late (the existing poll), and only when
  they start: a person who stays in view does not extend the hold.
- Replaying the moment that started the activity was tried and dropped: the
  recording played a short snippet and stopped, so tiles show live instead.

## [0.5.0-jjr] — 2026-09-20

### Added

- **Doorbell overlay shows the live picture.** When someone presses the
  doorbell, the pop-up now plays the doorbell's own video (it used to be a
  placeholder).
- **Answer starts two-way talk.** The button becomes **Hang up** while you are
  talking; the doorbell's sound is muted meanwhile so it can't feed back.
- **Quick replies come from the doorbell.** One button per clip the device
  reports, played on the doorbell's own channel (it used to be fixed to
  channel 0). If the NVR's normal list is empty, the app also asks the device
  over its native protocol. A doorbell with no clips says so; a list that can't
  be read shows the reason.

### Changed

- Quick-reply and talk problems show in the pop-up instead of a generic toast.
- `RL_MOCK_DOORBELL=<hostId>:<channel>` aims the test doorbell event at a real
  camera.

## [0.4.0-jjr] — 2026-09-19

### Added

- **Downloads manager.** Mark a start and an end on the playback timeline
  (⇥ and ⇤ buttons, at the playhead), then **Download range…** to save it. Any
  length: the old five-minute limit is gone. A range can cover several cameras
  at one site; each camera is its own item. Downloads to one NVR run one at a
  time (it serves one clip at a time), different NVRs in parallel.
- **Downloads list** (⬇ Downloads button, with a count while anything is
  waiting or running): camera, time range, status and progress for each item,
  with Cancel (removes what was saved so far), Retry, Open folder and Remove.
  The list lasts for the session; the files on disk are the lasting record.
- **Save folder** setting, remembered (default `~/Videos/Reolink`), and file
  names that include the site — `Woorabinda_Kitchen_20260919_142000.mp4` — so
  two cameras with the same name at different sites can't clash. An existing
  file is never overwritten.
- The old **Export clip** menu (15 s to 2 min) now goes through the same queue.
- **Playback speed** menu (0.25×, 0.5×, 1×, 2×, 4×, 8×) and **back / forward
  10 seconds** buttons, for one pane or the whole grid. Faster speeds can only
  go as fast as the link delivers the video, so 8× is realistic on the SD
  stream but not on HD over a slow link. Sound plays only at 1×. There is no
  reverse play: the cameras' streams only run forwards, so "back" jumps back.
- The range is marked with one red record button: press for the start, press
  again (now a stop icon) for the end, and the download dialog opens. The
  ±10 second buttons say "-10s" and "+10s", and after one the last picture
  stays up until the new position's first picture arrives.
- **Snapshot button** on the playback page: saves a JPEG of the picture on
  screen (every playing pane in the grid) into the save folder, named like a
  clip, e.g. `Woorabinda_Kitchen_20260919_142000_snapshot.jpg`.
- The **timeline** shows the time (with seconds) while you drag the red playhead
  as well as when you hover, and shades the range you have marked.

### Changed

- When a range spans more than one of the NVR's recording files, each file is
  saved as its own part (`…_part1.mp4`, `…_part2.mp4`) rather than only the
  first being kept.
- Long downloads no longer stop after a fixed time; they give up only when the
  NVR stops sending data for 90 seconds. The NVR cuts a clip before it starts
  sending (about 1.6 seconds per minute of footage), which the list shows as
  "Preparing". A download also checks there is enough free disk space first.

## [0.3.0-jjr] — 2026-09-19

### Added

- **Sound in playback.** Recorded playback (single pane and the 4-camera grid)
  now plays the camera's audio. A speaker (mute/unmute) button appears next
  to Stop once the stream has an audio track. Playback starts muted the first
  time; after that your choice is remembered. In the grid only
  the pane you click is audible (click it again to deselect).
- **Previous / next paging in live view.** When there are more cameras than
  grid cells, the ◀ ▶ buttons and a "page / pages" label step through them, and
  wrap around. On a maximized camera the same buttons step to the previous or
  next camera.

### Fixed

- Playback sound now stays within about 200 ms of the picture (a short, fixed
  buffer replaces live view's longer one), a seek no longer leaves a tail of
  sound from the old position, and audio waits for the first picture instead of
  playing ahead of it.
- A camera the NVR can't relay over RTSP (a third-party camera on a Reolink NVR
  answers "404 Stream Not Found") used to sit on "Connecting…" forever. The tile
  now says why, keeps the message while it retries, and switches to the NVR's
  native stream, which does carry the camera.
- The live grid held at most 16 cameras, so a 17th could only be opened from
  the sidebar. It now holds up to 32.

## [0.2.0-jjr] — 2026-09-19

First release of the **jjr-sys fork** of
[TodesengelX/reolink-native-linux](https://github.com/TodesengelX/reolink-native-linux),
based on upstream v0.1.8. The app reports itself as `0.2.0-jjr` and shows a
FORK badge next to the version; the update check follows the fork's releases,
not upstream's.

### Added

- **Two-way talk.** The microphone button on a live pane now works: click to
  start talking to that camera's speaker, click again to stop. Audio is
  captured from the default microphone, resampled, IMA-ADPCM encoded and sent
  over the native Baichuan talk channel (the format and block size are read from
  the camera's own talk ability). The camera's playback is muted on your side
  while you talk, so its speaker can't feed back into the microphone. Talk stops
  automatically when the pane is hidden, re-pointed at another camera, or the
  page is left, and always sends an explicit end-of-talk so the camera's speaker
  is released. Failures (busy camera, no microphone, unsupported camera) show on
  the tile.

- **Live audio from cameras.** The live grid, a maximized pane and pop-out
  windows can now play the camera's sound. In the grid, sound follows the
  selected tile (the one with the accent border): click a tile to hear it, and
  the previous one goes silent, so only one camera is ever audible. Click the
  selected tile again to deselect it and mute. A small
  speaker marks the tile you are hearing. Pop-out windows start muted with a
  speaker button in the corner. Works over both RTSP (grid tiles) and native
  Baichuan (HD/maximized). AAC and G.711/PCM are decoded and resampled to
  48 kHz stereo; muted panes decode no audio at all. Output follows the system
  default (e.g. Bluetooth headphones). Recorded clips and playback are still
  video-only.

- **Smoother live view.** Live sound is buffered so it no longer chops, and
  live RTSP video in grid tiles and pop-outs is held back about a second and
  shown on its own timestamps, so a network stall or burst no longer freezes and
  then rushes the picture. Sound follows the same delay.

## [0.1.8] — 2026-08-12

### Fixed

- **Playback failed with a black screen that retried forever** on systems with
  FFmpeg 8 or newer. Reolink devices serve HTTPS with a self-signed
  certificate, which the app has always accepted; FFmpeg 8.0 changed its
  default to reject it. Live view was unaffected because it uses RTSP. This
  only ever hit builds from source (Arch and other current distros) — the
  AppImage and Flatpak bundle an older FFmpeg — but it would have reached
  those too as their base images move forward.
- **Playback stopped dead at every event marker.** The NVR streams one
  recording file per connection and stores motion events as separate files, so
  continuous footage ended exactly where an event began. Playback now
  continues across recording boundaries automatically.
- **Removing a device and adding it back left its header stuck** on the first
  camera's name and "connecting…" forever, and a re-add could briefly show
  "undefined" in place of the camera count.

### Changed

- Clicking an event now starts playback **5 seconds before** the moment, so the
  approach is visible rather than only the aftermath.

### Internal

- Continuous integration: every push and pull request now builds and runs the
  test suite, including a second build against the newest Qt and FFmpeg so
  upstream changes surface before they reach anyone building from source.
- New regression test covering the TLS behaviour above.

## [0.1.7] — 2026-08-05

### Added

- Per-camera view rotation: right-click a camera in the sidebar →
  **Rotate view 90°** turns its picture in 90° steps, persisted per camera
  and applied in live view and both playback modes — for devices whose
  firmware misreports orientation. ([#3])

### Fixed

- HD live view failed ("Could not find codec parameters") on cameras whose
  newer firmware doesn't report the main-stream codec the way the app
  expected — it assumed H.264 and choked on H.265 streams. The codec report
  is now normalized across firmware spellings, and when it's genuinely
  unknown the stream is probed instead of assumed. ([#4])
- Cameras attached through a Home Hub could show rotated 90° because the hub
  declares their resolution transposed while the stream arrives upright; the
  auto-rotation heuristic now also requires the decoded frame to actually be
  portrait before correcting. ([#3])

[#3]: https://github.com/TodesengelX/reolink-native-linux/issues/3
[#4]: https://github.com/TodesengelX/reolink-native-linux/issues/4

## [0.1.6] — 2026-08-05

### Added

- **Synced multi-camera playback**: a 4-pane playback grid driven by one
  calendar and one timeline, like the official client's split playback. The
  timeline shows the union of every camera's recordings with a thin coverage
  lane per camera, and one playhead drives all panes to the same wall-clock
  moment; a camera with no footage there says so instead of looking broken.
- Playback grid interactions: drag cameras in from the sidebar or between
  panes (swaps, never drops), double-click to maximize a pane and again to
  restore, digital zoom with drag-to-pan in every pane, and an SD/HD toggle
  on the maximized pane that plays the full-resolution main stream with
  in-place seeking.
- Live view: drag cameras from the sidebar into any grid cell and drag panes
  between cells to rearrange; the arrangement persists across restarts.
- Sidebar recovery actions: right-click a device for **Reconnect** and
  **Update credentials…** — fix a wrong password in place instead of
  removing and re-adding the device.

### Fixed

- A device that was unreachable when the app started stayed "connecting…"
  forever; unreachable devices now retry automatically with backoff and
  recover on their own. Sign-in failures are shown with the device's
  attempts-left lockout warning and are deliberately never auto-retried.
- Adding a device now checks it first: a wrong address or rejected password
  is an inline message in the dialog instead of a phantom row.
- Rearranging grids no longer reconnects running streams — panes carry their
  streams with them, in live view and playback both. Restoring a maximized
  view reconnects nothing.
- Switching live-view layouts briefly holds hidden panes' streams so flipping
  between presets (or maximize and back) is instant.
- Concurrent commands to a device are now queued instead of racing, which
  was the main source of the NVR web server's intermittent 502 errors.

## [0.1.5] — 2026-08-02

### Fixed

- **Launching the app again raises the running window instead of starting a
  second copy.** Because closing the window leaves the app monitoring in the
  tray, relaunching from the launcher is the normal way back in — but every
  launch used to build a whole second client, with its own tray icon, NVR login
  and decoder set. They accumulated over a day of use.
- **Hardware video decoding now works in the AppImage.** The bundled `libva`
  could not load the host's VAAPI driver, so every stream silently fell back to
  software decode. The host's own `libva` is now used, with the bundled copy
  kept as a fallback so the app still starts on systems that have none.

## [0.1.4] — 2026-08-01

### Added

- System tray with background monitoring — close to tray, start on login, and
  an unread-event badge, so detection alerts keep working with the window shut.
- Offline and online alerts for cameras and for the NVR itself.
- Real thumbnails for each event in the inbox, and filtering events by camera.
- Detection events marked as red ticks on the playback timeline.
- Clip export — save the recording around the playhead to a file.
- Weekly recording-schedule editor: a 7×24 grid per recording type.

### Fixed

- Tray **Quit** always exits the app, instead of only removing the tray icon.

## [0.1.3] — 2026-07-30

### Added

- Clicking a detection notification opens the app, switches to Playback and
  plays that event back.
- The app version is shown in the nav bar.

### Fixed

- Clicking a notification now raises the window on Wayland. The compositor
  ignores a bare activation request as focus stealing, so the notification
  daemon's activation token is forwarded instead.

## [0.1.2] — 2026-07-30

### Fixed

- The AppImage runs natively on Wayland instead of falling back to XWayland;
  it now bundles the Qt Wayland platform and EGL client-buffer plugins.

## [0.1.1] — 2026-07-30

### Added

- Detection-zone editor — paint the parts of the image a camera should ignore.
- 6-camera live grid.
- Desktop notifications for detections, gated on each camera's Push setting.
- Built-in update checker, with one-click self-update for the AppImage.
- Event retention cap so the inbox stops growing without bound.

### Changed

- Device tree nests cameras under their NVR, with drawn NVR/camera icons and
  clearer online status.
- Right-click menus and the Add Device dialog restyled to match the rest of
  the app.

## [0.1.0] — 2026-07-11

Initial development release: live view, playback, events and device settings,
published as an AppImage and a Flatpak bundle.

[0.1.8]: https://github.com/TodesengelX/reolink-native-linux/compare/v0.1.7...v0.1.8
[0.1.7]: https://github.com/TodesengelX/reolink-native-linux/compare/v0.1.6...v0.1.7
[0.1.6]: https://github.com/TodesengelX/reolink-native-linux/compare/v0.1.5...v0.1.6
[0.1.5]: https://github.com/TodesengelX/reolink-native-linux/compare/v0.1.4...v0.1.5
[0.1.4]: https://github.com/TodesengelX/reolink-native-linux/compare/v0.1.3...v0.1.4
[0.1.3]: https://github.com/TodesengelX/reolink-native-linux/compare/v0.1.2...v0.1.3
[0.1.2]: https://github.com/TodesengelX/reolink-native-linux/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/TodesengelX/reolink-native-linux/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/TodesengelX/reolink-native-linux/releases/tag/v0.1.0
