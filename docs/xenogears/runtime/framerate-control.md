# Framerate And Runtime Timing

## 1. Runtime Timing Model

Xenogears does not have one global game-frame rate. The NTSC vertical blank is
the common hardware clock, but each major game module chooses how many vertical
blanks separate its logical iterations and which systems advance during those
iterations.

The principal rates are:

| Runtime path | VBlanks per ordinary iteration | Nominal rate |
|---|---:|---:|
| VBlank callback services | 1 | 60 Hz |
| General Menu | 1 | 60 FPS maximum |
| Central Battle | 1 when on time | 60 FPS maximum |
| Field, mode 0 | 2 | 30 FPS |
| Field, mode 1 | 3 | 20 FPS |
| Field, mode 2 | 4 | 15 FPS |
| World Map | 2 | 30 FPS |
| Battling live presentation | 2 | 30 FPS |
| Battling live simulation | Setting-dependent | 30, 15, 10, 7.5, or 6 updates/s |
| STR movie images | 8 VBlanks on average | 7.5 FPS |

NTSC vertical refresh is close to 59.94 Hz, so the physical rates are slightly
below their nominal integer labels.

Three events must be kept distinct:

1. **VBlank** is a hardware display event. Interrupt callbacks, controller
   sampling, and the play-time clock can run even when no gameplay iteration is
   admitted.
2. **Module iteration** is one pass through Field, World, Battle, Battling, or
   Menu logic. Most movement, camera, script, animation, and UI values advance
   once per such pass.
3. **Presentation** installs a display environment and submits or exposes a
   completed image. GPU work, CD reads, MDEC decoding, and callbacks may finish
   asynchronously around it.

The hardware VBlank count continues advancing at approximately 60 Hz in every
case. A 20 FPS Field scene still receives about 60 VBlank interrupts per second;
it admits one Field iteration for every three of them. The displayed page is
retained across the intervening refreshes. A VBlank counter therefore measures
the display clock, not the number of new simulation states or newly submitted
frames.

There is no general render-time interpolation between two simulation states.
Field and World normally update once and present the resulting state once.
Battle repeats selected fixed updates after a slow frame. Movies retain the
last completely decoded image until another image is published.

## 2. The VBlank Clock

### 2.1 Interrupt path

The resident interrupt path maintains a software VBlank count. Between counter
initialization and eventual integer wrap, `trapIntrVSync` at `0x8004BF78`
increments that count once per VBlank and invokes each installed callback in an
eight-entry callback array. `VSyncCallback` at `0x8004B7D0` changes the
registered callback.

This callback path is independent of a module's chosen simulation cadence. A
30 FPS Field iteration spans two callback opportunities; a 15 FPS Field
iteration spans four.

### 2.2 `VSync` modes

Resident `VSync` at `0x8004B54C` combines VBlank counting, horizontal-retrace
timing, and waits:

| Argument | Behavior |
|---:|---|
| `< 0` | Return the current VBlank count immediately. |
| `1` | Return elapsed horizontal-retrace timing immediately. It does not wait for a VBlank. |
| `0` | Wait through the next VBlank boundary. |
| `N > 1` | Complete a nominal interval of `N` VBlanks relative to the preceding synchronized call. |

Positive interval waits are implemented in two stages. The first stage catches
up to the interval target relative to the previous synchronized count. The
second waits through one further VBlank and performs the GPU field-state check.
If execution is already late, the first stage can finish immediately. The
second stage normally crosses a fresh boundary unless the bounded wait itself
times out.

`v_wait` at `0x8004B694` bounds each wait with a software timeout. Its timeout
path reports the failure and restores controller and root-counter interrupt
state before returning. The modes that wait are consequently synchronization
and recovery boundaries, not merely delays. Query modes `1` and `< 0` bypass
this path.

Two common interpretations are therefore incorrect:

- `VSync(1)` is a timing sample, not a 60 FPS limiter.
- `VSync(-1)` is a counter query, not a wait or presentation request.

### 2.3 GPU completion is separate

`DrawSync(0)` waits for queued GPU drawing to finish. `PutDispEnv` selects the
display page, `PutDrawEnv` configures subsequent drawing, and `DrawOTag` submits
an ordering table. None of those operations alone advances the VBlank clock.

The modules place these operations in different orders. Framerate is determined
by the surrounding VBlank policy, not by double buffering or ordering-table
submission itself.

## 3. VBlank Services

### 3.1 Controller sampling

`ControllerTick` at `0x8003634C` is the ordinary VBlank controller service. On
each invocation it:

1. Advances the resident VBlank-side frame counter.
2. Polls and translates both controllers.
3. Queues one logical input snapshot.
4. Advances the play-time clock.
5. Updates controller output state.
6. Invokes the optional tick callback.

Module loops consume those snapshots later. Slower modules can therefore
receive several hardware samples during one logical iteration. Field and World
drain their queues and merge held, pressed, and repeat categories rather than
assuming that one module iteration equals one controller sample.

This separation also explains modal waits. Field's disconnected-controller and
pause loops can continue crossing VBlanks and polling input while actor scripts,
movement, encounters, and camera simulation remain stopped.

### 3.2 Persistent play time

`IncrementCappedPlayTimeClock` at `0x80035E44` advances a four-level clock:

```text
60 VBlank ticks -> 1 second
60 seconds      -> 1 minute
60 minutes      -> 1 hour
100 hours       -> permanently saturated
```

The play-time clock is consequently based on VBlank callbacks, not on Field,
World, Battle, or Menu iterations. Selecting a slower Field mode does not make
the saved play time run at half speed. The resident game state stores the
separate tick, second, minute, and hour components; save and menu code consume
the resulting clock.

### 3.3 Work lists and audio

Resident work-list dispatch is another clock domain. Central Battle explicitly
runs `WorkListUpdate` at `0x8001C9F8` once in its frame path and can repeat
`TimerWorkListUpdate` at `0x8001C964` during missed-frame compensation.

Audio is not driven by render FPS. Its root-counter callback runs at 240 Hz,
with selected auxiliary work at 120 Hz and musical sequence ticks accumulated
from tempo. A slow render frame does not suspend that hardware-timed audio
service.

## 4. Field Cadence

### 4.1 Ordinary frame boundary

`FieldMain` at `0x80077E88` performs one ordinary iteration by calling
`FieldPerFrameReset` at `0x80077DAC` and `FieldPresentationPassA` at
`0x8007554C`.

The relevant timing sequence is:

1. Sample horizontal-retrace timing with `VSync(1)`.
2. Swap and clear the Field ordering-table context.
3. Poll queued input and synchronize persistent Field state.
4. Record the starting VBlank count with `VSync(-1)`.
5. Run actor scripts, movement, collision, camera, effects, and scene producers.
6. Wait for GPU completion.
7. Update dialogue windows and render text.
8. Call `VSync(0)` and install the display and draw environments.
9. Flush deferred image transfers, complete late presentation work, and submit
   the ordering table.
10. Poll `VSync(-1)` until the configured minimum interval has elapsed.

The final condition is:

```text
current_vblank >= frame_start_vblank + field_timing_mode + 2
```

The default timing mode is zero. An ordinary Field iteration therefore occupies
at least two VBlanks and runs at 30 FPS. The mid-frame `VSync(0)` supplies one
boundary; the final counter poll enforces the complete interval.

Field advances simulation once per admitted iteration. If scene construction
misses the target, Field does not run extra actor or VM updates to catch up. The
iteration becomes longer and the game state advances more slowly in real time.

### 4.2 Script-selected rates

Primary Field opcode `D6`, implemented by
`FieldScriptSetDialogAnimationSpeed` at `0x800925A0`, changes the pacing state:

| Opcode value | Minimum VBlanks | Field rate | Dialogue opening iterations |
|---:|---:|---:|---:|
| `0` | 2 | 30 FPS | 8 |
| `1` | 3 | 20 FPS | 6 |
| `2` | 4 | 15 FPS | 4 |

The opcode evaluates an immediate or variable operand and stores it directly as
the Field timing mode. Values `0..2` also select the corresponding dialogue
opening length. Other values leave the prior dialogue opening length unchanged.
For a nonnegative value that does not overflow the boundary arithmetic, the
final poll targets `value + 2` VBlanks. A negative variable value can make that
poll immediately satisfied, but cannot bypass the earlier unconditional
`VSync(0)` boundary.

The paired dialogue counts keep window opening near the same broad wall-clock
duration while allowing scenes to select a lower overall update rate. The
selection affects the complete Field iteration, not only dialogue rendering.

### 4.3 Scripted Field rate census

The USA Field script set contains 65 unique executable `D6` sites across 22
fields. Identical field resources present on both discs account for 80 physical
copies; deduplicating by field ID, identical script payload, and instruction
offset produces the 65-site count. Entity and routine roots are retained as
activation context rather than counted as additional sites:

| Logical mode | Minimum interval | Nominal rate | Instruction sites |
|---:|---:|---:|---:|
| `0` | 2 VBlanks | 30 FPS | 15 |
| `1` | 3 VBlanks | 20 FPS | 31 |
| `2` | 4 VBlanks | 15 FPS | 19 |

