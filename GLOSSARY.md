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
