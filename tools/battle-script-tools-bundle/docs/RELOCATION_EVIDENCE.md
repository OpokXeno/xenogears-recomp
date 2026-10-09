# Battle Event Relocation And Encoding Evidence

This guide records where the formats used by the tools come from and what the
recorded corpus runs established. It is evidence for the binary toolchain, not
for gameplay behavior.

## 1. Executable Evidence

The Battle Event overlay is directory `0x20` file `1`, loaded at `0x801E5000`.
Its dispatch table at `0x801E5030` has 76 entries; each case stub passes the
entity index and a pointer to the current instruction to its handler. Operand
layouts were taken from the byte loads relative to that pointer and from the
arguments passed to `BattleEventDecodeOperands` (`0x801E57F8`):

| Decoder argument | Meaning |
|---|---|
| `a0` | Instruction pointer; operands are read at `+1`, `+3`, ... |
| `a1` | Number of u16 operands |
| `a2` | Type mask (mode 0) |
| `a3` | Mode: nonzero is `v15`, zero is typed |

Mode 1 tests bit 15 of each u16. Mode 0 tests mask bit `0x80 >> n` for operand
`n`. Variable offsets are masked with `0xFFFE` (`0x7FFE` in mode 1) and index the
bank at runtime `+0x394`.

Layouts that refine the prose of `docs/xenogears/battle/07-battle-event-vm.md`:

| Op | Handler evidence | Layout used |
|---:|---|---|
| `02` | decode 2 typed operands with mask `+5`; selector from the same byte; false target `+6` | left `+1`, right `+3`, mask `+5`, target `+6` |
| `06..11` | decode 2 typed operands with mask `+5`; destination read again from `+1/+2` | destination `+1` (its type bit unread), value `+3`, mask `+5` |
| `12`, `13` | decode 2 operands with mask 0; destination from `+1/+2` | destination `+1`, count variable `+3` |
| `15` | random bound from `+1/+2`; result stored at offset from `+3/+4` | **bound `+1`, destination `+3`** |
| `16`, `17` | decode both typed operands and use the first as the left factor | left/destination `+1`, value `+3`, mask `+5` |
| `18` | message `+1/+2`, flags `+3`, portrait from the entity | as listed |
| `19` | portrait `+1`, message `+2/+3`, flags `+4` | as listed |
| `28` | `BattleCreateFader(a0=+5, a1=+1, a2=+2, a3=+3, stack=+4)`; `1E` passes `(duration, 2, 255, 255, 255)` | mode `+1`, red `+2`, green `+3`, blue `+4`, duration `+5` |
| `2C` | priority from `+1`; length 3 | priority `+1`, byte `+2` unread (`reserved`) |
| `36`, `40` | shared helper decodes one `v15` | sprite alias `+1` |
| `45` | operands 0 and 1 resolved as characters; operand 3 read as a byte (sprite mode); operand 2 passed to the sprite animation starter | destination, source, animation, sprite mode |
| `46` | operands 0 and 1 resolved; operand 2 passed to the swap helper | destination, source, effect |

Every other opcode reads only `v15` operands in order, or no operands. The schema
sizes match all 76 documented sizes; `battle_event_codec.check_table()` asserts
this whenever the catalog is generated.

### Operation names

Names describe what each handler does.

