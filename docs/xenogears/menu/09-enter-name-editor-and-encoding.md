# Enter Name Editor And Encoding

## 1. Scope

Enter Name edits one selected character or Gear name. It keeps an internal
16-bit edit buffer, converts that buffer to display bytes, and writes the
accepted display string to three mapped 20-byte name records.

Shared contracts are documented in:

- [Resource And String Formats](03-resource-and-string-formats.md)
- [Runtime Objects, Allocations, And Ownership](04-runtime-objects-allocations-and-ownership.md)
- [Frame, Input, Navigation, And Shared UI](05-frame-input-navigation-and-shared-ui.md)
- [Persistence, Return, Teardown, And Function Map](12-persistence-return-teardown-and-function-map.md)
- [Graphics Resource Formats](../graphics/03-resource-formats.md)

## 2. Selector And Target Records

The resident controller supplies selector byte `DAT_80059171`. Selectors
`0..10` use a nine-code limit and select character resources directly.
Selectors `11..30` use a ten-code limit and obtain a portrait/resource ID from
the selector map.

The complete selector contract is:

| Selector | Initial record | Code limit | Portrait/resource ID | Commit destinations |
|---:|---|---:|---:|---|
| `0` | Fei | 9 | 0 | `0, 0, 0` |
| `1` | Elly | 9 | 1 | `1, 1, 1` |
| `2` | Citan | 9 | 2 | `2, 2, 9` |
| `3` | Bart | 9 | 3 | `3, 3, 3` |
| `4` | Billy | 9 | 4 | `4, 4, 4` |
| `5` | Rico | 9 | 5 | `5, 5, 5` |
| `6` | Emeralda | 9 | 6 | `6, 6, 6` |
| `7` | Chu-Chu | 9 | 7 | `7, 18, 18` |
| `8` | Maria | 9 | 8 | `8, 8, 8` |
| `9` | Citan alternate form | 9 | 9 | `9, 9, 9` |
| `10` | Emeralda alternate form | 9 | 10 | `10, 10, 10` |
| `11` | Weltall | 10 | 0 | `11, 11, 12` |
| `12` | Weltall-2 | 10 | 0 | `12, 12, 12` |
| `13` | Vierge | 10 | 1 | `13, 13, 13` |
| `14` | Heimdal | 10 | 2 | `14, 14, 27` |
| `15` | Brigandier | 10 | 3 | `15, 15, 15` |
| `16` | Renmazuo | 10 | 4 | `16, 16, 16` |
| `17` | Stier | 10 | 5 | `17, 17, 17` |
| `18` | Chu-chu Gear record | 10 | 7 | `18, 18, 18` |
| `19` | Seibzehn | 10 | 8 | `19, 19, 19` |
| `20` | Crescens | 10 | 6 or 10 | `20, 20, 20` |
| `21` | Regurus | 10 | 1 | `21, 21, 21` |
| `22` | Fenrir | 10 | 2 | `22, 22, 22` |
| `23` | Andvari | 10 | 3 | `23, 23, 23` |
| `24` | Renmazuo alternate form | 10 | 4 | `24, 24, 24` |
| `25` | Stier alternate form | 10 | 5 | `25, 25, 25` |
| `26` | Xenogears | 10 | 0 | `26, 26, 26` |
| `27` | Heimdal alternate form | 10 | 2 | `27, 27, 27` |
| `28` | BARTHOS | 10 | 7 | `28, 28, 28` |
| `29` | Yggdra | 10 | 0 | `29, 29, 29` |
| `30` | Empty selector record | 10 | 2 | `0, 0, 0` |

Selector 20 normally uses resource ID 6. When persistent flag `0x0400` is set,
it uses resource ID 10 instead. This resource substitution does not change name
record 20 or its commit destinations.

The first displayed string always comes from `name_records[selector]`. The three
destination bytes are loaded from:

```text
selector_map + 0x20 + selector * 3
```

All three bytes are record indices, and all three receive the same accepted
display string. Repeated indices intentionally repeat an idempotent clear/write
operation. The linked mappings synchronize names across related forms:

- Selector 2 synchronizes Citan records 2 and 9.
- Selector 7 synchronizes character record 7 and Gear record 18.
- Selector 11 synchronizes Weltall records 11 and 12.
- Selector 14 synchronizes Heimdal records 14 and 27.
- Selector 30 redirects all three writes to Fei record 0.

## 3. Complete Lifecycle

`EnterNameMenuMain` at `0x801CBDBC` performs:

1. Allocate and clear seven top-level owners.
2. Configure `MoveImage` as `(0x2C0,0x100,0x140,0x0E0)`.
3. Build character availability and a filtered three-slot party copy used for
   portrait resources.
4. Load menu graphics, strings, selector data, portrait data, and sound.
5. Initialize shared text packets, background packets, border packets, and the
   name-entry display state.
6. Allocate two 24-byte local buffers and copy the selected persistent display
   name into the display buffer.
7. Open window 3 and the projection transition, then create window 2.
8. Enable keyboard, name, caret, and cursor rendering.
9. Process navigation and edits until Start or code `0x1F` accepts a nonempty
   encoded first unit.
10. Write the accepted display bytes to all three mapped records.
11. Disable cursors and editor graphics, close windows 2 and 3, and complete the
    projection transition.
12. Drain packet parity and release all retained ownership, ending with
    `SystemMenu`.

The resident controller allocates `SystemMenu`; the overlay frees it after all
children.

## 4. Managers And Ownership

### 4.1 Functional managers

| `SystemMenu` owner | Size | Role |
|---:|---:|---|
| `+0x32C` | `0x5034` | Loaded texture, selector, and portrait-resource state |
| `+0x33C` | `0x006C` | Render enables and three portrait/resource IDs |
| `+0x350` | `0x1194` | `MoveImage` rectangle |
| `+0x348` | `0x015C` | Overlay GPU packets and dim layer |
| `+0x1E20` | `0x0DEC` | Complete keyboard, name, selection, and caret display state |

The `+0x354` and `+0x330` managers are cleared lifecycle reservations. No Enter
Name operation reads their contents, so they contribute no editor state or
rendering behavior.

### 4.2 Display state

| Offset | Size | Use |
|---:|---:|---|
| `+0x000` | `0x0A0` | Four UI FT4 packets |
| `+0x0A0` | `0x0A0` | Four current-name FT4 packets |
| `+0x140` | `0x050` | Two projected-header FT4 packets |
| `+0x190` | `0x0B40` | 72 keyboard FT4 packets, two parities for 36 strips |
| `+0xCD0` | `0x050` | Two selection FT4 packets |
| `+0xD20` | `0x030` | Two top LINE_F3 packets |
| `+0xD50` | `0x030` | Two bottom LINE_F3 packets |
| `+0xD80` | `0x020` | Two blinking-caret LINE_F2 packets |
| `+0xDA0` | `0x020` | Projected-header source quad |
| `+0xDC0` | `0x020` | Selection source quad |
| `+0xDE0..+0xDE4` | 5 | UI, header, keyboard, selection, and line parity bytes |
| `+0xDE5` | 1 | Header/selection enable |
| `+0xDE6` | 1 | Keyboard/name/caret enable |
| `+0xDE7` | 1 | Caret blink counter, cycling `0..60` |
| `+0xDE8` | 1 | Current encoded length and caret position |
| `+0xDE9` | 1 | Maximum encoded length, 9 or 10 |
| `+0xDEA` | 1 | Current-name packet count |

### 4.3 Dynamic owners

| Owner | Size | Lifetime |
|---|---:|---|
| Pointer cursor state | `0x14C` | Window 2 open through accepted commit |
| Four prompt states | `4 * 0x80` | Screen setup through closing transition |
| Windows 2 and 3 | `2 * 0x720` | Their respective open/close intervals |
| Window animations | `2 * 0x18` | Their respective open/close intervals |
| Prompt rasters | `2 * 0x5CA` | Upload and immediate release |
| Keyboard raster | `0x2BE` | Build/upload all 36 strips, then release |
| Current-name raster | `0x3F6` | Allocate/upload/release on each redraw |
| Shared text raster | `0x38E` | Initialization through final teardown |

