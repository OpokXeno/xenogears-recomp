# Member Change And Party Formation

## 1. Scope

Member Change exchanges one active-party member with one available character on
the bench. It edits three menu-owned party bytes and commits them when the modal
session closes. It does not load Field actors or party model resources.

Shared contracts are documented in:

- [Runtime Objects, Allocations, And Ownership](04-runtime-objects-allocations-and-ownership.md)
- [Frame, Input, Navigation, And Shared UI](05-frame-input-navigation-and-shared-ui.md)
- [Persistence, Return, Teardown, And Function Map](12-persistence-return-teardown-and-function-map.md)
- [GPU Packets And Ordering Tables](../graphics/02-gpu-packets-and-ordering-tables.md)
- [Graphics Resource Formats](../graphics/03-resource-formats.md)

## 2. Formation State

| Object | Count | Representation |
|---|---:|---|
| Active party slot | 3 | Flags/party manager `+0x30..+0x32` |
| Persistent party slot | 3 | Global party bytes `+0x1D34..+0x1D36` |
| Playable character ID | 11 | IDs `0..10` |
| Availability cache | 16 | Menu bytes `+0x30C..+0x31B` |
| Bench-list entry | 11 | Menu bytes `+0x1E14..+0x1E1E` |
| Visible active record | 3 | Three `0xBEC` display records |
| Visible bench record | 6 | Six `0xBEC` display records |
| Empty/end marker | 1 | Character ID `0xFF` |

The bench list and the visible bench records are separate objects. The list has
eleven positions and ends in `0xFF` padding. The renderer reuses six display
records for the six rows beginning at the viewport offset.

## 3. Complete Lifecycle

`MemberChangeMenuMain` at `0x801CB0A8` owns the modal operation:

1. Allocate and clear the six top-level managers.
2. Allocate and clear six bench and three active display records.
3. Configure the `MoveImage` rectangle as `(0x2C0,0x100,0x140,0x0E0)`.
4. Initialize availability and copy valid persistent party IDs into working
   slots.
5. Load menu resources, upload textures, initialize sound, text packets,
   background packets, and window-border metadata.
6. Enable composition and enter `MemberChangeMenuMainLoop`.
7. Build the bench list, create window 2, create the two selection cursors, and
   wait for the window to finish opening.
8. Poll input, update the visible rows, and process two-stage exchanges.
9. On exit, destroy the cursors and window, then directly copy the three working
   bytes to persistent party storage.
10. `MemberChangeMenuFree` compacts the working party into persistent storage,
    drains both packet parities, and releases every owned allocation.

The resident menu controller supplies the outer `SystemMenu` block. Member
Change frees that block last.

## 4. Managers And Ownership

### 4.1 Functional managers

| Owner in `SystemMenu` | Size | Role |
|---:|---:|---|
| `+0x32C` | `0x5034` | Loaded resource and texture state |
| `+0x33C` | `0x006C` | Render enables and working party IDs |
| `+0x350` | `0x1194` | `MoveImage` rectangle |
| `+0x348` | `0x015C` | Overlay GPU packets and dim layer |
| `+0x1DF0[0..5]` | `6 * 0x0BEC` | Six visible bench-character states |
| `+0x1E08[0..2]` | `3 * 0x0BEC` | Three active-party character states |

The `+0x354` and `+0x330` managers are cleared lifecycle reservations: the menu
allocates and frees them but reads no field from either owner. They do not
participate in formation, input, rendering, or commit behavior.

Each `0xBEC` character state contains parity-paired portrait, name, label,
level, secondary-statistic, HP, maximum-HP, MP, and maximum-MP packets plus
packet counts and render enables.

### 4.2 Dynamic owners

| Owner | Size | Lifetime |
|---|---:|---|
| Window 2 packets | `0x720` | Opening through modal-loop exit |
| Window 2 animation | `0x18` | Opening through modal-loop exit |
| Cursor packets | `0x14C` | Opening through modal-loop exit |
| Shared text raster | `0x38E` | Initialization through final teardown |
| Small-image upload | `0x20` | Allocate, upload `16x1`, synchronize, free |
| Character-name upload | `0x3F6` | Rasterize two names, upload `40x13`, free |

Two loaded resource products remain owned by the menu until teardown. Debug
sound mode also owns its loaded sound data; normal mode borrows the resident
sound pointer.

## 5. Availability And Initial Party

Initialization computes:

```text
available_mask = (game_state+0x1D30 & game_state+0x1D32) & 0x07FF
```

`MemberChangeMenuIsCharacterFlagSet` tests this mask through a static 16-bit bit
table. The initializer fills sixteen cache bytes, but `0x07FF` admits only IDs
`0..10`.

For each persistent party slot:

```text
id = persistent_party[slot]
working_party[slot] = id if id != 0xFF and availability[id] != 0
                    = 0xFF otherwise
```

This copy filters characters that are no longer available. It preserves slot
positions, so an empty middle slot remains a hole until final compaction.

## 6. Bench Construction

`MemberChangeMenuRebuildAvailableCharacters` walks IDs `0..10` in ascending
order. It appends an ID when both conditions hold:

- The availability cache byte for that ID is set.
- The ID is absent from all three working active slots.

After the last admitted ID, every remaining bench byte becomes `0xFF`.

The same routine uploads name strips in ID pairs `(0,1)`, `(2,3)`, `(4,5)`,
`(6,7)`, `(8,9)`, and `(10,padding)`. These uploads populate the name atlas by
character ID; the bench bytes determine the displayed order.

## 7. Six-Row Viewport And Scrolling

The modal loop tracks:

```text
side            0 = active party, 1 = bench
row             active: 0..2, bench: 0..5
viewport_offset absolute bench index of visible row 0
```

`MemberChangeMenuUpdateCharacters(viewport_offset)` rebuilds three active rows
and up to six bench rows from:

```text
bench[viewport_offset + visible_row]
```

An entry equal to `0xFF` disables that row's render state. The loop caches the
last viewport offset and rebuilds only after the offset changes or a successful
exchange invalidates the cache with `0xFF`.

The retail input path fixes the viewport to the first six bench entries:

1. `viewport_offset` starts at zero.
2. Down increments the visible bench row and clamps it at row 5.
3. Up decrements the visible row.
4. Up from row 0 subtracts one from `viewport_offset`, then immediately clamps a
   negative result back to zero.
5. No command increments `viewport_offset`.
6. Window 2 is created without a scroll bar.

Therefore normal input never displays bench indices `6..10`. The offset-aware
renderer and decrement branch remain present, but the modal controller exposes
only `bench[0..5]`.

## 8. Two-Stage Exchange

The editor tracks the current side, current row, viewport offset, and one
captured source.

### 8.1 Capture

1. Circle stores the source side, row, and viewport offset.
2. Cursor 1 remains on the captured source.
3. The moving cursor switches to the opposite side.
4. Its destination row becomes zero.
5. Left and Right remain blocked until the exchange completes or the source is
   released.

### 8.2 Complete or release

1. Up and Down select a destination on the opposite side.
2. Circle calls `MemberChangeMenuSwapCharacters` with the captured and current
   locations.
3. An accepted exchange clears capture state, hides cursor 1, invalidates all
   displayed character rows, and plays sound 2.
4. A rejected exchange retains capture state and plays sound 4.
5. Cross releases the captured source, hides cursor 1, and changes no party or
   bench byte.

Because capture always switches sides and locks horizontal input, the menu has
no active-to-active or bench-to-bench reorder command.

## 9. Exchange Invariants

The exchange helper normalizes the two locations into one active index and one
absolute bench index:

```text
absolute_bench_index = viewport_offset + visible_bench_row
```

It rejects either endpoint when its ID is `0xFF`. It also tests the independent
16-bit exchange-exclusion mask at resident game-state `+0x2318`; a set bit for
either character ID rejects the exchange. On acceptance it exchanges exactly
the active byte and bench byte, counts the three active entries, and restores
both bytes if the result has no active member.

For ordinary menu state, both endpoints are nonempty, so an accepted exchange
preserves active-party cardinality. Empty active slots cannot be filled through
this command, and a member cannot be moved into an empty bench row.

`0xFF` serves as:

- An empty active slot.
- An empty or terminal bench entry.
- Final padding after compact commit.

It is always tested before a byte is used as a character index.

## 10. Input Command Map

`MemberChangeMenuPollInput` waits for controller 1. If the controller disconnects,
the menu mutes all SPU channels, waits without advancing formation state, and
restores audio after reconnection.

