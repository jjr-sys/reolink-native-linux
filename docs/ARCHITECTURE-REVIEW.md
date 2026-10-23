# Architecture review

## Candidate 4: live arrangement

Branch `arch/linux-live-arrangement`. Each decision below is reversible on its own commit; revert the commit named.

**Problem.** `LiveViewPage.qml` held pure state logic keyed by device row: paging and wrap, the swap rule, slot repair, save and restore through "hostId:channel" strings with a pending-pane timer, and row-shift fix-ups for maximized and selected on every remove and insert. None of it could be tested without QML, and rows shifting made each rule fragile.

**Module.** `rl::LiveArrangement` (`src/ui/LiveArrangement.{h,cpp}`), registered in `main.cpp` next to `MosaicLayout`. Interface: `setSaved`, `setCameras(keys)`, `reset()`, `assign(pane, key)`, `turnPage`, `neighbour`, plus properties `paneKeys`, `pageKeys`, `preset`, `page`, `maximized`, `selected`, and the `saveRequested(text)` signal. Everything is keyed by camera key. The seam is the device model: QML tells the module which cameras exist and the module repairs itself, so no row arithmetic is left in QML. `test_livearrangement` exercises it through that interface only, with an old-format saved value among the cases.

**Decisions.**
1. Keyed by camera key, not row. Removing, inserting or reordering devices needs no fix-up. Maximized and selected became keys too (`maximizedKey`, `selectedKey` in the page, aliases of the module). `showCamera(row)` still takes a row because `Main.qml` calls it.
2. Persistence text unchanged: comma-joined "hostId:channel", "-" for empty, QSettings category `liveGrid`, key `arrangement`. The `Settings` object stays in QML as a thin adapter (`saved: layoutStore.arrangement`, `onSaveRequested`). The module does not touch QSettings, so tests need no fixture.
3. Deliberate small change: the module asks to save whenever the arrangement differs from what was last saved (and nothing is waiting). The QML page saved only on remove, drop and pending resolved, so a newly added camera placed in a free pane was saved only on the next unrelated change. Now it is saved promptly. A saved value shorter than 32 entries is rewritten in full form once. Revert: make `settle()` in `LiveArrangement.cpp` emit only from `assign`, removal and `giveUp`/pending resolution.
4. `MosaicPacker` and `MosaicLayout` are unchanged. QML still builds the packer's pane list (rows, shapes, Sites) from `pageKeys` or the open tab's cameras; only the source of the page's cameras changed.
5. Duplicate rules found in the mosaic tabs. The drop rule in `tabDrop` (add before the target, or swap) is now `LiveArrangement.dropInto`, tested. `Mosaics.gridOrder` re-parsed the arrangement text; it now calls `savedKeys` on a private instance so the format lives in one place. Left alone: `tabDropAllowed` (Site rule, needs `Mosaics.siteOf`), `Mosaics.cameraKey`, and the several `hostId + ":" + channel` concatenations in `MosaicEditorDialog.qml` and the page's highlight lines. They are one-liners over model data, not rules. Revert: restore the previous bodies of `tabDrop` and `gridOrder`.
6. `LivePane.qml` is untouched. It still reports drops and double-clicks by device row; the page converts rows to keys at the handlers.

**Quality state: left where it is.** Stream quality has three owners but only one is arrangement state. In a mosaic the HD choice is per camera per mosaic and already lives in the `Mosaics` store (`hd` sets), which is the mosaic's own state, not the grid arrangement. In the plain grid it is a transient, unsaved toggle inside `LivePane` (`qualityMain` = maximized or the remembered HD), tied to the pane's stream lifecycle; `PoppedWindow` always plays the main stream, with no toggle. Moving it into `LiveArrangement` would add a quality map that is never saved for the grid and duplicates the `Mosaics` store, for no rule that could be tested. If quality is ever persisted for the grid, the module is the place for it, keyed the same way.

**Behaviour change.** Only item 3. Slot repair fills gaps in camera (model) order, as before.

**Known gaps.** The model-reset case follows the old code: the arrangement is restored from the last saved text. A camera that disappears and reappears (its host refreshing its channel list) loses its pane, as before. QML-level behaviour (drag, step, maximize) was verified by loading the page offscreen, not by automated tests.
