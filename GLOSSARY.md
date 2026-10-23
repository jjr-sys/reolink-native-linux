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
