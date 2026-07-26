# Xenogears Debug Overlay

In-game developer debug overlay for XenogearsRecomp. Exists only
in Debug/DebugTools builds — Release carries zero code, zero strings, zero
data staging for it.

## Toggle & input

- **Ctrl+F3** toggles the overlay (consumed before the savestate hotkeys, so
  plain F3 = savestate load slot 2 keeps working).
- While visible: game keyboard input is swallowed only when ImGui wants the
  keyboard (`WantCaptureKeyboard`); hotkeys are never gated.
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
| GPU state | live renderer/interp/widescreen state reads |
| RAM inspector | address read/watch over the address space |
| Toggles | runtime enhancement toggles (texfilter, native_wide, aspect, bd_stretch) + launcher settings (supersampling, antialiasing, screen model, turbo loads, HQ SPU) |
| Rings | dump event/latency/starv ring buffers |
| Map Teleport | field jump via the engine's own field-change poll recipe |
| Party | party editor (kernel master slots) + unlock bitfield + roster viewer |
| Gold & Vars | gold u32 and fieldVars[512] read/write |
| Force Battle | best-effort battle trigger (encounter vars not fully mapped) |
| Free Camera | camera eye/at SVECTOR writes |
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

- **Teleporting** and **event jumps** can occasionally cause a bug in the music, resulting in incorrect music or sounds playing.
- **Party** changes can cause a crash.