# flags.xml Schema

## Root element
`<flags>` — container for zero or more `<flagVar>` elements.

## `<flagVar>` attributes
| Attribute | Required | Type   | Description |
|-----------|----------|--------|-------------|
| `var`     | yes      | int    | Field variable index (the raw index used by `setVar`). |
| `name`    | yes      | string | Human-readable semantic name. |
| `status`  | no       | enum   | `verified` — meaning is explicitly documented in the reference source. `unverified` — meaning is inferred from `setVar`/`getVariable` usage patterns but lacks explicit documentation; included for research only. |

## `<meaning>` child element
Zero or more per `<flagVar>`.
| Attribute | Required | Type   | Description |
|-----------|----------|--------|-------------|
| `value`   | yes      | string | Specific value (hex or decimal) that has a documented meaning. |
| `desc`    | yes      | string | Human-readable description of what this value represents. |
