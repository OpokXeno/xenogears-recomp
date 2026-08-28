# General Menu Pages And Gameplay

## 1. Scope

This chapter specifies the General Menu module: top-level dispatch, normal and load-game modes, party summaries, item use, techniques, equipment editing, deathblow progress, Status and Gear routing, sound configuration, gameplay recalculation, and the save/load snapshot boundary.

All addresses below are prefixed **General Menu**.

This chapter stops at the logical UI and gameplay boundary. It does not repeat GPU packet layouts, ordering-table insertion, double buffering, menu-box packet construction, or font rasterization. Those subjects are covered by:

- [Renderer Architecture](../graphics/01-renderer-architecture.md)
- [GPU Packets And Ordering Tables](../graphics/02-gpu-packets-and-ordering-tables.md)
- [Original PlayStation Battle UI And Movies](../graphics/06-battle-ui-and-movies.md)

Field's suspension and restoration around an in-place menu are documented in [Encounters, Transitions, Loading, And Persistence](../field/11-encounters-transitions-loading-and-persistence.md#11-in-place-menus-and-modal-flows).

## 2. Address Convention

Function names identify their control contract. Numeric addresses and branch conditions define shared-page behavior when one page serves more than one visible menu entry.

## 3. Overlay Modes And Lifetime

`MenuOverlayEntry` at General Menu `0x801C62A8` creates common presentation state, initializes the shared menu runtime, selects a mode, and converges on `MenuMainLoopShutdown` at General Menu `0x801C5FE4`.

| Mode | Route | Allocated content |
|---:|---|---|
| `0` | Normal in-game menu | Pre-dispatch common manager block at `+0x348`, size `0x15C`, used for play-time and packet state; memory-card state, cursor, gold, three party cards, and normal pages |
| `2` | Reduced load-game menu | Pre-dispatch common manager block at `+0x348`, size `0x15C`, used for auxiliary state and packets; memory-card state and load-game presentation, without normal gold or party cards |
| `6` | Mode-six title/prompt route | Pre-dispatch common manager block at `+0x348`, size `0x15C`, used for auxiliary state and packets; memory-card state and a separate prompt/action composition, without normal gold or party cards |

Other mode values receive common setup and shutdown but do not enter one of these three dispatch branches in General Menu `0x801C62A8`.

The common lifetime is:

```text
allocate common groups
    -> initialize globals, font/text, cursor, background, and borders
    -> initialize mode-specific content
    -> run one modal coordinator
    -> transition out and render draining frames
    -> release page and common groups
```

`DrawMenuPrimaryElements` at General Menu `0x801C5F10` allocates the shared presentation groups. Before mode dispatch, every mode receives a `0x15C` allocation in the manager field at `+0x348`. Mode 0 uses this block for play-time and packet state; modes 2 and 6 use it for auxiliary state and packets. The three dispatched modes also allocate the `0x5034` memory-card workspace. Mode 0 additionally allocates the `0x328` normal cursor, `0x374` gold display, and three `0x127C` party-card workspaces; modes 2 and 6 omit those normal cursor, gold, and party-card allocations.

### 3.1 Normal mode

Normal mode calls `InitializeNormalMenuContent` at General Menu `0x801D2D38`, which builds normal windows, party and Gear names, the seven main entries, gold, play time, and party cards. `MainMenuExecute` at General Menu `0x801C55A0` then owns navigation until cancellation or an action requests overlay exit.

### 3.2 Load mode

Mode 2 calls `RunLoadSaveTitleSelectionLoop` at General Menu `0x801C58EC`. It uses a three-entry wrapped selector and invokes the common action dispatcher with an offset of seven. On Disc 1, a resident readiness test and a strict `counter > 600` condition terminate this loop on the 601st rendered iteration. This publishes result `1`, returns to resident game state `1`, and selects the title Field's idle-attract path that replays the opening sequence.

When load succeeds, the dispatcher sets the resident next-mode value and returns an overlay-exit result. When it does not, the reduced menu remains active or returns through its ordinary cancellation route.

### 3.3 Mode six

Mode 6 calls `LoadSaveGameMenuExecute` at General Menu `0x801C57A4`. It presents one or two prompts and enters the offset dispatcher when the selected action requires it. Its dedicated draw composition is selected by `DrawMenuModeSixComposition` at General Menu `0x801D1C48`. This route is relevant to the shared control architecture but is not one of the normal seven pages.

## 4. Frame And Input Contract

`MenuDraw` at General Menu `0x801C7BF4` samples and translates one queued
controller event and presents one frame. It processes the optional reset path,
selects the alternate environment and packet parity, clears its ordering table,
advances logical presentation, rebuilds current-parity packets, synchronizes
prior GPU work, installs the environments, performs the transfer, and submits
the ordering table. The modal caller interprets the published command only
after `MenuDraw` returns. Any resulting selection or display rebuild is therefore
seen in a subsequent composition, not in the frame that sampled the command.