## 5. Static Keyboard Layout

The backing table contains 216 bytes arranged as 36 six-byte groups. Keyboard
initialization converts the first five codes from each group, rasterizes 36
strips, and creates two parity packets per strip. The sixth byte supplies spacing
or the right-edge marker.

The selectable keyboard is five rows by 23 columns. Its exact lookup is:

```text
bank         = floor(column / 6)
table_offset = row * 6 + bank * 48 + column

cursor_x = 72 + column * 8
cursor_y = 54 + row * 16
```

The selectable codes are:

| Row | Columns `0..5` | Columns `6..11` | Columns `12..17` | Columns `18..22` |
|---:|---|---|---|---|
| 0 | `20 21 22 23 24 0F` | `25 26 27 28 29 0F` | `2A 2B 2C 2D 2E 0F` | `2F 30 31 32 33` |
| 1 | `34 35 36 37 38 0F` | `39 0F 0F 0F 0F 0F` | `10 11 12 13 14 0F` | `15 16 17 18 19` |
| 2 | `3D 3E 3F 40 41 0F` | `42 43 44 45 46 0F` | `47 48 49 4A 4B 0F` | `4C 4D 4E 4F 50` |
| 3 | `51 52 53 54 55 0F` | `56 0F CF CF CF CF` | `57 58 59 5A 5B 0F` | `5C 5D 5E 5F 60` |
| 4 | `3A 3B 3C 61 62 0F` | `63 64 65 CF CF CF` | `CF CF CF CF CF 0F` | `CF CF CF CF 1F` |

The initial cursor is `(22,4)`, directly on `0x1F`.

The principal encoded ranges are:

| Codes | Meaning |
|---|---|
| `0x10..0x19` | Digits `0..9` |
| `0x1A` | Hyphen |
| `0x20..0x39` | Uppercase `A..Z` |
| `0x3D..0x56` | Lowercase `a..z` |
| `0x3A..0x3C`, `0x57..0x65` | Punctuation and symbols |

## 6. Structural Codes And Exact Actions

| Code | Editor action | Display conversion |
|---:|---|---|
| `0x0F` | Empty/padding cell; skipped by navigation; unit zero blocks acceptance | Emits NUL |
| `0x1C` | Initial edit-buffer filler | Emits NUL |
| `0x1F` | Acceptance key and encoded-length terminator | Emits NUL |
| `0x4F` | Encoded lowercase `s`; commit treats trailing display `0x4F` bytes as trim bytes | Emits `0x4F` |
| `0xC3` | Blank code installed when a selectable `0x0F`/`0xCF` is confirmed | Emits display byte `0x10` |
| `0xCF` | Disabled keyboard cell; skipped by navigation | Emits NUL |
| `0xFF` | Right-edge marker used by Right navigation | Not appended |

Navigation skips one `0x0F` or `0xCF` candidate but does not repeatedly scan a
run. A cursor can consequently land on a later disabled cell. Confirming a cell
whose current table byte is `0x0F` or `0xCF` replaces that byte in the static
table with `0xC3`, then appends `0xC3`. The replacement persists for the rest of
the overlay session and produces a blank display character.

`0x1F` has two independent roles: the visible cell requests acceptance, while
the same value terminates the encoded-length scan.

## 7. Encoded And Display Buffers

The main loop owns two 24-byte local buffers:

| Buffer | Representation | Purpose |
|---|---|---|
| Encoded | Ten little-endian 16-bit code units | Editing and acceptance test |
| Display | One- or two-byte glyph codes followed by NUL | Rasterization and persistent commit |

Initialization performs these operations independently:

1. Fill all ten encoded units with `0x001C`.
2. Replace encoded unit 9 with `0x001F`.
3. Copy all 20 bytes from `name_records[selector]` into the display buffer.
4. Leave display-state length `+0xDE8` at zero from manager clearing.

