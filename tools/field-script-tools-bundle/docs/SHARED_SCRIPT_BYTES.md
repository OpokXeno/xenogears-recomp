# Standalone XGS and native operand islands

XGS is the complete input to compilation. No companion XGA or external original
binary is consulted. Every instruction is encoded from its current semantic
operands, whether source has been edited or not. XGA is an independent assembly
format, not an input to XGS compilation.

## One encoding and linking pipeline

Different native encodings can have the same visible operation: unused mask
bits, NOP aliases, skipped payload and shared native instruction/data layouts
cannot be recovered if the decompiler omits them. The lossless decompiler retains
format fields as typed arguments and gives distinct dispatch variants distinct
spellings. The compiler encodes these fields and derives layout from code.
Byte traces, checksums, `view` markers and
event-entry comments are purely informational and are discarded before parsing.
No comment selects an encoding, a data view, a shared terminal or a block position.

The old whole-file reconstruction and local trace preference routes have been
removed. `field_source_fidelity.py` serves only the decompiler. Before encoding,
every semantic operand bit is cleared from its canonical template; the operand
writer then writes the source value. This happens for unchanged operands too.
Event tables are rebuilt from the current symbolic bindings.

`field_native_selection.py` applies the same placement/relaxation loop to every
XGS compilation. It tries native forms, serializes their current operands and
relocations, and checks their actual reads and branch destinations. A form whose
constraints fail is replaced by an independent operand island. Choices only relax,
so edits cannot cause an oscillating layout. There is no source-equality test,
re-decompilation inside compilation, cached original program, or pristine-file path.

Removing or falsifying comments leaves the output unchanged, including after
edits. Code-backed `script_data` sharing is attempted from explicit data bytes and
accepted only when the current linked code matches. Interior terminal entries use
explicit `alias entry = instruction + offset fallback stop;` declarations. The
linker checks the current bytes and materializes the fallback if needed; historical
trace offsets never establish aliases.

## Handler evidence and source operations

Offsets include the opcode and, for extended instructions, the FE prefix. The
FE dispatcher (`800869B8`) advances IP by one before calling its handler.

| Handler | Native behavior | Standalone source |
|---|---|---|
| `10`, `80098C00` → `80098CAC` | Phase zero reads XYZ and advances 9; continuation rereads coordinates at IP-9 and finishes at continuation+2. | `movement.begin_actor_move(x, z, y)` / `movement.continue_actor_move(x, z, y)` |
| `11`, `80098C3C` → `80098CAC` | An uninitialized counter reads the limit at IP+11. Initialization advances 9; completion rewinds 9 and advances 13, hence continuation+4. | `movement.begin_actor_move_with_step_limit(x, z, y, limit)` / `movement.continue_actor_move_with_step_limit(x, z, y, fallback_limit)` |
| Short `57`, `80099214` | Completion changes IP to IP-11 **before** rereading XYZ and the walkmesh-height flag. | `movement.finish_ballistic_actor_move(x, z, y_or_walkmesh, use_walkmesh)` |
| `57`, mode 15 | Walkmesh-only refresh, no predecessor reads. | `movement.refresh_actor_walkmesh()` |
| `FE 18`, `8008BDD8` | One character byte; staged path resumes at +3, already-present path at +5. | `actor.add_immediate_party_character(character) staged goto a already_present goto b` |
| `FE 5C` mode 1, `800A0FD8` | Reads an evaluated resource word at +6/+7 but advances only 3. | `actor.load_current_actor_mecha(1, resource)` |
| `FE 5C` unsupported mode, same handler | Returns at the subopcode, which dispatches primary `5C`; its party selector spans the mode byte and next byte. | `actor.load_current_actor_mecha_reinterpret(party_selector)` |
| `FE 6C`, `8008A5A0` | Only clears controller enable if the following opcode is zero; that zero then stops the event. | `event.clear_controller_enable_flag_and_stop()` or `event.keep_controller_enable_flag()` |
| `FE 77` mode 0, `8008A2E8` | Resource word at +6/+7, mask bit 128 at +14, advancement 3. | `event.manage_overlay_image_asset(0, resource)` |
| `FE D7`, `80087AB8` | X/Z words at +2/+4, mask bits 128/64 at +10, advancement 7. | `movement.set_world_map_marker_position_xz(x, z)` |

The bounded-movement instruction was previously misclassified as one fixed
13-byte dispatch. Nine initialization bytes followed by a four-byte continuation
are two phases; the intervening opcode/mode are not unused padding.

