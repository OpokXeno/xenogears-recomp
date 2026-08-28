# Runtime Objects, Allocations, And Ownership

## 1. Ownership Model

```text
Resident SystemMenu
    +-- embedded frame environments and common state
    +-- owner pointers for managers, windows, cursors, and pages
    +-- selected Menu module
            +-- creates and clears child records
            +-- publishes each owner before use
            +-- disables drawing and callbacks before release
            +-- does not reuse stale owners after disable
            +-- Gear Shop: Gear Helper pools and model slots
```

Every allocation has one release path. Embedded records share the lifetime of
their parent. Borrowed pointers never release the referenced storage. Manager
disable paths do not clear owner fields after release; those fields can remain
stale until `SystemMenu` itself is released.

## 2. Resident `SystemMenu`

Resident `MenuMain` allocates and clears `0x1E98` bytes. The shared fields with
stable runtime roles are:

| Offset | Size | Field and owner |
|---:|---:|---|
| `+0x06C` | `0xB4` | Graphics environment 0. |
| `+0x120` | `0xB4` | Graphics environment 1. |
| `+0x1D4` | 4 | Active graphics-environment pointer. |
| `+0x1D8` | `0x78` | Rotation, translation, matrices, and camera/view state. |
| `+0x250` | `0x58` | Transition and view working state. |
| `+0x2D8` | 4 | Frame control state. |
| `+0x2DC` | 4 | Common Menu resource owner. |
| `+0x2E0` | 4 | Secondary resource owner. |
| `+0x2E4` | 4 | Sound resource owner. |
| `+0x308` | 4 | Render parity. |
| `+0x30C` | `0x10` | Character availability. |
| `+0x31C` | 9 | Number-to-digit workspace. |
| `+0x325` | 1 | Current input command; `8` is idle. |
| `+0x327` | 1 | Top-level draw enable. |
| `+0x329` | 1 | Transition state. |
| `+0x32A` | 1 | Menu sound-effect enable. |
| `+0x32C` | 4 | Primary resource/workspace owner. |
| `+0x330` | 4 | Menu resource and dressing-state owner. |
| `+0x33C` | 4 | Shared manager owner. |
| `+0x340` | 4 | Cursor-rendering owner. |
| `+0x344` | 4 | Gold-display owner. |
| `+0x348` | 4 | Overlay packet-state owner. |
| `+0x350` | 4 | Selection and main-entry owner. |
| `+0x354` | 4 | Text-batch owner. |
| `+0x364` | `7 * 4` | Window owners. |
| `+0x380` | `7 * 4` | Window-animation owners. |
| `+0x428` | 4 | Pointer-cursor owner. |
| `+0x43C` | 4 | Scroll-handle owner. |
| `+0x440` | 4 | Shoulder-button UI owner. |
| `+0x444` | `2 * 4` | Arrow-cursor owners. |
| `+0x450` | 4 | Shop-page owner. |
| `+0x454` | 4 | Gear-shop extended-display owner. |
| `+0x458` | `2 * 4` | Resource-state owners. |
| `+0x1DE0` | `4 * 4` | Prompt/string-record owners. |
| `+0x1E20` | 4 | Extended display-state owner. |
| `+0x1E2C` | 4 | Shop-definition pointer. |
| `+0x1E30` | `0x30` | Shop item identifiers. |
| `+0x1E60` | `0x30` | Shop item types. |
| `+0x1E90` | 4 | Gear-shop definition pointer. |
| `+0x1E94` | 1 | Display toggle controlled by command `0x0C`. |
| `+0x1E95` | 1 | Supplemental selector. |

Each module assigns the concrete child type behind each owner. The
owner offset and release discipline remain common.

## 3. Embedded Graphics Environments

Each `0xB4` environment contains:

| Offset | Size | Field |
|---:|---:|---|
| `+0x00` | `0x5C` | `DRAWENV`. |
| `+0x5C` | `0x14` | `DISPENV`. |
| `+0x70` | `0x40` | Sixteen-entry ordering table. |
| `+0xB0` | 4 | Environment state. |

The active pointer alternates between `SystemMenu+0x6C` and
`SystemMenu+0x120`. Both records are embedded and are released with
`SystemMenu`.

## 4. General Menu Owners

