# Changelog

All notable changes to this project are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and
versions follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
While the project is pre-1.0, minor versions may still change behaviour.

## [Unreleased]

### Added

- **New Playback speed controls.** The speed menu is replaced by a centred group:
  « · slow back · ◀ · ⏸ · ▶ · slow forward · », with the speed written above it ("▶ 4×",
  "Paused"). » steps 2× → 4× → 8× → 16× → back to 1×; slow forward steps ½× → ¼× → 1×;
  ▶ plays at 1×. Keys: **L** = », **K** = pause / play on, **←/→** = 10 s back / forward
  (J and the backward buttons arrive with reverse play). ↺10 s / ↻10 s move left with the
  tools. Jumps and timeline clicks keep the speed; a new day or a change of cameras goes
  back to ▶ 1×. Off 1× the sound is muted, each playing pane shows its speed, an HD pane
  shows "SD at speed" above 1×, and Frigate panes stop on their last picture with
  "1× only", catching up at the playhead back at 1× (with only Frigate panes the speed
  buttons are greyed).
- **The playhead follows the recording off 1×.** It now tracks the recording time of the
  frames on screen (the recorder's 16× runs a little faster than 16×, and a slow link holds
  it back) instead of counting wall time.

- **Fast Playback on Reolink recorders, paced by the recorder.** Playback now asks the
  recorder to play at the chosen speed instead of downloading as fast as the link allows,
  so 2×, 4×, 8× and the new **16×** play smoothly (16× reaches the recorder since the new
  speed controls; before them the app stopped at 8×). 16× shows one picture per keyframe,
  which is the recorder's own fast-forward. ¼× and ½× still play at 1× on the recorder
  and are slowed by the app. Above 1× an HD pane plays the standard-quality stream (the
  recorder sends HD at speed as keyframes only) and returns to HD at 1×. Seeking within a
  day now moves the running replay instead of restarting it.
- **Fisheye views in the Events feed.** A fisheye camera's circle snapshots (Woorabinda
  Kitchen today) are drawn in the camera's view and aim, the same one Live and Playback
  use, and each card takes that view's shape at about the usual card size (Quad 560×315,
  Original 420×420, Dual about 430×410…). The date bar follows. Views switch the same way
  on every tab: hovering a card shows the six view icons at its right edge, and
  right-clicking it opens its menu with **View ▸** as a backup; either changes the view
  everywhere. A dewarped card can be aimed too: drag turns the view (a click still plays),
  Ctrl+scroll zooms (plain scroll keeps scrolling the feed; Ctrl+scroll also zooms on the
  other tabs), double-click puts a turned view back. Flat
  snapshots (Frigate's Kitchen) and ordinary cameras are unchanged; stored files stay
  as they were.
- **Event snapshots in HD.** New event snapshots are kept at the camera's own resolution, up
  to 2560 px wide: the camera's JPEG is stored untouched (sharper, and smaller than
  re-encoding it), and only wider pictures (4K and up) are shrunk to 2560 px. They used
  to be shrunk to 640 px. Frigate's snapshots are kept the same way. Ordinary feed cards
  still load a card-sized copy, so scrolling stays quick; fisheye cards, the clip player
  and Save snapshot use the HD picture, and dewarped views sample a 2048² capture
  (1024² on cards). Measured: about 0.7–0.8 GB a day across the Sites, about 24 GB once
  30 days have built up. Snapshots already stored stay as they were.
- **Fisheye views in the Events clip player, in HD.** The player shows a fisheye camera's
  circle (the recording, and its snapshot before the first picture) in the camera's view,
  with a Playback pane's controls: view icons on hover, right-click View menu with Reset
  aim, drag to turn, scroll to zoom, double-click to put a turned view back. It takes the
  view's shape at the largest size that fits; switching view never restarts the clip.
  Clips now play in HD (main stream) by default, the HD button turns it off, and the
  snapshot shown before the first picture is full size.