Every site uses a literal encoded as `0x8000`, `0x8001`, or `0x8002`; the high
bit selects immediate interpretation and the low 15 bits provide the mode. The
field-level distribution is:

| Field | Disc | Scene | 30 FPS sites | 20 FPS sites | 15 FPS sites | Total | Principal activation |
|---:|---:|---|---:|---:|---:|---:|---|
| 002 | 1 | Lahan battle, intro version | 0 | 7 | 1 | 8 | Automatic update routine |
| 035 | 1 | Dazil, outside the waterworks | 1 | 6 | 9 | 16 | Directed event routines |
| 046 | 1, 2 | Duneman Isle, top of Sandfall | 1 | 0 | 1 | 2 | Automatic update routine |
| 054 | 1 | Aveh Desert, Fei ambushed by two Gears | 1 | 1 | 0 | 2 | Directed event routine |
| 080 | 1 | Bart's Hideout, Yggdrasil docking | 1 | 2 | 0 | 3 | Automatic update routines |
| 082 | 1 | Bart's Hideout, Fei fights Vance | 2 | 0 | 0 | 2 | Directed event routines |
| 083 | 1 | Bart's Hideout, Schpariel battle | 0 | 1 | 0 | 1 | Directed event routine |
| 174 | 1 | Assault on Aveh, Elly's unit attacks | 2 | 2 | 0 | 4 | Directed event routines |
| 176 | 1 | Assault on Aveh, Drive flashback | 0 | 1 | 0 | 1 | Automatic update routine |
| 243 | 1 | Kislev sewers, Fei's childhood dream | 0 | 1 | 1 | 2 | Automatic update routine |
| 247 | 1 | Nortune Purge evacuation | 1 | 1 | 0 | 2 | Automatic update routine |
| 326 | 1, 2 | Orphanage entrance | 1 | 0 | 1 | 2 | Automatic update routine |
| 424 | 1 | Shevat City entrance | 1 | 2 | 0 | 3 | Contact and directed event routines |
| 488 | 1 | Ft. Jasper, Omnigear hangar | 1 | 1 | 1 | 3 | Automatic update routine |
| 506 | 1 | Solaris opens defensively | 0 | 0 | 2 | 2 | Directed event routines |
| 509 | 1 | Etrenank, outside immigration office | 0 | 1 | 0 | 1 | Automatic update routine |
| 627 | 1, 2 | Anima Relic 1, Elements combine and attack | 0 | 1 | 0 | 1 | Initialization routine |
| 671 | 1, 2 | Etrenank, Krelian takes Lacan for pigments | 1 | 0 | 1 | 2 | Chained event routines |
| 680 | 1, 2 | Shevat prison, Miang posing as Elly | 0 | 2 | 0 | 2 | Automatic update routine |
| 682 | 1, 2 | Shevat, Krelian loses faith | 0 | 0 | 1 | 1 | Automatic update routine |
| 717 | 1, 2 | Merkava inner chamber before Deus | 1 | 1 | 0 | 2 | Automatic update routine |
| 723 | 1, 2 | Suzuki debug room | 1 | 1 | 1 | 3 | Automatic update routine |
| **Total unique** | | | **15** | **31** | **19** | **65** | |

The eight resources found on both discs are byte-identical. Counting both copies
adds five mode-0, five mode-1, and five mode-2 sites, producing the physical
distribution 20, 36, and 24, or 80 instructions in total.

An entry in this table does not mean that the complete field always uses every
listed rate. Directed routines select a mode only when their event path runs,
and several scenes restore mode 0 afterward. Automatic and initialization
routines can establish the lower rate as soon as the scene begins.

Fresh Field initialization sets mode 0, and opcode `D6` is its direct script
writer. A selected rate therefore persists throughout the currently loaded
field until a later script changes it. A fresh field load starts at 30 FPS, but
the transient Field reentry snapshot also contains the timing mode and dialogue
opening count. Returning from Battle or another snapshot-preserving handoff to
the same field restores those values after initialization. A cutscene can thus
leave the rest of its field, including a Battle round trip, at a lower rate when
no restoring `D6` is executed; an ordinary fresh field load does not inherit it.

These authored modes explain some repeatable 20 or 15 FPS scenes while the
VBlank clock continues at 60 Hz. They are scene state, not an automatic
response to rendering load. A slowdown that follows the current position or
camera view can instead be the overrun case described below.

### 4.4 Position- and camera-dependent slowdowns

Field rendering is view-dependent even though the frame-rate selector is not.
`FieldRenderModels` at `0x800748E8` scans the complete Field object collection
each iteration. It does not select spatial render chunks. For each enabled
object it prepares transforms and then applies two important rejection stages:

1. `FieldObjectOcclusionCull` at `0x800AAA74` projects a conservative center and
   extent against the current camera.
2. Resident `IsModelBoundsCulled` at `0x8003101C` tests selected corners and
   edge midpoints of the model resource's bounding box.

Crossing one of these visibility boundaries can abruptly admit a large model.
Once admitted, its primitive groups enter resident `ModelRender`, where their
vertices are transformed with the GTE, clipped, lit where required, written to
the parity-selected packet buffer, and linked into an ordering table. Primitive
rejection inside that path occurs after some of this work has already been
performed.

Other producers can add similarly position-dependent cost:

- all ten loaded Field Gear slots are traversed, with geometry-bearing skeletal
  parts subjected to model-bounds culling;
- character actors are projected individually, and their normal sprite packet
  path is less aggressively rejected than their shadows;
- active particles are transformed and depth-tested individually, without an
  early horizontal or vertical screen-bounds rejection;
- contact or proximity routines can enable additional models, particles,
  distortion, panorama, or other fixed-size effects.

The renderable Field geometry is not divided into camera-streamed map blocks.
The map's block-like walkmesh data controls collision and movement rather than
render selection. A location hotspot is therefore more commonly a model or
effect crossing a visibility/activation boundary than a terrain chunk being
loaded for drawing.

Field submits the ordering table near the end of iteration `N`. During iteration
`N + 1`, after constructing the current scene, `DrawSync(0)` waits for that
preceding command stream to finish before the mandatory fresh `VSync(0)`.
Expensive CPU/GTE packet construction can cross a VBlank before this point, and
expensive drawing from the preceding submission can extend the `DrawSync` wait.
Either case can make a mode-0 field take three or four VBlanks instead of its
two-VBlank minimum, producing approximately 20 or 15 new frames per second while
VBlank callbacks continue at 60 Hz.

A location trigger can also execute `D6`, so position is not by itself a perfect
distinction. The behaviors differ after activation:

- a visibility/workload overrun normally follows the camera composition and
  recovers when the expensive content leaves the view or deactivates;
- a `D6` change remains global for the current Field state after walking or
  looking away, until another `D6`, a fresh Field load, or restored state
  replaces it.

The current timing mode at `0x800B217C` distinguishes the two cases directly.
`FieldRenderModels` also resets and accumulates separate total and accepted
resident-model primitive counts each iteration. A mode remaining at zero while
those counts or the time around `DrawSync` changes identifies an actual frame
missing its VBlank deadline rather than an authored 20 or 15 FPS selection.

### 4.5 Systems affected by the selected rate

Most Field values are measured in Field iterations:

- The VM scheduler runs once per Field simulation pass.
- Opcode `26` sleep counts scheduler selections; a stored value `N` occupies
  `N + 1` selections.
- Actor movement, gravity, collision, party-follow history, and encounter
  updates advance from their normal per-iteration call sites.
- Camera orbit, interpolation, shake, and finite-duration script moves advance
  once per camera update.
- Dialogue opening, fades, particles, line scroll, and most UI animation use
  per-iteration counters or fixed deltas.
- Sprite and Gear animation interpreters are serviced from Field producer
  updates rather than from every VBlank.

Changing `D6` therefore changes the wall-clock duration of every counter that is
not separately compensated. The game only adjusts the dialogue opening count
as part of the opcode itself; it does not globally rescale script waits,
movement, cameras, effects, or encounter progression.

`FieldUpdateDeltaTime` at `0x8007781C` only records the `VSync(1)` horizontal
timing sample. Ordinary Field simulation does not multiply movement or script
time by that value.

### 4.6 Pause and controller-disconnect loops

Field's pause and disconnected-controller paths call `VSync(2)` while polling
input and servicing resident maintenance. They retain a 30 Hz polling boundary
without executing the ordinary actor frame. This is a suspension of Field
simulation, not a switch to a second active simulation rate.

### 4.7 Field-integrated movies

Field can present an STR stream in three ways:

| Mode | Presentation path | Decoder-service calls per pass |
|---:|---|---:|
| `0` | Direct movie pages synchronized with `VSync(0)` | 3 at each of two direct service points |
| `1` | Reduced `FieldPresentationPassB` | 6 |
| `2` | Full `FieldPerFrameReset` and `FieldPresentationPassA` | 9 |

Mode 2 retains the active Field timing mode because it uses the ordinary Field
frame boundary. The number of decoder-service calls controls how aggressively
the asynchronous STR pipeline is pumped; it is not the movie image rate.