The logical command produced by `UpdateMenuInput` at General Menu `0x801C7D78` is shared by the page state machines:

| Command | Meaning |
|---:|---|
| `0` | Right |
| `1` | Down |
| `2` | Left |
| `3` | Up |
| `4` | Confirm |
| `5` | Cancel |
| `6` | Square action; page-specific |
| `7` | Triangle action; page-specific |
| `8` | Idle/no recognized input |
| `9` | R1 party/page advance |
| `10` | L1 party/page reverse |
| `12` | Select action and diagnostic toggle |

Each modal loop follows the same invariant:

```text
MenuDraw samples/translates input and presents one frame
    -> after return, the modal caller interprets one logical command
    -> perform a nested modal action or update selection
    -> rebuild changed selection, page, character, or mode state for a subsequent composition
    -> leave through the page's cleanup path
```

Selection refresh is deliberately separated from the frame that samples its input. After `MenuDraw` returns, a current index and a previously rendered index are compared, and text, cursor, details, or cards are rebuilt only when they differ. The rebuilt state appears in a subsequent composition.

## 5. Seven-Entry Normal Dispatch

`MainMenuExecute` wraps a local selection in `0..6`. Up increments the index and wraps `6` to `0`; Down decrements it and wraps `0` to `6`. Confirm hides the top-level cursor and calls `MenuExecuteSelection` at General Menu `0x801C531C` with offset zero. Cancel terminates the normal menu without invoking a page.

The visible list is stored bottom-to-top. Internal index `0` is the bottom entry and index `6` is the top entry:

| Normal index | Visible entry | Target behavior | Callee |
|---:|---|---|---|
| `6` | Status | Character status router for Equip, Abilities, and Skills | `ProcessAbilitiesMenu`, General Menu `0x801E2BE4` |
| `5` | Equip | Character equipment | `OpenEquipmentMenu`, General Menu `0x801E0F78` |
| `4` | Items | Item inventory and use | `ProcessItemMenu`, General Menu `0x801DBE54` |
| `3` | Abilities | Character Abilities page, technique mode `0` | `OpenCharacterTechniqueMenu`, General Menu `0x801DE29C` |
| `2` | Gear | Gear status, equipment, Abilities, Gear Options, and mount state | `OpenStatusMenu`, General Menu `0x801E23CC` |
| `1` | File | Save, copy, and delete interface | `ProcessLoadSaveMenu`, General Menu `0x801D9F98` |
| `0` | Exit | Return an overlay-exit result without opening a page | Local exit target in General Menu `0x801C531C` |

Gear is disabled when the current party has no usable Gear. Other disabled states are controlled by each child page's eligibility tests.

The same dispatcher also has offset-only cases:

| Effective case | Route | Reachability in this overlay |
|---:|---|---|
| `7` | Sound menu | Reached by reduced/title dispatch offset, not normal index `7` |
| `8` | Load operation that can request resident mode `2` | Reached by reduced/title dispatch offset |
| `9` | Resident action followed by overlay exit | Reached by reduced/title dispatch offset |

After a page returns, `CleanupCompletedMenuAction` at General Menu `0x801E3088` selects teardown by the same effective case. A page result can either restore the top-level presentation or leave the overlay; cleanup itself does not decide whether gameplay changes made by a confirmed page are committed.

## 6. Party Cards, Gold, And Play Time

Normal mode presents up to three compact party cards. `BuildCompletePartyInfoCard` at General Menu `0x801D5A50` rebuilds one card from the active party slot and its animated position. The card contains portrait, name, fixed labels, HP, EP, experience-related values, and primary and secondary levels. Some prepared secondary fields are intentionally not submitted by the compact-card draw path.

The identifier and vital builders distinguish character and Gear data. Empty party slots are disabled rather than rendered as zero-valued members. Card transitions are coordinated by `TransitionPartyInfoCards` at General Menu `0x801D29A8`; the same transition controls gold-window and play-time visibility.

Gold is read from the persistent game-state value and formatted by `BuildGoldAmountDisplay` at General Menu `0x801D5BA4`. This page is display-only: opening or closing the normal menu does not debit or credit gold.

Play time is decomposed by `UpdatePlayTimeDigits` at General Menu `0x801C7F34` into seven displayed digits and two separators:

```text
HHH:MM:SS
```

`InitializePlayTimePanel` at General Menu `0x801D28FC` creates the first display, and `RefreshVisiblePlayTime` at General Menu `0x801D2968` rebuilds digits only while that panel is enabled. The menu therefore displays the live resident counter rather than a frozen value captured on entry.

Party-card invariants:

- There are exactly three display slots.
- An absent party ID suppresses the complete slot.
- Character and Gear vitals are selected by mode; they are not mixed in one row.
- Rebuild and draw flags are independent, so cleanup must disable the group
  before releasing its workspace.

