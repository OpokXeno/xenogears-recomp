# Persistence, Return, Teardown, And Function Map

## 1. Scope

This chapter consolidates the persistence and lifetime contracts established by
the preceding Menu chapters. It defines which operations mutate resident game
state, which use local staging, which create explicit snapshots, what modes
`0..6` publish to their caller, and what each module releases before Field or
World restoration begins.

Page mechanics and operation-specific arithmetic remain in their owning
chapters:

- [`06-general-menu-pages-and-gameplay.md`](06-general-menu-pages-and-gameplay.md)
- [`07-memory-card-save-load-and-disc-flows.md`](07-memory-card-save-load-and-disc-flows.md)
- [`08-member-change-and-party-formation.md`](08-member-change-and-party-formation.md)
- [`09-enter-name-editor-and-encoding.md`](09-enter-name-editor-and-encoding.md)
- [`10-shop-inventory-and-transactions.md`](10-shop-inventory-and-transactions.md)
- [`11-gear-shop-tuning-and-preview.md`](11-gear-shop-tuning-and-preview.md)

The lifecycle rule is:

```text
presentation state is disposable
staging state belongs to one operation
accepted gameplay writes survive parent cancellation
save snapshots are serialization boundaries, not whole-menu undo points
caller restoration starts after module teardown
```

## 2. Persistent Mutation Matrix

`Immediate` means that an accepted operation writes resident state at its local
success boundary. `Staged` means that browsing changes operation-owned work
until a named commit. `Snapshot` means an explicit copy for serialization or
caller reentry.