## 5. World Map Cadence

`WorldMapFrameCoordinator` at `0x800712D0` owns the ordinary World
Map loop. One iteration:

1. Drains and merges controller snapshots.
2. Services pending terrain streaming.
3. Selects the alternate graphics context and packet parity.
4. Clears the 1024-entry ordering table.
5. Dispatches the 64 World task slots.
6. Waits for GPU completion.
7. Calls `VSync(2)`.
8. Installs display and draw environments.
9. Updates animated texture streams and submits the ordering table.

The explicit two-VBlank interval makes ordinary World travel a 30 FPS system.
Camera, movement, encounters, effects, terrain streaming coordination, and task
delays are updated once per World iteration unless their own gates prevent it.

World does not perform Battle-style repeated simulation after an overrun. A
late iteration extends its wall-clock duration. Task delay values remain counts
of task-dispatch opportunities, not raw VBlanks.

Some World streaming waits call `VSync(0)` while the ordinary task set is
blocked. Those calls keep interrupts, input, and display-time services alive;
they do not create additional World simulation updates.

## 6. Central Battle Cadence

### 6.1 Maximum-rate presentation

`BattleRender` at `0x800BE790` is designed around a one-VBlank presentation
boundary. With the normal timing offset set to zero, it ends with `VSync(0)` and
can present at 60 FPS when all frame work completes before the next retrace.

One call builds and submits only one visible frame:

1. Record the starting VBlank count.
2. Select the alternate render context, clear its 4096-entry ordering table,
   and flip packet parity.
3. Update the camera, scene, Gear models, sprites, work lists, and UI.
4. Apply compensation inherited from the preceding slow frame.
5. Wait for GPU completion and measure elapsed VBlanks.
6. Compute and clamp the compensation for the next call.
7. Synchronize to VBlank, install environments, and submit one ordering table.

Battle's 60 FPS target is distinct from turn progression. Readiness, statuses,
menus, attacks, Battle Event operations, and transitions can independently
enable or suppress their gameplay clocks while the renderer keeps presenting.

### 6.2 Missed-frame compensation

At the end of a render call Battle computes:

```text
missed = current_vblank - frame_start_vblank - timing_offset
missed = clamp(missed, 0, 4)
```

The normal timing offset is zero. The resulting count is consumed during the
next call. It does not cause additional complete Battle frames. Instead Battle:

- repeats the camera update and timer work-list update `missed` times;
- runs one ordinary `BattleUpdateInputs(0)` and then
  `BattleUpdateInputs(1)` once per missed interval;
- supplies the combined elapsed count to Gear/mecha animation advancement;
- renders the scene and UI only once;
- submits only one ordering table.

This is bounded fixed-update catch-up, not temporal interpolation and not a full
gameplay rollback. Combatant turn selection, damage calculation, and the whole
scene graph are not re-executed once for every missed VBlank. Battle Event frame
timers serviced by the mode-1 input/update path can advance during catch-up.

If more than four intervals are missed, Battle discards the excess compensation.
The visible frame rate can still fall below 60 FPS because a slow call waits for
a fresh VBlank after its work completes.

### 6.3 Three-dimensional enemies and frame loss

Central Battle has no persistent rate selector equivalent to Field opcode `D6`.
A three-dimensional enemy instead enters the same model path used by Gears and
raises the amount of work performed before the frame boundary.

`BattleRender` calls `BattleRenderAndAdvanceMechas` at `0x800A9A50` before
waiting for GPU completion. That path:

1. Advances model animation using the current missed-frame amount.
2. Traverses 32 model-task slots for active animation and attachment work.
3. Refreshes linked and attached transforms.
4. Traverses up to 31 renderable model-task slots.
5. Processes model geometry, shadows, trails, deformable meshes, image
   animations, and attached effects owned by each active task.

`BattleRenderMechaBoneGeometry` at `0x8009F5B8` then walks every nonroot bone in
the active skeleton. For each bone carrying a geometry ID it:

- composes the view, root, and bone matrices;
- installs GTE rotation, translation, projection, lighting, and fog state;
- resolves the parity-selected packet buffer;
- invokes resident `ModelRender` at `0x8002C700` for that bone's model part.

`ModelRender` adds the resource's complete primitive capacity to the frame's
total-model counter before dispatch. It then walks every primitive group and
every primitive record. Family-specific handlers transform vertices, clip,
test orientation and depth, calculate lighting and fog where required, update
the packet, and link accepted packets into the ordering table. A primitive that
is eventually rejected still incurs format dispatch and part of its transform
and visibility work.

