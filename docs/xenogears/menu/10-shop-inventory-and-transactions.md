# Shop Inventory And Transactions

## 1. Runtime Contract

The ordinary character Shop is invoked by Field opcode `FE 59`. Its session
contract covers stock selection, Buy, Sell, equipment comparison, confirmation,
commit, cancellation, rendering state, and teardown. All monetary and inventory
writes occur at explicit commit points; browsing and comparison are temporary.

## 2. Field Request And Shop Index

`FieldScriptOpenShopMenu` at `0x800939A0` evaluates argument 1, stores its low
byte in resident menu state, selects module mode `4`, raises the menu request,
increments the request counter, advances the Field VM PC by three bytes, and
yields to the Field/menu coordinator.

The argument selects one `0x5C`-byte stock definition:

```text
shop_index = u8(FieldScriptVMGetArgument(1))
record     = shop_definition_base + shop_index * 0x5C
```

The Shop performs no index clamp. Field content must provide a valid index.

## 3. Stock Decoding

Initialization clears 48 item IDs and 48 type bytes, scans definition offsets
`0..0x59`, and compacts every nonzero byte:

```text
for source_offset in 0..89:
    item_id = record[source_offset]
    if item_id != 0:
        shopItemIDs[count]   = item_id
        shopItemTypes[count] = source_offset / 30
        count++
```

The three 30-byte source bands are:

| Runtime type | Class |
|---:|---|
| `0` | Weapon |
| `1` | Accessory |
| `2` | Item |

ID zero is the empty sentinel. The scan can encounter 90 nonzero bytes while
the compact destination holds 48 entries, so valid stock definitions must keep
the nonzero count at 48 or fewer.

## 4. Definition Records

All three definition records are `0x10` bytes. The Shop consumes these fields:

| Record | Offset | Width | Runtime role |
|---|---:|---:|---|
| Weapon | `+0x00` | `u16` | Character equip flags |
| Weapon | `+0x04` | `u16` | Buy price |
| Weapon | `+0x06` | `u8` | Weapon replacement family |
| Weapon | `+0x0C` | `u8` | Attack contribution |
| Accessory | `+0x00` | `u16` | Character equip flags |
| Accessory | `+0x02` | `u16` | Buy price |
| Accessory | `+0x08` | `u8` | Defense contribution |
| Accessory | `+0x0E` | `u16` | Accessory replacement family; zero means no family match |
| Item | `+0x02` | `u16` | Buy price |
| Item | `+0x06` | `u8` | Behavior flags; bit `0x10` excludes the item from Sell |

## 5. Persistent Inventory

| Category | ID array | Quantity array | Slots |
|---|---:|---:|---:|
| Weapons | `gameState+0x1D9C` | `gameState+0x1D38` | 100 |
| Accessories | `gameState+0x1EC8` | `gameState+0x1E00` | 200 |
| Items | `gameState+0x2026` | `gameState+0x1F90` | 150 |

IDs and quantities are parallel byte arrays. ID zero is empty. Quantity commits
cap each stack at 99. Gold is a `u32` at `gameState+0x1924`; Shop commits cap it
at 9,999,999.

## 6. Top-Level Menu

The top-level values run in reverse visual order:

| Internal value | Action |
|---:|---|
| `2` | Buy |
| `1` | Sell |
| `0` | Exit |

The initial value is `2`. Up increments and Down decrements with wrap over
`0..2`. Circle dispatches the selected action. Cross exits. Buy and Sell disable
the top-level controls until their modal flow returns. This dispatcher never
changes inventory or gold.

## 7. List, Cursor, And Scroll

Buy and Sell share an eight-row viewport:

```text
absolute_index = scroll_offset + row
row            = 0..7
```

Down advances the row; below row 7 it keeps row 7 and increments the offset up
to `item_count - 8`. Up reverses that process and stops at zero. The arrow uses
five animation frames and advances every six updates. Its Y coordinate is
`50 + row * 13`. For at most eight entries the scroll handle fills its track;
otherwise its position is proportional to `scroll_offset / (item_count - 8)`.
Empty rows have their render flags disabled.

## 8. Character Eligibility And Markers

Visible characters come from `(unlock_flags & FrMask) & 0x07FF`. Valid active
party IDs are retained separately, while Shop portraits represent every visible
character.

