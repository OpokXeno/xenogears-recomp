# Xenogears Debug Overlay

In-game developer debug overlay for XenogearsRecomp. Exists only
in Debug/DebugTools builds — Release carries zero code, zero strings, zero
data staging for it.

## Toggle & input

- **Ctrl+F3** toggles the overlay (consumed before the savestate hotkeys, so
  plain F3 = savestate load slot 2 keeps working).
- TCP equivalents: `overlay_state`, `overlay_toggle` (see
  `psxrecomp/TCP_COMMANDS.md`).

## Build gate

`PSX_DEBUG_OVERLAY` CMake option in `psxrecomp/runtime/runtime.cmake` (ON for
Debug/DebugTools). When ON: compiles `debug_overlay.cpp`,
`debug_overlay_data.cpp`, vendored `third_party/pugixml/pugixml.cpp`, defines
`PSX_DEBUG_OVERLAY=1`, and stages `debug_overlay/data/` next to the binary
(POST_BUILD). When OFF, `debug_overlay.h` collapses every entry point to a
static-inline no-op.

## Sections (window "Xenogears Debug")

| Section | What it does |
|---|---|
| GPU state | live display/draw/aspect/present-rate/vblank reads |
| RAM inspector | address read/watch over the address space |
| Toggles | runtime enhancement toggles, launcher settings, Controller 1/2 routing, independent 8 MiB Developer Mode, and native Kernel Menu actions |
| Rings | dump event/latency/starv ring buffers |
| Map Teleport | field jump via the engine's own field-change poll recipe |
| Party | party editor (kernel master slots) + unlock bitfield + roster viewer |
| Gold & Vars | gold u32 and fieldVars[512] read/write |
| Force Battle | explicit-battle selector: party + gears + levels, enemy search across all sets, arena picker, 8 lanes, fixed wild-standard record into slot 15 + opcode-71 handoff (snapshot + return) |
| Free Camera | Free camera movement in field, worldmap, battle and battling |
| Event Jump | script event jump by id |

All widget actions are also reachable over TCP as
`overlay_widget_action` (same code path as the click) — see
`psxrecomp/TCP_COMMANDS.md` for the per-name argument encodings.

## Data tables (`debug_overlay/data/`)

XML tables loaded at init via the vendored pugixml (`debug_overlay_data.*`):

- `fields.xml` — all 730 field ids (0–729; map names/ids for teleport)
- `characters.xml` — 11 characters + 20 gears
- `events.xml` — script-derived GameProgress beats and research presets for Event Jump
- `flags.xml` — named fieldVars byte offsets + the GameProgress timeline
- `addrs.xml` — verified guest address book (evidence + status per entry)
- `ram_map.xml` — named watches/regions for the RAM inspector

Each table has a `*.schema.md` and `*.example.xml`. Loader:
`psxrecomp/runtime/src/debug_overlay_data.{h,cpp}`; host-side test:
`psxrecomp/runtime/tests/test_debug_overlay_data.{cpp,sh}`.

## Hard-won address facts (do NOT regress)

- **Party master = kernel slots `0x80062590`** (3×u32, low byte = char id,
  `0xFF` = empty). gameState `currentParty` (`0x8006F368`) is a per-frame
  copy made by the kernel sync at `0x800A3200` — writes to it are silently
  reverted. Write the master; gameState and the var mirrors
  (`0x8006EFA2`) follow next frame.
- **Party writes mirror the engine's own add-member path.** Every engine
  path moves per-member 0x5000 resource buffers + mirrors (`0x8006FABC`)
  + flags together with the slots (opcodes `FUN_8008bc80/8008bdd8/8008c334`,
  boot `InitializeCharacterSkinSet` at `0x8001AD4C` which also compacts
  `0xFF` holes and rebuilds skin buffers per entry). The panel therefore:
  validates the whole formation (valid ids, no duplicates — the engine
  validator `FUN_8008a790` rejects both, lookups poison on holes — `>=1`
  member per the menu contract), packs left, ORs unlock bits into BOTH
  masks (`0x8006F364`+`0x8006F366`, never clears), writes bitfields first,
  then mirrors (`0x8006FABC`, kept `== slots` like every engine path) +
  slots atomically, and only while the field module is resident with the
  engine idle (skin streaming / menu / fade — same triple as teleport).
  New members load fully on the next field change (teleport/door); the
  leader/followers desync mid-field until then. Adding a member that fails
  the pre-write availability AND forces `0x8006F364 = 0x07FF` (branch
  uniformity: field scripts branch on raw unlock bits via
  FieldScriptCheckAvailablePartyMember — verified live that story+Bart
  crashes Lahan→worldmap while story-exact and full-unlock pass).
  gameState+0x22B1 is deliberately NEVER written by the party writer:
  those are per-slot mount bytes (docs/xenogears/field/07 §10: 0 = on
  foot, 1 = mounted — worldmap's party reconcile compares+counts them,
  and a blind clear breaks worldmap entry). The battle selector drives
  them explicitly per Mounted checkbox, since the battle loader takes
  the Gear placement path from them (a mounted slot needs a gear
  assigned in the roster, else it warns).
- **Level bytes are the number only.** Stats and unlocks are applied by
  the level-up event (`BattleResultApplyLevelUps`, battle module only —
  never called from the debug hook, same reentrancy rule as
  `loadNewField`), so the panel writes level bytes plus direct roster
  stats (engine caps) and offers EXP prime (`+0x44/+0x48` to 0): the next
  real battle result then runs the authentic threshold loop (growth rolls
  + unlock checks, docs/xenogears/battle/08). Nothing is fabricated.
  Refusals carry codes (1 = not field, 2 = busy, -1 = bad id, -2 = empty,
  -3 = duplicate) instead of corrupting RAM.
- **Party writes must keep the unlock bitfield (`0x8006F364`) consistent.**
  The camp menu lists members from the bitfield, and a party member whose
  bit is clear crashes the next field load.
  `party_slot` therefore auto-ORs the bits of every non-empty member
  (never clears — leaving the party does not re-lock a character).
- **Teleport** uses the recipe in `addrs.xml` (gates + `fieldMapNumber`
  `0x8004F34C` + entry u16 + arm `0x800ADBC4=0xFF`); the field poll fires
  `loadNewField`. NEVER write `fieldID` (`0x8006F94E`) directly (corrupts
  texture streaming) and NEVER call `loadNewField` from the debug poll
  (not reentrant).

## Known Issues

- **Party** formation writes are refused (never applied half-way) outside field, while busy, or with invalid/duplicate/empty shapes; new members load fully on the next field change.