This differs substantially from a sprite enemy. A sprite is assembled from its
current animation tiles and projected as sprite packets; it has no articulated
bone traversal and no per-bone model primitive stream. The cost of a 3D enemy
therefore scales with all of the following:

- active model-task count;
- skeleton bone count;
- geometry-bearing bone count;
- primitive groups and records in each bone model;
- lighting, fog, clipping, and depth mode selected by each primitive family;
- attached trails, particles, shadows, image animations, and deformable data;
- accepted screen coverage, overlap, and semitransparent rasterization.

The renderer maintains both total and accepted resident-model primitive counts.
The distinction matters: a high total raises CPU/GTE work even if clipping keeps
the accepted count modest, while a high accepted count also lengthens DMA and
GPU rasterization. Large overlapping or semitransparent polygons can be
fill-limited without having the largest primitive count.

Battle initialization counts qualifying 3D enemy lanes in slots 3 through 10.
A lane qualifies when its selector byte is below `0x11` and its Gear-scale/3D
byte is nonzero; the test does not exclude an initially hidden reserve lane.
`BattleInit3dRendering` at `0x800B88C4` derives:

```text
initial_animation_delta = max(floor(qualifying_3d_enemy_lanes / 2) - 1, 0)
```

The resulting values are zero for up to three qualifying lanes, one for four or
five, two for six or seven, and three for eight. Battle copies this value into
the initial model-animation delta and then clears the render timing offset. It
does not install a lower persistent FPS limit. Subsequent animation delta comes
from measured missed frames.

A frame becomes late when aggregate model construction, GTE work, other scene
work, or completion of the preceding GPU command stream crosses one or more
VBlank boundaries before `DrawSync(0)` returns. The following `VSync(0)` still
waits through a fresh boundary, so a one-boundary overrun turns a nominal
one-VBlank frame into a multi-VBlank frame. The hardware VBlank interrupt
continues at 60 Hz throughout; the game simply submits fewer new frames.

This can produce a repeatable, content-dependent slowdown. The same enemy tends
to present the same skeleton, primitive families, and attachments under similar
battle conditions, although pose, camera, effects, visibility, and screen
coverage can vary the cost from frame to frame. Static model structure alone
does not determine whether CPU/GTE packet construction or GPU rasterization is
the limiting component; that distinction requires phase timing for the actual
scene. Battle's partial catch-up adds selected camera, timer, input, and
model-animation work to the next call, but does not remove the geometry cost or
reconstruct the missed visible frames.

### 6.4 Thirty-frame transition paths

Battle startup effects that capture and transform the preceding framebuffer use
`VSync(2)` in their tile-rise and triangular-fragment loops. Those transitions
advance at 30 effect iterations per nominal second even though ordinary central
Battle targets 60 FPS.

### 6.5 Battle Event scheduling

Battle Event scripting adds another level of timing. A single Event scheduler
operation can present a Battle frame before dispatching an entity and can do so
for several entities before returning to its caller. Event opcode `2B` stores
`wait_units * 2` and is decremented by the Battle input/frame update service.
Its duration is therefore tied to those admitted update calls, including
eligible catch-up calls, rather than directly to VBlank count.

## 7. Battling Cadence

Battling is an independent competitive Gear runtime with its own main loop and
frame-rate option. `BattlingMain` at `0x80088E90` calls `VSync` with the current
signed synchronization interval before `DrawSync`, environment installation,
and ordering-table submission. Live bout setup selects an interval of two
VBlanks, so presentation runs at 30 FPS.

Practice setting `FRAME RATE` at `0x80099D9A` presents these labels:

| Index | Displayed label | Hold iterations after an update | Effective updates at 30 FPS presentation |
|---:|---:|---:|---:|
| `0` | 30 FPS | 0 | 30/s |
| `1` | 20 FPS | 1 | 15/s |
| `2` | 15 FPS | 2 | 10/s |
| `3` | 12 FPS | 3 | 7.5/s |
| `4` | 10 FPS | 4 | 6/s |

`BattlingAdjustFrameRateOption` at `0x800800CC` wraps the index through `0..4`.
The live updater runs when its hold counter is zero, reloads the counter directly
from this index, and otherwise decrements it without running fighter simulation.
The implementation therefore does not produce the four lower rates printed by
the menu: it divides the 30 Hz live update opportunity by `index + 1`.

This option is separate from `MOTION SPEED`, which selects a fixed-point scale
from `0x0060` through `0x0400`. The frame-rate index determines how often updates
are admitted; motion speed changes the amount of movement and animation
progression inside an admitted update. Match initialization resets the
frame-rate index to zero before a new bout.