Weapon and Accessory equip flags are tested against each character bit.
`ShopMenuGetCharacterEquippedItemFlags` scans equipment and builds the portrait
mask used for `E` markers. Weapons below and above ID 50 use separate five-slot
banks; Accessories use three slots. Items have no equip comparison.

## 9. Character Stat Sheet

`ShopMenuComputeCharacterStats` writes nine `u16` fields in
`MenuDressingRoom+0xB8..+0xC8`:

| View offset | Display field | Formula | Clamp |
|---:|---|---|---:|
| `+0xB8` | Attack | `character[0x04] + character[0x58] + character[0x28]` | 999 |
| `+0xBA` | Hit | `character[0x5E] + character[0x2E]` | 99 |
| `+0xBC` | Defense | `character[0x2D] + character[0x59] + character[0x29]` | 999 |
| `+0xBE` | Evade | `character[0x5F] + character[0x2F]` | 99 |
| `+0xC0` | Ether | `character[0x5B] + character[0x2B]` | 999 |
| `+0xC2` | Fixed Shop-sheet value | `99` | 99 |
| `+0xC4` | Ether Defense | `character[0x5C] + character[0x2C]` | 999 |
| `+0xC6` | Fixed Shop-sheet value | `10` | 99 |
| `+0xC8` | Agility | `character[0x5A] + character[0x2A]` | 99 |

The function contains a character-4 Attack assignment before the common Attack
assignment; the common assignment immediately replaces it. Runtime output
therefore always uses the formula in the table.

Buy snapshots only `+0xB8` and `+0xBC` for each of the eleven character slots.
Those cached values are the two comparison columns: Attack and Defense.

## 10. Weapon And Accessory Deltas

`ShopMenuComputeEquipmentStatChanges` returns two absolute deltas and two color
directions. Weapon candidates affect only Attack. Accessory candidates affect
only Defense. A larger candidate total selects the increase color; an equal or
smaller total selects the decrease color, with a zero delta left hidden by the
render path.

For a normal character, Weapon comparison is:

```text
old_attack = character[0x58] + current_weapon[0x0C]
new_attack = character[0x58] + candidate_weapon[0x0C]
```

Character 4 sums five weapon slots. Every equipped weapon whose `+0x06` family
matches the candidate family is replaced in the simulated sum, except family
`5`, which never matches for this operation.

Accessory comparison starts with `character[0x59]`, adds the three equipped
Accessory `+0x08` Defense contributions, and simulates replacement as follows:

1. Replace every slot whose nonzero `+0x0E` family equals the candidate family.
2. If no family matches, replace the slot with the smallest Defense contribution.
3. On equal minima, the later slot wins because the comparison uses `<=`.

The simulation never writes persistent equipment.

## 11. Buy Availability And Quantity

Each visible Buy row is available when:

```text
unit_price <= staged_resulting_gold
```

The staged balance includes every quantity already selected. Right increments
only an available row satisfying:

```text
owned_quantity + staged_quantity + 1 < 100
```

Accepted edits use exact integer arithmetic:

```text
Right: staged_quantity++; total += unit_price; resulting_gold -= unit_price
Left:  staged_quantity--; total -= unit_price; resulting_gold += unit_price
```

Persistent inventory and gold remain unchanged during editing.

## 12. Buy Detail And Capacity Behavior

Selection refreshes description, price, compatibility, `E` markers, Attack or
Defense delta, and owned quantity. It neither reserves an inventory slot nor
equips the candidate.

At commit, every inventory slot whose ID matches the purchased ID receives the
staged quantity and is capped independently at 99. Only when no slot matches
does the commit use the first empty slot. If no matching or empty slot exists,
no item is inserted and the already-assigned staged gold remains committed.

## 13. Confirmation Text Contract

`ShopMenuConfirmationWindowInitialize(stringIndex)` renders three consecutive
text rows beginning at `stringIndex`. Manual confirmation defaults to No. Left
selects Yes, Right selects No, Circle accepts, and Cross returns No.

| Start index | Rows |
|---:|---|
| `0x8C` | `Cancel all purchases?`, blank row, `Yes          No` |
| `0x8F` | Total-price row, `Is that okay?`, `Yes          No` |
| `0x92` | `Cancel all sales?`, blank row, `Yes          No` |
| `0x95` | Total-price row, `Is that okay?`, `Yes          No` |

