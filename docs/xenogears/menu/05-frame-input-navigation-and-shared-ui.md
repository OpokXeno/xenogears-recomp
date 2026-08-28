# Frame, Input, Navigation, And Shared UI

## 1. Frame Contract

General Menu `MenuDraw` at `0x801C7BF4` translates input and presents one frame:

1. Translate queued controller state into one command.
2. Select the alternate graphics environment and packet parity.
3. Clear the active sixteen-entry ordering table.
4. Advance transitions and animated windows.
5. Compose the active page from the page state that preceded the new command.
6. Link enabled current-parity packets into the ordering table.
7. Synchronize prior GPU work and vertical blank.
8. Install `DRAWENV`, then `DISPENV`.
9. Perform the parity-selected framebuffer `MoveImage` operation.
10. Submit the ordering table.
11. Service memory-card hotplug and enumeration work requested by the page.

After `MenuDraw` returns, the page loop consumes the command published in step
1 and updates selection or page state. That update is not present in the frame
just submitted; its visual effect appears in a later composition. Member Change,
Enter Name, Shop, and Gear Shop preserve the same command-consumption boundary
while supplying module-specific packet groups and commands.

## 2. Controller Sources

| Address | Meaning |
|---:|---|
| Resident `0x800594A4` | Current and queued logical buttons for directions and supplemental actions. |
| Resident `0x8005948C` | Press-edge logical buttons for actions and specialized commands. |
| `SystemMenu+0x325` | One translated Menu command. |

Input translation waits for controller availability, repairs queue overflow,
and drains snapshots until it finds a recognized event. It publishes at most
one command per frame.

## 3. Command Codes

| Code | Command | Default source | Contract |
|---:|---|---|---|
| `0x00` | Right | Right, mask `0x2000` | Move or increment horizontally. |
| `0x01` | Down | Down, mask `0x4000` | Move or increment vertically. |
| `0x02` | Left | Left, mask `0x8000` | Move or decrement horizontally. |
| `0x03` | Up | Up, mask `0x1000` | Move or decrement vertically. |
| `0x04` | Confirm | Circle, mask `0x0020` | Accept the current enabled selection. |
| `0x05` | Back | Cross, mask `0x0040` | Cancel or return to the parent state, except in Enter Name. |
| `0x06` | Square action | Square, mask `0x0080` | Run the page's secondary Square action. |
| `0x07` | Triangle action | Triangle, mask `0x0010` | Run the page's secondary Triangle action. |
| `0x08` | Idle | No recognized event | Preserve navigation state. |
| `0x09` | Next | R1, mask `0x0008` | Select the next member, category, or page. |
| `0x0A` | Previous | L1, mask `0x0004` | Select the previous member, category, or page. |
| `0x0B` | Start action | Start, mask `0x0800` | Run the specialized Start action. |
| `0x0C` | Select action | Select, mask `0x0100` | Toggle `SystemMenu+0x1E94` where supported. |

Each page accepts the subset defined by its current state.

## 4. Translation Priority

When one controller snapshot contains several recognized bits, the first test
wins:

```text
Right -> Down -> Left -> Up
      -> Circle -> Cross -> Square -> Triangle
      -> L1 -> R1 -> Start -> Select -> supplemental input
```

General Menu combines resident and module translation to provide directions,
Confirm, Back, Square, Triangle, L1/R1, and Select. Member Change, Enter Name,
and Shop process the sequence through Start and Select. Gear Shop processes the
sequence through L1/R1.

Priority is part of gameplay behavior: one snapshot produces one navigation or
action result.

## 5. Disconnect Wait

Specialized Menu modules suspend interaction while the controller is absent:

1. Poll controller type and readiness.
2. On disconnection, mute SPU voices and retain their prior state.
3. Poll without advancing Menu state.
4. Restore voices when the controller returns.
5. Reset an overflowed controller queue and publish Idle for that frame.
6. Otherwise drain snapshots until one recognized event is found.

