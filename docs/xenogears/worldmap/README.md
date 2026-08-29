# Xenogears World Map System

The World Map system runs Xenogears' large-scale travel and the scripted scenes
that use the same terrain, model, effect, and camera runtime. It selects a world
configuration from story progress or an explicit mode, streams the terrain around
the party, updates travel and encounters, and hands control to Field, Battle, or
Menu when an action completes.

This documentation describes the USA Disc 1 World Map system. All function names
are descriptive names assigned by this project.

## Reading Order

1. [`01-concepts-and-lifecycle.md`](01-concepts-and-lifecycle.md) defines world
   configurations, modes, travel forms, tasks, terrain slots, and the complete
   entry-to-exit lifecycle.
2. [`02-resources-formats-and-configuration.md`](02-resources-formats-and-configuration.md)
   documents the directory `0x24` file sets, progress selection, section archive,
   spawns, exits, encounters, models, textures, audio, and terrain files.
3. [`03-runtime-state-and-tasks.md`](03-runtime-state-and-tasks.md) documents the
   64-entry cooperative task array, polymorphic family layouts, command mailbox,
   task identities, effect ownership, cleanup, and transient runtime snapshot.
4. [`04-frame-input-camera-and-party.md`](04-frame-input-camera-and-party.md)
   follows an ordinary frame through controller sampling, task execution, camera
   and radar updates, party following, menus, and presentation.
5. [`05-terrain-streaming-movement-and-collision.md`](05-terrain-streaming-movement-and-collision.md)
   explains the toroidal coordinate space, the `9 x 9` streaming grid, terrain
   sampling, ground classes, movement acceptance, edge sliding, model collision,
   and dynamic collision cylinders.
6. [`06-gears-yggdrasil-and-travel.md`](06-gears-yggdrasil-and-travel.md) covers
   walking, party Gears, boarding, disembarking, the Yggdrasil flight model,
   takeoff, landing, and vehicle-owned exits.
7. [`07-encounters-exits-transitions-and-persistence.md`](07-encounters-exits-transitions-and-persistence.md)
   follows random encounters, exit prompts, Field and Battle handoffs, Menu
   restoration, persistent travel state, and teardown.
8. [`08-scripted-modes-and-function-map.md`](08-scripted-modes-and-function-map.md)
   catalogs the ordinary and scripted top-level modes, their shared command
   language, major cinematic families, and subsystem entry points.

## Core Cardinalities

| Object | Quantity | Lifetime |
|---|---:|---|
| Active top-level World mode | 1 | One overlay invocation |
| Runtime task slots | 64 | One loaded World runtime |
| Ordinary party members | Up to 3 | Current party composition |
| Ordinary party Gear tasks | Up to 3 | Current party and Gear availability |
| Terrain configuration dimensions | `16 x 16` slots | Selected file set |
| Streamed terrain grid | `9 x 9` slot identifiers | Recentered around travel |
| Terrain slot table | 256 pointers | Selected configuration |
| Pending stream batches | 16 | Streaming queue ring |
| Reads per stream batch | Up to 88 | One unpublished or queued batch |
| Dynamic collision cylinders | 32 | Rebuilt during task dispatch |
| Effect owners | 64 | Task or scene identity |
| Emitters per effect owner | 8 | Archive effect definition pool |
| Live particle records | 256 | One ordinary or scripted runtime |
| Cloud movement records | 80 | One ordinary or scripted runtime |
| Animated texture streams | 2 in set 1, 3 in set 2 | Primary World archive |
| Encounter regions | 16 | Primary World archive |
| Encounter formations per region | 16 | Region selection record |
| Encounter countdown slots | 16 | Transient travel state |

## Coordinate Conventions

| Value | Representation |
|---|---|
| Live World position | Signed `20.12` fixed point |
| Serialized spawn or exit position | Signed integer World coordinate |
| Heading | 12-bit angle, one turn = `0x1000` |
| Terrain slot coordinate | Position divided by `0x800000` |
| Terrain sample height | Signed byte scaled by eight, then converted to live fixed point |
| Streamed slot payload | First `0x710` bytes of a stored `0x800`-byte record |
| Runtime structure offset | Start of that runtime object |

World X and Z wrap at the selected configuration dimensions. Shortest-path
movement, camera tracking, streaming, models, sprites, effects, and radar all
apply the same toroidal relationship.

The documented World image is loaded at `0x8006FAF0`, has size `180422`, and has
SHA-256:

```text
4c15fd32b3a03d7cd5ea4403dcaabc70abaf99b6aaca65d63d6866803edaac70
```