## 7. Items

### 7.1 Inventory view

`InitializeItemMenuResources` at General Menu `0x801DA4A8` creates the item selector, text, glyphs, and inventory-view storage. `BuildItemInventoryPage` at General Menu `0x801DA5BC` renders exactly 16 slots as two columns of eight.

For each visible inventory position it:

1. Reads the item identifier and parallel quantity byte.
2. Suppresses an empty identifier.
3. Clears an identifier whose quantity is zero.
4. Clamps a displayed/stored quantity above 99 to 99.
5. Builds item-name and two-digit quantity text.
6. Applies usability coloring from item flags and the current party context.

`ComputeItemInventoryScrollRange` at General Menu `0x801DBDB4` scans all 150 inventory positions, remembers the final occupied position, and derives the page limit and scroll-marker geometry. If the final position is before slot 16, the marker uses the unscrolled configuration.

### 7.2 Navigation and scrolling

`ProcessItemMenu` at General Menu `0x801DBE54` maintains:

- A visible-cell index in `0..15`.
- A page offset measured in two-item rows.
- A previous page and previous cell for rebuild suppression.
- An optional absolute reorder source, with `0xFF` meaning no source selected.

Left/right move by one cell. Up/down move by two cells and cross a page boundary when possible. R1/L1 move the page offset by up to eight rows and clamp to the computed range. The animated cursor is hidden when its logical row is outside the current page.

### 7.3 Reorder state machine

```text
Browse
  Confirm -> remember absolute source -> ReorderArmed
  Cancel  -> Exit

ReorderArmed
  Move       -> source remains remembered
  Confirm at another slot -> swap IDs and quantities -> Browse
  Confirm at source       -> try item use -> Browse
  Cancel                  -> clear source -> Browse
```

`SwapInventorySlots` at General Menu `0x801DBD4C` exchanges both parallel bytes. Swapping only identifiers or only quantities would violate the inventory invariant.

### 7.4 Target and use

Confirming the armed source at its current position calls `RunItemTargetSelection` at General Menu `0x801DB920`. The item definition controls whether the target mask begins as the current member only or all present party members. Absent slots never become targets.

The nested target state machine is:

```text
Prepare target cards and dim parent list
    -> choose one eligible target or retain the all-target mask
    -> Confirm: call ApplyConsumableEffect for each selected party slot
    -> at least one successful application: decrement quantity once
    -> Cancel or no effective target: consume nothing
    -> restore list, detail, and cursor presentation
```

Quantity is decremented only after at least one selected target reports a successful effect. If the resulting quantity is zero, the item identifier is cleared. A multi-target item still decrements the stack once per activation, not once per affected target.

`UpdateSelectedItemDetails` at General Menu `0x801DA9A8` displays name, quantity, description, target classification, and item classification for a nonempty slot. It hides the detail group for an empty slot.

Commit/cancel behavior is local and explicit:

- Reorder commits immediately when the two slots are swapped.
- Successful use commits the effect and one quantity decrement immediately.
- Cancel from target selection commits neither an effect nor consumption.
- Cancel from the item page does not roll back earlier swaps or uses.

## 8. Techniques

The technique page is a shared engine with three modes selected by its caller:

| Mode | Visible route | Data route | Resource charged on success |
|---:|---|---|---|
| `0` | Top-level Abilities and Status -> Abilities | Character's ordinary Abilities | Character EP |
| `1` | Gear -> Abilities | Pilot Abilities available while operating the Gear | Character EP |
| `2` | Gear -> Gear Option | Gear option records | Gear fuel |

`BuildTechniqueList` at General Menu `0x801DC3D8` evaluates 14 logical entries. It builds names, costs, values, learned/available state, and affordability coloring. Character modes expose up to the first 12 ordinary technique records plus resource summary rows; Gear mode uses paired entries and Gear-specific records. Unavailable entries are disabled rather than treated as zero-cost actions.

`ProcessTechniqueMenu` at General Menu `0x801DDF24` uses a two-column grid. In character modes, horizontal movement changes by one and vertical movement by two, clamped to the ordinary selectable range. Gear mode disables the one-column-step branches. R1/L1 calls `SelectAdjacentAvailablePartySlot` at General Menu `0x801D9704` and rebuilds the list for the next valid character or Gear owner.

Confirm enters `RunTechniqueTargetSelection` at General Menu `0x801DD790` only when the selected entry's enabled bit is set. Targeting then follows the technique's target flag:

- Single-target techniques begin on the current eligible party member.
- Multi-target techniques build a mask over all present party members.
- Character-mode party switching is allowed where the target policy permits it.
- A target already at maximum HP does not count as successfully healed.

On successful activation, `ApplyTargetedHealing` at General Menu `0x801E35BC` updates each selected target, then the caller deducts the technique cost:

```text
character healing = caster ether-like byte * technique potency byte
Gear healing      = target maximum Gear HP / 10
```

Both paths clamp current HP to maximum. Character modes deduct the technique's one-byte cost from current EP. Gear mode deducts its halfword cost from current fuel. Affordability is checked before activation; the deduction is not applied when no selected target can benefit.

Cancel leaves HP, EP, and fuel unchanged. A confirmed successful heal persists after leaving the technique or parent page.

## 9. Equipment

### 9.1 Staging model

Equipment uses an explicit baseline rather than treating every preview as a permanent write. `StageCurrentEquipmentLoadout` at General Menu `0x801DF5D0` copies the selected character or Gear loadout into an editable/baseline area and copies nine derived comparison values.

The staged cardinalities differ:

| Mode | Primary/category bytes staged | Additional equipment bytes staged |
|---|---:|---:|
| Character | 5 | 3 |
| Gear | 4 | 3 |

The logical classes are weapon, armor, accessory, and Gear-specific slots. Numeric category IDs are mode-dependent; the code does not support one universal category enumeration shared by characters and Gears.

`SetupEquipmentCategoryWindow` at General Menu `0x801DE474` selects the correct label set, enables only applicable categories, and replaces the previous window with the required dimensions.

### 9.2 Compatibility and candidate page

`BuildCompatibleEquipmentPage` at General Menu `0x801DE5CC` scans the relevant inventory arrays and filters candidates by:

- Character versus Gear mode.
- Destination category and slot.
- A character compatibility mask or Gear compatibility mask.
- Item type and slot restrictions.
- Existing component restrictions and category-specific masks.

It emits at most eight candidate rows from the requested offset, including quantity and enabled state, and returns the remaining scroll range. The empty selection is represented separately from an inventory item; it is not an item with quantity zero.

### 9.3 Preview, confirm, and rollback

`StageSelectedEquipmentCandidate` at General Menu `0x801DFB68` writes the chosen candidate into the mode-appropriate staged destination. The page then rebuilds derived values:

- Character preview calls `RecalculateCharacterEquipmentEffects` at General Menu `0x801E36D4` and `CalculateCharacterDisplayedStats` at General Menu `0x801E3A80`.
- Gear preview calls `RecalculateGearEquipmentPreview` at General Menu `0x801DFE2C`, which synchronizes Gear effects and calculates displayed Gear values before copying comparison statistics.

The candidate browser has the following transactional behavior:

```text
Enter category
    -> capture current loadout as baseline
    -> browse candidate and preview derived values

Confirm candidate
    -> ApplyEquipmentSelection
    -> attempt a best-effort return of the displaced item
    -> capture the resulting loadout as the new baseline
    -> return to category selection

Cancel candidate browser
    -> CommitStagedEquipmentLoadout restores the captured baseline
    -> return to category selection
```

The name `CommitStagedEquipmentLoadout` at General Menu `0x801DF890` describes a copy from staging back to persistent character/Gear slots. In the cancellation branch this is a rollback to the baseline captured on browser entry. After a successful equip, the baseline is refreshed, so a later cancel does not undo an already confirmed replacement.

### 9.4 Inventory exchange

`ApplyEquipmentSelection` at General Menu `0x801DF0D4` performs the persistent exchange:

1. Resolve the selected destination and current item.
2. Install or remove the selected replacement.
3. Decrement the matching replacement inventory stack.
4. Attempt to return the displaced item: this inline path increments the first matching stack with a clamp at 99 and ends the search; only the absence of a match permits a first-empty-slot search, and either capacity failure loses the unit.
5. Clear identifiers whose quantities became zero.
6. Clamp normalized inventory quantities to 99.
7. Update the UI category context as required, without returning an exchange error result.

`ApplyEquipmentSelection` uses the first-match behavior above. The standalone
`ReturnReplacedEquipmentToInventory` at General Menu `0x801E0434` behaves
differently: after clearing the replaced slot, it scans all 100 inventory slots
and increments every stack whose ID matches the displaced item, clamping each
quantity to 99. If at least one duplicate matches, it performs no empty-slot
search. Only when no match exists does it use the first empty slot; if none
exists, the item is lost. The helper returns no status, so duplicate stacks can
multiply one displaced item and full matching stacks can absorb it silently.

Equipment invariants:

- Preview statistics must be recomputed from the preview loadout, never patched
  by adding only the selected item's headline values.
- Item identifier and quantity arrays remain parallel.
- Returning a displaced item is best-effort and never rolls back a confirmed
  replacement.
- `ApplyEquipmentSelection` stops at the first matching stack, clamps it to 99,
  and does not attempt an empty-slot fallback when that stack is full.
- `ReturnReplacedEquipmentToInventory` increments every duplicate matching
  stack; any match suppresses empty-slot fallback.
- Without a matching stack, either path stores the displaced unit only if an
  empty slot exists; otherwise it is lost without an error result.