The saved display name is not decoded into the edit buffer. The opening screen
shows the saved name, but the first appended code writes encoded unit zero and
starts a replacement name. Cross at length zero writes `0x000F` to unit zero,
converts the encoded buffer, and clears the initially displayed name.

`EnterNameMenuGetEncodedNameLength` scans at most ten units and stops only at
exact unit `0x001F`. A full ten-code Gear name can occupy all units without an
in-range terminator.

The resident converter indexes a two-byte mapping entry for every encoded unit.
If entry byte zero is zero, it emits only byte one; otherwise it emits both
bytes. It appends NUL after the requested unit count. The 9/10 limit counts
encoded units, while each persistent name record holds 20 display bytes.

## 8. Navigation And Editing

| Command | Exact behavior |
|---:|---|
| Right `0` | Test column `+1`; `0xFF` wraps to 0; one `0x0F`/`0xCF` advances to column `+2` |
| Down `1` | Increment row and wrap `4 -> 0`; `0xCF` selects row 0; one `0x0F` advances one more row |
| Left `2` | Test column `-1`; underflow wraps to 22; one `0x0F`/`0xCF` retreats to column `-2` |
| Up `3` | Decrement row and wrap `0 -> 4`; one `0x0F`/`0xCF` retreats one more row |
| Circle `4` | Append the selected code, or request acceptance on `0x1F` |
| Cross `5` | Move the caret left when possible, write `0x000F`, convert, and redraw |
| Square `6` | No editor action |
| Triangle `7` | No editor action |
| None `8` | No editor action |
| R1 `9` | Increment caret/length up to the configured maximum; do not alter either buffer |
| L1 `10` | Decrement caret/length when nonzero; do not alter either buffer |
| Start `11` | Request acceptance at the current cursor location |
| Select `12` | Toggle a diagnostic byte |

L2 increments another diagnostic byte without changing the name.

Append succeeds only while current length is below the selector limit. It writes
the selected code and zero high byte at the current unit, scans for `0x001F`,
converts that many units, increments current length, marks the name texture for
redraw, and plays sound 2. At the limit it plays sound 4 and changes nothing.

Cross plays sound 3 in the input translator. Editor processing then decrements
length if nonzero, writes `0x000F` at the resulting unit, converts through the
existing `0x001F` terminator, and schedules a redraw.

R1 and L1 alter only `+0xDE8`. Their branches perform no encoded write,
conversion, or texture update.

## 9. Name Texture And Caret

When an edit marks the display dirty,
`EnterNameMenuUpdateEnteredNameTexture`:

1. Allocates and clears `0x3F6` bytes.
2. Rasterizes the display string with width argument `0x24`.
3. Uploads a `40x13` rectangle to VRAM `(0x180,0x0EA)`.
4. Waits for GPU completion.
5. Frees the raster.

The caret uses `x = 0x50 + current_length * 8`. It is visible for 30 values of a
61-value blink cycle. The selection field width is `maximum_length * 8`, so both
caret and field measure encoded units rather than display-byte width.

## 10. Acceptance And Three-Record Commit

Start and the `0x1F` cell share the acceptance branch. Acceptance succeeds when
the low byte of encoded unit zero is not `0x0F`.

Because initialization puts `0x001C` in unit zero, immediate acceptance commits
the original display buffer even though current edit length is zero. Once Cross
writes `0x000F` at unit zero, acceptance remains blocked until another code is
appended there.

For each mapped destination, in table order, commit performs:

1. Clear all 20 destination bytes.
2. Copy display bytes through NUL, with an upper bound of 18 copied nonterminal
   positions.
3. Track the last copied byte whose raw value is not `0x4F`.
4. Clear every byte after that tracked position through byte 19.

Thus trailing display bytes `0x4F` are removed. In the editor alphabet,
encoded lowercase `s` converts to display `0x4F`; a terminal run of lowercase
`s` characters is consequently trimmed during acceptance. The tracker starts at
zero, so a string made entirely of `0x4F` retains byte zero and clears bytes
`1..19`.