| Page or operation | Mutated domain | Model | Commit boundary | Cancel or failure result |
|---|---|---|---|---|
| Party cards, gold, play time, Deathblow, and status views | None | Read-only presentation | None | Teardown changes no gameplay value |
| Item reorder | Item IDs and parallel quantities | Immediate | General Menu `SwapInventorySlots` `0x801DBD4C` | Cancel clears only an armed source; prior swaps remain |
| Consumable use | Character state, global effect state, item quantity, and the item ID when quantity reaches zero | Immediate after at least one successful effect | General Menu `RunItemTargetSelection` `0x801DB920` and `ApplyConsumableEffect` `0x801E31C0` | Cancel or no beneficial target applies no effect and consumes nothing |
| Character technique | Target HP and caster EP | Immediate after successful targeting | General Menu `RunTechniqueTargetSelection` `0x801DD790` and `ApplyTargetedHealing` `0x801E35BC` | Cancel or no beneficial target changes neither HP nor EP |
| Gear technique | Gear HP and caster/current Gear fuel | Immediate after successful targeting | General Menu `RunTechniqueTargetSelection` `0x801DD790` and `ApplyTargetedHealing` `0x801E35BC` | Cancel or no beneficial target changes neither HP nor fuel |
| Equipment preview | Resident equipment IDs and derived comparison values | Resident reversible preview guarded by a baseline | None while browsing | Candidate cancel copies the current baseline back and recalculates derived state |
| Equipment acceptance | Character or Gear equipment IDs, inventory IDs/quantities, and derived state | Explicit page commit with best-effort return of the displaced item | General Menu `ApplyEquipmentSelection` `0x801DF0D4`, then capture a new baseline | Acceptance reports no return failure. The displaced item is lost if its first matching stack is already 99, or if no matching stack and no empty slot exist; a later candidate cancel does not undo the replacement |
| Status/Abilities child | Domain owned by the selected child | Child-specific | Child commit helper | Closing the parent does not undo child commits |
| Gear get on/off | Character mount-state byte | Immediate on accepted toggle | Accepted branch in General Menu `OpenStatusMenu` `0x801E23CC` | Closing Gear preserves an accepted toggle |
| Sound selection | Resident output mode and active mixer state | Staged selection | Confirm in General Menu `ProcessSoundMenu` `0x801D9808` | Cancel leaves the resident mode unchanged |
| Member Change | Three current party IDs | Three-byte working party | Direct copy followed by compact commit in Member Change `MemberChangeMenuFree` `0x801C9748` | Cross with no captured source exits and commits; no entry rollback exists |
| Enter Name | Three mapped 20-byte name records | Two local edit buffers | Acceptance in Enter Name `EnterNameMenuMainLoop` `0x801CB33C` | No cancel exit; resident records remain unchanged until acceptance |
| Shop Buy | Gold, category IDs, and quantities | Staged quantities and resulting gold | Shop `ShopMenuHandleBoughtItems` `0x801CF2A0`; every duplicate matching ID receives the staged quantity, while no match uses the first empty slot; without either, gold commits and insertion is skipped | No resumes staging; discard confirmation drops it |
| Shop Sell | Gold, inventory IDs/quantities, or the selected one of three equipped Accessory slots | Staged selected counts and resulting gold | Shop `ShopMenuHandleSoldItems` `0x801D0C18`; each staged count is subtracted from every duplicate matching persistent ID | No resumes staging; discard confirmation drops it |
| Gear Shop Buy | Gold, Gear Part IDs/quantities, and Gear Weapon IDs/quantities | Staged quantities and resulting gold | Gear Shop `GearShopMenuHandleBoughtItems` `0x801D44FC`; every duplicate matching ID receives the staged quantity, while no match uses the first empty slot; without either, gold commits and insertion is skipped | No resumes staging; discard confirmation drops it |
| Gear Shop Sell | Gold, Gear Part IDs/quantities, and Gear Weapon IDs/quantities | Staged selected counts and resulting gold | Gear Shop `GearShopMenuHandleSoldItems` `0x801D1F20`; each staged count is subtracted from every duplicate matching persistent ID | No resumes staging; discard confirmation drops it |
| Armor/Frame/Engine Tune Up | Gold and one equipped component ID | One staged candidate with quantity one | Gear Shop `GearShopMenuHandleBoughtItems` `0x801D44FC` | No or Cross on the actionable prompt preserves gold and component ID |
| Gear Fuel/HP service | Gold, current fuel, and current HP | Immediate after prompt acceptance | Gear Shop `GearShopMenuFuelTuneUpMenu` `0x801D5398` | Rejection and the full-state information path write nothing |
| Save block construction | Resident configuration shadow `+0x2324..+0x2343`, new payload buffer, and runtime names during encoding conversion | Immediate 16-halfword shadow synchronization followed by a snapshot | After temporary-entry creation, reopen, and header write, General Menu `BuildMemoryCardSaveImage` `0x801CBA4C` copies active configuration into the resident shadow before `SnapshotMenuGameState` `0x801E4A28` | Runtime names are decoded before payload I/O; the shadow write survives subsequent payload-write or rename failure, while earlier I/O failure occurs before the write |
| New save | One destination card block | Snapshot followed by temporary publication | Successful rename to the final card name | Failure after temporary-entry creation leaves temporary data until the next scan; the final name stays absent |
| Overwrite | Existing destination card block | Destructive replacement | Successful rename of the new temporary entry | Failure after old-entry erasure leaves the old entry absent; no rollback copy exists |
| Copy | Destination card blocks | Temporary copy followed by an unchecked rename attempt | Completion of the preceding copy I/O; physical publication still requires rename, but Copy ignores its return | Copy reports success and invalidates caches even when rename fails; on rename failure, the final name is absent and the next scan removes the temporary entry. Source data remains unchanged |
| Delete | Selected card entry | Immediate unchecked erase request | The BIOS `erase` call is issued; Delete ignores its return | Cancellation before the call preserves the entry. After the call, Delete reports success and invalidates caches even if erasure failed; on erase failure, a later scan rediscovers the entry |
| Format | Selected card contents | Immediate after confirmation | BIOS `format` | Completion is destructive and has no Menu rollback |
| Load application | Gameplay payload domains in Section 4 plus play time, configuration, names, and ordered Gear restoration | Read, checksum, then non-transactional ordered restore | General Menu `ApplyLoadedSavePayload` `0x801CB28C` after I/O completion and checksum success | A bounded one-block I/O failure or checksum mismatch does not invoke restoration. A malformed multi-block count can overflow the retail buffer before validation and has no atomicity guarantee |
| Mode-2 outcome | Resident result byte `0x800594D0` | Immediate coordination write | Exit selected by New Game, idle-attract, or accepted load | Values `0`, `1`, and `2` retain the contracts in Section 7 |

### 2.1 Inventory And Quantity Invariants

Whenever an inventory slot changes, its identifier and quantity are updated
together. Reordering, selling, consuming, equipping, or returning equipment
preserves that pairing, and a zero quantity clears its identifier where the
owning commit helper performs that normalization. Pairing does not guarantee
item conservation: equipment acceptance loses the displaced item when the first
matching stack is already 99, or when there is neither a matching stack nor an
empty slot. The helper reports no error, so the new equipment and rollback
baseline remain accepted. The standalone return helper instead increments every
duplicate matching stack and uses an empty slot only when no match exists. Shop
and Gear Shop Buy likewise add the staged quantity to every duplicate matching
ID; Sell subtracts each staged count from every duplicate matching persistent
slot. Sale subtraction is byte-sized, and only exact zero clears the ID, so
underflow wraps. A committed purchase can change only gold when no matching
stack or empty destination slot exists.

Consumable special effect `OverwriteInventoryCatalogRangesWithTenEach` does not
perform additive inventory updates. It overwrites IDs with their slot indices
and quantities with 10 for Weapons `1..71`, Accessories `1..149`, Items
`1..75`, Gear Parts `1..71`, and Gear Weapons `1..104`; all other slots remain
unchanged.

Ordinary Shop capacities are 100 Weapons, 200 Accessories, and 150 Items. Gear
Shop capacities are 100 Gear Parts and 150 Gear Weapons. Quantity commits cap at
99. Staged arithmetic reserves neither a stack nor an empty slot.

