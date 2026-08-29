# Scripted Modes And Function Map

## 1. Mode Dispatch Table

The top-level selector indexes a 19-entry table of resource-preload, one-shot
setup, and teardown callbacks. Modes `0..7` share the complete ordinary travel
lifecycle. Modes 8 and 11 use the ordinary mode wrapper with explicit
configuration 0. The remaining modes create dedicated scripted task sets.

| Mode | Resource base | Setup callback | Teardown callback |
|---:|---:|---:|---:|
| `0..7` | Progress selected | `0x80072238` | `0x8007299C` |
| 8 | `0x2B` | `0x80077214` | `0x80077480` |
| 9 | `0x8E` | `0x80077A64` | `0x80077CC0` |
| 10 | `0x8E` | `0x80078A60` | `0x80078D24` |
| 11 | `0x2B` | `0x80077214` | `0x80077480` |
| 12 | `0xA4` | `0x8007BF50` | `0x8007C260` |
| 13 | `0xBA` | `0x8007FF70` | `0x80080218` |
| 14 | `0x99` | `0x8007A5DC` | `0x8007A8AC` |
| 15 | `0xAF` | `0x8007D918` | `0x8007DCE0` |
| 16 | `0xC5` | `0x80080D00` | `0x8008106C` |
| 17 | `0xD0` | `0x80082324` | `0x800826B4` |
| 18 | `0xDB` | `0x8008355C` | `0x800837DC` |

Modes `0..7` use `WorldMapAllocatePartySpriteResources` at `0x80071CDC`; modes
`8..18` use `WorldMapQueuePrimaryResourceFiles` at `0x80071EF0`. Mode-selected
configuration files are queued by `WorldMapQueueModeResourceFiles` at
`0x80072090`. The setup callback constructs resources and registers tasks, the
central frame coordinator runs the 64-slot dispatcher, and teardown releases the
mode. Dedicated scripted teardowns select their destination; ordinary teardown
at `0x8007299C` only releases or snapshots state, while the top-level coordinator
commits the already selected ordinary route.

## 2. Shared Scripted-Scene Architecture

Scripted modes retain the ordinary terrain and rendering foundation while
replacing travel tasks with a scene graph:

```text
mode setup
  |
  +-- director task ------ timed command tables
  +-- camera task -------- keyframes, target, zoom, shake
  +-- model tasks -------- paths, attachments, transforms
  +-- effect tasks ------- particles and textured quad layers
  +-- fader task --------- transition color and blend
  +-- ground task -------- reduced terrain/model/effect producer
```

The mode setup chooses a fixed or table-selected streaming origin, drains the
initial terrain queue, loads scene models and textures, installs tasks, and then
hands control to the central 64-slot dispatcher. The teardown callback stops or
fades audio, releases every scene allocation, and chooses the destination Field
and entry mode.

Scene tasks reuse a wrapped-vector interpolation helper for camera targets and
model positions. It computes the shortest toroidal delta on X/Z, advances each
component by one eighth, and snaps a component when its step magnitude drops
below `0x40`. A task can therefore share the same target-following rule without
duplicating seam handling.

## 3. Shared Entity Command Stream

`WorldMapProcessEntityCommandStream` at `0x80076B34` advances a task-owned command
pointer until a handler requests a pause. The shared operations are:

| Opcode | Operation | Arguments | Advance |
|---:|---|---|---:|
| 0 | End | None | Stop |
| 1 | Wait | Delay | Stop, then 4 bytes on completion |
| 2 | Change entity state | Target slot, command | 8 bytes |
| 3 | Set streaming origin | X, Y, Z | 8 bytes |
| 4 | Set effect position | X, Y, Z | 8 bytes |
| 5 | Spawn effect | Owner | 4 bytes |
| 6 | Clear effect layer | Owner | 4 bytes |
| 7 | Deactivate owner effects | Owner | 4 bytes |
| 8 | Fade music | Volume, duration | 8 bytes |
| 9 | Play sound | Sound ID | 4 bytes |
| 10 | Play positioned sound | Sound ID, volume, pan | 8 bytes |
| 11 | Set fade parameters | Blend mode, per-frame step | 8 bytes |

