# Editing, Compiling And Repacking Field Scripts

Run commands from the bundle directory. Python 3.11+ is sufficient; the tools
use only the standard library. Pytest is needed only for the test suite.

## 1. Normal Editing Workflow

```bash
python3 tools/field_script.py decompile /path/to/scripts.bin \
  --field 7 --xgs script.xgs --xga script.xga

# Edit script.xgs, then compile that file:
python3 tools/field_script.py compile script.xgs \
  --output scripts.modified.bin --xga script.modified.xga --map script.map.json
```

`--xga` and `--map` are optional diagnostic outputs.

The source groups each entity's events and `code` together. Shared instructions
appear once under `shared_code`; preserved bytes appear in named objects under
`data`.

Calls use positional arguments:

```text
dialogue.set_portrait(2);
movement.set_actor_movement_speed(16);
flow.sleep(30);
```

To insert an instruction, add a statement. To delete one, remove the statement.
To change its size, replace it with another supported operation. The linker
rebuilds the addresses and reference fields.

Labels bind to the following instruction. Inserting code **after** an entry
label makes callers execute that code; inserting it **before** the label does
not move the entry onto the inserted code.

### Comments

```text
// event: entity 1: update
flow.sleep(30); // 01A5: 26 1E 80
```

Byte traces describe the original input, not necessarily the result after an
edit. Event comments summarize original entry aliases. Both are informational;
the compiler ignores comments. Handler descriptions are documented in the
operation catalog rather than repeated in generated scripts.

## 2. Variables, Entities And Events

### Existing Variables

```text
state {
  persistent {
    signed story_progress at 0x0000;
  }
  scene {
    signed movement_speed at 0x040A;
  }
}
```

`at` is a **VM memory binding**, not a position in the script. Preserve an
existing binding when changing instructions or renaming that variable. The
engine and raw operations may also refer to that slot.

`signed`/`unsigned` determine how the VM reads its 16-bit value. An optional
`unsigned slots[...]` declaration preserves unsigned slots not represented by
named variables. See the DSL reference for its range syntax.

### Complete Compilable Example

Save this as `example.xgs` and compile it with the command in section 1:

```text
field 7 {
  state {
    scene {
      new unsigned interaction_count;
    }
  }
  entities {
    entity 0 {
      events {
        initialize -> initialize_counter;
        update -> idle;
        interact -> interaction;
        routine[4] -> clear_counter;
      }
      code {
        initialize_counter:
          scene.interaction_count = 0;
          goto idle;
        interaction:
          scene.interaction_count++;
          dialogue.set_portrait(2);
          stop;
        clear_counter:
          scene.interaction_count = 0;
          stop;
        idle:
          stop;
      }
    }
  }
}
```

Entity IDs must be consecutive from zero. To add an entity, use the next ID,
declare its events and provide its labeled code. Each entity has **32 routine
slots**: `initialize`, `update`, `interact`, `contact` occupy slots 0–3;
`routine[4]` through `routine[31]` are available for additional events.

Unassigned slots receive an empty implementation. Multiple slots can point to
the same label. Duplicate bindings and unresolved labels are errors. To assign
one slot previously covered by `routine[4..31]`, split that range so the slot
is bound only once.

A new entity also needs appropriate game initialization and valid resources:
graphics, position and behavior as required. Adding a routine row does not
create new graphics or placement records. Test the resulting behavior in-game.

## 3. Control Flow And Binary Details

Use labels for jumps, calls and data addresses. `fallthrough label;` expresses
a continuation that may lie in shared code; the linker retains adjacency when
possible or emits a routing jump. Add `stop`, `return`, `goto` or an explicit
continuation when finishing a routine.

Computed tables use an explicit list of case labels and targets:

```text
flow.dispatch_triplet_table(scene.selection, [case_zero -> handle_zero, case_one -> handle_one]);
```

The linker builds the three-byte case slots. Case bodies are ordinary labeled
code and may grow. `flow.skip_triplets_to(label);` describes a fixed destination.
Some camera/dialogue handlers also have an alternate completion path:

```text
camera.leave_or_reacquire_follow_camera(0) otherwise goto fast_path;
fallthrough normal_path;
```

Generic operands without a verified typed schema remain encoded byte values
(`0..255`). The byte `80` is not universally a separate argument: for example,
`21 10 80` is a tagged immediate and renders as movement speed `16`.

Behavior-neutral encodings are normalized where verified. The variable-operation
family `35`, `38`, `39`, `3A`, `3B`, `3E`, `3F`, `40`, `DE`, `DF` observes only
control bit `0x40`. Differences in the other control bits do not force `raw`.
This means XGS recompilation can differ in those bits while preserving behavior.
The original bytes remain in the trace; use XGA for strict byte identity.