Navigation, transitions, and page timers remain paused during this modal wait.

## 6. Navigation Domains

| Domain | Rule |
|---|---|
| Main menu | Seven entries; movement wraps without skipping disabled entries, and Confirm checks eligibility. |
| Load/save title | Three entries; movement wraps and Confirm dispatches the selected operation. |
| Party selector | Three active slots; Next and Previous wrap while skipping unavailable members or Gears. |
| Item list | Sixteen visible cells in two columns by eight rows; horizontal movement is `+/-1`, vertical movement is `+/-2`, and page offset, selected cell, final occupied slot, and scrollbar update together. |
| Technique list | Fourteen visible entries; eligibility and resource cost are validated separately from cursor position. |
| Equipment candidates | Eight visible rows; category, filtered count, scroll offset, and staged loadout update together. |
| Memory-card grid | Two fifteen-block grids backed by 32 records; records 15 and 31 are sentinels, and three-column movement searches for an eligible tile. |
| Member Change | Three active slots and the first six bench entries; captured source and destination side determine legal exchanges. Bench scrolling is not reachable from normal input. |
| Enter Name | Character grid and name controls edit an encoded buffer and redraw after accepted changes; Cross is backspace only and cannot exit the module. |
| Shop and Gear Shop | Mode, category, eight-row list, quantity, confirmation, and preview are separate states. |

Domains that define an eligibility search resolve it before changing the cursor.
This applies to selectors such as memory-card tiles, party members, and targets;
it does not apply to the Main menu, whose wrapped movement can land on a disabled
entry and whose eligibility check occurs on Confirm.

## 7. Selection State

| Offset | Meaning |
|---:|---|
| `SystemMenu+0x336` | Current top-level choice. |
| `SystemMenu+0x337` | Previous top-level choice. |
| `SystemMenu+0x338` | Current submenu choice. |
| `SystemMenu+0x339` | Previous submenu choice. |
| `SystemMenu+0x33A` | Submenu choice count. |

Pages additionally own row, scroll, category, quantity, source, target, and
confirmation state. An accepted selection update follows this order:

1. Consume the command published by the preceding draw call.
2. Resolve the requested direction or page action.
3. In domains that define an eligibility search, search for an eligible
   destination and retain the current selection when none exists.
4. Update previous and current values for an accepted move.
5. Mark cursor geometry and selected text or preview state for refresh.
6. Expose the update when a later composition rebuilds current-parity
   presentation.

Directional sound is emitted when translation recognizes the command, including
a command that page policy leaves at the current selection.

## 8. Confirmation And Cancellation

Confirm validates the selected operation before changing gameplay state. The
validation covers card-slot eligibility, target compatibility, affordability,
inventory capacity, and party constraints as required by the active page.

Back unwinds one logical level:

```text
transaction confirmation -> quantity or list
quantity or list          -> category or mode
category or mode          -> parent menu
top-level menu            -> closing transition
```

Pages discard or restore staged state at the boundary defined by the operation.
Back exits the module only from the top-level closing path. Enter Name is the
exception: Cross performs backspace only and cannot cancel or exit name entry.

## 9. Transitions

General Menu stores this transition state at `SystemMenu+0x329`:

| Value | State |
|---:|---|
| 0 | Complete. |
| 1 | Opening. |
| 2 | Closing. |
| 3 | Start opening. |
| 4 | Start closing. |

Menu `0x801D1E80` begins opening, Menu `0x801D1EB0` begins closing, and Menu
`0x801D1D40` advances interpolation and installs current GTE matrices. Normal
interaction begins after opening completes. Closing keeps packet owners alive
through their final visible submission.

## 10. Animated Windows

The seven shared window slots each pair a packet owner with an animation owner.
Menu `0x801D397C` creates a complete window immediately or starts centered
expansion. Menu `0x801D3B00` changes width and height by at most 32 pixels per
frame and clamps both axes at their targets.