Repeated destination indices run the same clear/write sequence again and leave
the same final bytes. No persistent name record changes before acceptance.

## 11. No-Cancel Contract

The editor has no modal cancel return:

- Triangle has no editor action.
- Cross is backspace, including at length zero.
- Start requests commit.
- Selecting `0x1F` requests commit.
- The loop exits only after the encoded-unit-zero test accepts.

All three mapped records remain intact while editing and change together at the
single acceptance point.

## 12. Windows And Transitions

Screen setup creates window 3 at `(16,154,192,60)`, initializes prompt, header,
name field, and keyboard graphics, then enables display composition. The open
transition plays sound `0x5B` and converges projection depth to `0x200`.

After that transition completes, window 2 opens at `(56,38,208,96)`. Once its
animation flag is set, the loop enables keyboard/name/caret rendering and the
pointer cursor.

After acceptance, the loop:

1. Clears the two auxiliary enables used by the editor.
2. Disables, flushes, and frees pointer cursors.
3. Disables keyboard/name/caret rendering.
4. Frees window 2 and starts the close transition with sound `0x5C`.
5. Renders until projection depth reaches `0x600`.
6. Disables the complete name-entry display group.
7. Frees all four prompt states and window 3.

## 13. Frame Submission And Teardown

Each frame polls input, checks soft reset, flips environment and packet parity,
clears a 16-entry reverse ordering table, updates projection state, composes the
menu, synchronizes GPU/VBlank work, installs drawing/display environments,
performs `MoveImage`, and submits the ordering table.

`EnterNameMenuFree` renders twice, disables top-level composition, renders until
parity is zero, frees the functional and lifecycle-reservation managers, frees
retained resource products and the shared text raster, releases debug-owned
sound data when enabled, frees the display state, and frees `SystemMenu` last.
Keyboard, prompt, and current-name raster allocations were already released
after their uploads.

## 14. Behavioral Contract

- Selectors `0..10` admit nine encoded units; selectors `11..30` admit ten.
- The opening display name and encoded edit buffer are independent.
- The selectable keyboard is exactly 5 rows by 23 columns.
- `0x0F` marks an empty first unit, `0x1F` accepts and terminates scans,
  `0xCF` marks disabled cells, and `0xC3` supplies a blank replacement.
- Start and `0x1F` are the only completion commands.
- Acceptance rewrites exactly three mapped 20-byte records.
- Linked selectors propagate names to alternate character or Gear forms.
- The editor cannot return without acceptance.

## 15. Complete Logical Function Index

