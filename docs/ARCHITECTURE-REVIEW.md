# Architecture review

## Candidates 2, 1, 5: device model

Three changes to `DeviceManager` and what hangs off it, made in this order on branch
`arch/linux-device-kind-session`. Each is one commit (or a short run of them) and
reverts with `git revert <sha>`; the later ones were written on top of the earlier
ones, so revert in reverse order (5, then 1, then 2).

### Candidate 2: device kind is a capability, not a string

**Module:** `src/device/DeviceKind.{h,cpp}`. `Kind` enum, `Capabilities` table
(`reolinkProtocol`, `multiChannel`, `alwaysOnline`, `frigateApi`, `detections`),
`liveUrl(LiveSource, mainStream)` (live URL per kind and quality), `directLiveUrl` (moved
here unchanged, still reachable as `rl::directLiveUrl`), `streamVendor()` (Hikvision) and
`looksLikeBoschFeed()`. Test: `tests/test_devicekind.cpp`.

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