The bout elapsed-time counter advances only on admitted live updates, saturates
at 180,000, and is always formatted using 30 update units per displayed second.
It consequently runs slower than wall time at nonzero frame-rate indices.
Replay history likewise records one sample per admitted live update, up to 256
samples.

`BattlingVsyncCallback` at `0x80088C00` calls `VSync(1)` and stores the
horizontal timing result for the performance display. It does not pace the bout.
The presentation limiter is the synchronization interval used by
`BattlingMain`; the practice setting is the separate live-update hold counter.

Menus, loading states, replay, and result presentation can temporarily use zero
or fixed two-VBlank synchronization independently of the live-update hold
counter. The practice option governs live simulation, not every screen in the
Battling module.

## 8. Menu And Resident Presentation

### 8.1 General Menu

`MenuDraw` at `0x801C7BF4` performs one complete Menu iteration:

1. Translate at most one queued input command.
2. Select context and parity and clear the 16-entry ordering table.
3. Advance windows, cursors, page transitions, and active composition.
4. Call `VSync(0)`.
5. Install draw and display environments, move the framebuffer region, and
   submit the ordering table.

The ordinary General Menu therefore targets 60 iterations per second. Its
window growth, cursor blink, memory-card delays, inactivity counters, and page
transitions are counts of Menu iterations. They are not scaled from the Field
rate that was active before opening the Menu.

Member Change, Enter Name, Shop, and Gear Shop use equivalent one-VBlank menu
boundaries in their own overlay loops. Their shutdown paths may draw additional
settling frames so both packet parities can be released safely.

A disconnected controller enters a modal polling wait. Navigation and Menu
animation stop while VBlank-side input service remains available.

### 8.2 Resident screens and module handoffs

The resident dispatcher uses `VSync(2)` while stabilizing graphics before a
module change, then uses one-VBlank waits around overlay installation and cache
synchronization. Logo fades and resident error display loops use `VSync(0)` and
advance their counters once per retrace.

These waits belong to startup, loading, or handoff state. They do not establish
the cadence of the module entered afterward.

CD retry and timeout code frequently queries `VSync(-1)` and some blocking
archive paths wait several VBlanks between polls. Such calls use VBlank as a
timeout clock while gameplay is blocked; they are not alternate gameplay FPS
modes.

## 9. Movie Cadence

### 9.1 Stream rate

STR streams begin one new video frame every ten physical CD sectors. The
frame itself uses eight or nine video sectors; the remaining sectors in the
ten-sector cadence carry interleaved XA audio or stream data. At the standard
75-sector-per-second CD rate:

```text
75 sectors per second / 10 sectors per frame = 7.5 movie frames per second
```

The encoded image cadence is therefore 7.5 FPS. It must not be inferred from the
eight- or nine-sector compressed frame size alone.

### 9.2 Display and decode loop

`MovieRunPlayback` at `0x80076488` runs a one-VBlank display loop and calls
`MovieStrUpdatePlayback` three times per outer iteration. The update-call count
does not impose a 15 or 30 FPS image rate. It services sector assembly, VLC
decode, MDEC input/output DMA, completion, looping, and stall recovery.

`MovieStrMdecOutputCallback` at `0x801D30C4` uploads the decoded image in
16-pixel strips. Only after the final strip does it publish the frame number,
invoke the movie callback, and flip output parity. Until that event, the display
continues showing the preceding completed image.

With a nominal 60 Hz display loop and a 7.5 FPS stream, one movie image normally
remains visible for about eight VBlanks. Decode and CD timing can vary, so this
is a producer/consumer relationship rather than an explicit eight-VBlank sleep
for each image.

### 9.3 Stalls and completion

If no complete ring-buffer frame is available, the STR library increments its
stall counter while the outer display loop continues. A prolonged stall can
restart the stream from its saved origin or from a captured following-sector
location when that mode is enabled. Final-frame handling either stops playback
or restarts a looping stream.

Movie skip input remains VBlank-serviced. Ending playback waits for CD/MDEC and
GPU activity, removes the streaming callbacks, and restores a synchronized
display page before returning to the requesting module.

## 10. Overruns And Slow Frames

The modules use three different policies when work extends beyond its nominal
VBlank interval:

| Policy | Modules | Result |
|---|---|---|
| Minimum-interval fixed update | Field, World, Menu, most transitions | Run one logical update, wait as required, and allow real-time slowdown when late. |
| Bounded partial catch-up | Central Battle | Repeat selected camera, timer, input, and animation work up to four times, then render once. |
| Asynchronous producer/consumer | STR movies, CD and MDEC | Continue showing the last complete result until a callback publishes the next one. |

