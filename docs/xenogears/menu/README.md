# Xenogears Menu System

The Xenogears Menu system provides the normal game menu, party formation,
name entry, shops, Gear tuning, memory-card operations, and disc-change flow.
Resident code creates the shared runtime state and selects a mode; the selected
Menu module owns interaction, presentation, and teardown until control returns
to Field, World, or the resident state dispatcher.

## Reading Order

1. [`01-concepts-modules-and-lifecycle.md`](01-concepts-modules-and-lifecycle.md)
   defines the system boundary, resident coordinator, five Menu modules, Gear
   Helper, entry points, and complete lifecycle.
2. [`02-entry-modes-handoffs-and-coordination.md`](02-entry-modes-handoffs-and-coordination.md)
   specifies modes 0 through 6, request arguments, dispatch, and Field and World
   handoffs.
3. [`03-resource-and-string-formats.md`](03-resource-and-string-formats.md)
   specifies LZSS, PAK-LZ, offset tables, string bundles, the bitmap font, and
   the PlayStation save header.
4. [`04-runtime-objects-allocations-and-ownership.md`](04-runtime-objects-allocations-and-ownership.md)
   defines `SystemMenu`, runtime records, allocation ownership, and teardown
   order.
5. [`05-frame-input-navigation-and-shared-ui.md`](05-frame-input-navigation-and-shared-ui.md)
   follows one frame through input translation, navigation, transitions,
   windows, parity selection, and presentation.
6. [`06-general-menu-pages-and-gameplay.md`](06-general-menu-pages-and-gameplay.md)
   specifies the seven-entry General Menu and its gameplay pages.
7. [`07-memory-card-save-load-and-disc-flows.md`](07-memory-card-save-load-and-disc-flows.md)
   specifies memory-card operations, save/load state, and disc-change flows.
8. [`08-member-change-and-party-formation.md`](08-member-change-and-party-formation.md)
   specifies active-party and benched-character formation changes.
9. [`09-enter-name-editor-and-encoding.md`](09-enter-name-editor-and-encoding.md)
   specifies the name editor, keyboard, length rules, and encoding conversion.
10. [`10-shop-inventory-and-transactions.md`](10-shop-inventory-and-transactions.md)
    specifies ordinary shop inventories, previews, purchases, and sales.
11. [`11-gear-shop-tuning-and-preview.md`](11-gear-shop-tuning-and-preview.md)
    specifies Gear shops, tune-up transactions, model preview, and Gear Helper.
12. [`12-persistence-return-teardown-and-function-map.md`](12-persistence-return-teardown-and-function-map.md)
    specifies persistence, return values, final teardown, and the function map.

## Address Convention

General Menu, Member Change, Enter Name, Shop, and Gear Shop share the overlay
region beginning at `0x801C5000`; only the selected module occupies it. An
address in that region is therefore always qualified by its module, such as
Menu `0x801C62A8` or Enter Name `0x801CBDBC`. Resident and Gear Helper addresses
use the `Resident` and `Gear Helper` qualifiers. A notation such as `+0x325`
means an offset from the runtime object named by the surrounding text.

Unless a format states otherwise, integers are little-endian and measurements
are in bytes.