Shop and Gear Shop assign staged resulting gold only in their commit helpers and
cap it at 9,999,999. Gear Fuel/HP service writes gold directly in its accepted
branch. Full Fuel service charges `10 * max(1, missing_fuel / 100)`. Accepted
partial service adds `floor(gold / 10) * 100` fuel, stores `gold % 10`, caps fuel
at maximum, and restores HP when initial gold is nonzero. Initial gold `1..9`
therefore changes neither fuel nor gold but still restores HP. Fuel-full,
HP-damaged Gear receives the accepted free repair.

## 3. Staging, Immediate Writes, And Snapshots

### 3.1 Local Staging

Shop quantities, staged gold, Gear tune candidates, Enter Name buffers, Member
Change party IDs, and the General Menu equipment baseline belong to their
owning operation. Their lifetime ends with that operation or module.

Equipment has an explicit rollback baseline. General Menu
`StageCurrentEquipmentLoadout` at `0x801DF5D0` captures the current loadout and
nine comparison values. `StageSelectedEquipmentCandidate` at `0x801DFB68`
changes the preview. Candidate cancellation calls
`CommitStagedEquipmentLoadout` at `0x801DF890` to copy the captured baseline to
the resident slots. Acceptance performs the inventory exchange and captures the
accepted loadout as the next baseline even when the displaced item cannot be
stored. That return failure is silent and does not roll back acceptance.

### 3.2 Immediate Mutation

Item swaps, successful consumables, successful techniques, accepted mount-state
toggles, and accepted Fuel/HP service mutate resident state at their local
success boundary. Returning to a parent page or closing mode 0 does not revert
them. General Menu captures no top-level cancellation snapshot.

Member Change edits a working three-byte party. Exit first copies all three
bytes, then `MemberChangeMenuFree` compacts non-`0xFF` IDs leftward and pads the
remaining slots. Cross without a captured source means finish and commit.

### 3.3 Explicit Snapshots

| Snapshot | Producer and consumer | Purpose |
|---|---|---|
| Memory-card payload | General Menu `SnapshotMenuGameState` `0x801E4A28` and `RestoreMenuGameState` `0x801E4D10` | Copy the persistent domains defined in Section 4 to or from one save payload |
| Field transient reentry | Field `FieldSaveSerializedRuntimeState` `0x800A3F4C` and `FieldRestoreSerializedRuntimeState` `0x800A3474` | Recreate the active Field map, actors, camera, mutable walkmesh, and VM state |
| World transient travel state | World `WorldMapSaveRuntimeState` `0x80075460` and `WorldMapRestoreRuntimeState` `0x8007565C` | Preserve World travel state around volatile resource reconstruction |

These snapshots have separate owners and layouts. Field actor state and World
travel reconstruction are not memory-card payload fields.

## 4. Closed Memory-Card Layout

A save occupies one `0x2000`-byte card block:

| Card offset | Meaning |
|---:|---|
| `0x0000..0x00FF` | PS1 title, icon CLUT, and one static icon |
| `0x0100..0x1FFE` | Xenogears payload covered by the checksum |
| `0x1FFF` | Wrapping byte-sum checksum |

Payload offsets below are relative to card offset `0x0100`. For party-summary
rows, `i = 0..2`.

| Payload range | Persistent meaning |
|---:|---|
| `+0x000..+0x003` | Play time |
| `+0x004 + i*2` | Current HP |
| `+0x00A + i*2` | Maximum HP |
| `+0x010 + i` | Current EP |
| `+0x013 + i` | Maximum EP |
| `+0x016 + i` | Primary level |
| `+0x019 + i` | Secondary level |
| `+0x01C + i` | Character ID; `0xFF` means absent |
| `+0x01F..+0x022` | Cleared padding |
| `+0x023` | Save number `0..14` |
| `+0x024..+0x28F` | 31 stored name records of `0x14` bytes |
| `+0x290..+0x99B` | 11 complete character records of `0xA4` bytes |
| `+0x99C..+0xE4B` | 20 compact Gear records of `0x3C` bytes |
| `+0xE4C..+0xEC3` | Resident game state `+0x1648..+0x16BF` |
| `+0xEC4..+0x1023` | 11 character ability/progression records of `0x20` bytes from resident `+0x16C0` |
| `+0x1024..+0x1123` | World-map and travel state from resident `+0x1820..+0x191F` |
| `+0x1124..+0x1B5B` | Resident persistent state `+0x1920..+0x2357` |
| `+0x1B5C..+0x1EFE` | Cleared padding |
| `+0x1EFF` | Checksum |

### 4.1 Compact Gear Records

Each `0x3C`-byte compact Gear record maps the resident `0xA4`-byte record as
follows:

| Compact range | Resident Gear range | Meaning |
|---:|---:|---|
| `+0x00..+0x27` | `+0x00..+0x27` | Core Gear fields |
| `+0x28..+0x2B` | `+0x5C..+0x5F` | Weapon channel bytes |
| `+0x2C..+0x2F` | `+0x60..+0x63` | Gear state bytes |
| `+0x30..+0x33` | None | Cleared padding |
| `+0x34..+0x35` | `+0x38..+0x39` | Current Gear fuel (`u16`) |
| `+0x36..+0x37` | None | Cleared padding |
| `+0x38` | `+0x99` | Gear state byte |
| `+0x39` | `+0x74` | Gear state byte |
| `+0x3A` | `+0x75` | Gear state byte |
| `+0x3B` | None | Cleared padding |

### 4.2 Resident Persistent Tail

For payload `+0x1124..+0x1B5B`, `payload_offset = resident_offset - 0x7FC`.

| Payload range | Resident range | Persistent meaning |
|---:|---:|---|
| `+0x1124..+0x1127` | `+0x1920..+0x1923` | Persistent prefix |
| `+0x1128..+0x112B` | `+0x1924..+0x1927` | Gold |
| `+0x112C..+0x1133` | `+0x1928..+0x192F` | Persistent prefix |
| `+0x1134..+0x1533` | `+0x1930..+0x1D2F` | 512 Field variables; payload `+0x11D8` stores the zero-based disc request, valid values `0..1` |
| `+0x1534..+0x1537` | `+0x1D30..+0x1D33` | Party membership and party-frame masks |
| `+0x1538..+0x153A` | `+0x1D34..+0x1D36` | Three current party IDs; each valid value is `0..10` or `0xFF` |
| `+0x153B` | `+0x1D37` | Persistent party byte |
| `+0x153C..+0x1603` | `+0x1D38..+0x1DFF` | 100 weapon quantities followed by 100 weapon IDs |
| `+0x1604..+0x1793` | `+0x1E00..+0x1F8F` | 200 Accessory quantities followed by 200 Accessory IDs |
| `+0x1794..+0x18BF` | `+0x1F90..+0x20BB` | 150 item quantities followed by 150 item IDs |
| `+0x18C0..+0x1987` | `+0x20BC..+0x2183` | 100 Gear Part quantities followed by 100 Gear Part IDs |
| `+0x1988..+0x1AB3` | `+0x2184..+0x22AF` | 150 Gear Weapon quantities followed by 150 Gear Weapon IDs |
| `+0x1AB4` | `+0x22B0` | Persistent party byte |
| `+0x1AB5..+0x1ABF` | `+0x22B1..+0x22BB` | Character mount-state bytes |
| `+0x1AC0..+0x1B1B` | `+0x22BC..+0x2317` | Persistent party and travel tail |
| `+0x1B1C..+0x1B27` | `+0x2318..+0x2323` | Party-frame lock, Field ID, camera yaw, World initial position, World mode, and previous music |
| `+0x1B28..+0x1B47` | `+0x2324..+0x2343` | 16 configuration halfwords |
| `+0x1B48..+0x1B5B` | `+0x2344..+0x2357` | Persistent trailing block |

Before snapshotting, `BuildMemoryCardSaveImage` copies 16 active configuration
halfwords into the resident configuration shadow at `+0x2324..+0x2343`. It then
converts all 31 name records to storage encoding, captures the payload, and
restores runtime encoding before payload I/O. The configuration-shadow mutation
is resident state, not staging, and survives subsequent payload-write or rename
failure. Temporary-entry creation, reopen, and header write occur before this
mutation, so failure at those earlier stages leaves the shadow unchanged. On
load, `ApplyLoadedSavePayload` restores the gameplay ranges
beginning at payload `+0x024`, the 16 configuration halfwords, play time, and
runtime name encoding. The party-summary fields at `+0x004..+0x01E` and save
number at `+0x023` remain card-menu metadata and are not applied to resident
gameplay state.

`RestoreMenuGameState` copies gameplay party IDs at payload
`+0x1538..+0x153A` verbatim. These are distinct from the three summary IDs at
`+0x01C..+0x01E`. Retail checksum acceptance does not validate the gameplay
domain `0..10` or sentinel `0xFF` before later Field code consumes the restored
bytes.

For each restored Gear, `RestoreMenuGameState` first copies compact core and
component fields `+0x00..+0x27` to resident `+0x00..+0x27` and weapon channels
`+0x28..+0x2B` to resident `+0x5C..+0x5F`. It then reloads frame, armor, and
engine base stats through General Menu `0x801E41C0`, `0x801E4258`, and
`0x801E42AC` and calls `RecalculateGearEquipmentEffects` at `0x801E433C`. Only
after that recalculation does it restore compact saved current fuel
`+0x34..+0x35` to resident `+0x38..+0x39`, state bytes `+0x2C..+0x2F` to
resident `+0x60..+0x63`, compact `+0x38` to resident `+0x99`, and compact
`+0x39`/`+0x3A` to resident `+0x74`/`+0x75`.

`CalculateGearWeightPenalty` at `0x801E4928` reads `+0x75` during the earlier
recalculation, so derived byte `+0x4A` is calculated from the prior capacity
tier. Restore does not recalculate it after writing saved `+0x75`; `+0x4A`
remains at that result until a later recalculation.

