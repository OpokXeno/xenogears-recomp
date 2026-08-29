# Loading Resources And Formations

## 1. Format Conventions

This chapter follows an encounter from Field or World Map selection into a
32-byte formation, then through enemy, arena, party, Event, and Result loading.
The complete runtime lifecycle is in
[`Concepts And Lifecycle`](01-concepts-and-lifecycle.md).

File IDs are one-based, directory IDs belong to the indexed filesystem,
multibyte values are little-endian, and `0x7F` in the low seven bits of an
identity selector marks an empty slot.

## 2. Two Routes To A Formation

### 2.1 Field route

Field section 6 contains 16 fixed 32-byte formations followed by 16 one-byte
weights:

```text
+0x000  Formation formations[16]  /* 0x200 bytes */
+0x200  uint8 weights[16]         /* 0x010 bytes */
```

Field random encounters maintain map-local countdowns. Held directional input
advances them through the player-control update. On expiration, Field sums the
weights and scales a random value across `0..sum(weights)`. Zero continues Field
travel; the remaining values select one of the 16 formations cumulatively.

Explicit opcode `71` selects a requested battle after transition readiness.
Opcode `FE 84` also records a post-Battle Field destination and script value.
Random and explicit paths write a selected index for the resident section-6
formation table. Random selection, opcode `71`, and opcode `FE 84` with
destination `0x7FFF` save the ordinary transient Field snapshot. Opcode `FE 84`
with a concrete destination installs its post-Battle continuation and suppresses
that snapshot. Each path returns the Battle route to the resident dispatcher.

### 2.2 World Map route

Each World region pointer addresses a `0x260`-byte record. It contains 16
formations and six serialized weight rows; the ordinary World selector consumes
the first four progress-sensitive rows:

```text
+0x000  Formation formations[16]  /* 0x200 bytes */
+0x200  uint8 progress_weights[6][16]
+0x260  end of selected region record
```

Rows 4 and 5 at `+0x240..+0x25F` preserve the serialized six-row layout but are
outside the ordinary selector's four progress bands.

`WorldMapInitializeEncounterSchedule` at `0x80075228` clears 16 countdown slots,
activates one countdown, and sets the travel interval to 384 or 768 movement
updates according to bit `0x4000` of persistent travel/vehicle state. The
movement updater at `0x8007528C` generates distinct countdowns in `1..interval`,
decrements them on accepted movement updates, and reports expirations to the
World frame coordinator.

`WorldMapSelectRandomEncounter` at `0x80075E7C` resolves the current region and
chooses a weight row from total game progress:

| Progress | Weight row |
|---:|---:|
| `0..53` | 0 |
| `54..200` | 1 |
| `201..339` | 2 |
| `340+` | 3 |

It sums the row and returns zero when the total is zero. For a positive total,
it draws in `1..sum(weights)`, subtracts cumulative weights until one formation
remains, copies all 16 region formations into the resident Battle encounter
buffer, writes the chosen formation index to the selected-formation request
byte, and returns one.

The World frame coordinator accepts that success while travel control, menu,
transition, and world-action gates are clear. It copies the three party
Gear-presentation values to the resident handoff slots, clears the World running
state, and raises the World-to-Battle route request.

Before leaving World, `WorldMapSaveRuntimeState` at `0x80075460` snapshots travel
position, world entities, camera and movement state, plus the encounter interval,
active count, schedule counter, and countdown array. An ordinary World return
selects resident module 3. World reloads its travel resources and
`WorldMapRestoreRuntimeState` at `0x8007565C` restores the snapshot before travel
continues. Battle Event return operations can instead commit a Field or Movie
destination for the resident dispatcher.

### 2.3 Shared Battle input

Both routes converge here:

```text
Field formation or World region formation
                    |
                    v
        resident Battle request state
      selected index + 16 formation records
                    |
                    v
       BattleMain materializes one record
          as the active 32-byte formation
          + enemy set + arena + policy
                    |
                    v
            staged Battle loader
```

Field uses the section-6 table already present in resident memory. World copies
the current region's 16 records into that same table. Both routes write the
selected index, and `BattleMain` copies `table[index]` into the active formation
configuration. The ordinary origin snapshot remains resident for its matching
return route.

### 2.4 Request globals

The encounter origins communicate with Battle through a compact set of named
handoff values:

| Request global | Role |
|---|---|
| Encounter formation table | Holds the 16 resident formation records available to the request origin. Field section 6 supplies it directly; World copies the current region table into it. |
| Selected formation index | Selects the 32-byte record that `BattleMain` copies into the active configuration. |
| Party Gear-presentation slots | Carry each active party slot's World presentation mode into Battle setup. |
| Post-Battle Field destination and script value | Carry the optional continuation selected by Field opcode `FE 84`. |
| Battle route request | Transfers control from the active Field or World coordinator to Battle. |