| Address | Logical function | Address | Logical function |
|---:|---|---:|---|
| `0x801C5040` | `EnterNameMenuIsCharacterFlagSet` | `0x801C505C` | `EnterNameMenuManageResourceState` |
| `0x801C50C0` | `EnterNameMenuManageFlagsState` | `0x801C5124` | `EnterNameMenuManageMoveImageState` |
| `0x801C5188` | `EnterNameMenuManageReservedLargeState` | `0x801C51EC` | `EnterNameMenuManageReservedSmallState` |
| `0x801C5250` | `EnterNameMenuManageOverlayGpuState` | `0x801C52B4` | `EnterNameMenuManageDisplayState` |
| `0x801C5318` | `EnterNameMenuLoadResources` | `0x801C58B8` | `EnterNameMenuInitialize` |
| `0x801C5A30` | `EnterNameMenuResetRenderContext` | `0x801C5A40` | `EnterNameMenuUploadSmallVramImage` |
| `0x801C5ABC` | `EnterNameMenuInitializeStringFt4Pair` | `0x801C5CFC` | `EnterNameMenuInitializeTextEntryPairs` |
| `0x801C5EAC` | `EnterNameMenuInitializeTextResources` | `0x801C5F08` | `EnterNameMenuInitializeWindowBorders` |
| `0x801C6010` | `EnterNameMenuClearManagerFlags` | `0x801C6040` | `EnterNameMenuInitializeShadedQuad` |
| `0x801C60BC` | `EnterNameMenuInitializeBackgrounds` | `0x801C6408` | `EnterNameMenuSetVertices` |
| `0x801C6460` | `EnterNameMenuSetWindowBorderPrimitive` | `0x801C64A8` | `EnterNameMenuInitializeWindowGraphics` |
| `0x801C67C4` | `EnterNameMenuInitializeScrollBar` | `0x801C6928` | `EnterNameMenuInitializeWindowBorderCorners` |
| `0x801C6B70` | `EnterNameMenuSetWindowBorderTop` | `0x801C6EB4` | `EnterNameMenuSetWindowBorderBottom` |
| `0x801C7200` | `EnterNameMenuSetWindowBorderLeft` | `0x801C7548` | `EnterNameMenuSetWindowBorderRight` |
| `0x801C7894` | `EnterNameMenuSetWindow` | `0x801C7A18` | `EnterNameMenuFreeWindow` |
| `0x801C7AA4` | `EnterNameMenuInitializeWindow` | `0x801C7C28` | `EnterNameMenuUpdateWindows` |
| `0x801C7D74` | `EnterNameMenuRenderTopWindowBorder` | `0x801C7F48` | `EnterNameMenuRenderBottomWindowBorder` |
| `0x801C811C` | `EnterNameMenuRenderLeftWindowBorder` | `0x801C82F0` | `EnterNameMenuRenderRightWindowBorder` |
| `0x801C84C4` | `EnterNameMenuRenderWindowBackground` | `0x801C8600` | `EnterNameMenuRenderWindowBorderCorners` |
| `0x801C874C` | `EnterNameMenuRenderScrollBar` | `0x801C8970` | `EnterNameMenuRenderWindows` |
| `0x801C8AE8` | `EnterNameMenuRenderPointerCursors` | `0x801C8E38` | `EnterNameMenuRenderAuxGroup4` |
| `0x801C8EC8` | `EnterNameMenuRenderAuxGroup8` | `0x801C8F58` | `EnterNameMenuRenderAuxQuads` |
| `0x801C90D0` | `EnterNameMenuRenderAuxGroup6` | `0x801C9160` | `EnterNameMenuRenderPromptText` |
| `0x801C92BC` | `EnterNameMenuRenderBackgroundDim` | `0x801C9338` | `EnterNameMenuRenderNameEntryUi` |
| `0x801C97FC` | `EnterNameMenuRenderAuxiliaryGroups` | `0x801C983C` | `EnterNameMenuRender` |
| `0x801C989C` | `EnterNameMenuPlaySoundEffect` | `0x801C98E8` | `EnterNameMenuPollInput` |
| `0x801C9AF4` | `EnterNameMenuUpdateTransitionEffect` | `0x801C9C34` | `EnterNameMenuUpdateAndRender` |
| `0x801C9D5C` | `EnterNameMenuInitializePointerCursors` | `0x801C9F1C` | `EnterNameMenuFreePointerCursors` |
| `0x801C9F60` | `EnterNameMenuStartOpenMenuTransition` | `0x801C9F90` | `EnterNameMenuStartCloseMenuTransition` |
| `0x801C9FC0` | `EnterNameMenuInitializePromptText` | `0x801CA39C` | `EnterNameMenuFreePromptText` |
| `0x801CA400` | `EnterNameMenuFree` | `0x801CA558` | `EnterNameMenuInitializeCharacterGridGraphics` |
| `0x801CADC8` | `EnterNameMenuInitializeNameEntryScreen` | `0x801CB1C4` | `EnterNameMenuUpdateEnteredNameTexture` |
| `0x801CB25C` | `EnterNameMenuInitializeNameBuffers` | `0x801CB2F0` | `EnterNameMenuGetEncodedNameLength` |
| `0x801CB33C` | `EnterNameMenuMainLoop` | `0x801CBDBC` | `EnterNameMenuMain` |
