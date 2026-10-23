
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