End clears the World loop and transition result. The other operations move the
stream origin, populate the scratch effect position, control one of the 64 effect
owners, or combine the active sequence ID with a sound command as indicated.

Scene-specific directors add higher-level opcodes for camera shots, model phases,
quad layers, resource release, mode changes, and final Field handoff.

The stream is halfword-oriented. The first 32-bit word contains a low-halfword
opcode and a signed high-halfword argument; the next word supplies two more
signed halfword arguments. A handler returns the number of halfwords consumed:

| Return | Stream effect |
|---:|---|
| `0` | Stop processing and retain the current command pointer. |
| `2` | Advance four bytes. |
| `4` | Advance eight bytes. |

The processor immediately executes the next command while the return value is
nonzero. Wait returns zero while its `+0x22` counter is active and two when the
delay completes. End returns zero without advancing the command pointer; it
clears the World frame-loop continuation flag and transition/result flag.
Change-state consumes eight bytes and attempts one delivery through the target
task's one-entry command mailbox.

The shared handlers establish the script ABI precisely:

- streaming-origin and scratch-position commands consume three signed values;
- effect creation, layer clearing, and owner deactivation use the first argument
  as the owner ID;
- ordinary sound combines the current sequence ID with the first argument;
- positioned sound additionally consumes volume and pan;
- fader parameters write two scene-control words.

## 4. Ordinary Wrapper And Citan Flyover Tasks

`WorldMapMode0Update` at `0x80077214` is the historically named setup callback for
the explicit ordinary-mode variants: it constructs and registers the ordinary
task set, which the central coordinator then runs. The same family includes a
compact Citan's House flyover task pair:

| Address | Task |
|---:|---|
| `0x8007756C` | Initialize camera angles, distance, eye, and target. |
| `0x800776E0` | Apply controller orbit input and smooth camera values. |
| `0x80077954` | Initialize the persistent reduced renderer. |
| `0x8007795C` | Render decorations, models, terrain, horizon, sky, and clouds. |

The reduced renderer omits party actors, shadows, effects, and radar while
retaining terrain streaming around the flyover camera.

## 5. Goliath Flight, Mode 9

`WorldMapGoliathFlightModeUpdate` loads the scene at a fixed World position and
creates fader, camera, Goliath model, and cinematic renderer tasks.

- The camera follows a scripted quadratic path, controls positional sound, and
  starts the closing transition.
- The Goliath task attaches thirteen configured submodels, advances the root
  through wrapped World space, and rebuilds animated child rotations.
- The shared cinematic renderer emits effects, models, terrain, horizon, sky,
  and clouds.
- Teardown transfers to the Grahf attack Field sequence.

## 6. Goliath Destruction, Mode 10

`WorldMapGoliathDestructionModeUpdate` creates a timed director, terrain anchor,
crash model, explosion-ring layers, smoke emitter, blast task, camera, fader, and
cinematic renderer.

The director advances crash stages by timer. Each stage commands subordinate
tasks, starts sound effects, changes camera shake, and selects impact particles.
The crash model animates fall and rebound while all attached submodel matrices
remain synchronized. Expanding textured rings and smoke bursts have independent
lifetimes. Teardown selects the following Grahf, Ramsus, and Miang Field scene.

## 7. Aveh Rescue Flyover, Mode 14

The Aveh rescue mode uses a table-driven sequence director and a Yggdrasil model
following a quadratic flight path. Its task set contains:

- A camera with commanded zoom, target movement, and bounded random shake.
- Two pairs of expanding semitransparent quad layers anchored to the vehicle.
- A moving burst emitter with a fixed 60-frame phase.
- A vehicle root with three attached child models, path speed, acceleration, and
  tangent-derived orientation.
- A reduced cinematic renderer and closing fader.

Its free callback releases the flyover and enters the Yggdrasil Maison's bar
Field sequence.

## 8. Goliath Aftermath And Fleet, Mode 12

`WorldMapGoliathAftermathModeUpdate` installs a fleet director, custom camera,
multiple model groups, effects, and the cinematic renderer.

The director reads timed opcodes that assign camera moves and command model
groups. Root and child groups advance through wrapped space, publish camera
targets, emit continuous trails, and replace them with terminal effects when
hidden. The teardown path frees the complete fleet scene and selects the Solaris
Field sequence.