- A confirmed replacement advances the rollback baseline even if the displaced
  unit cannot be retained.
- Cancel restores the current baseline but never restores an older, already
  confirmed baseline.
- Character and Gear compatibility masks are not interchangeable.

## 10. Deathblows

`SetupDeathblowMenuDisplay` at General Menu `0x801E1014` allocates the page, loads character assets, validates party eligibility, and creates 13 double-buffered logical entry displays. `ProcessDeathblowMenu` at General Menu `0x801E20C8` rejects two ineligible character IDs, supports R1/L1 party switching while skipping those IDs, and exits only on cancel.

`PopulateDeathblowMenuEntries` at General Menu `0x801E1AC8` evaluates each of the 13 entries in this order:

1. Test the character's learned bit.
2. For an unlearned entry, calculate mastery.
3. Classify it as hidden/unavailable, learned, or mastery-visible.
4. Build the name and level indicator when visible.
5. Build either the input sequence or mastery percentage/progress bar.

`CalculateDeathblowMastery` at General Menu `0x801E1418` consumes seven character learning counters and seven entry-specific requirements:

```text
for each nonzero requirement:
    ratio = min(counter * 100 / requirement, 100)
mastery = sum(ratio) / number_of_nonzero_requirements
```

A requirement of `0xFFFF` participates in the denominator but bypasses ratio addition. If no requirement is nonzero, the result is zero. Entries `7..12` return zero mastery unless persistent enable bit `0x4000` is set.

The deathblow page is read-only. It displays the counters updated elsewhere, including Battle, but does not learn a deathblow or mutate mastery on menu exit.

## 11. Gear

`SetupStatusMenuDisplay` at General Menu `0x801E2250` allocates three Gear work areas, loads Gear assets, finds the first enabled party slot, and creates the selector. `OpenStatusMenu` at General Menu `0x801E23CC` coordinates detailed statistics, equipment names, character/Gear view, and four contextual actions.

The status presentation includes:

- Character or Gear portrait and name.
- Current and maximum HP.
- Character EP or Gear resource values where selected by mode.
- Primary and secondary levels.
- Progression and experience totals.
- One mode-specific summary value.
- Seven attribute rows with numerical values and scaled comparison bars.
- Current equipment names.
- A Gear-present/equipped indicator for applicable characters.

`BuildStatusAttributeValuesAndBars` at General Menu `0x801D8644` scales both a base value and a signed difference against the maximum across two seven-value sets. Increase and decrease styling is selected from the sign; it is a preview comparison, not a stat mutation.

The four Gear actions route as follows:

| Gear action index | Visible action | Route |
|---:|---|---|
| `0` | Equip | Gear equipment page |
| `1` | Abilities | Pilot Ability mode `1` |
| `2` | Gear Option | Gear option mode `2` |
| `3` | Get on/off | Toggle the selected party member's character/Gear state when a Gear exists and the global gate permits it |

R1/L1 changes the selected available party slot. Cancel closes Gear and frees all Gear work areas. Confirmed equipment or technique effects reached through Gear follow their own commit rules; closing Gear does not roll them back.

## 12. Status Routing

`ProcessAbilitiesMenu` at General Menu `0x801E2BE4` implements the visible Status page and its child router. It begins on the first present party slot, recalculates character equipment effects and displayed statistics when the character changes, and presents three actions:

| Status action index | Visible action | Child route |
|---:|---|---|
| `0` | Equip | Character equipment page |
| `1` | Abilities | Character Ability mode `0` |
| `2` | Skills | Deathblow page |

Before entering a child it hides the parent cursor and action labels. After the child returns it restores labels, detail flags, transition state, and the previous action selection. R1/L1 changes party member without changing the selected child action. Cancel tears down only the Status parent; it does not undo changes confirmed in a child page.

Equipment, techniques, and deathblows are shared by direct top-level, Status, and Gear routes rather than duplicated.

## 13. Sound Menu

`ProcessSoundMenu` at General Menu `0x801D9808` is reached through effective dispatcher case `7`, not through a seventh normal selector index. It has three wrapped choices:

| Internal choice | Label | Resident value | Mixer contract |
|---:|---|---:|---|
| `0` | Mono | `0` | Disable pan and send equal gain to left and right |
| `1` | Wide | `2` | Enable pan and apply opposite-polarity channel routing to widen the image |
| `2` | Stereo | `1` | Enable ordinary left/right pan without phase inversion |

The visible list is top-to-bottom Stereo, Wide, Mono because the selector indices run bottom-to-top. On entry, the inverse mapping selects the current resident sound mode. Confirm replaces sound flag bits `0x700`, rebuilds channel state, updates active sound managers, and writes the mapped mode. Cancel leaves the resident mode unchanged.

Resident `SoundSetOutputMode` at `0x800386C4` and `SoundGetOutputMode` at `0x80038824` encode the modes in sound-system flag bits `0x700`:

| Resident value | Flag bits | Pan bit `0x100` | Phase-routing bits `0x600` |
|---:|---:|---|---|
| `0` Mono | `0x000` | Disabled | Clear |
| `1` Stereo | `0x100` | Enabled | Clear |
| `2` Wide | `0x300` | Enabled | `0x200` |

The setter clears `0x700` before installing the selected value. The getter returns `0` when `0x700` is clear, `1` when only the pan bit is present, and `2` when either phase-routing bit is present.
Setter input `3` installs `0x500`; the getter normalizes it to resident value `2`. The Sound menu writes only inputs `0`, `1`, and `2`.

## 14. Gameplay Calculations

### 14.1 Consumables

`ApplyConsumableEffect` at General Menu `0x801E31C0` reads one 16-byte item effect record and mutates one `0xA4` character record. Effect classes are:

| Effect flag | Operation |
|---:|---|
| `0x8000` | Add `potency * 50` to current HP, then clamp to maximum HP |
| `0x4000` | Add `potency * 10` to current EP, then clamp to maximum EP |
| `0x0004` | Apply masked unsigned increases to four attributes and maximum HP/EP |
| `0x0002` | Apply a signed change to a one-byte status/resource field |
| `0x0001` | Dispatch one of two special global effects for potency values `1` or `2` |

For class `0x0004`, the mask at effect-record `+0x0C` selects the destination fields. Each selected byte attribute first stores `(value + potency) & 0xFF`, then values `201..255` are replaced by 200. Maximum HP and EP are increased and capped at 999 and 99. Class `0x0002` supplies the separate signed change and bounds its one-byte destination to `0..200`. Potency `1` calls `OverwriteInventoryCatalogRangesWithTenEach` at General Menu `0x801E5058`; potency `2` calls `UnlockAllCharactersAndAbilities` at General Menu `0x801E5178`.

`OverwriteInventoryCatalogRangesWithTenEach` overwrites selected parallel ID and
quantity slots; it does not add ten to existing quantities and does not clear
slots outside these ranges:

| Inventory | Slot range | Written ID | Written quantity |
|---|---:|---:|---:|
| Weapons | `1..71` | Slot index | `10` |
| Accessories | `1..149` | Slot index | `10` |
| Items | `1..75` | Slot index | `10` |
| Gear Parts | `1..71` | Slot index | `10` |
| Gear Weapons | `1..104` | Slot index | `10` |

The return value is used by item targeting as an "already full" result for requested HP/EP recovery. If both HP and EP recovery are requested, it reports full only when both requested channels were already full. Effects that do not request either recovery channel return the ordinary success value through this test.

### 14.2 Character recalculation

`RecalculateCharacterEquipmentEffects` at General Menu `0x801E36D4` first
clears accumulated equipment-derived state, then folds three equipped items
into:

- Attribute modifiers.
- Status and elemental masks.
- Weapon power, effect, and attribute fields.
- Accuracy/response-style modifiers.
- Other accumulated bytes consumed by Battle and menu display.

`CalculateCharacterDisplayedStats` at General Menu `0x801E3A80` combines base
character values, weapon values, and accumulated equipment modifiers into seven
displayed values. Caps are 250 for attack/defence/ether-style values,
99 for hit and evade, and a special agility correction when the result exceeds
20. These are derived display values; changing equipment must rerun the fold.

### 14.3 Gear recalculation

Gear recalculation is layered:

```text
LoadGearComponentBaseStats
    -> frame maximum HP and core values
    -> engine fuel capacity/output and fuel clamp
    -> armor defensive values
RecalculateGearEquipmentEffects
    -> equipment totals, masks, weight, and weight penalty
LoadGearWeaponStats
    -> weapon power/effects and multi-weapon variant handling
CalculateGearDisplayedStats
    -> combine Gear, equipment, weapon, and pilot modifiers
```

`LoadGearFrameBaseStats` at General Menu `0x801E41C0` resets current HP to the new frame maximum and enforces its limit. `LoadGearEngineBaseStats` at General Menu `0x801E42AC` clamps current fuel to the new engine capacity. `CalculateGearWeightPenalty` at General Menu `0x801E4928` returns a nonnegative penalty from total equipment weight and the Gear's innate offset.

`SynchronizeLinkedGearLoadouts` at General Menu `0x801E3ECC` handles linked
Gear records. It recalculates the selected record, mirrors three equipment IDs
into the linked record selected by Gear identifier, and refreshes the affected
Gear. The 20 Gear records therefore do not represent independent loadouts.

`CalculateGearDisplayedStats` at General Menu `0x801E3C2C` writes current and
maximum HP, fuel, attack, defence, response, and related preview values into the
shared comparison area. It includes pilot-derived modifiers and special cases
for selected Gear IDs; it is not equivalent to summing component table
columns.

