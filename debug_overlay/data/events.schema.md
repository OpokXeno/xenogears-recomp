# events.xml Schema

## Root element
`<events>` — container for zero or more `<event>` elements.

## `<event>` attributes
| Attribute | Required | Type   | Description |
|-----------|----------|--------|-------------|
| `id`      | yes      | int    | Unique event identifier within the file. |
| `name`    | yes      | string | Human-readable event / cutscene name. |
| `mapId`   | yes      | int    | Target field ID. Must resolve against the known field ID set (0–729). |
| `entryPoint` | yes   | int    | Value written to `var[2]` (FieldEntryPoint) before loading the field. |
| `status`  | yes      | enum   | `verified` — both map and progress semantics are documented in the reference. `unverified` — map is known but the exact progress preset is speculative or the field is flagged disc-2 / hangs; these ship disabled until Ghidra verification. |

## `<varWrite>` child element
Zero or more per `<event>`.
| Attribute | Required | Type | Description |
|-----------|----------|------|-------------|
| `var`     | yes      | int  | Field variable index to overwrite. |
| `value`   | yes      | int  | Value to write (decimal or hex in source, stored as string). |
