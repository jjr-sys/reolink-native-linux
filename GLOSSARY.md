# Camera client terminology

This glossary describes live viewing in the Reolink fork and the Bosch camera controls shared with the Lascelles Home Assistant dashboard.

## Live viewing

**Live stream**:
A camera's ongoing picture and available sound, viewed as it arrives rather than from stored footage.
_Avoid_: recorded playback

**Degraded live stream**:
A live stream using a fallback transport after its preferred transport fails; the picture may be less reliable even at the same resolution.
_Avoid_: SD (when referring to transport fallback)

**Live replay**:
A Live View pane (or up to four, in step) stepping back from now into its recorder camera's recording, across days as far back as storage holds, with Playback's controls on the pane bar and its time line as the "time behind live" line; it starts from now in the pane's live buffer, and its live stream stays connected meanwhile (not shown) so the buffer keeps filling. "● Live", leaving Live View, or playing forwards into the recorder's newest footage returns it to live. Several panes replay **in step** ("together" on screen): ↺ or ◀ on another pane while one replays **joins** it at the same moment; one playhead and speed for all, **led** by the maximised (else selected) replaying camera, whose footage decides where plays land and whose frames move the time; each pane's "● Live" takes only that pane back.
_Avoid_: timeshift, instant replay

**Live buffer**:
A Live pane's last five minutes of its own live stream, kept in memory as received (in the quality the pane shows, stamped with the camera's clock) while it streams and while it replays, so Live replay starts from now and runs back into the recorder's footage, and forwards back into live.
_Avoid_: timeshift buffer, DVR

**Catch-up**:
What a replaying pane's live stream does when it returns to live: while replaying, its player is **receive-only** (it keeps receiving, for the live buffer, but doesn't decode or show) and holds the frames since the newest keyframe; on "● Live" it decodes those unseen and shows the newest, so the picture is current at once instead of waiting for the camera's next keyframe.
_Avoid_: resync, warm-up

**Pane bar**:
The one floating bar at the bottom of every video pane (Live View, Playback, the Events clip player): a thin time line, the transport, HD, sound, the page's common tools and ⋯ for the rest. Shown on hover.
_Avoid_: toolbar, rail, pill (in user-facing text)

## Camera positions and tours

**Camera preset**:
A saved pan, tilt, and zoom viewpoint that the camera can return to.
_Avoid_: position name

**Position**:
A user-facing named place associated with a camera preset, such as Driveway or Road.
_Avoid_: raw preset name

**Tour order**:
The ordered list of positions that the camera visits during an automatic patrol.
_Avoid_: position list

**Tour selection mode**:
A mode for adding positions to or removing them from the tour order; it is separate from starting or stopping the patrol.
_Avoid_: touring mode

**Patrol**:
The camera automatically visiting positions in the saved tour order and dwelling at each one.
_Avoid_: tour edit mode

## Lens controls

**Iris**:
The physical aperture in the camera lens that controls how much light reaches the image sensor.
_Avoid_: IR-cut filter

**Fisheye view**:
How a ceiling-mounted fisheye camera's circle is shown: Original (the circle), Quad, Virtual PTZ, Panorama, Dual or Fisheye + PTZ. One per camera, shared by Live View, Playback and Events.
_Avoid_: view mode (code name only), layout