- **Fisheye views in Playback, and the same controls in Live View.** Fisheye cameras
  (Lascelles Kitchen, Woorabinda Entrance and Kitchen) show all six views in Playback as
  in Live: Original, Quad, Virtual PTZ, Panorama, Dual and Fisheye + PTZ. Hovering a
  fisheye pane shows the six view icons at its right edge (smaller on short panes); a
  right-click opens the same View menu with ↺ Reset aim. Live View's "◔" toolbar button
  is replaced by these. One view per camera, shared by both pages. Switching view never
  restarts the stream, at any speed or direction. Dragging turns the view under the
  pointer and scrolling zooms it; each view's aim is now remembered per camera and
  shared by both pages. Double-clicking a turned view puts it back (otherwise
  double-click maximises, as before). A fisheye pane takes its view's shape in the
  mosaic: Quad 16:9, Original 1:1, PTZ 16:9, Panorama 3.3:1, Dual about 1.05:1,
  Fisheye + PTZ 7:3 (the circle beside a 4:3 view).

- **Playback stills.** While Playback is stopped, each pane shows a picture from the
  playhead's moment instead of black: when the page opens, the day changes, a camera
  is added, a maximised pane is left, or a timeline drag rests for a quarter second
  (letting go of the drag plays, as before). A still is the pane's own recording opened at that moment
  and stopped at its first picture, on the sub stream and silent, at most four at once
  per recorder; meanwhile the previous picture shows dimmed with a small spinner.
  Where a camera recorded nothing, it says so without asking the recorder.
- **Event switches (groundwork).** A camera's event type can be switched off so the
  app no longer stores, pictures, notifies or highlights it, live or from a Frigate
  catch-up. Switching off first reports how many events and how much snapshot space
  it would delete, then deletes them; switching back on logs afresh. Status can't be
  switched off. Kept in the app only, never on a recorder. No controls yet: those
  come with the new Events layout.
- **First switch-offs.** A one-time list of event types to switch off, kept in the
  settings file (`[events] firstSwitchOffs`, entries `Recorder/Camera/type` with `*`
  for any), is offered once a few seconds after launch: one confirmation showing
  what it deletes (events and snapshot size). Switch off applies them all; Cancel
  keeps everything logging. Either way it isn't asked again. Without the setting
  nothing is shown.
- **Events: unseen counts.** Each camera in the Events camera list shows coloured
  bubbles of the events it has logged since it was last opened on this desktop
  (blue Person, amber Vehicle, green Pet, red Visitor; 99+), or ✓ when nothing is
  new; Motion and Status don't count, nor do switched-off types. Opening a camera
  marks it seen, and it stays seen while it is on screen. Each Site tab shows its
  total, and the Events tab's badge and the tray tooltip show the total across every
  camera, kept across restarts (they used to count events since the app started). In
  the feed, new events carry a blue dot, a "Seen up to here" line marks where you had
  looked up to, and a band on the date bar covers the new part. Upgrading starts
  every camera already known as seen (database schema v5).
- **Events: switch controls in the feed.** Each event's ⋯ offers "Switch off <type>
  on <camera>…", confirmed with how many it logs a day and what it deletes. A dashed
  line at the top of a camera's feed lists the types switched off there, each with
  Turn on. When a type logs 1,000 or more a day (per day of the app running, over
  the last 24 h), the camera's feed opens with a noise card: four recent snapshots,
  the rate, Switch off… and Keep logging (remembered per camera and type, database
  schema v6).
- **Events: the clip player.** Clicking an event's snapshot plays it beside the feed
  (over the feed on a narrow window): a Reolink recorder's sub stream from 5 s before
  the event (HD on request), a Frigate camera's own clip from 15 s before, at 1×, muted
  until the speaker is turned on. The snapshot stays up until the first picture. At
  +30 s it stops with Keep playing and Open in Playback; ↑/↓ step to the newer/older
  event. It stops when you leave the Events page or hide the window. The time to the
  first picture is logged (`clip: first picture after … ms`).

### Changed

- **Quad view sees the whole circle.** Its four views are now 120° wide and tilted 44°
  from straight down, covering from the floor under the camera out to the rim with no
  sky. The old 85° views left wedges at the rim unseen.

