# Concepts And Lifecycle

## 1. What The World Map System Owns

One World invocation owns a selected configuration and all live state needed to
travel through it or stage a scripted scene:

```text
World Map system
    |
    +-- selected configuration
    |     section archive, textures, audio, two terrain traversal files
    |
    +-- streamed world
    |     9 x 9 identifiers, 256 chunk pointers, pending sector batches
    |
    +-- runtime tasks
    |     party, Gears, Yggdrasil, cameras, UI, effects, scene directors
    |
    +-- shared state
          input, camera, radar, encounters, exits, transitions, render contexts
```

The ordinary travel mode and the scripted World scenes use the same terrain,
streaming, model, effect, camera, task, and presentation foundations. Their task
lists and top-level update functions determine which parts are active.

## 2. Vocabulary

| Term | Meaning |
|---|---|
| **World mode** | Top-level selector `0..18` choosing resource policy and lifecycle callbacks. |
| **Configuration** | One directory `0x24` file set with a section archive, textures, audio, and terrain. |
| **Travel form** | Current controllable representation: on-foot party, party Gear, or Yggdrasil. |
| **Task** | One `0x80`-byte slot in the cooperative World scheduler, with callbacks and family-specific state. |
| **Terrain slot** | One stored `0x800`-byte record representing four adjacent `8 x 8` quad tiles and shared metadata. |
| **Tile** | A `9 x 9` sample grid that renders an `8 x 8` quad area. |
| **Ground cell** | One material cell from the slot's `16 x 16` material table. |
| **Streaming grid** | Wrapped `9 x 9` array of terrain-slot identifiers centered around travel. |
| **Exit** | Rectangular trigger selected from a travel-form-specific exit group. |
| **Encounter region** | Four-bit terrain-sample value selecting one of 16 region records, each with 16 formations. |
| **Runtime snapshot** | Copy of tasks and shared travel state used around Battle and volatile reconstruction. |

A mode, configuration, and travel form are separate selectors. Modes `0..7` all
use the progress-selected configuration family, while travel form is derived
from persistent party and vehicle state.

## 3. Top-Level Lifecycle

`WorldMapOverlayEntryPoint` at `0x80070CFC` performs the complete invocation:

```text
synchronize cache, GPU, display, and VSync callback
                        |
                        v
initialize geometry and first-entry defaults
                        |
                        v
open directory 0x24 and allocate paired render state
                        |
                        v
derive travel form and apply mode/configuration table
                        |
                        v
resource preload -> one-shot mode setup -> central task/frame loop
                        |
                        v
mode teardown and route to Field, Battle, Menu, or resident state
```

The selected mode supplies three callbacks:

| Callback | Responsibility |
|---|---|
| Load | Queue party or common resources needed before mode initialization. |
| Update | Construct the mode runtime and run its main loop or scripted sequence. |
| Free | Stop audio, release mode resources, save any required state, and choose a destination. |

The overlay entry calls the update and task dispatcher while the shared running
state remains active. The resulting exit state has three principal values:

| Exit state | Resident route |
|---:|---|
| `0` | Field |
| `1` | Battle |
| Other terminal state | Fixed resident-state-0 fallback and blank-screen path |

## 4. Fresh Entry And Restored Entry

The first overlay entry initializes persistent World defaults, party slots,
camera values, saved vehicle state, and module-owned flags. Later entries retain
those values and choose one of two broad initialization paths:

| Entry path | Position source | Runtime treatment |
|---|---|---|
| Fresh travel | Field-supplied World position or indexed spawn table | Build tasks, streaming grid, encounters, models, and effects from configuration data. |
| Restored travel | Transient World snapshot | Restore task and shared simulation state, then rebuild volatile resources and rearm their owning task initializers. |

`WorldMapSetupPositionFromField` at `0x80073398` reads the saved Field position
for walking and Gear travel. Vehicle-oriented travel reads the separately saved
Yggdrasil position. `WorldMapSetupPositionFromFileData` at `0x80073448` resolves
an indexed spawn and uses `(0,0,0)` when the requested ID is absent.

## 5. Ordinary Runtime Construction

`WorldMapInitializeModeRuntime` at `0x80072238` stages ordinary construction:

1. Initialize paired graphics contexts and preserve the incoming framebuffer.
2. Run the startup fade.
3. Read the primary section archive and both texture bundles.
4. Relocate archive sections and establish spawns, exits, models, effects,
   dialogue, animated textures, and encounter pointers.
5. Allocate the 64-entry task array and shared rendering buffers.
6. Select the starting position and initialize the `9 x 9` streaming grid.
7. Load the terrain slots required around that position.
8. Create models, effects, party tasks, cameras, radar, exit UI, and ground task.
9. Initialize the encounter schedule and enter the frame loop.

The initial streaming load is blocking because terrain queries and task
initializers immediately require valid altitude and material data. Subsequent
grid changes enqueue only entering rows, columns, and corner repairs.

## 6. Ordinary Frame Lifecycle

One travel frame has four coordinated layers:

```text
controller and global frame state
              |
              v
64 task slots in ascending index order
              |
              +-- actors and vehicles update movement and exits
              +-- camera updates tracking and streaming origin
              +-- ground task updates effects and emits render producers
              +-- UI tasks update radar, prompts, and fades
              |
              v
GPU synchronization and environment swap
              |
              v
pause/controller/encounter/menu arbitration
              |
              v
animated textures and DrawOTag
```

Task order is semantic. The leader updates before followers, party Gears update
before the Yggdrasil, cameras update before the ground producer, and the exit
prompt observes the exit selected by the current travel task.

## 7. Teardown Ownership

`WorldMapOrdinaryModeTeardown` at `0x8007299C` releases the ordinary runtime
in dependency order: audio and callbacks, tasks, models and collision state,
effects and packet pools, streamed chunks and streaming queues, relocated
resource buffers, ordering tables, and the main task allocation.

Battle preserves a transient snapshot before the ordinary runtime is released.
Menu keeps the live task, model, effect, and terrain simulation allocations and
temporarily releases only selected presentation buffers. Direct Field exits
persist destination and travel values instead. Scripted modes own analogous
teardown callbacks because their models, effects, command tables, and destination
fields differ.

## 8. System Boundaries

The World Map system decides where objects are, how they move, what terrain and
exit they occupy, when an encounter starts, and which mode receives control.
The packet-level behavior of terrain, models, clouds, sky, horizon, radar,
sprites, particles, fades, and ordering tables is documented in
[`../graphics/05-field-and-world-rendering.md`](../graphics/05-field-and-world-rendering.md).
The serialized graphics payloads are documented in
[`../graphics/03-resource-formats.md`](../graphics/03-resource-formats.md).