| Command | Controller event | Formation behavior |
|---:|---|---|
| `0` | Right press/repeat | Active -> bench; blocked after source capture |
| `1` | Down press/repeat | Active wraps `2 -> 0`; bench clamps at row 5 |
| `2` | Left press/repeat | Bench -> active; blocked after source capture |
| `3` | Up press/repeat | Active wraps `0 -> 2`; bench row 0 remains row 0 |
| `4` | Circle release | Capture source or attempt exchange |
| `5` | Cross release | Release source or exit the modal loop |
| `6` | Square release | No formation action |
| `7` | Triangle release | No formation action |
| `8` | No recognized event | No action |
| `9` | R1 release | No formation action |
| `10` | L1 release | No formation action |
| `11` | Start release | No formation action |
| `12` | Select release | Toggle a diagnostic byte |

L2 increments a second diagnostic byte without replacing the current command.
Directional commands use press/repeat events; face and shoulder commands use
release events.

## 11. Commit Semantics

The menu writes persistent party storage twice:

1. At modal-loop exit, after destroying cursors and window 2, it directly copies
   all three working bytes. Slot positions and `0xFF` holes are preserved.
2. At the start of `MemberChangeMenuFree`, it copies non-`0xFF` working bytes
   left-to-right and pads the remaining persistent slots with `0xFF`.

Cross has contextual behavior:

- With a captured source, Cross releases only that source.
- Without a captured source, Cross exits and commits all accepted exchanges.

The menu takes no entry snapshot and has no rollback command. Backing out of the
modal session commits the current formation.

## 12. Presentation Boundary

Each update polls input, checks soft reset, switches packet parity, clears a
16-entry reverse ordering table, advances windows, queues cursors and character
packets, synchronizes GPU/VBlank work, installs `DRAWENV` and `DISPENV`, copies
the configured `320x224` VRAM rectangle to the parity-selected page, and submits
the ordering table.

Window 2 uses logical rectangle `(120,6,184,200)`, ordering-table depth 4, a
`0x720` packet owner, and a `0x18` animation owner. Packet, texture, VRAM, and
ordering-table interpretation follows the graphics chapters linked in Section
1.

## 13. Teardown

The exit sequence is:

1. Disable the cursors, render one flush frame, and free cursor ownership.
2. Disable and free window 2 and its animation state.
3. Direct-copy the three working party bytes.
4. Compact-copy nonempty members and append `0xFF` padding.
5. Render twice, disable top-level composition, render once, and continue until
   packet parity returns to zero.
6. Free resource, flags/party, MoveImage, lifecycle-reservation, and GPU managers.
7. Free retained resource products and the shared text raster.
8. In debug sound mode, unregister and free the owned sound resource.
9. Free six bench and three active display records.
10. Free `SystemMenu`.

The flush frames ensure that neither parity bank still references released
packet storage.

## 14. Behavioral Contract

- Initial roster availability is the intersection of the two persistent masks,
  restricted to character IDs `0..10`.
- The exchange helper separately rejects either endpoint whose bit is set in
  the resident `+0x2318` exchange-exclusion mask.
- The active party has three working slots and at least one member after every
  accepted exchange.
- The bench is sorted by character ID and excludes active IDs.
- Six bench rows are visible, and retail input reaches only bench entries
  `0..5`.
- Every accepted operation exchanges one nonempty active member with one
  nonempty bench member.
- Exiting commits; final teardown compacts active IDs to the left.

## 15. Complete Logical Function Index

