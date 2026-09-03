# Gear Shop Tuning And Preview

## 1. Runtime Contract

The Gear Shop is invoked by Field opcode `FE 5A`. Its session contract covers
eligible pilots, selected-Gear switching, Buy, Sell, Tune Up, Fuel and HP
service, component simulation, aggregate recomputation, model preview,
confirmation, commit, and teardown.

## 2. Field Request And Definition Index

`FieldScriptOpenGearShopMenu` at `0x80093A04` evaluates argument 1, stores its
low byte in resident menu state, selects module mode `5`, raises the request,
increments the request counter, advances the Field VM PC by three bytes, and
yields.

The argument selects one `0x64`-byte Gear Shop definition:

```text
definition = gear_shop_definition_base + u8(argument) * 0x64
```

The Gear Shop performs no index clamp. Each definition holds five sparse
20-byte stock tables. ID zero is empty.

## 3. Stock Tables And Runtime Types

| Runtime type | Definition offset | Class | Transaction path |
|---:|---:|---|---|
| `0` | `+0x14` | Armor | Tune Up |
| `1` | `+0x00` | Frame | Tune Up |
| `2` | `+0x28` | Engine | Tune Up |
| `3` | `+0x50` | Part | Buy |
| `4` | `+0x3C` | Gear Weapon | Buy |

The active Buy or Tune Up screen copies one 20-byte table into the shared item
arrays and writes its runtime type beside every copied ID. Sell reads persistent
Gear Weapon and Part inventories instead of these tables.

## 4. Component Records

### 4.1 Armor, Frame, And Engine

| Record | Size | Offset | Width | Runtime role |
|---|---:|---:|---:|---|
| Armor | `0x14` | `+0x00` | `u32` | Gear equip flags |
| Armor | `0x14` | `+0x04` | `u16` | Price |
| Armor | `0x14` | `+0x08` | `u16` | Base Defense |
| Armor | `0x14` | `+0x0A` | `u16` | Base Ether Defense |
| Frame | `0x18` | `+0x00` | `u32` | Gear equip flags |
| Frame | `0x18` | `+0x04` | `u32` | Maximum HP |
| Frame | `0x18` | `+0x08` | `u16` | Chassis Weight |
| Frame | `0x18` | `+0x0A` | `u16` | Price |
| Frame | `0x18` | `+0x14` | `u8` | Agility |
| Frame | `0x18` | `+0x15` | `u8` | Ether Amplification |
| Frame | `0x18` | `+0x16` | `u8` | Display byte copied to Gear `+0x9D` |
| Frame | `0x18` | `+0x17` | `u8` | Base Response copied to Gear `+0x9F` |
| Engine | `0x10` | `+0x00` | `u32` | Gear equip flags |
| Engine | `0x10` | `+0x06` | `u16` | Maximum Fuel |
| Engine | `0x10` | `+0x0A` | `u16` | Price |
| Engine | `0x10` | `+0x0C` | `u8` | Engine Output |
| Engine | `0x10` | `+0x0D` | `u8` | Engine byte copied to Gear `+0x3D` |
| Engine | `0x10` | `+0x0E` | `u8` | Engine byte copied to Gear `+0x3E` and `+0x3F` |

Reducing maximum HP or Fuel clamps the corresponding current value down to the
new maximum.

### 4.2 Part Record

Each Part record is `0x1C` bytes:

| Offset | Width | Runtime role |
|---:|---:|---|
| `+0x00` | `u32` | Gear equip flags |
| `+0x04` | `u16` | Price |
| `+0x06` | `u16` | Weight contribution |
| `+0x08` | `u16` | Replacement family; zero never family-matches |
| `+0x0D` | `u8` | Defense contribution |
| `+0x0E` | `u8` | Ether Defense contribution |
| `+0x10..+0x13` | four `u8` | Four additive secondary counters |
| `+0x14` | `u8` | Response contribution |
| `+0x15` | `u8` | Effect selector |
| `+0x16` | `u16` | Selector payload |
| `+0x18` | `u8` | Additive Gear `+0x4C` contribution |
| `+0x1A` | `u8` | Per-bit counter increment for selector `4` |