Auto-advance mode hides both choices and exits on input or after 60 updates.
Ordinary Buy and Sell use manual mode and no second prompt.

## 14. Buy Commit And Cancellation

Circle with total zero plays the invalid-action sound. Circle with a nonzero
total displays group `0x8F`. Yes calls `ShopMenuHandleBoughtItems` at
`0x801CF2A0`:

1. Play the transaction sound.
2. Store staged resulting gold, capped at 9,999,999.
3. Add each staged quantity to every matching stack; if no match exists, install
   it in the first empty slot.
4. Cap every affected quantity at 99.

No resumes the same staged transaction. No item is equipped automatically.

Cross with total zero exits Buy. Cross with a nonzero total uses group `0x8C`.
No resumes with all quantities intact; Yes exits and discards staging.

## 15. Sell Dispatch And Excluded Items

Sell begins on Items and wraps across four choices:

| Choice | Source passed to generic Sell |
|---|---|
| Equipment | Three Accessory slots from one selected character, each with working quantity 1 |
| Accessories | 200-slot Accessory inventory |
| Weapons | 100-slot Weapon inventory |
| Items | 150-slot Item inventory |

The Equipment path does not enumerate equipped weapons. Item records with the
Sell-exclusion flag `0x10` at `+0x06` are omitted.

## 16. Sell Arithmetic

The Sell loop compacts entries having both nonzero ID and nonzero quantity into
local arrays. Unit proceeds are:

```text
sale_value = u16(buy_price) >> 1
```

Right transfers one unit from remaining quantity to staged sale and adds the
sale value to transaction total and staged gold. Left reverses that transfer.
Persistent arrays stay unchanged until confirmation.

## 17. Sell Commit And Cancellation

Circle with total zero is rejected. A nonzero total uses group `0x95`. Yes calls
`ShopMenuHandleSoldItems` at `0x801D0C18`:

1. Store staged gold, capped at 9,999,999.
2. Subtract each selected count from every matching persistent inventory slot,
   or clear the selected Accessory slot in Equipment mode.
3. Clear an inventory ID when its quantity becomes zero.

Inventory subtraction is byte-sized. With duplicate IDs, each staged count is
applied to every matching persistent slot; only an exact zero clears the ID, so
an underflowed quantity wraps and remains nonzero.

No resumes the exact staged sale. Cross with staged units uses group `0x92`;
No resumes and Yes discards. Cross with zero total exits immediately.

## 18. Commit Boundaries And Invariants

| Operation | Persistent write point |
|---|---|
| Buy | `ShopMenuHandleBoughtItems`, `0x801CF2A0` |
| Sell | `ShopMenuHandleSoldItems`, `0x801D0C18` |

Browsing, scrolling, quantity edits, detail refresh, compatibility checks,
portrait markers, and stat comparisons are nonpersistent. No and Cross never
commit. Sale value is always half the unsigned buy price. Buy affordability
always uses staged resulting gold. Inventory commits do not assume ID
uniqueness: duplicate IDs are all changed by Buy and Sell.

## 19. Cleanup And Ownership

`ShopMenuClearSellScreenState` disables detail, portrait, row, explanation,
price, and stat render groups. Full cleanup also frees windows 2, 3, and 5 when
present, the scroll handle, and arrow cursor 0.

`ShopMenuFreeTransactionScreen` at `0x801D1F10` performs common post-dispatch
cleanup for Buy. Sell category handlers already release their transaction
screens, preventing duplicate release.

Final teardown flushes two updates, disables drawing, drains render parity, and
then releases manager blocks, definition data, description data, presentation
resources, optional sound state, and the root menu object. Render parity must
reach zero before packet-backed allocations are released.

## 20. Rendering Boundary

This chapter owns logical rows, cursor and scroll state, comparison flags,
confirmation state, transaction staging, and lifetime ordering. Primitive
layout, text upload, projection, ordering-table submission, palettes, and host
renderer interpretation belong to the `graphics` chapters.

## 21. Function Index