World selection writes the formation table and selected index together before
raising its route request. Field random and explicit selection update the index
for the resident section-6 table through the Field coordinator.

## 3. Formation Record

| Offset | Size | Meaning |
|---:|---:|---|
| `+0x00` | 1 | Enemy-set index selecting a definition and visual pair. |
| `+0x01` | 1 | Battle policy: bit `0x08` suppresses Result presentation and the ordinary post-combat gray fade, bit `0x10` selects forced party presentation, bit `0x20` enables Battle Event, bit `0x40` disables calling Gears, and bit `0x80` disables escape. |
| `+0x02` | 1 | Arena ID selecting the environment and initialization pair. |
| `+0x03` | 1 | Battle Event data index used with policy bit `0x20`. |
| `+0x04` | 3 | Normal-character placement selectors for party slots `0..2`; low seven bits select a formation position. |
| `+0x07` | 1 | Zero-filled structural alignment separating party placement from the eight enemy lanes. |
| `+0x08` | 8 | Enemy definition selectors for slots `3..10`; low seven bits select definition `0..7`, bit 7 selects Gear-scale realization, and low value `0x7F` marks an empty lane. |
| `+0x10` | 8 | Enemy lane flags for slots `3..10`; bit 7 starts the lane hidden, inactive, and excluded from ordinary targeting, victory, and reward participation until runtime activation, while bit 0 copies into per-slot startup marker `+0x05` in the `0x1C`-byte entity-layout record. |
| `+0x18` | 8 | Enemy arena-position and facing selectors; low seven bits select the position and bit 7 selects the initial facing. |

Party members already represented as Gears use the Gear placement path. This
choice comes from persistent party state. Each enemy lane is evaluated
independently, allowing occupied lanes to share a definition while retaining
separate combat state.

## 4. Eleven-Slot Realization

### 4.1 Party slots

`BattleLoaderLoadPartyResources` at `0x801E5384` derives up to three active party
IDs from persistent membership and eligibility state. It copies each selected
character's `0xA4`-byte persistent record and the selected Gear's `0xA4`-byte
record into the larger per-slot Battle representation. Empty destinations
receive the `0x7F` identity marker.

Forced presentation can select a different party or Gear identity, so selected
character ID, selected Gear ID, and combatant slot remain separate values.

### 4.2 Enemy slots

`BattleLoaderConfigureEntityLayout` at `0x801E4160` maps formation lane `i` to
combatant slot `3 + i`. Each occupied lane receives its definition selector,
character-or-Gear scale, placement selector, occupancy state, and formation-row
membership. Formation `+0x10` bit 7 initializes a separate inactive/reserve state;
enemy scripts can activate or deactivate that lane during combat.

`BattleLoaderLoadEnemyDefinitions` at `0x801E4870` copies the chosen definition
into an independent `0x170`-byte entity record. Eight lanes can therefore share
one definition while keeping separate HP, statuses, script values, and action
state.

### 4.3 Arena positions

The arena initialization resource supplies normal-character and Gear position
tables. Repeated occupants at one normal position receive successive coordinate
pairs. Gear placement selectors index the Gear table directly. The loader also
builds occupied-row masks consumed by target selection and enemy scripts.

## 5. The Loading Journey

The indexed filesystem groups the encounter into a small sequence of related
stops:

| Directory | Stage | Files selected during Battle |
|---:|---|---|
| `0x0C` | Bootstrap | Battle sequence, common 38-member archive, loader image, and game-over sequence. |
| `0x0D` | Encounter cast | One enemy definition and visual pair chosen by the formation's enemy-set index. |
| `0x0F` | Arena | One environment and initialization pair chosen by arena ID. |
| `0x2D` | Character arrivals | Character packages, deathblow packages, voices, sequences, and sample groups. |
| `0x29` | Gear arrivals | Party Gear model and animation pairs plus associated weapon resources. |
| `0x2C` | Shared presentation | Common sprite, effect, sequence, and sample resources. |
| `0x2A` | Actions | Ether and deathblow animation resources loaded as actions request them. |
| `0x0E` | Visual effects | One selector-addressed Battle effect image at a time. |
| `0x20` | Battle Event | Event image, dialogue and GUI packets, event data, and sequence resources. |
| `0x10` family | Completion | Result, Gear helper, and setup support selected by logical file ID. |

The journey starts with common Battle data while the enemy and arena pairs are
queued together. Arena installation creates the scene, enemy definitions create
the combat records, and the enemy visual package begins the callback-driven
arrival sequence. Character and Gear packages then join according to occupied
party slots. Shared sprites and music complete the entrance gate. Event
resources enter after loader startup, and Result resources enter after combat.

Logical IDs in the `0x10` family can cross physical FAT directory boundaries;
the indexed directory and file pair remains the lookup identity.

## 6. Common Battle Archive