### 4.3 Gear Weapon Record

Each Gear Weapon record is `0x14` bytes:

| Offset | Width | Runtime role |
|---:|---:|---|
| `+0x00..+0x03` | four `u8` | Per-channel weapon bytes copied to Gear `+0x5C..+0x5F` |
| `+0x04` | `u32` | Gear equip flags |
| `+0x08` | `u16` | Price |
| `+0x0E` | `u8` | Flat attack contribution |
| `+0x0F` | `u8` | Special-slot replacement subtype |
| `+0x10` | `u8` | Weapon parameter copied beside the attack contribution |
| `+0x11` | `u8` | Weapon behavior selector |
| `+0x12` | `u16` | Weapon effect mask |

## 5. Eligible Pilots And Gear Switching

The visible roster is:

```text
visible_flags = unlock_flags & FrMask & 0x077F
eligible(character) = character bit is set and character.gearId != 0xFF
```

The first eligible character supplies the initial Gear. L1/R1 switching runs
only when the transition mask is zero and at least two eligible characters
exist. It wraps through the eligible roster, releases helper model slot 1,
loads the new Gear data, updates the selected Gear ID, reconstructs slot 1, and
refreshes the Gear stat panel.

## 6. Compatibility And Equipped Markers

`GearShopMenuGearCanEquip` returns the bitwise intersection:

```text
item_equip_flags & gear_equip_mask[gear_id]
```

Armor, Frame, and Engine rows require both affordability and selected-Gear
compatibility. Part and Gear Weapon row availability uses all bits, while their
detail view applies each record's actual flags per portrait.

`GearShopMenuGetItemEquippedGearFlags` scans every represented Gear. Types
`0..2` inspect their single component IDs, type `3` scans three Part slots, and
type `4` scans ordinary or special weapon slots according to item ID. The
resulting Gear mask drives `E` markers only.

## 7. Top-Level And Submenus

| Internal value | Action |
|---:|---|
| `3` | Tune Up |
| `2` | Buy |
| `1` | Sell |
| `0` | Exit |

The cursor starts on Buy. Up increments and Down decrements with wrap. Cross
leaves the Gear Shop. The submenu choices are:

| Action | Choices |
|---|---|
| Buy | Parts, Gear Weapons |
| Sell | Gear Weapons, Parts |
| Tune Up | Armor, Frame, Engine, Fuel |

## 8. Shared Transaction Viewport

Buy, Sell, and component Tune Up use an eight-row viewport with
`absolute_index = scroll_offset + row`. Row movement, edge scrolling, arrow
timing, scroll-handle proportional placement, row refresh, and detail refresh
match the ordinary Shop contract. The details panel adds compatibility,
equipped markers, owned quantity, and Attack/Defense deltas.

## 9. Buy Staging And Commit

Buy uses mode `0`:

```text
available = unit_price <= staged_resulting_gold
Right: quantity++; total += unit_price; resulting_gold -= unit_price
Left:  quantity--; total -= unit_price; resulting_gold += unit_price
```

Right also requires `owned_quantity + staged_quantity + 1 < 100`. Parts use a
100-slot persistent inventory and Gear Weapons use 150 slots. Stack quantities
cap at 99.

`GearShopMenuHandleBoughtItems` at `0x801D44FC` stores staged gold first. For
type `3` or `4`, it adds the staged quantity to every slot with a matching ID,
clamping each to 99. Only when no slot matches does it use the first empty slot.
If neither a match nor an empty slot exists, the item is not inserted and the
gold assignment remains. Buy never equips the purchased item.

## 10. Sell Type Mapping And Arithmetic

Sell passes a mode-specific type mapping:

| Sell type | Semantic class | ID array | Quantity array | Slots |
|---:|---|---:|---:|---:|
| `3` | Gear Weapon | `gameState+0x221A` | `gameState+0x2184` | 150 |
| `4` | Gear Part | `gameState+0x2120` | `gameState+0x20BC` | 100 |