## 9. Babel Approach, Mode 15

`WorldMapBabelCinematicModeSetup` chooses a table-based origin and installs its
director, camera, approach object, five follower models, finale model pair,
effects, fader, and ground tasks.

The lead object advances through scripted phases and commands the five followers.
Each follower applies a table-selected offset, rotation, and scale until its
scale collapses and the model hides. Finale models grow together while their
packet colors fade toward black. Teardown selects Babel Tower Field `0x1A1` and
its table-selected entry mode.

## 10. Fixed-Origin Scene, Mode 13

`WorldMapFixedOriginCinematicModeSetup` initializes a fixed streaming origin and
creates a sequence director, interpolated camera, burst emitter, expanding
quad effect, fader, and ground task.

The director commands camera, burst, fader, textured-effect, resource-release,
and transition phases. The quad effect expands two layers while fading their RGB
values from neutral intensity to black. The mode-specific free callback saves
exit state after audio and scene resources have been released.

The director itself is a compact parallel opcode/duration table. Its task stores
the active opcode at `+0x20`, countdown at `+0x22`, and table cursor at `+0x50`.
Opcodes command the fixed fader, camera, burst, and quad slots by number; one
opcode starts three sounds in succession, and opcode `0x40` clears the active
scene words and terminates the sequence.

## 11. Warp Scene, Mode 16

Mode 16 adds captured-screen distortion to the shared scripted architecture:

- A primary textured quad fades between black, white, and neutral color.
- Two additional quad layers run an asymmetric RGB ramp.
- A scanline task captures a 192-row screen region and emits one displaced
  textured row per line.
- A pattern controller randomizes, grows, shrinks, reseeds, or flattens the
  192-entry displacement-amplitude table.
- The sequence director coordinates distortion, fades, resource release, music
  fade-out, and final transition.

`WorldMapMode16ScanlineWarpTaskUpdate` at `0x80081D80` owns the per-row packet
generation; the renderer sees the completed effect through bucket zero.

## 12. Scripted Families 17 And 18

Modes 17 and 18 share a command-stream updater while using distinct camera and
staged-model tasks.

| Family | Behavior |
|---|---|
| Mode 17 | Camera shot commands, multiple pulsing semitransparent models, effect commands, and an explicit terrain-streaming reload task. |
| Mode 18 | Six camera-shot commands and a staged landmark/effect model pair at a fixed origin. |

Both free callbacks release their resources and select Field `0x269`, with
different Field return modes.

Mode 17's task registration order is part of its script protocol: fader,
command-stream task, camera, five model tasks, streaming reload, and alternate
renderer occupy slots `0..9`. The five model tasks derive model identity as
`task_index - 3`. When the reload task receives command 1, it clears the current
terrain residency, rebuilds the initial `9x9` grid, and blocks while repeatedly
servicing streaming until the 16-slot queue is empty.

Mode 18's command-stream task targets a camera with six command-selected shots
and staged object tasks whose commands configure effect owners before spawning
the corresponding effect layer. Its use of the shared owner ranges means scene
cleanup can disable emitters and retire live particles without scanning by model
pointer.

## 13. Renderer Variants

| Renderer | Producers after view construction |
|---|---|
| Ordinary ground task | Effects, actors, decorations, shadows, models, streaming, terrain, horizon, sky, clouds, radar |
| Alternate renderer | Effects, decorations, models, streaming, terrain, horizon, sky, clouds |
| Citan flyover renderer | Decorations, models, streaming, terrain, horizon, sky, clouds |
| Shared cinematic renderer | Effects, models, streaming, terrain, horizon, sky, clouds |

Every variant updates streaming and terrain visibility before ground emission.
Their differences correspond to the task families present in that scene.

## 14. Subsystem Function Map

### Entry, Configuration, And Resources

| Address | Function |
|---:|---|
| `0x80070CFC` | `WorldMapOverlayEntryPoint` |
| `0x80071B9C` | `WorldMapApplyModeParameterTable` |
| `0x80071CDC` | `WorldMapAllocatePartySpriteResources` |
| `0x80072090` | `WorldMapQueueModeResourceFiles` |
| `0x80072238` | `WorldMapInitializeModeRuntime` |
| `0x8007299C` | `WorldMapOrdinaryModeTeardown` |
| `0x80073300` | `WorldMapSetupTravelMode` |
| `0x80073530` | `WorldMapFinalizeFile1Loading` |

