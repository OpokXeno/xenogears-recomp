# flags.xml Schema

## Root element
`<flags>` — container for zero or more `<flagVar>` elements.

## `<flagVar>` attributes
| Attribute | Required | Type   | Description |
|-----------|----------|--------|-------------|
| `var`     | yes      | int    | Even byte offset used by the script VM. Divide by two for the underlying u16 slot. |
| `name`    | yes      | string | Human-readable semantic name. |
| `status`  | no       | enum   | `verified` — meaning is explicitly documented in the reference source. `unverified` — meaning is inferred from `setVar`/`getVariable` usage patterns but lacks explicit documentation; included for research only. |

## `<meaning>` child element
Zero or more per `<flagVar>`.
| Attribute | Required | Type   | Description |
|-----------|----------|--------|-------------|
| `value`   | yes      | string | Specific value (hex or decimal) that has a documented meaning. |
| `desc`    | yes      | string | Human-readable description of what this value represents. |

## Memory ranges

- Offsets 0–1022 address the 512 persistent u16 fieldVars mirrored from gameState+0x1930.
- Offsets 1024–2046 address the volatile per-field scratch half of the script VM memory.
- For v80 opcode arguments, bit 0x8000 marks an immediate value and is not part of the value.