An encoding with no verified semantic equivalent uses `raw("...");`. Its size
must still match an instruction. Address-bearing raw instructions take symbolic
labels after the bytes, for example `raw("01 00 00", destination);`.

The compiler checks structure, references and encoded ranges. It cannot prove
that a new event implements the intended gameplay or uses suitable external
resources. Generated routing jumps can also add VM dispatches; inspect the map
or assembly for timing-sensitive edits. The VM's 16-bit PC limits bytecode to
65,536 bytes.

## 4. Low-Level Assembly

```bash
python3 tools/field_script.py assemble script.xga --output scripts.assembled.bin
python3 tools/field_script.py assemble script.xga \
  --layout exact --output scripts.exact.bin
```

Exact address assertions belong to XGA. XGS always uses symbolic linking.

| XGA directive | Meaning |
|---|---|
| `.xga 1` | Format version |
| `.bitmap "HEX"` | Complete 128-byte variable bitmap |
| `.size N` | Source bytecode extent |
| `.label name = PC` | Source address to resolve |
| `.bind name` | Bind to the following record |
| `.alias name = base + N` | Byte-relative symbolic alias |
| `.row ID = name, ...` | Exactly 32 routine labels |
| `.block NAME START END` / `.end` | Low-level source span and record sequence |
| `.at PC op "HEX"` / `.at auto op "HEX"` | Positioned or automatically placed instruction |
| `.at PC data "HEX"` | Preserved bytes |
| `.at PC padding "HEX"` | Generated, recomputable zero alignment padding |
| `.at auto edge "" @1=label` | Semantic fallthrough edge |
| `@OFFSET=label` | Encode a label as LE `u16` at a record byte offset |
| `.triplets PC = PC, ...` | Source anchors for a computed table |
| `!alternate=label` / `!jump=label` | Symbolic alternate landing / fixed A6 destination |
| `!align=N:R` / `!align=N:high(label)` | Alignment residue, including shared-byte landing constraints |

PCs and sizes accept decimal or `0x` hexadecimal. Hex strings contain byte
pairs. `//` introduces comments. Overlapping fixups are valid only if their
shared bytes agree. Mode-dependent instruction sizes are checked after linking.

## 5. Repacking And Runtime Overrides

```bash
python3 tools/field_script.py repack field.bin \
  --scripts scripts.modified.bin --output field.modified.bin
```

`field.bin` is the stored Field container. The repacker replaces section 5,
updates its expanded size and the later section offsets, and preserves other
payloads. Routine-row counts may change; placement records are a separate table.
When necessary to complete an LZSS group, up to seven zero trailer bytes are
accounted for in the expanded size without moving live script offsets.

For the recomp's existing indexed-file override mechanism:

```bash
# Obtain the canonical digest from the runtime (repository root):
./build/XenogearsRecomp --disc-hash /path/to/disc1.cue

# From this bundle, substitute that digest:
python3 tools/field_script.py override /path/to/disc1.cue \
  --field 7 --scripts scripts.modified.bin \
  --disc-sha256 CANONICAL_DIGEST --id local.field7 --output field7-mod
python3 ../../psxrecomp/tools/psxmod_pack.py field7-mod field7.psxmod
```

Install and enable the package in the launcher. For Disc 2, supply its canonical
digest and `--game-id SLUS-00669`; the default is `SLUS-00664`. A BIN/CUE file's
ordinary SHA-256 is not the runtime's canonical disc identity. See the root
`MOD_AUTHORING.md` for the package model.

## 6. Regeneration, Tests And Performance

Normal `compile` processes one source file. It does not run a corpus test or
repack every map.

```bash
# Recreate all generated sources from extracted assets; overwrites catalog XGS:
python3 tools/field_script.py regenerate extracted-field-scripts

# Focused maintenance of generated sources, without full compilation:
python3 tools/field_script.py regenerate extracted-field-scripts --comments-only
python3 tools/field_script.py regenerate extracted-field-scripts --layout-only
python3 tools/field_script.py regenerate extracted-field-scripts --simplify-raw

# Fast unit suite:
python3 -m pytest tests -q
```

Full regeneration validates each generated source against its original binary,
allowing only the verified neutral instruction differences described above.
The maintenance flags refresh traces/events, entity grouping, or known raw
variants respectively; they are for generated sources matching the original
assets, not a substitute for compiling authored modifications.

`verify` is an **explicit, potentially lengthy regression run**, not a normal
editing step:

```bash
python3 tools/field_script.py verify extracted-field-scripts
python3 tools/field_script.py verify extracted-field-scripts --repack --resize
```

It checks the unique corpus resources, lossless XGA, semantic XGS reconstruction
and LZSS. `--repack` reads the disc paths in the manifest; `--resize` additionally
exercises insertion and relocation. Use these for tool development or broad
regression checks, rather than after each small script edit.