- **Playback keeps the picture.** Pause, Stop and the end of a recording leave each
  pane's last picture on screen instead of going black; it stays until a new picture
  replaces it, the day changes, the playhead moves to where that camera has no
  footage, or the pane leaves the mosaic. Playback panes now learn each camera's
  picture shape as Live View does, into the same remembered shapes, so a fisheye
  camera that Live shows dewarped gets a square pane in Playback instead of a 16:9
  one with bars.
- **Events: pictures in their own shape, no black bars.** Each feed card shows the
  camera's snapshot in its own shape (a doorbell tall, a fisheye circle square, a
  wide gate camera wide) at about the area of the old 16:9 card, never wider than
  the feed, centred. The clip player shows the picture as large as fits in its own
  shape, from the snapshot's shape until the video's first picture. Nothing is
  cropped. The date bar follows the new card heights.
- **Events: clicking a snapshot plays it in the clip player** instead of opening
  Playback; Open in Playback is in the event's ⋯ and the player.

- **Events: a camera list and one camera's feed.** Site tabs pick the Site; its
  cameras are listed on the left, the most recently active first ("last 3 min ago"),
  with cameras that can only log Status now at the bottom. The middle shows one
  camera's events over the whole month, newest first: a full-width snapshot under each
  caption (type, date and time, ⋯ with Open in Playback and Save snapshot), sticky day
  headers, and slim lines for Status and "App not running". The camera's type chips
  (with their counts) filter it and are remembered per camera; switched-off types have
  no chip. The player panel on the right waits for the clip player. Sync Frigate
  history, desktop notifications and deleting all events are in the ⋯ menu; right-click
  a Site tab or a camera for its menu. This replaces the per-camera columns, the day
  picker, the camera drawer (with Clear all and Undo) and the day's counts by Site shown
  when nothing was picked.
- **Events date bar.** A bar beside the feed marks where each day starts (Today,
  Yest., then weekday and date, with the month where it changes); labels stack so they
  never overlap, and on a full month every day keeps a tick with every second one
  labelled. A thumb shows the visible part; press or drag on the bar to jump; while
  the feed moves, a bubble shows the date and time of the top event.
- **Events keep a month.** Motion is now kept for 24 hours and no longer counts
  toward the per-recorder limit, which rises from 10,000 to 50,000. Person, Vehicle,
  Pet, Visitor and Status events stay for 30 days on every Site, even the busy ones.
  On the first launch, motion older than a day is cleared with its thumbnails, so
  the Playback timeline no longer marks motion on earlier days.
- **When a whole recorder or Frigate server goes offline, each of its cameras gets
  its own Status event**, rather than one event under the recorder's name. You still
  get one desktop notification.
- The Events list shows the newest 500 matching events, read straight from the
  store, rather than loading up to 50,000 into memory. The Playback timeline's
  event marks come from the store the same way.
- Groundwork for the Events page on Site tabs: the app now notes which event types
  each camera supports, and when it was running.

### Fixed

- **Frigate Playback plays busy stretches again.** Playback asked Frigate for three hours of
  footage at a time, which Frigate refuses when the camera recorded all of it (over about
  1,000 pieces), so the camera showed an error. It now asks for an hour at a time and carries
  on into the next hour by itself.
- **Cameras added to or removed from Frigate show up without restarting the app.** The app
  re-reads Frigate's camera list every five minutes and updates the Site when it changed,
  so a removed camera no longer lingers greyed out or keeps retrying.
- **Playback no longer shows the wrong camera for the Farm Hikvision.** It offered the Farm
  NVR's channel 8, which is the Driveway camera, not the workshop one. Its Playback card now
  says it has no recordings of its own until the NVR records the workshop camera.
- **A Frigate camera renamed in the app keeps working.** Renaming one in the app made the app
  ask Frigate for a camera under the new name, which Frigate doesn't know: live video,
  Playback, clips and its events stopped. Requests now use Frigate's own name for it.
- **Frigate cameras keep their place when another is removed.** The app numbered a Frigate
  server's cameras by their order in its list, so removing one could hand the next camera
  another's name, HD and fisheye settings, picks and history. Each camera now keeps its number
  for good, and a new camera never takes over an old camera's number. (A camera renamed in
  Frigate counts as a new camera.)