## 5. Commit, Cancel, And Failure Boundaries

The required ordering is:

1. Validate selection, compatibility, affordability, and target benefit before
   the associated persistent write. For card load, validate I/O completion and
   checksum. Format-valid card state additionally keeps the header block count,
   cumulative owner-map use, payload save number, gameplay party IDs, and
   zero-based disc request within their documented domains; retail does not
   enforce all of those domains.
2. Keep previews and staged arithmetic reversible until explicit acceptance.
3. Apply every member of a parallel-state mutation together.
4. Recompute derived character or Gear state after committed identifiers change.
5. Preserve accepted child writes when a parent page closes.
6. Never call the memory-card restore routine as a mode-0 cancellation action.

Generated saves declare one block. Retail load trusts the header block count,
reads `block_count << 13` bytes into its `0x2100`-byte allocation, and validates
the checksum over card offsets `0x0100..0x1FFE` before
`RunMemoryCardLoadOperation` at `0x801CB304` calls `ApplyLoadedSavePayload`.
For a bounded one-block input, I/O failure or checksum mismatch leaves current
gameplay state unchanged because restoration is not called. A malformed header
that declares multiple blocks can overrun the allocation before checksum
validation; retail provides no memory-safety or atomicity guarantee for that
path.

Header scanning has two earlier unchecked write surfaces. `LoadMemoryCardSaveHeader`
at `0x801C90B0` accumulates the untrusted block count across all files on a port
and writes one owner byte per block without limiting the 15-block map.
`MarkMatchingGameSavesOnCard` at `0x801C9270` writes byte `1` to a 16-byte
occupancy row indexed by the unvalidated payload save number. Out-of-range save
numbers can reach the retained CD callback words that card shutdown later
restores. Both writes occur during automatic scanning, before the user accepts
a load.

The persisted zero-based disc request is valid only in `0..1`. Retail restores
the byte without checking it and later passes it to `EnsureRequestedDiscLoaded`
at `0x801C8694`. Any other low-byte value selects an expected disc that no
supported medium can satisfy, and the non-cancellable retry loop never returns.

| Operation | Commit/publication point | Failure guarantee |
|---|---|---|
| New save | Rename fully written temporary data to the final card name | Failure after temporary-entry creation leaves temporary data until the next scan; final name is absent |
| Overwrite | Erase old final entry, write temporary replacement, then rename | Old and new final entries may both be absent |
| Copy | Physical publication requires rename, but Copy ignores the rename return and reports success from the preceding I/O result | If rename fails, the final name is absent and the temporary entry remains until the next scan removes it; source is unchanged |
| Delete | Issue BIOS `erase` after two confirmations, ignore its return, report success, and invalidate caches | If erase fails, the entry remains intact and the next scan rediscovers it |
| Format | BIOS `format` after confirmation | Destructive after `format` |
| Load | For a bounded one-block input, valid checksum followed by ordered payload restoration | Ordinary read/checksum failure does not invoke restoration; a malformed multi-block count can overflow the retail buffer before validation and is not atomic |

Save retries cover create, reopen, header write, payload writes, and rename, but
they do not create an overwrite backup. Copy retries its data-transfer I/O but
neither checks nor retries the final rename. Delete neither checks nor retries
`erase`. Consequently, Copy/Delete success presentation and cache invalidation
do not prove physical publication or deletion. A later scan removes a stale
Copy temporary entry, can rediscover an entry whose Delete erase failed, and
cannot recreate an old entry erased before replacement failure.

## 6. Memory-Card Services And Callbacks

General Menu `InitializeLoadSaveMemoryCards` at `0x801D9C84` restarts card
services, opens four events, enters a critical section, detaches the CD sync,
ready, and read callbacks, and retains all three prior callback pointers.

Every load/save action exit converges on both cleanup levels:

1. General Menu `FinalizeMemoryCardEventHandles` `0x801C8960` closes all four
   card events.
2. General Menu `ShutdownLoadSaveMemoryCards` `0x801D9E3C` clears preview and
   tile ownership, drains draw/VSync work, and restores all three CD callbacks
   in a critical section.

No card event, scan cache pointer, preview owner, or detached callback survives
General Menu lifetime.

## 7. Mode Return Contracts

Module entries return `void`; caller-visible results are resident coordination
values and accepted mutations.

