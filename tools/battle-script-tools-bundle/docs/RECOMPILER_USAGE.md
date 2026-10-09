# Editing, Compiling And Repacking Battle Event Scripts

Run commands from the bundle directory. Python 3.11+ is sufficient; the tools
use only the standard library.

## 1. Normal Editing Workflow

```bash
python3 tools/battle_script.py decompile /path/to/script.bin \
  --event 7 --dialog /path/to/dialog.bin --xgs script.xgs

# Edit script.xgs, then compile that file:
python3 tools/battle_script.py compile script.xgs \
  --output script.modified.bin --xga script.modified.xga --map script.map.json
```

`--dialog`, `--xga` and `--map` are optional. The extracted catalog already
contains `script.xgs` for every event, so decompiling is only needed for other
binaries. Compilation never reads a sibling XGA, original binary or dialogue
file.

Each entity's events and the code only it reaches are grouped together; shared
code appears once under `shared_code`, preserved bytes under `data`.

Operations use positional arguments:

```text
dialogue.set_portrait(0x18);
dialogue.show(0, 0x00);
flow.wait(30);
actor.move(0x10, 300, 0, script.z_0204);
```

To insert an instruction, add a statement. To delete one, remove it. The linker
rebuilds every address, jump and entry row.

Labels bind to the following instruction. Inserting code **after** an entry
label makes that entry execute it; inserting it **before** the label does not.

### Comments

```text
// event: entity 1: entry[2]
dialogue.show(0, 0x00); // 00F3: 18 00 00 00 | "Fei / \"Reinforcements, / huh?!\""
```

The compiler ignores all `//` comments. Removing or changing them produces the
same binary.

## 2. Variables, Entities And Events

### Existing Variables

```text
state {
  engine {
    signed battle_exit_signal at 0x0000;
  }
  script {
    signed counter_0200 at 0x0200;
  }
}
```

`at` is a byte offset in the Event variable bank, not a bytecode address. Keep
existing bindings when renaming variables: other entities, Enemy AI opcode `0x70`
(offsets `0x0000..0x01FE`) and resident Battle (`engine` offsets) may use the
same slot.

### Complete Compilable Example

Save as `example.xgs` and compile it with the command above:

```text
battle_event 0 {
  state {
    script {
      new signed lines_shown;
    }
  }
  entities {
    entity 0 {
      events {
        start -> conductor;
        idle -> done;
      }
      code {
        conductor:
          visual.fade_in(30);
          flow.start_script_wait_finished(1, 2, 3);
          if (!(script.lines_shown == 1)) goto skip_fade;
          visual.fade_out(10);
          fallthrough skip_fade;
        skip_fade:
          battle.set_silent_result();
          flow.yield_to_battle();
          stop;
        done:
          stop;
      }
    }
    entity 1 {
      events {
        start -> speaker_init;
        idle -> speaker_idle;
        entry[2] -> first_line;
      }
      code {
        speaker_init:
          dialogue.set_portrait(0x00);
          fallthrough speaker_idle;
        speaker_idle:
          stop;
        first_line:
          dialogue.show(0, 0x00);
          script.lines_shown++;
          stop;
      }
    }
  }
}
```

Entity IDs must be consecutive from zero and at most 16. Each entity has eight
entries: `start` (0), `idle` (1) and `entry[2]`..`entry[7]`. Unbound entries
share one generated `stop`. Duplicate bindings and unresolved labels are errors.

Message IDs refer to the event's own `dialog.bin`. Compiling does not change the
dialogue; adding a message requires a new dialogue bundle (see section 4).

## 3. Control Flow And Binary Details

- Every block ends with `stop`, `goto` or `fallthrough label;`.
- `if (!(condition)) goto label;` continues when the condition holds.
- Generated jumps add one VM dispatch (three bytes). The link map reports them.
- Numbers in `goto`, `if` and event bindings are raw offsets that the linker does
  not relocate; prefer labels.
- `v15` immediates are `0..32767`; put negative values in a variable first.
- Handlers poll for completion (`flow.wait`, dialogue, moves, mecha setup,
  start-and-wait). They hold the entity, so long waits in entity 0 delay the
  operations that follow it, not other entities.

The compiler checks structure, references and operand ranges. It cannot prove
that an edit plays well in game or that resource, sprite, music, sound or
animation IDs exist.

## 4. Low-Level Assembly

```bash
python3 tools/battle_script.py assemble script.xga --output script.assembled.bin
python3 tools/battle_script.py assemble script.xga --layout exact --output script.exact.bin
```

| XGA directive | Meaning |
|---|---|
| `.xga 1` | Format version |
| `.prefix "HEX"` | The 64-byte resource prefix |
| `.size N` | Informational bytecode size |
| `.entities N` | Number of entity rows |
| `.row ID = target, ...` | Exactly eight labels or raw offsets |
| `.alias name = base + N` | Byte-relative label |
| `name:` | Bind a label to the next record |
| `.at PC op "HEX"` / `.at auto op "HEX"` | Instruction; `@OFFSET=label` encodes a `u16` label |
| `.at PC data "HEX"` | Preserved bytes |

`--layout exact` requires every `.at PC` to match its placement.

## 5. Repacking And Runtime Overrides

```bash
python3 tools/battle_script.py repack battle-event-data.bin \
  --event 7 --script script.modified.bin --output battle-event-data.modified.bin
```

`battle-event-data.bin` is the stored directory `0x20` file `2` (all 48
events). Repeat `--event`/`--script` to replace several events at once. Add one
`--dialog` per `--event` to replace dialogue bundles too, using `-` to keep an
event's original dialogue. Other streams are preserved.

For the port's indexed-file override mechanism:

```bash
# Obtain the canonical digest from the runtime (repository root):
./build/XenogearsRecomp --disc-hash /path/to/disc1.cue

# From this bundle, substitute that digest:
python3 tools/battle_script.py override /path/to/disc1.cue \
  --event 7 --script script.modified.bin \
  --disc-sha256 CANONICAL_DIGEST --id local.battle-event7 --output event7-mod
python3 ../../psxrecomp/tools/psxmod_pack.py event7-mod event7.psxmod
```

Install and enable the package in the launcher. For Disc 2, supply its canonical
digest and `--game-id SLUS-00669`. Because every event lives in one indexed
file, a package replaces that whole file: put all edited events of a mod in one
`override` command, and do not enable two Battle Event packages at once.

To see an edited event, trigger a battle whose formation selects that Battle
Event data index.

## 6. Regeneration And Validation

```bash
# Recreate generated catalog sources from the extracted assets:
python3 tools/battle_script.py regenerate extracted-battle-scripts

# Corpus regression (fast; the corpus is small):
python3 tools/battle_script.py verify extracted-battle-scripts
python3 tools/battle_script.py verify extracted-battle-scripts --repack --resize
```

`regenerate` overwrites catalog `.xgs`/`.xga` and checks that each source
rebuilds its asset exactly. `verify` checks XGA and XGS identity with and
without comments; `--resize` adds a `flow.nop()` before every operation and
checks relocation; `--repack` rebuilds the event data file from the disc paths
in `manifest.json`. These checks do not run the game.