## 15. Snapshot And Restore Boundary

`SnapshotMenuGameState` at General Menu `0x801E4A28` is called by
`BuildMemoryCardSaveImage` at General Menu `0x801CBA4C`. It copies mutable
character records, compact Gear state, names/settings, ability/progression
blocks, inventories, and related persistent state into the menu save image.

`RestoreMenuGameState` at General Menu `0x801E4D10` is called by
`ApplyLoadedSavePayload` at General Menu `0x801CB28C`. For each Gear it restores
core fields and weapon channels, reloads frame, armor, and engine base stats,
and recalculates equipment effects. It then restores saved current fuel and the
remaining compact state bytes. Because saved capacity tier `Gear+0x75` is
written after recalculation, derived weight penalty `Gear+0x4A` retains the
result calculated from the prior tier until a later recalculation.

These functions serve memory-card serialization and load application. They are
not a general transaction mechanism for item, technique, or equipment cancel.
Page rollback is implemented locally, notably by the equipment baseline. They
also differ from Field's transient reentry snapshot, which preserves a live
Field map and actor runtime rather than the memory-card game payload.

Snapshot/restore invariants:

- Runtime pointers are not treated as portable save-state fields.
- Restored core component identifiers are followed by Gear recalculation; saved
  current fuel and compact tail state are restored afterward.
- Runtime names are converted around save-image construction by the caller.
- A successful load replaces persistent gameplay state; cancellation before a
  successful load does not call the restore routine.

## 16. Cross-Page Commit And Cancel Rules

| Page/action | Confirm commits | Cancel behavior |
|---|---|---|
| Main selector | Opens page or requests overlay exit | Leaves normal menu |
| Item reorder | Immediate ID+quantity swap | Clears armed source only |
| Item target | Effects plus one stack decrement after any success | No effect and no decrement |
| Technique target | Healing plus EP/fuel deduction after success | No HP or resource change |
| Equipment candidate | Inventory exchange, loadout, recalculation, new baseline | Restore current staged baseline |
| Deathblow | No gameplay write | Close read-only page |
| Status/Abilities parent | Child owns any writes | Close parent; do not undo child commits |
| Sound | Write mapped resident mode | Leave resident mode unchanged |
| Memory-card load | Replace payload and recalculate | Do not restore payload |

No top-level cancel performs a whole-menu rollback. Once a child action has
passed its own commit point, returning through `CleanupCompletedMenuAction` only
releases presentation resources.

## 17. Function Index

### 17.1 Entry, dispatch, input, and common state

- General Menu `0x801C531C` `MenuExecuteSelection`; `0x801C55A0` `MainMenuExecute`; `0x801C57A4` `LoadSaveGameMenuExecute`; `0x801C58EC` `RunLoadSaveTitleSelectionLoop`.
- General Menu `0x801C5F10` `DrawMenuPrimaryElements`; `0x801C5FE4` `MenuMainLoopShutdown`; `0x801C62A8` `MenuOverlayEntry`; `0x801C6400` `SetupMenuModeSpecificElements`.
- General Menu `0x801C6AA0` `InitMenuGlobals`; `0x801C7B0C` `SetupMainMenu`; `0x801C7BF4` `MenuDraw`; `0x801C7D78` `UpdateMenuInput`; `0x801C8574` `PlayMenuSoundEffect`.
- General Menu `0x801D1BE8` `DrawMenuLoadMode`; `0x801D1C48` `DrawMenuModeSixComposition`; `0x801D1CA0` `DrawMenuSpecificElements`; `0x801E3088` `CleanupCompletedMenuAction`.

### 17.2 Party, gold, time, windows, and party selection

- General Menu `0x801C7F34` `UpdatePlayTimeDigits`; `0x801D28A8` `RefreshGoldPanel`; `0x801D28FC` `InitializePlayTimePanel`; `0x801D2968` `RefreshVisiblePlayTime`; `0x801D29A8` `TransitionPartyInfoCards`.
- General Menu `0x801D2D38` `InitializeNormalMenuContent`; `0x801D2EC0` `RefreshPartyInfoCard`; `0x801D397C` `InitializeMenuWindowSlot`; `0x801D3B00` `AdvanceAnimatedMenuWindows`; `0x801D4EA0` `ReleaseMenuWindowResources`.
- General Menu `0x801D5A50` `BuildCompletePartyInfoCard`; `0x801D5BA4` `BuildGoldAmountDisplay`; `0x801D5CF8` `BuildPlayTimeDisplay`; `0x801D9704` `SelectAdjacentAvailablePartySlot`.

### 17.3 Sound and memory-card logical boundary