General Menu uses paired manager operations: enable allocates and clears the
record; disable releases children and then releases the record. These managers
do not require a `NULL` owner on enable and do not write `NULL` on disable, so an
owner must not be read or passed to a manager again after disable.

| Owner | Allocation | Role | Manager |
|---:|---:|---|---:|
| `+0x32C` | `0x5034` | Memory-card workspace. | Menu `0x801C5B54` |
| `+0x33C` | `0x006C` | Presentation and manager flags. | Menu `0x801C5BB8` |
| `+0x350` | `0x1194` | Main-menu entries and `MoveImage` state. | Menu `0x801C5C1C` |
| `+0x354` | `0x140C` | Menu text batches. | Menu `0x801C5C80` |
| `+0x330` | `0x00CC` | Resource and dressing state. | Menu `0x801C5CE4` |
| `+0x340` | `0x0328` | Cursor rendering. | Menu `0x801C5D48` |
| `+0x344` | `0x0374` | Gold display. | Menu `0x801C5DAC` |
| `+0x348` | `0x015C` | Play-time and packet state. | Menu `0x801C5E10` |
| `+0x39C[0..2]` | `3 * 0x127C` | Three party-card workspaces. | Menu `0x801C5E74` |

Page-specific children are released before the workspace that owns them.

## 5. Enter Name Owners

Enter Name creates its functional top-level records through resident allocation,
clears each complete record, and releases it through the matching manager:

| Owner | Allocation | Role | Manager |
|---:|---:|---|---:|
| `+0x32C` | `0x5034` | Resource state. | Enter Name `0x801C505C` |
| `+0x33C` | `0x006C` | Manager and render flags. | Enter Name `0x801C50C0` |
| `+0x350` | `0x1194` | Selection and `MoveImage` state. | Enter Name `0x801C5124` |
| `+0x354` | `0x140C` | Lifecycle reservation; unused after clear. | Enter Name `0x801C5188` |
| `+0x330` | `0x00CC` | Lifecycle reservation; unused after clear. | Enter Name `0x801C51EC` |
| `+0x348` | `0x015C` | Overlay packet state. | Enter Name `0x801C5250` |
| `+0x1E20` | `0x0DEC` | Name-entry display state. | Enter Name `0x801C52B4` |

The resource record stores resource pointers and the selected resource variant
at `+0x4F7C`.

## 6. Enter Name Display State

The display allocation contains these packet groups and controls:

| Offset | Size | Field |
|---:|---:|---|
| `+0x000` | `0x0A0` | Four FT4 packets for two UI pairs. |
| `+0x0A0` | `0x0A0` | Four dynamic-text FT4 packets. |
| `+0x140` | `0x050` | Two projected-header FT4 packets. |
| `+0x190` | `0xB40` | Seventy-two keyboard FT4 packets. |
| `+0xCD0` | `0x050` | Two selection FT4 packets. |
| `+0xD20` | `0x030` | Two top `LINE_F3` packets. |
| `+0xD50` | `0x030` | Two bottom `LINE_F3` packets. |
| `+0xD80` | `0x020` | Two caret `LINE_F2` packets. |
| `+0xDA0` | `0x020` | Projected-header source quad. |
| `+0xDC0` | `0x020` | Selection source quad. |
| `+0xDE0` | 5 | UI, header, keyboard, selection, and line parity. |
| `+0xDE5` | 1 | Header and selection enable. |
| `+0xDE6` | 1 | Keyboard, name, and caret enable. |
| `+0xDE7` | 1 | Caret blink counter from 0 through 60. |
| `+0xDE8` | 1 | Current encoded character count. |
| `+0xDE9` | 1 | Maximum character count, 9 or 10. |
| `+0xDEA` | 1 | Dynamic-text packet count. |

## 7. Shared Windows

Each of the seven `SystemMenu+0x364` slots owns one `0x720` window record. The
matching `SystemMenu+0x380` slot owns one `0x18` animation record.

| Offset | Size | Window field |
|---:|---:|---|
| `+0x000` | `0x4B0` | Thirty FT4 packets. |
| `+0x4B0` | `0x048` | Two G4 packets. |
| `+0x4F8` | `0x018` | Two draw-mode packets. |
| `+0x510` | `0x200` | Sixteen source quads. |
| `+0x710` | 4 | Loaded primitive count. |
| `+0x714` | 4 | Transform mode. |
| `+0x718` | 4 | Ordering-table depth. |
| `+0x71C` | 1 | Packet parity. |
| `+0x71D` | 1 | Vertical trim and scroll ornament control. |