Coordinates and resource words remain immediate values or declared VM variables
according to their actual operand reader. The compiler reconstructs the reader's
mode bits from those operands. Variable values are still evaluated by the game,
not frozen by decompilation. Continuation coordinates are explicit source
arguments, so editing or moving their initializer does not leave an accidental
physical link to old bytes. A normally paired bounded continuation whose unused
fallback lies outside the resource uses the limit supplied by its initializer;
an unpaired continuation without readable operands is rejected.

## Private native layouts

These are the fallback forms selected when the current native layout cannot
represent the operation. Native forms remain available after edits when their
current operands still satisfy the constraints:

- **Forward reads:** native opcode, jump over private operand data, then normal
  continuation. Mecha loading places its evaluated word at +6; image loading
  places word/mask at +6/+14; the world marker places its mask at +10.
- **Backward reads:** a jump skips a private coordinate descriptor and enters
  the native continuation. The descriptor is always exactly 9 or 11 bytes before
  that continuation, wherever the source operation moves.
- **Bounded initialization:** an always-taken conditional branch following the
  nine-byte initializer also holds the limit at the required +11/+12 positions.
- **Bounded continuation:** its private descriptor and fallback-limit data are
  rebuilt together. Repeated native updates retain their IP and yield behavior.
- **Controller clear:** emits the native handler followed by Stop. The keep
  operation is an equivalent single-dispatch NOP, with no next-byte dependency.
- **Party staging:** a compiler-owned two-entry gate directs either native path
  to arbitrary source labels. At +3 it executes an always-taken conditional
  branch; at +5 it executes an overlapping unconditional jump. After relocation
  the compiler adjusts the comparison constant if necessary to keep the first
  branch unconditional. Neither destination requires a fixed address or alignment.

These islands use ordinary retail bytecode, no runtime patches or scratch VM
variables. Their references use the normal linker. Native retries and waits
remain in the original handlers. Routing can add VM dispatches, as ordinary
linker-generated jumps already do; instruction-budget-sensitive timing is not
claimed to be cycle-identical.

Native operand data is represented explicitly so the compiler can reproduce and
revalidate its sharing. Party gates use a relocatable `party_landing` data form.
An independently entered one-byte terminal can share an operand byte only while
that byte still encodes the same terminal; otherwise it gets its own code. Legacy `script.byte_at`,
`script.masked_word_at` and physical-origin syntax must be re-decompiled into the
semantic forms above; they are not the new standalone API.

## Script bytes used as actual data

Primary `48/49` can read instruction bytes as a lookup table. Normalizing an
otherwise irrelevant encoding bit would change such a lookup. These reads refer
to immutable `script_data` contents declared **inside XGS**:

```text
data {
  script_data lookup_image {
    bytes "10 20 30 40";
  }
}
```

This is data consumed by the program, not a saved program used to reconstruct
instructions. The compiler never uses it as a replacement executable. If its
contents equal the currently encoded code bytes, the read-only view may share
them. If an edit changes those bytes, the data is materialized independently.
For an unconstrained index the decompiler conservatively retains all original
script bytes that could be observed. The native image has a small compiler-owned
header so a later decompilation can recover the same image without duplicating
the entire newly compiled program. Source labels refer to its contents, not its
header. Editing code no longer changes the lookup's byte values accidentally;
edit the data image when changing that table is intended.

## Canonicalization corrections

- `02` switches on the entire high nibble. Unsupported modes evaluate neither
  input and compare zeros; their source becomes the equivalent constant test.
- `6D/6E/CA` use input mask bits `0x40/0x20`, not `0x80/0x40`.
- `FE AB/AC` retain the party selection in the low two bits as a visible input.
- NOP aliases have distinct spellings; unobserved instruction fields have typed
  reserved arguments. Script-as-data
  observations are preserved by the data representation above.
- Calls relocate their return continuation; no original call-size hint is needed.

The native 16-bit PC and resource-size limits still apply. Large data images and
lowering islands count toward them. Undefined reads outside the resource, invalid
game IDs and incorrect authored control flow cannot be made valid by compilation.
The strict corpus verifier requires original byte identity in addition to linked
structure and edit checks. All 729 unique resources (935 occurrences) compile,
and all 729 reproduce their original bytes; the strict identity check passes.
All 729 edited sources pass through that same pipeline. Every original and edited
source produces identical output with normal, removed and false comments.
Focused tests poison comments,
change operands and events, break and preserve sharing, and block the old
restoration functions. See
[`RELOCATION_EVIDENCE.md`](RELOCATION_EVIDENCE.md) for the full results and scope.
No in-game execution was performed, and corpus coverage does not exercise every
possible handler/mode combination.