The common 38-member compressed archive supplies the long-lived Battle toolkit:

| Member | Content and use |
|---:|---|
| 0 | Polygon-composed Battle font. |
| 1 | Battle UI image and CLUT atlas. |
| 2 | Battle inventory data mirrored by general menu resources. |
| 3 | Enemy attack-power data. |
| `4..14` | Character attack, deathblow, and Ether data. |
| 15 | Item text. |
| `16 + Gear ID` | Gear attack and ability data for the active Gear. |
| 35 | Twelve portrait TIMs. |
| 36 | Player weapon data. |
| 37 | Attack and status-name strings. |

The party loader expands fixed UI, command, attack, text, and portrait regions,
uploads the three active portraits, retains the members used during combat, and
releases temporary expansion storage.

## 7. Enemy Pair

For enemy-set index `n`, the pair is:

```text
definition_file = 2 + n * 2
visual_file     = 3 + n * 2
```

The definition file begins with eight script-block offsets, carries the enemy
definitions at `+0x32` with stride `0x170`, and retains the enemy text and name
region used later in Battle. Each selected script block begins with four
relative entries: the turn program, clone-retained pointer, optional reaction
program, and optional end-turn program.

For every occupied lane, the definition loader:

1. Copies the selected definition into the lane's entity record.
2. Relocates the four script entries.
3. Records the available optional entries.
4. Clears the lane's byte, word, and dword script-register banks and
   resets its interpreter state.

The paired visual package stays with the startup task until enemy sprite sheets,
animations, shared visuals, or Gear meshes have been installed and the visual
actors have been created.

## 8. Arena Pair

For arena ID `n`, the pair is:

```text
environment_file    = 6 + n * 2
initialization_file = environment_file + 1
```

The environment file supplies textures, models, bones, panorama and ground
objects, lighting, and matrices. The initialization file supplies placement,
camera, lighting, background, and terrain data.

`BattleLoaderLoadEnvironment` at `0x801E7210` moves initialization data into
mutable storage, resolves terrain views, relocates the environment archive,
uploads packed images, creates the selected scene objects, installs lighting and
matrices, and builds terrain vertices and triangles. Ownership then rests with
the installed scene and its private initialization storage.

## 9. Asynchronous Visual Loading

`BattleLoaderCreateSpriteLoadingTask` at `0x801E62E0` creates the startup task
and gives it the enemy visual package. Its callbacks advance through these
stages:

1. Wait for the enemy package, install enemy visual assets, and create enemy
   sprite or Gear actors.
2. Release the enemy package and queue character packages for occupied
   non-Gear party slots.
3. Install party character actors, VRAM bindings, and entrance animations.
4. Queue and install common textures, animations, and the Battle sequence.
5. Wait for music readiness and start one Gear loading task for each occupied
   Gear party slot.
6. Wait for all Gear tasks, begin the sixteen-frame entrance delay, and wait for
   normal actors to reach terrain height.
7. Enable Battle time and retire the startup task.

Each Gear task loads its selected model and animation pair, adds its associated
weapon resource when selected, installs the completed Gear actor, and then
retires itself. Encounters containing only one visual class move directly to the
next required callback stage.

## 10. Four Initialization Phases

`BattleLoaderRunInitializationPhase` at `0x801E5840` interleaves four synchronous
phases with the loading transition:

### Phase 0: party and common data

- resolve active party members and selected Gears;
- copy persistent character and Gear statistics;
- expand common UI, command, attack, text, and portrait data;
- upload active party portraits;
- queue the selected enemy pair.

### Phase 1: entity construction

- reset visual entity state;
- configure all eleven slot mappings and positions;
- install enemy definitions and scripts;
- initialize party display state.

### Phase 2: gameplay state

- compact persistent items into the Battle inventory;
- randomize entity order and normalize initial readiness;
- install command layouts, facing targets, and encounter restrictions.

### Phase 3: HUD state

- create gauges, AP and fuel polygons, names, and values;
- create overlay packets and item-target labels.

## 11. Transition And Phase Resources

The startup transition renders while initialization phases and asynchronous
requests advance. The standard transition uses central Battle's fragment
effect; loader-resident modes provide tile-rise, triangle-burst,
triangle-spiral, and tile-fade presentations.

Formation policy bit `0x20` leads from completed startup into Battle Event.
`BattleEventOverlayLoad` at `0x80070E2C` activates the Event phase; Event
initialization then loads dialogue, entities, scripts, sequence state, and
selected music.

After combat, `BattleMain` activates Result.
`BattleResultLoadResources` at `0x801E211C` prepares progression and result-screen
data. Policy bit `0x08` skips the ordinary post-combat gray fade and Result
presentation. Eligible ordinary victories synchronize represented party and Gear
state during progression; successful escape has already synchronized that state
before setting its terminal result. Result then releases central Battle
allocations after mode-specific reward and persistence processing.