The generic Sell loop compacts entries with nonzero ID and quantity. Unit
proceeds are `u16(price) >> 1`. Right transfers one unit into staged sale; Left
reverses it. `GearShopMenuHandleSoldItems` at `0x801D1F20` stores capped gold,
subtracts each sold count from every persistent slot with the matching ID, and
clears IDs whose quantity reaches zero. Duplicate IDs are all modified. `E`
markers do not cause unequip writes. Subtraction is byte-sized; only exact zero
clears the ID, so duplicate-driven underflow wraps and remains nonzero.

## 11. Tune Up Trade-In And Commit

Tune Up uses mode `1` and one candidate table:

| Choice | Type | Replaced Gear field |
|---|---:|---|
| Armor | `0` | `armorId` |
| Frame | `1` | `frameId` |
| Engine | `2` | `engineId` |

```text
trade_in        = current_component.price >> 1
resulting_gold  = current_gold - candidate_price + trade_in
transaction     = candidate_price
displayed_credit = trade_in
```

Availability requires the full candidate price to be no greater than current
gold and requires compatibility. Trade-in credit does not make an otherwise
unaffordable row selectable. Tune Up forces candidate quantity to one only
after confirmation; `GearShopMenuHandleBoughtItems` then replaces the selected
Gear's component ID and stores resulting gold.

## 12. Tune Up Prompt Sequence

`GearShopMenuIsTuneUpUpgrade` compares IDs:

```text
upgrade = candidate_id > current_component_id
```

An upgrade uses one prompt beginning at `0xA3`:

```text
Change equipment?

Yes          No
```

An equal or lower ID uses two prompts. The first is informational and the
second controls commit:

```text
Chosen equipment has
abilities less than
or equal to current?

Change anyway?

Yes          No
```

The second prompt is shown after the first regardless of the first prompt's
return value. No or Cross on `Change anyway?` preserves gold and equipment.

## 13. Confirmation Window Contract

`GearShopMenuConfirmationWindowInitialize(stringIndex)` renders three
consecutive entries beginning at `stringIndex`. Manual choice defaults to No;
Left selects Yes, Right selects No, Circle accepts, and Cross returns No. A
second index other than `0xFF` causes a second three-row prompt.

| Start index | Displayed text |
|---:|---|
| `0x8C` | `Cancel all purchases?` plus Yes/No row |
| `0x8F` | Total-price row, `Is that okay?`, Yes/No row |
| `0x92` | `Cancel all sales?` plus Yes/No row |
| `0x95` | Total-price row, `Is that okay?`, Yes/No row |
| `0x98` | `Choose party member.` |
| `0x9E..0xA2` | `Engine`, `Frame`, `Armor`, `Equip`, `Fuel` |
| `0xA3` | `Change equipment?` plus Yes/No row |
| `0xA6` | `Restore HP and Fuel?` plus total and Yes/No rows |
| `0xA9` | `Not enough money! Buy` / `as much as possible?` / Yes/No row |
| `0xAC` | `No free blocks` / `available.` |
| `0xAF` | `Chosen equipment has` / `abilities less than` / `or equal to current?` |
| `0xB2` | `Change anyway?` plus Yes/No row |
| `0xB5` | `Restore Gear HP?` plus total and Yes/No rows |
| `0xB8` | `HP and Fuel` / `are at max.` / `No need to restore.` |

## 14. Fuel And HP Service

Fuel service selects its prompt from current Gear state:

| State | Prompt | Choice mode |
|---|---:|---|
| Fuel below maximum | `0xA6` | Manual |
| Fuel full, HP below maximum | `0xB5` | Manual |
| Fuel and HP full | `0xB8` | Auto-advance |

For missing Fuel:

```text
missing_fuel = maxFuel - fuel
units        = missing_fuel / 100
if units == 0: units = 1
price        = units * 10
```

If gold covers `price`, Yes deducts `price`, fills Fuel, and fills HP. If gold
is insufficient, prompt `0xA9` offers partial service. Yes adds
`floor(gold / 10) * 100` Fuel, caps it at maximum, stores `gold % 10`, and fills
HP whenever initial gold was nonzero. With 1 through 9G, Fuel and gold do not
change but HP still fills. If Fuel is full and HP is low, Yes fills HP without a
gold deduction. The full-state informational path writes nothing.

## 15. Preview Replacement Families