- General Menu `0x801D9808` `ProcessSoundMenu`; Resident `0x800386C4` `SoundSetOutputMode`; Resident `0x80038824` `SoundGetOutputMode`; General Menu `0x801D9C84` `InitializeLoadSaveMemoryCards`; `0x801D9E3C` `ShutdownLoadSaveMemoryCards`; `0x801D9F98` `ProcessLoadSaveMenu`.
- General Menu `0x801CB28C` `ApplyLoadedSavePayload`; `0x801CB304` `RunMemoryCardLoadOperation`; `0x801CBA4C` `BuildMemoryCardSaveImage`; `0x801CBD90` `RunMemoryCardSaveOperation`.

### 17.4 Items and techniques

- General Menu `0x801DA4A8` `InitializeItemMenuResources`; `0x801DA518` `ReleaseItemMenuResources`; `0x801DA5BC` `BuildItemInventoryPage`; `0x801DA9A8` `UpdateSelectedItemDetails`.
- General Menu `0x801DB02C` `AllocateAnimatedSelectionCursor`; `0x801DB0A8` `UpdateAnimatedSelectionCursor`; `0x801DB340` `ReleaseAnimatedSelectionCursor`; `0x801DB39C` `SetItemMenuDimming`; `0x801DB5E4` `BuildItemTargetCards`.
- General Menu `0x801DB920` `RunItemTargetSelection`; `0x801DBD4C` `SwapInventorySlots`; `0x801DBDB4` `ComputeItemInventoryScrollRange`; `0x801DBE54` `ProcessItemMenu`.
- General Menu `0x801DC1D4` `InitializeTechniqueMenuResources`; `0x801DC2CC` `ReleaseTechniqueMenuResources`; `0x801DC3D8` `BuildTechniqueList`; `0x801DCE60` `UpdateSelectedTechniqueDetails`; `0x801DD5E8` `SetTechniqueMenuDimming`; `0x801DD790` `RunTechniqueTargetSelection`; `0x801DDF24` `ProcessTechniqueMenu`; `0x801DE29C` `OpenCharacterTechniqueMenu`.

### 17.5 Equipment, deathblow, status, and abilities

- General Menu `0x801DE2C8` `InitializeEquipmentMenuResources`; `0x801DE36C` `ReleaseEquipmentMenuResources`; `0x801DE400` `ReleaseEquipmentPreviewResources`; `0x801DE474` `SetupEquipmentCategoryWindow`; `0x801DE5CC` `BuildCompatibleEquipmentPage`.
- General Menu `0x801DF0D4` `ApplyEquipmentSelection`; `0x801DF5D0` `StageCurrentEquipmentLoadout`; `0x801DF890` `CommitStagedEquipmentLoadout`; `0x801DFB68` `StageSelectedEquipmentCandidate`; `0x801DFE2C` `RecalculateGearEquipmentPreview`; `0x801DFF5C` `BuildEquipmentDescriptionLines`; `0x801E0434` `ReturnReplacedEquipmentToInventory`; `0x801E05D0` `ProcessEquipmentMenu`; `0x801E0F78` `OpenEquipmentMenu`.
- General Menu `0x801E1014` `SetupDeathblowMenuDisplay`; `0x801E1398` `TeardownDeathblowMenuDisplay`; `0x801E1418` `CalculateDeathblowMastery`; `0x801E1544` `BuildDeathblowEntryDetails`; `0x801E1AC8` `PopulateDeathblowMenuEntries`; `0x801E20C8` `ProcessDeathblowMenu`.
- General Menu `0x801E2250` `SetupStatusMenuDisplay`; `0x801E23CC` `OpenStatusMenu`; `0x801E2AE0` `SetupAbilitiesMenuDisplay`; `0x801E2B80` `TeardownAbilitiesMenuDisplay`; `0x801E2BE4` `ProcessAbilitiesMenu`.

### 17.6 Gameplay, derived state, and serialization

- General Menu `0x801E31C0` `ApplyConsumableEffect`; `0x801E35BC` `ApplyTargetedHealing`; `0x801E36D4` `RecalculateCharacterEquipmentEffects`; `0x801E3A80` `CalculateCharacterDisplayedStats`; `0x801E3C2C` `CalculateGearDisplayedStats`; `0x801E3ECC` `SynchronizeLinkedGearLoadouts`.
- General Menu `0x801E4170` `LoadGearComponentBaseStats`; `0x801E41C0` `LoadGearFrameBaseStats`; `0x801E4258` `LoadGearArmorBaseStats`; `0x801E42AC` `LoadGearEngineBaseStats`; `0x801E433C` `RecalculateGearEquipmentEffects`; `0x801E4754` `LoadGearWeaponStats`; `0x801E4928` `CalculateGearWeightPenalty`; `0x801E4998` `InitializeGearResourceHpThreshold`.
- General Menu `0x801E4A28` `SnapshotMenuGameState`; `0x801E4D10` `RestoreMenuGameState`; `0x801E5058` `OverwriteInventoryCatalogRangesWithTenEach`; `0x801E5178` `UnlockAllCharactersAndAbilities`.