**Aim**:
Where each part of a fisheye view has been turned to by dragging or scrolling (pan, tilt and zoom; a panorama strip's slide). Remembered per camera and view; double-click or Reset aim puts it back.
_Avoid_: zoom (digital zoom is a different thing)

**Picture surface**:
The picture rules of one video pane (Live View, Playback, the pop-out window, the Events clip player), kept in one place: how far the picture is zoomed and panned and where panning stops, how far a press must move before it picks the pane up, the picture's measured shape, and whether the picture is a fisheye circle drawn through its Fisheye view. A picture is drawn through a view only when it is a circle (square within 1 %) from a fisheye camera, a view other than Original is chosen, and it is not Frigate's flat copy; until a first picture is seen the camera's lens and view decide.
_Avoid_: viewer, canvas

## Scene modes and schedule

**Scene mode**:
A named set of camera image settings, such as Standard, Fast Movement, or Color only.
_Avoid_: camera position

**Scene Mode Scheduler**:
The camera's schedule that assigns one scene mode to the Marked range and another to the Unmarked range.
_Avoid_: tour schedule

**Marked range**:
The scheduled time range to which the selected marked-range scene mode applies.
_Avoid_: scene mode

**Unmarked range**:
The time outside the Marked range, to which the selected unmarked-range scene mode applies.
_Avoid_: default mode

## Live grid arrangement

**Camera key**:
A camera's identity as "hostId:channel". Unlike a model row it does not change when devices are added, removed or reordered.
_Avoid_: row, device index

**Arrangement**:
Which camera sits in which pane of the Live grid, kept at 32 panes long so a layout made in the 16 grid survives a visit to the 4 grid. Saved as one key per pane joined by commas, "-" for an empty pane.
_Avoid_: layout (that is the grid size or the mosaic packing)

**Page**:
The run of preset-many panes of the arrangement the grid shows at once; previous/next turns it and wraps.
_Avoid_: screen

**Waiting camera**:
A camera in the saved arrangement whose host has not yet reported it. Its pane is held for it, and nothing is saved over the arrangement meanwhile, until it arrives or the wait is given up.
_Avoid_: missing camera

## Camera selection

**Selection session**:
One page's (Live View or Playback) whole handling of the cameras picked on the Site tabs: the selection, its Site budget, the stored selection and its one-time migration, pruning of cameras that are gone, each camera's remembered picture shape, and the mosaic panes in pane order. The page keeps only its transport. Live's launch wait, the gather time before a re-pack and whether a selection change re-packs at once are its parameters.
_Avoid_: page state, selection store

## Home Assistant seam (Bosch controls)

**Home Assistant transport**:
The one-method interface through which the Bosch tour controller sends a bearer-authenticated request to Home Assistant and receives status, body, and network error. Production uses a network adapter; tests use a scripted adapter that answers each request on demand.
_Avoid_: HA client, connection

**Scripted transport**:
A test adapter for the Home Assistant transport that records requests and lets the test choose when and how each is answered.
_Avoid_: mock server

## Playback session

**Playback session**:
One playback view's decisions about how recorded footage reaches the screen: which transport carries it, whether a seek moves the running stream or reopens it, and when a change of quality or speed needs a reopen.
_Avoid_: playback state, playback controller

**Still**:
While Playback is stopped, the one picture a pane shows from the playhead's moment: the pane's own recording opened there and stopped at its first picture, then kept on screen.
_Avoid_: thumbnail, preview, snapshot (a snapshot is a saved JPEG)

**Playback route**:
The transport chosen for recorded footage. HD holds up to 4× both ways while the link and decoding keep up: forwards, the recorder-paced native (Baichuan) main stream at 1× and slower and windows of the main stream above 1×; backwards, windows of the main stream. Otherwise (SD, above 4×, or too slow) the native sub stream: recorder-paced forwards, windows backwards. Direct (Frigate) footage always uses HTTP.
_Avoid_: playback mode, quality

**Seek in place**:
Moving a running native playback stream to another moment without reconnecting. Each pane's stream seeks only itself.
_Avoid_: fast seek

**Playback day**:
Playback's rules for one day of the selected cameras' footage: which camera leads, where a play lands, where every pane carries on when the lead's stream runs out, the playhead's clock and the day's searches. Live replay keeps the same rules across days.
_Avoid_: timeline controller

**Lead camera**:
The camera whose recordings decide where a play lands and carries on: the selected one, or with a Frigate camera selected, the first recorder camera in pane order. In Live replay, the camera chosen to lead.
_Avoid_: master, primary

**Frame driver**:
The pane whose frames move the playhead off 1×: the lead camera's pane while it is on screen, not frozen and has footage at the playhead, else the first such pane in order. With none, the playhead runs on the wall clock.
_Avoid_: clock pane

**In step**:
Panes playing one playhead together. A pane that isn't the frame driver plays at its own pace; where it has footage at the playhead and isn't playing, or is more than 5 s × speed away, it starts again at the playhead (not within 3 s of its last restart).
_Avoid_: synced, aligned (alignment is the pane's own catch-up at its first picture)

## Device model

**Host kind**:
What sort of thing a device row hangs off: a camera, an NVR, a bare stream URL or a Frigate server. It is stored as text but the app asks what a kind can do, not which kind it is.
_Avoid_: device type (as a string compare)

**Capability**:
One thing a host kind can do, such as logging in with the Reolink protocol, fanning out into several cameras or being reached through Frigate's API.
_Avoid_: feature flag

**Direct host**:
A host that is only played from a URL: a bare stream or a Frigate server. It has no Reolink login, settings, talk or recording search.
_Avoid_: non-Reolink host

**Host session**:
The app's standing with one host: whether a login is running, why the host is offline, when to try again and whether its polls are healthy. Unreachable hosts are retried with backoff; hosts that refused the login are never retried automatically.
_Avoid_: connection

**API-only state**:
A host whose HTTP API has stopped answering but which still answers on the Baichuan port; its cameras stay online and only the API-driven features are down.
_Avoid_: half-online

**Detection**:
One motion, person, vehicle, pet or visitor event on a camera, once the app has decided it is new, however it arrived (poll, push or Frigate).
_Avoid_: alarm (that is the device's own word)

**Push takeover**:
Once a host's Baichuan push is subscribed, its detections come from push and the poll stops reporting them.
_Avoid_: push mode

**Burst**:
Several Frigate events for one camera and type that start within 30 seconds of each other; they raise one detection.
_Avoid_: duplicate

**Inbox**:
The one way an event reaches the store. A source (live detections, a recorder's history, a connectivity change) hands it a candidate, and it answers with a filing: stored, or why not (switched off, too old, the same event as a stored row, the same burst in a history batch), and whether to announce it. The Events list shows what it stored.
_Avoid_: event inbox (the Events page and its list model, `EventManager`)

**Same event**:
A candidate that a stored row of the same camera and type already covers (within a burst's 30 seconds): a history sync run twice, or a live report of what the history filed.
_Avoid_: duplicate

**Status row**:
The stored Online or Offline event a connectivity change writes for each camera it touched. Status can't be switched off.
_Avoid_: health event