| Mode | Module entry | Caller-visible contract after teardown |
|---:|---|---|
| `0` | General Menu `0x801C62A8` | Return to the invoking Field, World, or resident path. Accepted page writes persist; close performs no global rollback. |
| `1` | Member Change `0x801CB0A8` | Return after direct and compact party commit. Cross with no captured source is a committing exit. |
| `2` | General Menu `0x801C62A8` | Publish result `0` for New Game or another no-load exit, `1` for the 601-frame idle-attract exit when the current disc value is 1, or `2` after accepted load; accepted load enforces the unvalidated persisted disc request before resident dispatch requests game state 1, and a request outside `0..1` cannot leave that retry loop. |
| `3` | Enter Name `0x801CBDBC` | Return only after the encoded-unit acceptance test and three mapped name-record writes. |
| `4` | Shop `0x801CCD28` | Return with confirmed Buy/Sell commits; abandoned staging is discarded. |
| `5` | Gear Shop `0x801CE024` | Return with confirmed Buy/Sell/Tune/Fuel writes after Gear Helper and preview teardown. |
| `6` | General Menu `0x801C62A8` | Complete the dedicated save/disc route, enforce the recorded disc request, reload required indexes after a change, then request resident game state 1; a request outside `0..1` cannot satisfy or cancel the enforcement loop. |

Resident byte `0x800594D0` defines the complete mode-2 Field decision:

| Result | Field action |
|---:|---|
| `0` | Install the no-load title transition, including Field ID `4` |
| `1` | Enter the idle-attract route and replay the opening sequence |
| `2` | Restore saved Field destination and persistent values |

## 8. Field And World Restoration

Field `FieldLoadAndOpenMenu` at `0x800799D4` is an in-place modal coordinator.
Before Menu it preserves required Field state and framebuffer content, prepares
common Menu resources and the selected module, prepares Gear Helper for mode 5,
rearranges required VRAM rectangles, suspends ordinary presentation, publishes
mode inputs, and calls Resident `MenuMain`.

After the selected module has returned and released its owners, Field:

1. Consumes mode-2 result `0`, `1`, or `2` when applicable.
2. Restores display and draw environments and preserved framebuffer pages.
3. Recreates Field graphics uploads and restores moved VRAM rectangles.
4. Restores GTE geometry and projection state.
5. Reallocates party skin buffers and synchronizes mode-1 party changes or
   reloads the pre-Menu compact party resource IDs for mode 2.
6. Reinitializes or restores actor and party presentation for the selected path.
7. Clears the request to `0xFF`, restores ordinary Field flags, and resumes the
existing Field loop.

The immediate mode-2 return does not derive compact resource IDs from party IDs
restored by the accepted save. `FieldLoadAndOpenMenu` skips the mode-1 skin
initializers and reloads the pre-Menu compact IDs. A later eligible map
transition can call the party synchronization path and consume the restored
IDs; reentry state, current body type, and character-versus-Gear mode determine
whether it rebuilds them. The resulting resource-selection risk is therefore
conditional on a later synchronization, not an unconditional part of immediate
Menu return.

Field script `FE 87`, implemented by `FieldScriptWaitForMenuClose` at
`0x800936E4`, resumes only after restoration and request clear.

World has a separate restoration path. `WorldMapPrepareForMenu` at `0x800758C0`
snapshots transient travel state, releases volatile rendering resources, and
preserves the framebuffer. After Menu, `WorldMapRestoreAfterMenu` at
`0x80075B58` rebuilds graphics and resource buffers, recreates effects, and
calls `WorldMapReconcilePartyGearState` at `0x80075D4C`. Field restoration is
not a substitute for these World-specific steps.

## 9. Per-Module Teardown Order

### 9.1 General Menu

1. Hide page cursors, close page windows, and run action cleanup through General
   Menu `CleanupCompletedMenuAction` `0x801E3088` where applicable.
2. Finalize card events and restore CD callbacks before a card action returns.
3. Draw settling frames, disable top-level composition, and drain render parity
   to zero in `MenuMainLoopShutdown` `0x801C5FE4`.
4. Release common presentation, main-entry, text-batch, resource, play-time,
   cursor, indexed-resource, secondary-resource, and text-raster owners.
5. Release mode-specific card work and normal-mode cursor, gold, party-card, and
   remaining window owners.
6. Free the root `SystemMenu` allocation last and return to Resident
   `MenuExecute` `0x8001C1A8`.

### 9.2 Member Change

1. `MemberChangeMenuFreeCursors` `0x801CAB04` disables cursors, flushes one
   frame, and frees them.
2. `MemberChangeMenuFreeWindow` `0x801C76FC` releases window 2 and animation.
3. Copy three working party bytes directly.
4. `MemberChangeMenuFree` `0x801C9748` compacts and commits the final party.
5. Render twice, disable composition, render once, and drain parity to zero.
6. Free top-level states, retained resources, optional debug sound, six bench
   records, and three active records.
7. Free `SystemMenu` last and return through `MemberChangeMenuMain`
   `0x801CB0A8`.

### 9.3 Enter Name

1. After commit, clear auxiliary enables and flush/free pointer cursors through
   `EnterNameMenuFreePointerCursors` `0x801C9F1C`.
2. Disable keyboard, name, and caret packets; free window 2; start close.
3. Render to the close depth, disable the display group, and free prompts and
   window 3.
4. `EnterNameMenuFree` `0x801CA400` renders settling frames, disables top-level
   composition, and drains parity.
