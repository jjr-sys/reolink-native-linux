# Camera client terminology

This glossary describes live viewing in the Reolink fork and the Bosch camera controls shared with the Lascelles Home Assistant dashboard.

## Live viewing

**Live stream**:
A camera's ongoing picture and available sound, viewed as it arrives rather than from stored footage.
_Avoid_: recorded playback

**Degraded live stream**:
A live stream using a fallback transport after its preferred transport fails; the picture may be less reliable even at the same resolution.
_Avoid_: SD (when referring to transport fallback)

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

**Playback route**:
The transport chosen for recorded footage: the NVR's HTTP-FLV sub stream, the native (Baichuan) main stream for HD, or the native sub stream for faster than real time. Direct (Frigate) footage always uses HTTP.
_Avoid_: playback mode, quality

**Seek in place**:
Moving a running native playback stream to another moment without reconnecting.
_Avoid_: fast seek

**Recording file snap**:
Choosing the NVR recording file that contains a requested moment (or the nearest earlier one across a gap) and the offset into it, because the HTTP-FLV endpoint only starts on file boundaries.
_Avoid_: seek rounding
