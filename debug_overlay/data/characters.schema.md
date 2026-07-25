# characters.xml Schema Documentation

## Root Element: `<data>`

The root element contains two required child sections: `<characters>` and `<gears>`.
An optional `<items>` section may be added when a clean item table is available.

---

## `<characters>`

Container for the playable character roster.

### Child Element: `<character>`

Represents one playable character slot.

| Attribute | Required | Description |
|-----------|----------|-------------|
| `id`      | Yes      | Zero-based slot index. Must be unique within `<characters>`. |
| `name`    | Yes      | Canonical display name. |
| `alias`   | Yes      | Internal code spelling / alternate identifier. |

---

## `<gears>`

Container for the gear (mecha) roster.

### Child Element: `<gear>`

Represents one gear slot.

| Attribute | Required | Description |
|-----------|----------|-------------|
| `id`      | Yes      | Zero-based slot index. Must be unique within `<gears>`. |
| `name`    | No       | Display name. Omit when the slot has no known name. |

---

## `<items>` (Optional)

Container for the item table. Included only when a clean, complete item ID→name table is available.

### Child Element: `<item>`

Represents one item.

| Attribute | Required | Description |
|-----------|----------|-------------|
| `id`      | Yes      | Item ID. Must be unique within `<items>`. |
| `name`    | Yes      | Display name. |

---

## Validation Rules

1. `id` attributes must be non-negative integers.
2. `id` values within each section must be strictly sequential starting from 0 with no gaps.
3. `name` and `alias` attributes must be non-empty strings when present.
4. The `<characters>` section must contain exactly 11 entries with ids 0–10.
5. The `<gears>` section must contain exactly 20 entries with ids 0–19.