The animation record stores origin, target and current dimensions, depth, slot,
completion, transform mode, and trim control. Window release disables rendering
before releasing the packet and animation owners.

## 8. Cursor And Prompt Owners

| Owner | Allocation | Contract |
|---|---:|---|
| `SystemMenu+0x428` | `0x14C` | Pointer cursor with eight FT4 packets, enables, projection flags, and parity. |
| `SystemMenu+0x1DE0[0..3]` | Four `0x80` records | Prompt FT4 pair, source quad, upload rectangle, transient raster pointer, and controls. |
| `SystemMenu+0x364[0..6]` | `0x720` each | Window packet owner. |
| `SystemMenu+0x380[0..6]` | `0x18` each | Window animation owner. |

Even prompt records allocate raster surfaces during setup. Adjacent prompt
records can borrow one surface; the owning prompt performs the sole release and
all borrower pointers are then cleared.

## 9. Specialized Manager Contract

Member Change, Enter Name, Shop, and Gear Shop use the same ownership operation
with module-specific record types:

```text
enable:
    owner = HeapAlloc(record_size)
    clear(owner, record_size)

disable:
    disable draw and callback reachability
    release children
    HeapFree(owner)
```

The managers neither test `owner == NULL` before allocation nor clear the owner
after release. Each enable therefore has exactly one matching disable, and no
code may reuse the owner after disable. Each module defines its own sizes and
field meanings. Shared manager order does not merge their runtime types.

## 10. Gear Helper Ownership

| Resource | Ownership contract |
|---|---|
| Animation tracks | Gear Helper `0x801DF5F4` initializes the pool; each track occupies 20 bytes; Gear Helper `0x801DF7A8` returns tracks and `0x801DF668` releases the pool. |
| Trail segments | The pool contains sixteen renderable `0x7C`-byte records and one fallback record. |
| Model slots | Gear Helper `0x801E738C` initializes ten slots; a loaded slot owns mesh, skeleton, packet, script, and auxiliary storage. |
| Image animation | Owns enabled pixel buffers until Gear Helper `0x801E165C`. |
| Deformable mesh | Owns scaled vertices, constraints, packets, and attachments until Gear Helper `0x801E3438`. |

The fallback trail record absorbs allocation pressure after all renderable
records are active. Shutdown releases all ten model slots before the shared
track and trail pools.

## 11. Teardown Order

1. Stop accepting input that creates child objects.
2. Disable page, cursor, prompt, window, and model rendering.
3. Complete the final frame synchronization needed by submitted packets.
4. Release transient raster surfaces and clear borrower pointers.
5. Release page children, cursors, prompts, and windows.
6. For Gear Shop, release model slots and then Gear Helper pools.
7. Release top-level managers in reverse dependency order.
8. Treat released owner fields as stale and do not reuse them.
9. Release `SystemMenu` last; this discards the stale owner fields.
10. Return through the selected module entry to resident `MenuExecute`.

## 12. Function Index

| Address | Function |
|---:|---|
| Resident `0x8001C634` | Allocate and clear `SystemMenu`. |
| Resident `0x80031BDC` | Allocate heap storage. |
| Resident `0x800320E8` | Release heap storage. |
| Resident `0x8003F8E8` | Clear an allocated record. |
| Menu `0x801D397C` | Initialize one shared window slot. |
| Menu `0x801D4EA0` | Release one shared window slot. |
| Enter Name `0x801C9D5C` | Allocate pointer cursors. |
| Enter Name `0x801C9F1C` | Disable, flush, and release pointer cursors. |
| Enter Name `0x801CA400` | Tear down Enter Name resources and state. |
| Member Change `0x801C9748` | Commit party state and release module ownership. |
| Shop `0x801CBB08` | Tear down Shop ownership. |
| Gear Shop `0x801CCD20` | Tear down Gear Shop ownership. |
| Gear Helper `0x801E738C` | Initialize pools and model slots. |
| Gear Helper `0x801E7FD4` | Release all model slots and pools. |
| Gear Helper `0x801E8030` | Release one model slot. |