### Party, Vehicles, And Controls

| Address | Function |
|---:|---|
| `0x8008A72C` | `WorldMapPlayerTaskUpdate` |
| `0x8008B644` | `WorldMapPartyMemberTaskUpdate` |
| `0x8008C844` | `WorldMapPlayerGearTaskUpdate` |
| `0x8008D678` | `WorldMapPartyGearTaskUpdate` |
| `0x8008E76C` | `WorldMapYggdrasilTaskUpdate` |
| `0x80090A84` | `WorldMapUpdatePlayerControls` |
| `0x80090C68` | `WorldMapUpdateGearControls` |
| `0x80090E14` | `WorldMapUpdateYggdrasilLandingControls` |
| `0x80090FB4` | `WorldMapUpdateYggdrasilFlightControls` |

### Terrain, Collision, And Streaming

| Address | Function |
|---:|---|
| `0x80093660` | `WorldMapGetChunkForPosition` |
| `0x80093978` | `WorldMapGetAltitude` |
| `0x80093E8C` | `WorldMapGetMaterial` |
| `0x80093F18` | `WorldMapGetGroundType` |
| `0x80093FE4` | `WorldMapGetMaterialShape` |
| `0x80094004` | `WorldMapGetTerrainTextureIndex` |
| `0x80094028` | `WorldMapGetTerrainAuxiliaryType` |
| `0x80094154` | `WorldMapHorizontalDistance` |
| `0x800941C4` | `WorldMapSelectDirectionToTarget` |
| `0x80094A5C` | `WorldMapClassifyTerrainTraversal` |
| `0x80095324` | `WorldMapDeriveSlideVectorFromNormal` |
| `0x80095414` | `WorldMapCheckPosition` |
| `0x80095CD4` | `WorldMapCheckYggdrasilPosition` |
| `0x800967E4` | `WorldMapServiceStreaming` |
| `0x800981C8` | `WorldMapUpdateGridIndices` |
| `0x80098CC0` | `WorldMapUpdateStreamingCells` |

### Camera, Encounters, Exits, And Persistence

| Address | Function |
|---:|---|
| `0x8007528C` | `WorldMapUpdateEncounterSchedule` |
| `0x80075460` | `WorldMapSaveRuntimeState` |
| `0x8007565C` | `WorldMapRestoreRuntimeState` |
| `0x80075E7C` | `WorldMapSelectRandomEncounter` |
| `0x800914D0` | `WorldMapWorldCameraTaskUpdate` |
| `0x80091C18` | `WorldMapCameraTaskUpdate` |
| `0x80094238` | `WorldMapSetupExits` |
| `0x80094364` | `WorldMapFindExitAtPositionByType` |

### Tasks, Effects, And Shared Scene Utilities

| Address | Function |
|---:|---|
| `0x80076098` | `WorldMapRenderModeSpecificModelAtFixedPosition` |
| `0x800762FC` | `WorldMapFlushCacheAndSync` |
| `0x800767D4` | `WorldMapSwitchMusic` |
| `0x80076858` | `WorldMapQuadraticPathInterpolate` |
| `0x80076B34` | `WorldMapProcessEntityCommandStream` |
| `0x80076DA4` | `WorldMapSmoothPositionTowardTarget` |
| `0x80076F54` | `WorldMapSmoothCameraDistance` |
| `0x80076FA8` | `WorldMapSmoothPositionAndStreamingOrigin` |
| `0x800771D8` | `WorldMapStepValueTowardTarget` |
| `0x80088F64` | `WorldMapInitializeEffectStatePools` |
| `0x80089580` | `WorldMapUpdateEffectParticles` |
| `0x80089748` | `WorldMapUpdateEffectEmitters` |
| `0x8009766C` | `WorldMapAllocateTaskState` |
| `0x80097718` | `WorldMapSetupTask` |
| `0x80097770` | `WorldMapChangeEntityState` |
| `0x80097800` | `WorldMapDispatchTasks` |