Part simulation first replaces every equipped Part whose nonzero `+0x08`
family equals the candidate family. If no family matches, it replaces the Part
with the smallest `+0x0D` Defense contribution; equal minima select the later
slot.

Gear Weapon simulation uses two policies:

| Candidate ID | Simulated replacement |
|---:|---|
| `< 50` | Replace the ordinary `weaponId` |
| `>= 50` | Replace every special slot whose record `+0x0F` subtype matches the candidate subtype |

All original IDs are restored after aggregate comparison.

## 16. Aggregate Recalculation Order

`GearShopMenuRecomputeGearStats` applies components in this order:

1. Frame.
2. Engine.
3. Armor.
4. Three Parts.
5. Ordinary and special Gear Weapons.

Buy initialization recomputes all eleven character-linked Gears and snapshots
`MenuDressingRoom+0xB0` as Attack and `+0xA4` as Defense. Candidate simulation
recomputes the same fields, produces absolute deltas with increase/decrease
colors, restores every equipment ID, and recomputes the original aggregates.

## 17. Part Aggregates And Effect Selectors

Before processing three Part slots, `GearShopMenuApplyAccessoryStats` clears:

```text
Gear +0x40, +0x42, +0x44, +0x48
Gear +0x4C..+0x4F
Gear +0x50..+0x53, +0x54..+0x57
Gear +0x6E
Gear +0x7E, +0x82, low twelve bits of +0x86
Gear +0x88..+0x97
```

It also applies `flags &= 0xDB7F` to the linked pilot flag word, clearing
`0x2000`, `0x0400`, and `0x0080`. The high four bits of Gear `+0x86` survive.
Each Part then adds Defense to
`+0x40`, Ether Defense to `+0x42`, Weight to `+0x44`, record `+0x18` to `+0x4C`,
Response to `+0x4D`, and record bytes `+0x10..+0x13` to `+0x50..+0x53`.

The selector at Part `+0x15` dispatches the following operations. Selectors
`1..5` and `9..11` consume payload `+0x16`; selectors `6..8` inspect only the
linked pilot flags.

| Selector | Operation |
|---:|---|
| `1` | OR payload into Gear `+0x7E`, the selector-0 protection/resistance word |
| `2` | OR payload into Gear `+0x82`, the selector-1 protection/resistance word |
| `3` | OR payload into Gear `+0x86`, the selector-3 protection/resistance word |
| `4` | OR payload into Gear `+0x6E`; for each set bit `15-i`, add record `+0x1A` to Gear `+0x88+i` |
| `5` | Add the payload low byte to Gear `+0x4F` |
| `6` | If linked pilot flags `0x1000` and `0x0800` are both set, set `0x0400` |
| `7` | If linked pilot flags `0x0200` and `0x0100` are both set, set `0x0080` |
| `8` | If linked pilot flags `0x0040` and `0x0020` are both set, set `0x0010` |
| `9` | OR payload into Gear `+0x48`, then also perform selector `10` |
| `10` | Add the payload low byte to Gear `+0x56` |
| `11` | Add the payload low byte to Gear `+0x57` |

After all three Parts, the function stores the weight penalty at Gear `+0x4A`.
If Gear `+0x4F` is nonzero, it sets linked pilot flag `0x8000`. If Gear `+0x4F`
is zero, it clears `0x8000` only when the Gear/linked-character ownership test
matches; otherwise that bit retains its previous state.

## 18. Gear Weapon Aggregates

The ordinary Gear Weapon installs:

```text
record +0x0E      -> Gear +0x12 flat attack
record +0x12      -> Gear +0x10 effect mask
record +0x10      -> Gear +0x13 weapon parameter
record +0x11      -> Gear +0x14 behavior selector
record +0x00..03  -> Gear +0x5C..5F channel bytes
```

Behavior selector `100` at Gear `+0x14` clears the high four bits of Gear
`+0x86`, preserves the existing low twelve bits, and ORs the effect mask from
Gear `+0x10` into that result.