5. Free top-level states, retained resources, and optional debug sound.
6. Free `SystemMenu` last and return through `EnterNameMenuMain` `0x801CBDBC`.

### 9.4 Shop

1. Close confirmation and transaction windows, scrollbar, arrows, cursors, and
   category screens through `ShopMenuFreeTransactionScreen` `0x801D1F10` and
   the owning Sell cleanup paths.
2. Return to Buy/Sell/Exit until Exit or Cross is accepted.
3. `ShopMenuFree` `0x801CBB08` performs two updates, disables drawing, and
   drains render parity to zero.
4. Free manager blocks, definitions, descriptions, common resources, shop data,
   and optional debug sound.
5. Free `SystemMenu` last and return through `ShopMenuMain` `0x801CCD28`.

### 9.5 Gear Shop And Gear Helper

1. Wait for the Gear preview transition mask to become zero.
2. Clear preview and helper-render enables, then render once so no helper packet
   can be queued.
3. `GearShopMenuFreePreviewResources` `0x801D5EB8` calls Gear Helper
   `GearHelperShutdown` `0x801E7FD4`.
4. Gear Helper frees all ten model slots, the shared animation-track pool, and
   the 17-record trail allocation.
5. Mark both data wrappers inactive and free them after helper shutdown.
6. Disable and free shoulder-button UI at Gear Shop `0x801CE2E8`.
7. `GearShopMenuFree` `0x801CCD20` drains Menu parity and frees managers,
   definitions, text, common resources, and optional sound.
8. `GearShopMenuFree` releases `SystemMenu` last and returns through
   `GearShopMenuMain` `0x801CE024`.

Changing the selected Gear follows the same inner order:
`GearHelperFreeModelSlot` `0x801E8030` releases slot 1 before Gear Shop replaces
the wrapper contents used to reconstruct it.

## 10. Non-Persistable Runtime State

The following state is never serialized as a save field:

- `SystemMenu` and every heap pointer in its owner fields.
- GPU packet addresses, DMA links, ordering-table heads, render parity, and
  current `DRAWENV`/`DISPENV` pointers.
- Window, cursor, prompt, text-raster, card-tile, preview, and scroll owners.
- Card event handles and retained CD callback pointers.
- Open card handles, temporary operation names, retry counters, and scan-cache
  pointers.
- Gear Helper model slots, mesh/skeleton pointers, track and trail records,
  attachment tasks, animation pixels, and Gear Shop data wrappers.
- Field and World volatile render objects, framebuffer backup pointers, actor
  resource pointers, streaming packet state, and reconstruction work.

Only the values and padding defined in Section 4 belong to the memory-card
block. Heap addresses and volatile ownership remain process-local.

## 11. Lifecycle And Persistence Invariants

1. Exactly one primary Menu module is active at a time.
2. Mode 5 initializes Gear Helper before preview use and shuts it down before
   wrapper or Gear Shop root destruction.
3. Gameplay mutations have operation-local acceptance boundaries; card UI
   completion is not proof of physical success when Copy or Delete ignores the
   final BIOS return. No whole-menu cancel rollback exists.
4. Identifier and quantity arrays remain parallel through every commit. Inline
   equipment acceptance can lose a displaced item when its first matching stack
   is 99 or when neither a match nor an empty slot exists; the standalone return
   helper instead increments every duplicate matching stack.
5. Equipment preview restores the current baseline unless acceptance advances
   that baseline.
6. A confirmed child-page write survives parent-page cancellation.
7. A bounded one-block load invokes restoration only after complete read and
   checksum success. A malformed multi-block count can overflow the retail
   buffer before validation and has no atomicity guarantee.
8. A persisted disc request outside `0..1` enters a non-cancellable enforcement
   loop whose expected disc cannot be satisfied. Retail reaches that loop after
   applying the restored state.
9. After temporary-entry creation, reopen, and header write, save construction
   copies 16 active configuration halfwords into resident `+0x2324..+0x2343`
   before snapshotting. Subsequent payload-write or rename failure does not roll
   back that write; runtime name encoding is restored before payload I/O.
10. New-save success observes rename. Copy does not: when rename fails, it still
   reports success and invalidates caches while the final name is absent and the
   temporary entry remains for removal by the next scan.
11. Overwrite and format are not rollback-safe after successful destructive BIOS
    operations. Delete ignores the erase result, so its success presentation
    can precede rediscovery of an unerased entry.
12. Four card events close and three CD callbacks are restored on every
    load/save exit.
13. Drawing and helper-render flags become unreachable before packet or model
    storage is freed.
14. GPU-visible packet ownership drains before top-level managers are released.
15. `SystemMenu` is the last primary-module root freed and is never serialized.
16. Field or World restoration starts only after the selected module returns.
17. A waiting Field script resumes only after restoration and request clear.
18. Mode-2 results `0`, `1`, and `2` mean no-load transition, idle-attract, and
    accepted load respectively.