```text
inactive
  -> allocate packet and animation owners
  -> opening around center
  -> active at target dimensions
  -> closing or final disabled frame
  -> release both owners
  -> inactive
```

Menu `0x801D4EA0` disables and releases a window after its presentation lifetime
ends. Record ownership is specified in
[`04-runtime-objects-allocations-and-ownership.md`](04-runtime-objects-allocations-and-ownership.md#7-shared-windows).

## 11. Cursors And Markers

| Indicator | Purpose |
|---|---|
| Main cursor | Points at one menu entry or list row. |
| Selection markers | Show targets, party members, or memory-card blocks. |
| Animated row cursor | Tracks list and target geometry and hides outside the visible page. |
| Arrow cursors | Indicate scrolling, quantity changes, or category changes. |
| Shoulder hints | Advertise previous and next party-member or Gear switching. |
| Scroll handle | Represents page offset and visible extent in a filtered list. |

Indicators consume semantic selection state after navigation. Gameplay
operations read selected item, party, Gear, and card identifiers from page state.

## 12. Framebuffer And Parity

Menu uses two 320 by 224 frame environments and two packet parities. Menu
`0x801C7BF4` flips `SystemMenu+0x308`, selects current-parity packets, and leaves
the opposite parity alive for prior GPU work.

The presentation boundary preserves:

1. Two-frame packet ownership.
2. Presentation of the pre-command state, followed by page-loop command
   consumption and a later visual rebuild.
3. `DRAWENV` and `DISPENV` selection.
4. Ordering-table insertion order.
5. `MoveImage` timing relative to environment installation and submission.
6. Caller-owned framebuffer content across the modal handoff.

Menu logic owns selection, eligibility, transitions, values, enables, and object
lifetimes. Resource decoders provide strings, glyphs, and image data. Packet
producers build current-parity primitives, and the caller preserves and restores
Field or World presentation state.

## 13. Behavioral Invariants

1. Publish exactly one command per Menu frame.
2. Publish command `8` when no recognized event exists.
3. Resolve simultaneous inputs by translator test order.
4. Update cursor, selection, scroll offset, and preview in one accepted action.
5. Skip ineligible entries only in navigation domains that define an eligibility
   search; validate Main-menu eligibility on Confirm.
6. Apply gameplay mutations after confirmation and operation validation succeed.
7. Keep visible packet storage alive through its final submission.
8. Advance animated window dimensions by at most 32 pixels per axis.
9. Build and submit only current-parity packets.
10. Restore Field or World after selected-module teardown.

## 14. Function Index

| Address | Function |
|---:|---|
| Resident `0x8001BF38` | Translate resident Menu controller input. |
| Resident `0x8001C074` | Present one resident Menu frame. |
| Menu `0x801C7BF4` | Run one complete General Menu frame. |
| Menu `0x801C7D78` | Translate General Menu controller state. |
| Menu `0x801D1D40` | Advance transforms and install matrices. |
| Menu `0x801D1E80` | Begin opening transition. |
| Menu `0x801D1EB0` | Begin closing transition. |
| Menu `0x801D1EE0` | Rebuild cursor and highlight geometry. |
| Menu `0x801D22F4` | Configure selection markers. |
| Menu `0x801D397C` | Allocate and configure one window slot. |
| Menu `0x801D3B00` | Advance animated windows. |
| Menu `0x801D4EA0` | Disable and release one window. |
| Menu `0x801D9704` | Select an adjacent eligible party slot. |
| Menu `0x801DB0A8` | Update an animated list or target cursor. |
| Member Change `0x801C92AC` | Translate Member Change input. |
| Enter Name `0x801C98E8` | Translate Enter Name input. |
| Shop `0x801CACC8` | Translate Shop input. |
| Gear Shop `0x801CB4E4` | Translate Gear Shop input. |