Gear IDs `5` and `13` also process special weapon IDs at Gear `+0x04`, `+0x05`,
and `+0x07`. Their masks, flat attacks, parameters, and selectors populate the
three layouts beginning at Gear `+0x10`, `+0x18`, and `+0x20`; their attack
bytes are Gear `+0x12`, `+0x1A`, and `+0x22`. Record channel bytes 1, 2, and 3
populate Gear `+0x5D`, `+0x5E`, and `+0x5F` respectively.

## 19. Display Aggregates

`GearShopMenuComputeDisplayStats` writes:

| View offset | Display value |
|---:|---|
| `+0x9C` | Current HP |
| `+0xA0` | Maximum HP |
| `+0xA4` | `Gear[0x70] + Gear[0x40]` Defense |
| `+0xA6` | Pilot Ether Defense base/equipment plus Gear `+0x72` and `+0x42` |
| `+0xA8` | Gear `+0x68` chassis Weight plus Gear `+0x44` Part Weight |
| `+0xAA` | Gear `+0x6A` secondary display value |
| `+0xAC` | Current Fuel |
| `+0xAE` | Maximum Fuel |
| `+0xB0` | Attack |
| `+0xB2` | `Gear[0x9F] + Gear[0x4D]` Response |
| `+0xB3` | `Gear[0x98] - Gear[0x4A]` adjusted Agility |
| `+0xB4` | Gear `+0x9E` Ether Amplification |
| `+0xB5` | Gear `+0x9D` Frame display byte |
| `+0xB6` | Gear `+0x9C` intrinsic display byte |

Attack is:

```text
engine_term = Gear[0x3C] * (Gear[0x74] + Gear[0x56])

normal Gear:
    attack = Gear[0x12] + engine_term

Gear 5 or 13:
    attack = ((Gear[0x12] + Gear[0x22]) * 6) / 10 + engine_term
```

The Gear Shop displays only Attack and Defense deltas during Part and Gear
Weapon comparison.

## 20. Weight Capacity Tier

Gear `+0x75` is the Part-weight capacity tier. The exact penalty is:

```text
weight_units = Gear[0x44] / 120
penalty      = trunc_toward_zero((weight_units - Gear[0x75]) / 2)
if penalty < 0: penalty = 0
Gear[0x4A] = u8(penalty)
```

The first one-point Agility penalty begins when
`weight_units >= Gear[0x75] + 2`, equivalent to raw Part Weight
`120 * (Gear[0x75] + 2)`. The display subtracts this byte from Frame Agility
without an additional saturation step.

## 21. Preview Ownership And Initialization

The Gear Shop owns selected-Gear policy, two `0x14` data wrappers, menu
transforms, effect state, transition gates, and helper enable flags. Gear Helper
owns ten model slots, constructed mesh/skeleton state, animation tracks,
auxiliary objects, and trail storage.

`GearShopMenuInitializePreviewResources` initializes Gear Helper with 64 tracks,
binds menu lighting state, sets three light vectors, allocates two cleared
wrappers, chooses the first eligible Gear, loads its paired data, and counts
eligible pilots.

`GearShopMenuInitializeGearPreviewModel` constructs helper slot 1, applies
Gear-specific scale and animation selectors, subtracts `0x20` from helper slot
1 coordinate `+0x54`, subtracts `0x400` from coordinate `+0x56`, starts the
selected animation, releases the temporary input buffer, marks the wrapper
live, and refreshes Gear name, HP, Fuel, and Weight.

## 22. Preview Transition Coordinates

The transition state holds start and target triplets. The three mask bits have
these exact consumers:

| Mask bit | Written destination | Role |
|---:|---:|---|
| `0` | `SystemMenu+0x228` (`cameraPosition.vz`) | Camera Z |
| `1` | `SystemMenu+0x224` (`cameraPosition.vy`) | Camera Y |
| `2` | Helper model slot 1 `+0x56` | Model root coordinate |

`GearShopMenuStartGearPreviewTransition` uses the prior Camera-Z target as its
start. Its targets are selected as follows:

```text
camera_z_target = camera_z_by_gear[currentGearId]
camera_y_target = camera_y_by_gear_mode_submenu[(currentGearId * 3 + mainMode) * 4 + submenu]
root_target     = root_by_mode_submenu[mainMode * 4 + submenu]
```

The 20 Camera-Z targets, in Gear-ID order, are:

```text
352, 352, 312, 352, 352, 400, 596, 0, 564, 300,
352, 452, 500, 508, 676, 396, 352, 0, 128, 0
```

The 16 mode/submenu root targets are:

```text
88, 0, 88, -60,
1024, 0, 0, 0,
-1024, -2048, 0, 0,
1024, -1024, -1024, 1024
```

The start routine resets preview animation state, enables all three transition
bits, seeds the small effect position to `(0x18, 0x6E)`, and waits for two
animation state bytes to reach `2`.

## 23. Interpolation And Transform Update

`GearShopMenuInitializeTransitionInterpolation` computes absolute Camera-Z and
Camera-Y distances, normalizes the larger fixed-point step to `0x10000`, scales
the other proportionally, counts the longer axis duration, and derives the
model-root step from that duration. Direction bytes select addition or
subtraction. Normal transition updates use `0x10` substeps.

`GearShopMenuUpdateTransitionEffect` advances each enabled destination,
clamps it exactly to target, and clears its mask bit. Mask zero means complete.
`GearShopMenuUpdateTransforms` then builds the camera rotation/translation
matrix from `SystemMenu+0x218/+0x220` and the model transform matrix from
`SystemMenu+0x1D8/+0x1E0`.

`GearShopMenuInitializePreviewTransition` reverses from the current targets to:

```text
camera Z = 0x400
camera Y = 0
model root +0x56 = -0x400
```

It enables the transition mask and the preview effect group for the return
sequence.

## 24. Commit Boundaries And Teardown

| Operation | Persistent write point |
|---|---|
| Buy Part/Gear Weapon | `GearShopMenuHandleBoughtItems`, `0x801D44FC` |
| Armor/Frame/Engine Tune Up | `GearShopMenuHandleBoughtItems`, `0x801D44FC` |
| Sell | `GearShopMenuHandleSoldItems`, `0x801D1F20` |
| Fuel and HP service | `GearShopMenuFuelTuneUpMenu`, `0x801D5398` |

Selection, quantity editing, trade-in display, compatibility, equipped markers,
aggregate simulation, Gear switching, and preview motion are nonpersistent.

Teardown waits for transition mask zero, disables helper rendering, renders one
frame, shuts down all helper slots and shared pools, marks both wrappers
inactive, releases them, disables shoulder-button UI, drains menu render parity,
and releases the remaining menu state. Gear switching likewise releases helper
slot 1 before replacing its wrapper contents.

## 25. Function Index