- **A camera removed from Frigate also leaves Live View and Playback.** Its pane, its
  Playback icon and its share of the Site's bandwidth budget used to stay behind, which could
  stop another camera from being added. Cameras on a Reolink recorder that are only offline
  for a while stay picked.
- **Fisheye cameras keep their views when the recorder is slow to answer.** A camera is
  known as a fisheye from its square picture. The app now remembers that, so a refresh
  where the recorder doesn't report the size (the Lascelles RLN8 sometimes answers with an
  error) no longer hides the six views on Live, Playback and Events until the next good
  refresh. The "Fisheye lens" tick in a camera's menu still wins.
- **Frigate Playback works on a server that asks for a login.** On Frigate's
  authenticated port (8971) every Playback answered "Sign-in refused (401)": the
  recordings list signed in, but the player asked for the video without the
  Frigate session. The player now sends the session with the playlist and every
  piece of video it lists, and signs in again if the server has dropped it.
  Detection clips and live view already signed in and are unchanged. Known gap: if
  the server drops the session part-way through a long playback, that playback
  stops rather than signing in again; reopening it signs in.
- **Clip export and Download range work again on NVRs.** Firmware v3.6.5.597,
  which both RLN36 recorders installed by themselves, broke the HTTP download the
  app used: the NVR still named a clip but no longer served it. NVR downloads now
  use the Baichuan time-range download (port 9000) that Reolink's own clients use.
  It sends exactly the asked range, in full quality with sound, and works while
  Playback is open. H.264 and H.265 cameras both export. Standalone cameras keep
  the HTTP download.

- **A Frigate server whose live stream asks for a login now plays.** Frigate's
  go2rtc restream can require its own username and password (a Basic login, not
  the Frigate login), and every camera on such a server answered
  `401 Unauthorized` and retried silently. The Add Frigate dialog and the
  sidebar's Update credentials now take that login (kept in the keyring, never
  the database), and live addresses carry it. A refused stream login now reads
  "Sign-in refused (401)" instead of the library's text.
- **The Farm Hikvision tile no longer pulls the camera's 4K main stream into the
  grid.** A direct-URL camera always played the URL as saved, so the grid's
  Fluent tile decoded 3840x2160 over the farm link. The link could not carry it,
  the decoder reported errors and the tile reconnected every few minutes. A
  Hikvision `/Streaming/Channels/<N>01` URL now plays `<N>02` (the camera's sub
  stream) when a tile asks for Fluent, as every other camera does. Maximize and
  the HD switch still play the saved main stream.

## [0.10.0-jjr] — 2026-09-20

### Added

- **Frigate recordings in the Playback page.** Pick a Frigate camera in
  Playback: the calendar marks the days with footage, the timeline shows the
  recorded stretches, and clicking anywhere plays from there. Playing runs at
  normal speed as one stream; a Frigate camera has no HD switch, faster speeds,
  export or download manager, so those controls are hidden for it.

### Fixed

- **A dropped Frigate live stream now reconnects by itself.** Before, if the
  Frigate server restarted or the link dropped, the tile stopped and stayed
  stopped. The player now treats Frigate's live stream like a camera stream and
  keeps retrying. It also starts faster and is no longer paced like a file.
- Confirmed the Frigate server going down and coming back is reported in the
  events inbox as offline, then online.

## [0.9.0-jjr] — 2026-09-20

### Added

- **Doorbell auto-reply settings.** Settings, Detection / Alerts has a new
  **Doorbell auto-reply** section for doorbells that support it: a switch, the
  clip to play, and how long to wait for a person before replying. It reads the
  setting from the device and shows what the device confirms after each change.
  A doorbell with no clips stored says so, since there is nothing to play.

## [0.8.0-jjr] — 2026-09-20

### Added

- **Frigate detection clips.** Clicking a Frigate camera's event in the events
  inbox (or its desktop notification) opens a window playing the recording from
  about 25 s before to 20 s after the detection, with Replay and Close. The
  server cuts the clip on request, so the picture takes a moment to appear.
  Other cameras still open the Playback page at that moment.
- `RL_OPEN_CLIP=<hostId>:<channel>` opens a clip at startup, for testing.

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
