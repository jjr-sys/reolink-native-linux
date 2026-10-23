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