`GearShopMenuGetRandomRangeValue` (`801C511C`) is a correctly-implemented
range-roll helper with no discoverable caller anywhere in this overlay or the
rest of the recompiled game — no direct call, no computed call, and no
reference to its address as data in this overlay's raw binary. Nothing in
this chapter's tuning or preview flow appears to actually use it; treat it as
dead code in the retail build rather than a live source of randomness. See
[`rng/01` §2.4](../rng/01-generators-and-determinism.md#24-consumers-across-modules).

```text
801C511C GearShopMenuGetRandomRangeValue | 801C51B8 GearShopMenuSetFt4Rect | 801C5228 GearShopMenuIsCharacterFlagSet | 801C5244 GearShopMenuGetCharacterBitMask | 801C5260 GearShopMenuGetGearEquipFlags | 801C527C GearShopMenuGearCanEquip | 801C5298 GearShopMenuParseNumberToString | 801C5344 GearShopMenuManageResourceState
801C53A8 GearShopMenuManageCoreState | 801C540C GearShopSelectionMenuManager | 801C5470 GearShopMenuManageTextBatchState | 801C54D4 GearShopMenuDressingRoomManager | 801C5538 GearShopMenuManageOverlayPacketState | 801C559C GearShopMenuManageExtendedDisplayState | 801C5600 GearShopMenuShopManager | 801C5664 GearShopMenuManagePreviewState
801C56C8 GearShopMenuLoadResources | 801C5B08 GearShopMenuFilterPartyMembers | 801C5C98 GearShopMenuResetRenderContext | 801C5CA8 GearShopMenuInitializeStringFt4Pair | 801C5EE8 GearShopMenuInitializeMenuStringArray | 801C6098 GearShopMenuUploadSmallVramImage | 801C6114 GearShopMenuInitializeTextResources | 801C6170 GearShopMenuInitializeWindowBorders
801C6278 GearShopMenuMovePointerCursor | 801C665C GearShopMenuClearManagerFlags | 801C668C GearShopMenuSetPolyGradientColor | 801C6708 GearShopMenuInitializeBackgrounds | 801C6A54 GearShopMenuShopDataManager | 801C6E74 GearShopMenuInitializeShopData | 801C7604 GearShopMenuSetVertices | 801C765C GearShopMenuSetWindowBorderPrimitive
801C76A4 GearShopMenuUpdateScrollBarHandle | 801C782C GearShopMenuFreeScrollBarHandle | 801C7870 GearShopMenuInitializeArrowCursor | 801C78EC GearShopMenuUpdateArrowCursor | 801C7A88 GearShopMenuFreeArrowCursor | 801C7AE4 GearShopMenuInitializeWindowGraphics | 801C7E00 GearShopMenuInitializeScrollBar | 801C7F64 GearShopMenuInitializeWindowBorderCorners
801C81AC GearShopMenuSetWindowBorderTop | 801C84F0 GearShopMenuSetWindowBorderBottom | 801C883C GearShopMenuSetWindowBorderLeft | 801C8B84 GearShopMenuSetWindowBorderRight | 801C8ED0 GearShopMenuSetWindow | 801C9054 GearShopMenuFreeWindow | 801C90E0 GearShopMenuInitializeWindow | 801C9264 GearShopMenuUpdateWindows
801C93B0 GearShopMenuRenderPolygons | 801C94CC GearShopMenuRenderString | 801C9550 GearShopMenuRenderScrollBarHandle | 801C959C GearShopMenuRenderSelectionPointer | 801C962C GearShopMenuRenderShoulderButtonUi | 801C9690 GearShopMenuRenderTopWindowBorder | 801C9864 GearShopMenuRenderBottomWindowBorder | 801C9A38 GearShopMenuRenderLeftWindowBorder
801C9C0C GearShopMenuRenderRightWindowBorder | 801C9DE0 GearShopMenuRenderWindowBackground | 801C9F1C GearShopMenuRenderWindowBorderCorners | 801CA068 GearShopMenuRenderScrollBar | 801CA28C GearShopMenuRenderWindows | 801CA404 GearShopMenuRenderPointerCursors | 801CA754 GearShopMenuRenderAuxGroup4 | 801CA7E4 GearShopMenuRenderAuxGroup8
801CA874 GearShopMenuRenderAuxQuads | 801CA9EC GearShopMenuRenderAuxGroup6 | 801CAA7C GearShopMenuRenderConfirmationText | 801CABD8 GearShopMenuRenderBackgroundDim | 801CABE0 GearShopMenuRenderAuxiliaryGroups | 801CAC20 GearShopMenuRenderSelectionMenu | 801CB2E8 GearShopMenuRenderAuxTextGroups | 801CB35C GearShopMenuRenderArrowCursors
801CB3D0 GearShopMenuRender | 801CB498 GearShopMenuPlaySoundEffect | 801CB4E4 GearShopMenuPollInput | 801CB690 GearShopMenuInitializeTransitionInterpolation | 801CBA2C GearShopMenuUpdateTransitionEffect | 801CBDA0 GearShopMenuUpdateTransforms | 801CBE60 GearShopMenuRenderDebugValues | 801CC1C4 GearShopMenuUpdateAndRender
801CC31C GearShopMenuInitializePointerCursors | 801CC4DC GearShopMenuFreePointerCursors | 801CC520 GearShopMenuNoOpCallback0 | 801CC528 GearShopMenuNoOpCallback1 | 801CC530 GearShopMenuConfirmationWindowInitialize | 801CC9A0 GearShopMenuConfirmationWindowFree | 801CCA40 GearShopMenuConfirmationWindowGetChoice | 801CCC18 GearShopMenuConfirmationWindow
801CCD20 GearShopMenuFree | 801CCE90 GearShopMenuInitializeAuxTextGroup | 801CCEBC GearShopMenuClearByteFlags | 801CCEE8 GearShopMenuPositionSelectionString | 801CD310 GearShopMenuInitializeShopModeSelectionMenu | 801CD564 GearShopMenuInitializeSubmenu | 801CD838 GearShopMenuUpdateShopModeSelectionMenu | 801CDA0C GearShopMenuUpdateSubmenuTextures
801CDC68 GearShopMenuShopModeMenuHandleSelectedOption | 801CDD74 GearShopMenuShopModeMain | 801CE024 GearShopMenuMain | 801CE1D0 GearShopMenuShoulderButtonUiInitialize | 801CE2E8 GearShopMenuShoulderButtonUiFree | 801CE32C GearShopMenuRenderShopScreen | 801CE7E0 GearShopMenuRenderHelperModels | 801CE82C GearShopMenuRenderGearPreviewUi
801CEA68 GearShopMenuUpdateDualPreviewEffects | 801CEEA8 GearShopMenuUpdatePreviewEffect | 801CF184 GearShopMenuUpdatePreviewAnimation | 801CF33C GearShopMenuRenderPreviewEffects | 801CF38C GearShopMenuUploadGearName | 801CF448 GearShopMenuUpdateGearStatsGraphics | 801CF9BC GearShopMenuLoadGearModelResources | 801CFAB8 GearShopMenuInitializeGearPreviewModel
801CFC60 GearShopMenuStartGearPreviewTransition | 801CFC68 GearShopMenuStartGearPreviewTransitionWithLoadedX | 801CFF18 GearShopMenuInitializePreviewTransition | 801D0054 GearShopMenuSetDigitColor | 801D0220 GearShopMenuUpdateCharacterPortraits | 801D0348 GearShopMenuSetAvailableCharacterCount | 801D0398 GearShopMenuChangeCurrentGear | 801D04E8 GearShopMenuUpdateBuyExplanationGraphics
801D05EC GearShopMenuUpdateSellExplanationGraphics | 801D06D8 GearShopMenuUpdateTransactionGraphics | 801D0C20 GearShopMenuSetFinalPriceGraphics | 801D0D4C GearShopMenuUpdateCurrentGoldGraphics | 801D0EC8 GearShopMenuClearSellScreenState | 801D1078 GearShopMenuGetItemEquippedGearFlags | 801D1304 GearShopMenuUpdateSellItemPreview | 801D18F8 GearShopMenuUpdateSellItemListGraphics
801D1F20 GearShopMenuHandleSoldItems | 801D2054 GearShopMenuSellMenu | 801D2784 GearShopMenuSellWeaponsMenu | 801D27C4 GearShopMenuSellPartsMenu | 801D2804 GearShopMenuSellModeMenuHandleSelectedOption | 801D2950 GearShopMenuUpdateEquippedPartGraphics | 801D2B74 GearShopMenuUpdateBuyItemListGraphics | 801D3558 GearShopMenuComputeEquipmentStatChanges
801D3A3C GearShopMenuLookupByteByKey | 801D3A80 GearShopMenuUpdateItemQuantityGraphics | 801D3C78 GearShopMenuUpdateItemPreview | 801D44FC GearShopMenuHandleBoughtItems | 801D4888 GearShopMenuIsTuneUpUpgrade | 801D498C GearShopMenuBuyOrTuneUpMenu | 801D5398 GearShopMenuFuelTuneUpMenu | 801D573C GearShopMenuExitSubmenu
801D57A8 GearShopMenuDispatchTuneUpOption | 801D5828 GearShopMenuSubmenuMain | 801D5D38 GearShopMenuInitializePreviewResources | 801D5EB8 GearShopMenuFreePreviewResources | 801D5F94 GearShopMenuComputeDisplayStats | 801D6150 GearShopMenuRecomputeGearStats | 801D61B8 GearShopMenuApplyFrameStats | 801D6250 GearShopMenuApplyArmorStats
801D62A4 GearShopMenuApplyEngineStats | 801D6334 GearShopMenuApplyAccessoryStats | 801D6738 GearShopMenuApplyWeaponStats | 801D690C GearShopMenuComputeWeightPenalty
```