| Op | Name | Evidence |
|---:|---|---|
| `1C`, `1D` | `battle.enter_cutscene_mode`, `battle.enter_combat_mode` | Write 2 or 1 to `0x800C3E4C`, the selector of `BattleUpdateInputs` (`0x8008A9C0`): mode 1 runs active-time combat, mode 2 the cutscene input path that also counts down `flow.wait`. `BattleMain` sets 2 for the opening Event and 1 when combat starts. |
| `1E`, `1F` | `visual.fade_out`, `visual.fade_in` | `BattleCreateFader(duration, 2, c, c, c)` with `c = 255` or `0`. The fader draws with `GetTPage(abr = mode)`; mode 2 subtracts its colour from the screen, and a fader that reaches colour zero removes itself. Retail scripts use `1F` after building a scene and `1E` before movies and exits. |
| `22` | `flow.yield_to_battle` | Sets `+0x801` to 2; `BattleEventUpdate` returns after the pass and the Event continues on its next update. |
| `23` | `actor.play_mecha_animation_wait` | Calls `BattleStartEventControlledMechaAnimation` (`0x800AA384`) and polls entity `+0x34`. The mecha bytecode's end command calls `BattleMarkEventMechaCommandComplete` (`0x80080C6C`) for the mecha's slot. On completion the handler releases animation resources. The actor operand indexes the mecha slot array at `0x800D3368`. |
| `24` | `battle.queue_next_battle(formation, transition_effect)` | Stores `formation + 1` at `0x8005947C`. `RunBattleAndDispatchOutcome` re-enters Battle while it is nonzero, and `BattleMain` copies the 32-byte record `formation` of the table at `0x800658DC` into the formation buffer at `0x8006F9DC`. |
| `25` | `battle.continue_on_defeat` | Sets `0x800C3D5C`; on a defeat result with Battle Event enabled, `BattleMain` selects exit mode 3 and rewrites the result to `0x01`. |
| `3A` | `actor.play_animation` | Calls `SpriteStartAnimationById` (`0x800245D8`) on the character sprite; negative IDs first prepare special animation data. |
| `3B` | `actor.restart_animation` | Starts animation `sprite+0xAF`, which `SpriteStartAnimationById` sets to the last started ID. |
| `3C` | `actor.reset_frame` | Clears `+0x34`, `+0x9E` and bits 2..7 of `+0x40`, the decoded tile count written by the frame decoders. |
| `41` | `audio.set_sound_volume` | Calls `0x8003A2E4`, which writes channel field `+0x10A` of matching effect channels, the field `SoundPlayEffect` fills from its volume argument (`SetSedsVoicePairVolume` writes the same field). Retail scripts ramp it in loops. |
| `44` | `actor.clear_mecha_event_flags` | Clears `+0x35` of the eleven mecha slots at `0x800D3368`; only the Event-controlled animation start sets it, and the animation end command reads it before notifying the Event. |
| `4B` | `actor.suppress_critical_pose` | Sets combatant `+0x36` bit 0. Its only reader is `BattleGetGearConditionFlags`, called only by `BattleResolveMechaAnimationKeyframe` to choose the critical-HP idle pose; KO state lives at `+0x7C`. |

### Loading and LZSS

`BattleEventInitialize` queries the size of directory `0x20` file `2`, allocates
it, loads it with file `3`, relocates the table with `0x8003342C` and expands
`table[1 + 2k]` and `table[2 + 2k]` with `0x80032E88`, where `k` is the byte at
`0x8006F9DF`. The decoder allocates the declared size and loops over control
bytes; within a group it handles all eight tokens and only compares the output
cursor with the end before reading the next control byte. Streams must therefore
end exactly at a group boundary, which every retail stream does.

## 2. Corpus Results

Recorded with the current tools on the USA discs. Directory `0x20` file `2` is
byte-identical on both discs (38,368 bytes, SHA-256 `cffb36a5…`), so 96
occurrences share 48 unique resources.

| Measure | Value |
|---|---:|
| Unique scripts | 48 |
| Entities | 234 (maximum 12 per script) |
| Script resource bytes | 27,142 |
| Bytecode bytes | 20,134 |
| Instructions | 4,615 |
| Instruction bytes | 19,966 |
| Preserved data bytes | 168 |
| Orphan code blocks | 12 |
| Jumps into instructions | 0 |
| Flow diagnostics | 0 |
| Distinct opcodes used | 51 of 76 |
| Streams reading past their extent | 71 of 96 |

Opcodes absent from retail scripts: `04 07 09 0A 0B 0C 0F 10 11 12 13 14 15 16 17
19 2C 2D 2E 30 32 33 34 3D 47`. Their layouts rest on section 1 only.

`battle_script.py verify extracted-battle-scripts --repack --resize` passed for
all 48 resources:

- XGA disassembly reassembles every resource byte for byte (exact layout).
- Unedited XGS rebuilds every resource byte for byte, with comments and with all
  comments removed.
- The XGA written by the compiler reassembles to the compiled bytes.
- Inserting `flow.nop()` before all 4,615 source operations leaves every
  existing operation and reference unchanged; the 48 resized resources total
  31,757 bytes, and their XGA round-trips.
- Rebuilding the event data file with each original or resized script expands to
  the expected payload; recompression of the 48 originals totals 16,153 bytes.

A hand-written example and an edited retail event (new variable, new label,
conditional) were compiled, decompiled again and repacked; only the edited
stream changed and the rebuilt file no longer reads past any stream.