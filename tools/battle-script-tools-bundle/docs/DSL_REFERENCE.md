# Battle Event DSL Reference

## 1. Lexical Rules

- `//` starts a comment that runs to the end of the line (outside strings).
  Comments never affect compilation.
- Statements end with `;`. A label is an identifier followed by `:`.
- Numbers are decimal or `0x` hexadecimal; a leading `-` is allowed where the
  field accepts signed values.
- Identifiers match `[A-Za-z_][A-Za-z0-9_]*`. Generated labels are `L_XXXX` for
  code and `D_XXXX` for data, where `XXXX` is the original bytecode offset.

## 2. Values

| Form | Meaning |
|---|---|
| `42`, `0xF3`, `-175` | Immediate. |
| `engine.name`, `script.name` | Variable declared in `state`. |
| `slot(0x0201)` | Raw variable offset. Used for odd offsets and offsets outside the 512-variable bank, which the decompiler never names. |

Each operand has one of these encodings (see the catalog for every opcode):

| Encoding | Accepts | Bytes written |
|---|---|---|
| `v15` | Immediate `0..32767` or variable | Immediate: `0x8000 \| value`; variable: its offset. |
| typed | Immediate `-32768..65535` or variable | Immediate: the 16-bit value and its type-mask bit set; variable: its offset with the bit clear. |
| variable | Variable only | Offset. |
| `u8`, `u16`, `s8` | Immediate in range | Raw value. |

`v15` cannot hold negative or larger immediates. Retail scripts pass negative
coordinates by assigning the value to a variable first:

```text
script.y_0202 = -175;
actor.move_eased(0x13, script.x_0200, script.y_0202, script.z_0204);
```

Typed immediates at or above `0x8000` are rendered as negative numbers because
the VM's variables and ordered comparisons are signed 16-bit.

## 3. Control Flow

| Statement | Opcode | Meaning |
|---|---:|---|
| `stop;` | `00` | End the running slot and reinstall the entity's `idle` entry. Ends the block. |
| `goto label;` | `01` | Continue at label. Ends the block. |
| `if (!(condition)) goto label;` | `02` | Continue when the condition is true; otherwise jump. |
| `fallthrough label;` | none | Continue at label. Emitted as nothing when label is placed next, otherwise as `goto`. Must end its block. |

There are no calls or returns in this VM. Parallelism comes from starting
scripts in other slots or entities:

```text
flow.start_script(entity, entry, slot_state);            // 03
flow.start_script_wait_started(entity, entry, slot_state);  // 04
flow.start_script_wait_finished(entity, entry, slot_state); // 05
```

`entry` is `0..31` (values above 7 read past the entity's row, as in the VM) and
`slot_state` is `0..7`. Retail scripts mostly use slot state 3.

Every code block must end with `stop`, `goto`, or `fallthrough`. A label in the
middle of straight-line code requires an explicit `fallthrough` before it.

## 4. Conditions

`flow.jump_unless` compares two typed operands with a selector stored in the low
nibble of its mask byte:

| Condition | Selector |
|---|---:|
| `a == b` | 0 |
| `a != b` | 1 |
| `a > b` | 2 (signed) |
| `a < b` | 3 (signed) |
| `a >= b` | 4 (signed) |
| `a <= b` | 5 (signed) |
| `a & b` | 6, true when any bit is shared |
| `compare(a, b, 7)` | 7, a second `!=` encoding |
| `a \|\| b` | 8, true when either is nonzero |
| `mask_contains(mask, entity)` | 9, Battle entity bit present in target mask |
| `mask_lacks(mask, entity)` | 10, Battle entity bit absent |
| `compare(a, b, N)` | any selector; 11..15 are always false |

Bits `0x20` and `0x10` of the mask byte are not read. When an original
instruction sets them, the condition is rendered as
`compare(a, b, N, reserved_flags=0x10)` so the bytes are preserved.

## 5. Assignments

| Statement | Opcode |
|---|---:|
| `x = value;` | `06` |
| `x = true;` / `x = false;` | `07` / `08` |
| `x += value;` / `x -= value;` | `09` / `0A` |
| `x \|= value;` | `0B` |
| `x &= ~value;` | `0C` |
| `x++;` / `x--;` | `0D` / `0E` |
| `x &= value;` | `0F` |
| `x ^= value;` | `11` |
| `x <<= count;` / `x >>= count;` | `12` / `13`; `count` must be a variable |
| `x = random();` | `14`, `0..0x7FFF` |
| `x = random(N);` | `15`, fixed `u16` bound |
| `x *= value;` / `x /= value;` | `16` / `17`; division by zero stores `0xFFFF` |

Opcode `10` duplicates `0B`; it is written `state.or_bits_alternate(x, value);`.

The type-mask byte of `06..11` also has a bit for the destination field
(`0x80`), which the handlers do not read. For `16` and `17` that bit is read: the
left factor is the destination field decoded by its type. Whenever a mask has
bits the concise form cannot express, the decompiler writes the function form:

```text
state.assign(script.flag_0210, 1, reserved_flags=0x80);
```

## 6. Operations

All other opcodes are `namespace.operation(arguments);` with positional arguments
in the order listed in the catalog. Every opcode, including the ones with a
concise form, also accepts its function form (except `flow.jump_unless`, which
is always written as an `if`). Two keyword arguments exist:

| Keyword | Meaning |
|---|---|
| `reserved_flags=N` | Type-mask bits not implied by the operands. |
| `reserved=N` | The unread second byte of `flow.set_priority`; omitted when zero. |

Identifiers such as `0xF3` in actor, sprite and character arguments are the
VM's alias values: `0xF3..0xF5` are party slots and other values resolve as
described in [`OBJECTS_AND_NAMESPACES.md`](OBJECTS_AND_NAMESPACES.md).

## 7. Declarations

```text
battle_event 7 {
  state {
    engine { signed battle_exit_signal at 0x0000; }
    script {
      signed counter_0200 at 0x0200;
      new signed retries;
    }
  }
  alias L_0017 = L_000D + 2;
  entities { ... }
  shared_code { ... }
  data { D_0300 { bytes "00 00"; } }
}
```

Bindings must be even offsets in `0x0000..0x03FE` and unique. `new` declarations
belong to `script`. `alias` defines a label at a byte offset inside another
label's instruction; it exists only to express retail jumps into the middle of
an instruction, which the current corpus does not contain.

Event bindings accept `start`, `idle`, `entry[N]` and `entry[A..B]`, each bound
once, to a label or a raw number. Unbound entries share one generated `stop`.

## 8. Comments And Diagnostics

```text
L_00E8:
// event: entity 1: entry[2]
dialogue.show(0, 0x00); // 00F3: 18 00 00 00 | "Fei / \"Reinforcements, / huh?!\""
```

| Comment | Meaning |
|---|---|
| `// event: ...` | Entries of each entity that start here (unused entries are omitted). |
| `// PPPP: XX ...` | Original offset and bytes. |
| `\| "text"` | Decoded message for dialogue operations. |
| `// orphan: ...` | Code that no stored entry or jump reaches. |
| `// Diagnostic: ...` | Analysis findings, such as a flow path reaching an invalid opcode. |

All of them are informational and ignored by the compiler.
