# Battle Event Script Format And Concepts

This guide explains what each section of `script.xgs` represents and how it
relates to the format executed by Xenogears. The DSL is a conservative
representation of the bytecode; it is not recovered original source code.

## 1. Physical Structure

### Event data file

All Battle Event scripts and dialogue live in directory `0x20`, file `2`
(FAT index 3088 on disc 1, 3083 on disc 2; the file is identical on both discs):

| Offset | Contents |
|---|---|
| `0x0000` | `u32 count` (96 in retail) |
| `0x0004` | `u32 offset[count]`, relative to the file |
| `4 + count * 4` | `u32` end offset, equal to the file size |
| After the table | `count` LZSS streams, each 4-byte aligned |

Stream `2 * k` is the script resource and stream `2 * k + 1` the dialogue bundle
of Battle Event data index `k`. A formation enables Battle Event with policy bit
`0x20` and selects `k` with its byte `+0x03`; `BattleEventInitialize`
(`0x801E5160`) loads the file, relocates the table and decompresses both
streams of the selected pair into independent allocations.

Each stream starts with its `u32` expanded size. The resident decoder
(`0x80032E88`) processes complete groups of eight tokens and compares its output
cursor with the declared end only between groups. A valid stream therefore
reaches its declared size exactly at a group boundary. The retail encoder filled
the last group with literal tokens that it did not store, so 71 of the 96 streams
read up to seven bytes from the start of the following stream (or from the zero
padding completing the file's last sector). Those bytes become part of the
expanded resource. They follow the last routine of a script and are shown as
preserved `data`.

### Script resource (`script.bin`)

| Region | Contents |
|---|---|
| `0x0000..0x003F` | Resource prefix; zero in every retail script |
| `0x0040` | `u32 entity_count` |
| `0x0044` | One row of eight `u16` entry offsets per entity |
| After the rows | Shared bytecode |

Entry offsets and jump targets are unsigned 16-bit offsets relative to the start
of the shared bytecode. There is no per-entity bytecode array, and there are no
stored routine lengths. The runtime has storage for 16 entities; the loader does
not clamp `entity_count`, so the compiler rejects more than 16.

### Dialogue bundle (`dialog.bin`)

```text
+0x00  u16 highest_message_id   (0xFFFF: no messages)
+0x02  u16 zero
+0x04  u16 offset[highest + 1]  relative to the bundle
...    one more u16 per message (not interpreted here), then encoded text
```

The compiler never reads or writes dialogue. `dialog.txt` and the comments
after `dialogue.show` statements decode the narrow single-byte glyph table
(`0x10` space, `0x16..0x1F` digits, `0x20..0x39` `A..Z`, `0x3D..0x56`
`a..z`, punctuation). Code `0x01` is rendered as ` / ` (line break), `0x0F XX YY`
as `{0F XX YY}` and other control codes as `{XX}`. The decoding is informational.

## 2. Sections Of `script.xgs`

```text
battle_event N {
  prefix "...";        // only when the prefix is not all zero
  state { ... }
  alias ...;           // only for jumps into the middle of an instruction
  entities { ... }
  shared_code { ... }
  data { ... }
}
```

### `battle_event`

Identifies the Battle Event data index. It is informational; compilation
produces one script resource and `repack`/`override` receive the index
explicitly.

### `state`

Declares the variables observed in operands. The VM owns 512 signed 16-bit
variables; operands address them by **byte offset**, so `at 0x0200` is variable
index 256. Bit 0 of an offset is discarded by the VM.

```text
state {
  engine {
    signed battle_exit_signal at 0x0000;
  }
  script {
    signed counter_0200 at 0x0200;
    new signed retries;
  }
}
```

| Group | Meaning |
|---|---|
| `engine` | Offsets written by resident Battle: `battle_exit_signal` (`0x0000`, set to `0x00FF` when the exit route reaches the Event handoff) and `party0_exit_status`..`party2_exit_status` (`0x0020..0x0024`, bit 15 merged from each party slot's character and Gear status). |
| `script` | Every other offset. Names describe the observed use: `flag_`, `counter_` and `random_` from the writing operation, or the operand name the variable feeds (`x_`, `y_`, `camera_animation_`); otherwise `var_`. |

The whole bank is zeroed when the Event starts. Enemy AI opcode `0x70` can write
indices `0..255` (offsets `0x0000..0x01FE`) between Event updates, so a script
may read a value that no event instruction writes. Retail scripts keep their own
variables at `0x0200` and above. A `new` variable receives the lowest free even
offset from `0x0200` after reserving every declared binding and every `slot()`
reference; the link map reports the allocation.

### `entities`

```text
entities {
  entity 1 {
    events {
      start       -> L_00E5;
      idle        -> L_00E7;
      entry[2]    -> L_00E8;
      entry[3..7] -> L_0000;
    }
    code {
      L_00E5:
      ...
    }
  }
}
```

Entity IDs are consecutive from zero. Each entity has eight entry offsets:

| Entry | Name | Use |
|---:|---|---|
| 0 | `start` | Installed in slot 0 for every entity when the Event starts. |
| 1 | `idle` | Reinstalled as the primary program whenever a script ends with `stop`. Usually a single `stop`. |
| 2..7 | `entry[N]` | Started by other scripts with `flow.start_script*`. |

Retail rows fill entries that no script starts with offset `0`. Those bindings
are preserved but do not make the code at offset 0 belong to that entity.

### Slots and scheduling

Each entity has eight execution slots. Slot 0 is the primary program; slots 1..7
are secondary scripts started by `flow.start_script*(entity, entry, slot_state)`
(opcodes `03..05`). The low five bits of the packed byte select the entry and
become the caller's awaited tag; the high three bits are stored as the started
slot's state. The VM always runs the highest-numbered active slot of an entity,
so a started secondary script preempts the primary program until it ends.

`stop` (`00`) releases the running slot, reinstalls `idle` as the primary
program and ends the entity's dispatch budget for the pass. The scheduler gives
each entity, in execution-order array order, up to four instructions per pass and
renders one Battle frame per entity visit. Polling operations (`flow.wait`,
dialogue, movement, `actor.play_mecha_animation_wait`, start-and-wait, start when
all slots are busy) keep their PC until complete. `flow.wait` counts down only in
cutscene mode (`battle.enter_cutscene_mode()`). `flow.set_priority(-2)` moves an
entity to the front of the execution order. `flow.yield_to_battle()` returns
control to Battle after the current pass; the Event continues on its next
update.

### `code` and `shared_code`

Code reached only by one entity's used entries appears in that entity's `code`.
Code reached by several entities, and orphan code that no entry or jump reaches,
appears once in `shared_code`. Ownership is presentation only: every label is
global and any entity may jump to it.

A block starts at a label and ends with `stop`, `goto`, or an explicit
`fallthrough label;`. Conditional jumps (`if (!(...)) goto`) continue in the same
block.

### `data`

Bytes not reached as code are preserved byte for byte in named objects:

```text
data {
  D_01C3 {
    bytes "00 00 D9 00 00 00 00";
  }
}
```

In retail scripts this is the LZSS group filler after the last routine. Data
objects are placed like code blocks, by name order.

## 3. Coverage

The per-resource report separates instruction coverage from total coverage:

```text
instruction_bytes + data_bytes = classified_bytes = bytecode_size
total_coverage_percent = 100.0
```

Across the 48 retail scripts, 19,966 of 20,134 bytecode bytes are decoded as
instructions (4,615 instructions, 12 orphan blocks) and 168 bytes are preserved
data. No retail jump enters the middle of an instruction; the decompiler would
express such a target as `alias L_xxxx = L_yyyy + N;`.
