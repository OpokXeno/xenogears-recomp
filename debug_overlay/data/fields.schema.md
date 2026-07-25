# fields.xml Schema

## Root Element

- `<fields>` — container for all field entries.
  - Attribute `count` (integer): total number of `<field>` children. Must match actual count.

## Child Elements

- `<field>` — represents a single map/field.
  - Attribute `id` (integer): unique field identifier in the complete 0–729 range.
  - Attribute `name` (string): human-readable field name. XML-escaped, UTF-8, trimmed whitespace.
  - Child elements (optional): up to 4 `<entity>` elements when script entities are present.
  - Self-closing (`<field .../>`) when no entities are attached.

- `<entity>` — named script entity within a field (e.g. "Director", interactables, cutscene actors).
  - Attribute `id` (integer): entity slot index within the field.
  - Attribute `name` (string): entity name. XML-escaped, UTF-8, trimmed whitespace.

## Notes

- The table contains every field ID from 0 through 729.
- Entity children are optional and omitted when a field has no named script entities.
- All text values are trimmed of leading/trailing whitespace before XML escaping.
- The file encoding is UTF-8.