| Address | Function |
|---:|---|
| `0x801C5040` | `ShopMenuSetFt4Rect` |
| `0x801C50B0` | `ShopMenuIsCharacterFlagSet` |
| `0x801C50CC` | `ShopMenuGetCharacterBitMask` |
| `0x801C50E8` | `ShopMenuParseNumberToString` |
| `0x801C5194` | `ShopMenuManageResourceState` |
| `0x801C51F8` | `ShopMenuSetManager` |
| `0x801C525C` | `ShopMenuSelectionMenuManager` |
| `0x801C52C0` | `ShopMenuManageTextBatchState` |
| `0x801C5324` | `ShopMenuDressingRoomManager` |
| `0x801C5388` | `ShopMenuManageOverlayPacketState` |
| `0x801C53EC` | `ShopMenuManageExtendedDisplayState` |
| `0x801C5450` | `ShopMenuShopManager` |
| `0x801C54B4` | `ShopMenuLoadResources` |
| `0x801C58F4` | `ShopMenuInitialize` |
| `0x801C5A6C` | `ShopMenuResetRenderContext` |
| `0x801C5A7C` | `ShopMenuInitializeStringFt4Pair` |
| `0x801C5CBC` | `ShopMenuInitializeMenuStringGroup` |
| `0x801C5E6C` | `ShopMenuUploadSmallVramImage` |
| `0x801C5EE8` | `ShopMenuInitializeTextResources` |
| `0x801C5F44` | `ShopMenuInitializeWindowBorders` |
| `0x801C604C` | `ShopMenuMovePointerCursor` |
| `0x801C6430` | `ShopMenuClearSelectionFlags` |
| `0x801C6460` | `ShopMenuInitializeShadedQuad` |
| `0x801C64DC` | `ShopMenuInitializeBackgrounds` |
| `0x801C6828` | `ShopMenuLoadShopItemsData` |
| `0x801C6A6C` | `ShopMenuInitializeShopData` |
| `0x801C6E90` | `ShopMenuSetVertices` |
| `0x801C6EE8` | `ShopMenuSetWindowBorderPrimitive` |
| `0x801C6F30` | `ShopMenuUpdateScrollBarHandle` |
| `0x801C70B8` | `ShopMenuFreeScrollBarHandle` |
| `0x801C70FC` | `ShopMenuInitializeArrowCursor` |
| `0x801C7178` | `ShopMenuUpdateArrowCursor` |
| `0x801C7314` | `ShopMenuFreeArrowCursor` |
| `0x801C7370` | `ShopMenuInitializeWindowGraphics` |
| `0x801C768C` | `ShopMenuInitializeScrollBar` |
| `0x801C77F0` | `ShopMenuInitializeWindowBorderCorners` |
| `0x801C7A38` | `ShopMenuSetWindowBorderTop` |
| `0x801C7D7C` | `ShopMenuSetWindowBorderBottom` |
| `0x801C80C8` | `ShopMenuSetWindowBorderLeft` |
| `0x801C8410` | `ShopMenuSetWindowBorderRight` |
| `0x801C875C` | `ShopMenuSetWindow` |
| `0x801C88E0` | `ShopMenuFreeWindow` |
| `0x801C896C` | `ShopMenuInitializeWindow` |
| `0x801C8AF0` | `ShopMenuUpdateWindows` |
| `0x801C8C3C` | `ShopMenuRenderPolygons` |
| `0x801C8D58` | `ShopMenuRenderString` |
| `0x801C8DDC` | `ShopMenuRenderScrollBarHandle` |
| `0x801C8E28` | `ShopMenuRenderSelectionPointer` |
| `0x801C8EB8` | `ShopMenuRenderTopWindowBorder` |
| `0x801C908C` | `ShopMenuRenderBottomWindowBorder` |
| `0x801C9260` | `ShopMenuRenderLeftWindowBorder` |
| `0x801C9434` | `ShopMenuRenderRightWindowBorder` |
| `0x801C9608` | `ShopMenuRenderWindowBackground` |
| `0x801C9744` | `ShopMenuRenderWindowBorderCorners` |
| `0x801C9890` | `ShopMenuRenderScrollBar` |
| `0x801C9AB4` | `ShopMenuRenderWindows` |
| `0x801C9C2C` | `ShopMenuRenderPointerCursors` |
| `0x801C9F7C` | `ShopMenuRenderAuxGroup4` |
| `0x801CA00C` | `ShopMenuRenderAuxGroup8` |
| `0x801CA09C` | `ShopMenuRenderAuxQuads` |
| `0x801CA214` | `ShopMenuNoOpCountdown` |
| `0x801CA22C` | `ShopMenuRenderConfirmationText` |
| `0x801CA388` | `ShopMenuRenderBackgroundDim` |
| `0x801CA404` | `ShopMenuRenderAuxiliaryGroups` |
| `0x801CA444` | `ShopMenuRenderSelectionMenu` |
| `0x801CAB0C` | `ShopMenuRenderAuxTextGroups` |
| `0x801CAB80` | `ShopMenuRenderArrowCursors` |
| `0x801CABF4` | `ShopMenuRender` |
| `0x801CAC7C` | `ShopMenuPlaySoundEffect` |
| `0x801CACC8` | `ShopMenuPollInput` |
| `0x801CAED4` | `ShopMenuUpdateTransitionEffect` |
| `0x801CB014` | `ShopMenuUpdateAndRender` |
| `0x801CB13C` | `ShopMenuInitializePointerCursors` |
| `0x801CB2FC` | `ShopMenuFreePointerCursors` |
| `0x801CB340` | `ShopMenuStartOpenMenuTransition` |
| `0x801CB370` | `ShopMenuStartCloseMenuTransition` |
| `0x801CB384` | `ShopMenuConfirmationWindowInitialize` |
| `0x801CB7F4` | `ShopMenuConfirmationWindowFree` |
| `0x801CB894` | `ShopMenuConfirmationWindowGetChoice` |
| `0x801CBA50` | `ShopMenuConfirmationWindow` |
| `0x801CBB08` | `ShopMenuFree` |
| `0x801CBC88` | `ShopMenuConfigureAuxTextGroup` |
| `0x801CBCF0` | `ShopMenuPositionSelectionString` |
| `0x801CC024` | `ShopMenuInitializeShopModeSelectionMenu` |
| `0x801CC278` | `ShopMenuInitializeSubmenu` |
| `0x801CC54C` | `ShopMenuUpdateShopModeSelectionMenu` |
| `0x801CC720` | `ShopMenuUpdateSubmenuTextures` |
| `0x801CC97C` | `ShopMenuShopModeMenuHandleSelectedOption` |
| `0x801CCAD8` | `ShopMenuShopModeMenuMain` |
| `0x801CCD28` | `ShopMenuMain` |
| `0x801CCE1C` | `ShopMenuComputeCharacterStats` |
| `0x801CCFF4` | `ShopMenuRenderShopGraphics` |
| `0x801CD404` | `ShopMenuSetStatChangeColor` |
| `0x801CD5D0` | `ShopMenuUpdateCharacterPortraits` |
| `0x801CD6F8` | `ShopMenuUpdateBuyMenuExplanationGraphics` |
| `0x801CD7E4` | `ShopMenuUpdateSellMenuExplanationGraphics` |
| `0x801CD8D0` | `ShopMenuUpdateGoldGraphics` |
| `0x801CDBA0` | `ShopMenuGetCharacterEquippedItemFlags` |
| `0x801CDD14` | `ShopMenuUpdateBuyItemListGraphics` |
| `0x801CE480` | `ShopMenuComputeEquipmentStatChanges` |
| `0x801CE8D8` | `ShopMenuLookupByteByKey` |
| `0x801CE91C` | `ShopMenuUpdateOwnedItemQuantityGraphics` |
| `0x801CEB3C` | `ShopMenuUpdateBuyItemDetails` |
| `0x801CF2A0` | `ShopMenuHandleBoughtItems` |
| `0x801CF678` | `ShopMenuSetFinalPriceGraphics` |
| `0x801CF780` | `ShopMenuBuyMenu` |
| `0x801CFF58` | `ShopMenuUpdateSellItemDetails` |
| `0x801D05BC` | `ShopMenuUpdateSellItemListGraphics` |
| `0x801D0C18` | `ShopMenuHandleSoldItems` |
| `0x801D0E68` | `ShopMenuSellMenu` |
| `0x801D1658` | `ShopMenuSellEquipmentMenu` |
| `0x801D18A8` | `ShopMenuSellAccessoriesMenu` |
| `0x801D18E8` | `ShopMenuSellWeaponsMenu` |
| `0x801D1928` | `ShopMenuSellItemsMenu` |
| `0x801D1968` | `ShopMenuClearSellScreenState` |
| `0x801D1B18` | `ShopMenuSellModeMenuHandleSelectedOption` |
| `0x801D1CA4` | `ShopMenuSellModeMenu` |
| `0x801D1F10` | `ShopMenuFreeTransactionScreen` |