A fourth visible case is an authored minimum interval. Field mode 1 or 2 can
hold each completed game state for three or four VBlanks even when all CPU and
GPU work finished early. This is intentionally lower logical and presentation
cadence while the VBlank interrupt itself remains at 60 Hz.

No load-adaptive polygon-detail or gameplay-FPS selector appears in the
principal runtime paths described here. Field changes rates through script state,
Battling through its explicit option or phase policy, and the other principal
modules use their fixed boundaries.

The GPU can still be the reason a frame is late because `DrawSync(0)` precedes
the final VBlank boundary in the major renderers. Double buffering prevents the
game from drawing into the currently displayed page; it does not remove the
CPU/GPU deadline.

## 11. Timing Consequences

### 11.1 Fixed-step behavior

Movement speeds, interpolation deltas, fade steps, animation waits, and script
countdowns are generally authored for the update rate of their owning module.
They are not expressed in seconds. A value described as a frame count means the
specific update event that decrements it:

- a Field scheduler selection;
- a World task dispatch;
- a Battle input/frame update;
- a Battling live update;
- a Menu draw iteration;
- a sprite or Gear animation interpreter tick;
- or a raw VBlank callback.

Converting all such counters through one global FPS would change game behavior.

### 11.2 Input visibility

Input is sampled more frequently than some simulation loops. Queue draining and
OR-merging preserve short press events across the two to four VBlanks of a Field
iteration and the two VBlanks of a World iteration. Menus generally consume at
most one translated command per rendered iteration even if several snapshots
are pending.

Central Battle's catch-up mode explicitly distinguishes the ordinary input
update from repeated timing updates. This prevents every missed interval from
being treated as a fresh full player command while still advancing eligible
frame-side timers.

### 11.3 Presentation parity

Field, World, Battle, Battling, and Menu all alternate packet or framebuffer
state. A logical update normally builds one parity while the other is displayed.
Cleanup often needs one or more extra synchronized frames before memory or VRAM
owned by both parities can be released. Those settling iterations are part of
resource lifetime and do not imply an extra simulation tick.

## 12. Function Index

| Address | Function |
|---:|---|
| Resident `0x80035E44` | Advance the 60-tick persistent play-time clock |
| Resident `0x8003634C` | VBlank controller, input-queue, play-time, and output service |
| Resident `0x8004B54C` | Query timing or synchronize to VBlank |
| Resident `0x8004B694` | Bounded VBlank wait |
| Resident `0x8004B7D0` | Install the VBlank callback |
| Resident `0x8004BF78` | Increment VBlank count and dispatch callbacks |
| Resident `0x8002C700` | Transform and dispatch one resident model resource |
| Resident `0x8003101C` | Reject a model whose tested bounding points are outside the view |
| `0x800705DC` | Initialize Field runtime state and reset timing mode to 30 FPS |
| `0x800748E8` | Traverse, cull, and submit the Field model collection |
| `0x8007554C` | Run the full Field presentation pass and minimum-interval poll |
| `0x8007781C` | Record Field horizontal-retrace timing |
| `0x80077DAC` | Begin one Field iteration and process queued input |
| `0x80077E88` | Coordinate the Field main loop and modal waits |
| `0x800925A0` | Select Field timing mode and dialogue opening length |
| `0x800A3474` | Restore transient Field state, including the selected timing mode |
| `0x800A3F4C` | Save transient Field state, including the selected timing mode |
| `0x800A7C58` | Run Field-integrated STR playback |
| `0x800AAA74` | Apply the coarse camera-relative Field object visibility test |
| `0x800712D0` | Run the 30 FPS World Map frame loop |
| `0x8009F5B8` | Traverse and render geometry-bearing Battle model bones |
| `0x800A9A50` | Advance and render Battle model tasks and their attachments |
| `0x800B88C4` | Initialize Battle 3D rendering and initial animation delta |
| `0x800BE790` | Render one central Battle frame and calculate catch-up |
| `0x8008A9C0` | Dispatch ordinary or catch-up Battle input/frame updates |
| `0x800800CC` | Adjust the Battling frame-rate option |
| `0x80083CE8` | Format Battling elapsed update time |
| `0x80088C00` | Sample Battling horizontal timing for diagnostics |
| `0x80088E90` | Run the Battling render and synchronization loop |
| `0x801C7BF4` | Run one complete General Menu frame |
| `0x80076488` | Run standalone Movie playback and display synchronization |
| `0x801D30C4` | Publish a completed MDEC frame after strip upload |
| `0x801D3F7C` | Service STR decode, completion, looping, and stall recovery |