19. Gear restore copies core/component fields and weapon channels before base
    stat/effect recalculation, then restores saved current fuel, state bytes
    `+0x60..+0x63`, `+0x99`, and `+0x74`/`+0x75`. The earlier weight-penalty
    calculation uses the old `+0x75` capacity tier, and restore leaves derived
    `+0x4A` at that result until a later recalculation.

## 12. Global Grouped Function Map

| Domain | Anchors | Owning chapter |
|---|---|---|
| Resident dispatch and root lifetime | Resident `ChangeGameState` `0x8001996C`; `MenuExecute` `0x8001C1A8`; `MenuMain` `0x8001C634` | [Chapter 2, Function Index](02-entry-modes-handoffs-and-coordination.md#11-function-index) |
| Field request and restoration | Field `FieldLoadAndOpenMenu` `0x800799D4`; `FieldScriptWaitForMenuClose` `0x800936E4`; transient restore/save `0x800A3474`/`0x800A3F4C` | [Chapter 2, Field Handoff](02-entry-modes-handoffs-and-coordination.md#6-field-handoff) |
| World restoration | World save `0x80075460`; restore `0x8007565C`; prepare `0x800758C0`; rebuild `0x80075B58`; reconcile `0x80075D4C` | [Chapter 2, World Handoff](02-entry-modes-handoffs-and-coordination.md#8-world-handoff) |
| General entry, return, and teardown | General Menu entry `0x801C62A8`; mode-2 loop `0x801C58EC`; mode-6 loop `0x801C57A4`; action cleanup `0x801E3088`; shutdown `0x801C5FE4` | [Chapter 6, Entry, Dispatch, Input, And Common State](06-general-menu-pages-and-gameplay.md#171-entry-dispatch-input-and-common-state) |
| Items and techniques | General Menu item target `0x801DB920`; item swap `0x801DBD4C`; technique target `0x801DD790`; effects `0x801E31C0`/`0x801E35BC` | [Chapter 6, Items And Techniques](06-general-menu-pages-and-gameplay.md#174-items-and-techniques) |
| Equipment commit and rollback | General Menu apply `0x801DF0D4`; capture `0x801DF5D0`; baseline copy `0x801DF890`; candidate stage `0x801DFB68`; return item `0x801E0434` | [Chapter 6, Equipment, Deathblow, Status, And Abilities](06-general-menu-pages-and-gameplay.md#175-equipment-deathblow-status-and-abilities) |
| Gameplay recalculation and snapshots | General Menu character recalc `0x801E36D4`; Gear display `0x801E3C2C`; Gear effects `0x801E433C`; snapshot `0x801E4A28`; restore `0x801E4D10` | [Chapter 6, Gameplay, Derived State, And Serialization](06-general-menu-pages-and-gameplay.md#176-gameplay-derived-state-and-serialization) |
| Card services and transactions | General Menu finalize events `0x801C8960`; load `0x801CB304`; save `0x801CBD90`; copy `0x801CC6D8`; delete `0x801CD2AC`; initialize/shutdown `0x801D9C84`/`0x801D9E3C` | [Chapter 7, Grouped Function Index](07-memory-card-save-load-and-disc-flows.md#21-grouped-function-index) |
| Save payload | General Menu decode names `0x801CB184`; apply load `0x801CB28C`; build block `0x801CBA4C`; snapshot/restore `0x801E4A28`/`0x801E4D10` | [Chapter 7, Grouped Function Index](07-memory-card-save-load-and-disc-flows.md#21-grouped-function-index) |
| Member Change | Member Change swap `0x801CAB48`; loop `0x801CAD14`; commit/free `0x801C9748`; root `0x801CB0A8` | [Chapter 8, Function Index](08-member-change-and-party-formation.md#15-complete-logical-function-index) |
| Enter Name | Enter Name cursor free `0x801C9F1C`; final free `0x801CA400`; commit loop `0x801CB33C`; root `0x801CBDBC` | [Chapter 9, Function Index](09-enter-name-editor-and-encoding.md#15-complete-logical-function-index) |
| Shop | Shop Buy `0x801CF2A0`; Sell `0x801D0C18`; transaction free `0x801D1F10`; final free `0x801CBB08`; root `0x801CCD28` | [Chapter 10, Function Index](10-shop-inventory-and-transactions.md#21-function-index) |
| Gear Shop | Gear Shop Sell `0x801D1F20`; Buy/Tune `0x801D44FC`; Fuel/HP `0x801D5398`; preview free `0x801D5EB8`; final free `0x801CCD20`; root `0x801CE024` | [Chapter 11, Function Index](11-gear-shop-tuning-and-preview.md#25-function-index) |
| Gear Helper | Gear Helper initialize `0x801E738C`; slot free `0x801E8030`; shutdown `0x801E7FD4`; pool frees `0x801DF668`/`0x801E00DC` | [Chapter 4, Gear Helper Ownership](04-runtime-objects-allocations-and-ownership.md#10-gear-helper-ownership) |
