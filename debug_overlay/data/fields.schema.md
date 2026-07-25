# fields.xml Schema

## Root Element

- `<fields>` — container for all field entries.
  - Attribute `count` (integer): total number of `<field>` children. Must match actual count.

## Child Elements

- `<field>` — represents a single map/field.
  - Attribute `id` (integer): unique field identifier. Sparse — not all integers are used.
  - Attribute `name` (string): human-readable field name. XML-escaped, UTF-8, trimmed whitespace.
  - Child elements (optional): up to 4 `<entity>` elements when script entities are present.
  - Self-closing (`<field .../>`) when no entities are attached.

- `<entity>` — named script entity within a field (e.g. "Director", interactables, cutscene actors).
  - Attribute `id` (integer): entity slot index within the field.
  - Attribute `name` (string): entity name. XML-escaped, UTF-8, trimmed whitespace.

## Notes

- Field IDs are sparse (e.g. 0–60, 70, 80–329, 600–729). Gaps are normal.
- Entity children are optional and omitted when a field has no named script entities.
- All text values are trimmed of leading/trailing whitespace before XML escaping.
- The file encoding is UTF-8.