| Address | Logical function |
|---:|---|
| `0x801C5018` | `MemberChangeMenuIsCharacterFlagSet` |
| `0x801C5034` | `MemberChangeMenuManageResourceState` |
| `0x801C5098` | `MemberChangeMenuManageFlagsAndPartyState` |
| `0x801C50FC` | `MemberChangeMenuManageMoveImageState` |
| `0x801C5160` | `MemberChangeMenuManageReservedLargeState` |
| `0x801C51C4` | `MemberChangeMenuManageReservedSmallState` |
| `0x801C5228` | `MemberChangeMenuManageOverlayGpuState` |
| `0x801C528C` | `MemberChangeMenuManageCharacterDisplayStates` |
| `0x801C5390` | `MemberChangeMenuLoadResources` |
| `0x801C559C` | `MemberChangeMenuInitialize` |
| `0x801C5714` | `MemberChangeMenuResetRenderContext` |
| `0x801C5724` | `MemberChangeMenuUploadSmallVramImage` |
| `0x801C57A0` | `MemberChangeMenuInitializeStringFt4Pair` |
| `0x801C59E0` | `MemberChangeMenuInitializeStringGroup` |
| `0x801C5B90` | `MemberChangeMenuInitializeTextGraphics` |
| `0x801C5BEC` | `MemberChangeMenuInitializeWindowBorders` |
| `0x801C5CF4` | `MemberChangeMenuClearManagerFlags` |
| `0x801C5D24` | `MemberChangeMenuInitializeShadedQuad` |
| `0x801C5DA0` | `MemberChangeMenuInitializeBackground` |
| `0x801C60EC` | `MemberChangeMenuSetVertices` |
| `0x801C6144` | `MemberChangeMenuSetWindowBorderPrimitive` |
| `0x801C618C` | `MemberChangeMenuInitializeWindowGraphics` |
| `0x801C64A8` | `MemberChangeMenuInitializeScrollBar` |
| `0x801C660C` | `MemberChangeMenuInitializeWindowBorderCorners` |
| `0x801C6854` | `MemberChangeMenuSetWindowBorderTop` |
| `0x801C6B98` | `MemberChangeMenuSetWindowBorderBottom` |
| `0x801C6EE4` | `MemberChangeMenuSetWindowBorderLeft` |
| `0x801C722C` | `MemberChangeMenuSetWindowBorderRight` |
| `0x801C7578` | `MemberChangeMenuSetWindow` |
| `0x801C76FC` | `MemberChangeMenuFreeWindow` |
| `0x801C7788` | `MemberChangeMenuInitializeWindow` |
| `0x801C790C` | `MemberChangeMenuUpdateWindows` |
| `0x801C7A58` | `MemberChangeMenuDrawCursors` |
| `0x801C7DA8` | `MemberChangeMenuRenderAuxGroup4` |
| `0x801C7E38` | `MemberChangeMenuRenderAuxGroup8` |
| `0x801C7EC8` | `MemberChangeMenuRenderAuxQuads` |
| `0x801C8040` | `MemberChangeMenuRenderBackgroundDim` |
| `0x801C80BC` | `MemberChangeMenuRenderCharacter` |
| `0x801C83D0` | `MemberChangeMenuRenderCharacters` |
| `0x801C846C` | `MemberChangeMenuRenderAuxiliaryGroups` |
| `0x801C849C` | `MemberChangeMenuRenderTopWindowBorder` |
| `0x801C8670` | `MemberChangeMenuRenderBottomWindowBorder` |
| `0x801C8844` | `MemberChangeMenuRenderLeftWindowBorder` |
| `0x801C8A18` | `MemberChangeMenuRenderRightWindowBorder` |
| `0x801C8BEC` | `MemberChangeMenuRenderWindowBackground` |
| `0x801C8D28` | `MemberChangeMenuRenderWindowBorderCorners` |
| `0x801C8E74` | `MemberChangeMenuRenderScrollBar` |
| `0x801C9098` | `MemberChangeMenuRenderWindows` |
| `0x801C9210` | `MemberChangeMenuRender` |
| `0x801C9270` | `MemberChangeMenuPlaySoundEffect` |
| `0x801C92AC` | `MemberChangeMenuPollInput` |
| `0x801C94A0` | `MemberChangeMenuUpdateAndRender` |
| `0x801C95A0` | `MemberChangeMenuUploadCharacterNamePair` |
| `0x801C969C` | `MemberChangeMenuParseNumberToString` |
| `0x801C9748` | `MemberChangeMenuFree` |
| `0x801C9908` | `MemberChangeMenuRebuildAvailableCharacters` |
| `0x801C9A08` | `MemberChangeMenuBuildCharacterLabel` |
| `0x801C9F80` | `MemberChangeMenuSetCharacterLevelStrings` |
| `0x801CA24C` | `MemberChangeMenuSetCharacterHpAndMpStrings` |
| `0x801CA5C0` | `MemberChangeMenuUpdateCharacter` |
| `0x801CA690` | `MemberChangeMenuUpdateCharacters` |
| `0x801CA810` | `MemberChangeMenuSetCursorToCharacter` |
| `0x801CA944` | `MemberChangeMenuInitializeCursors` |
| `0x801CAB04` | `MemberChangeMenuFreeCursors` |
| `0x801CAB48` | `MemberChangeMenuSwapCharacters` |
| `0x801CAD14` | `MemberChangeMenuMainLoop` |
| `0x801CB0A8` | `MemberChangeMenuMain` |
