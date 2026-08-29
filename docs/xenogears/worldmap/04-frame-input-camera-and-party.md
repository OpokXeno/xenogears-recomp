# Frame, Input, Camera, And Party

## 1. Frame Coordinator

`WorldMapFrameCoordinator` at `0x800712D0` coordinates the global
state surrounding task dispatch. It samples controller state, tracks pressed and
held channels, manages pause and controller availability, evaluates Menu and
encounter requests, and publishes the frame's transition decision.

The ordinary loop alternates two render contexts:

1. Drain controller samples and service one streaming step.
2. Select the other context and packet-buffer parity.
3. Clear its `0x400` ordering-table entries and reset per-frame packet cursors.
4. Dispatch all 64 tasks in index order.
5. Synchronize the GPU and install draw/display environments.
6. Arbitrate pause, controller loss, encounter, Menu, and loop termination.
7. Advance the two animated texture sets and draw from bucket 1023 toward bucket
   0, including the final frame of a selected transition.

Simulation tasks and presentation share a frame, so ordering determines which
position, exit, camera, and streaming origin the ground producer observes.

The coordinator owns the complete ordinary frame loop. It drains all queued
controller samples into held/pressed channels, dispatches tasks, installs the
presentation environment, arbitrates pause, controller loss, encounters, Menu,
and termination, then updates animated textures and submits the ordering table.
Battle, Field, and Menu requests are published as shared transition state rather
than executed from an individual render task.

## 2. Input Representation

World maintains current and previous values for three input channels. The first
contains direction and vehicle axes; the second contains action requests; the
third supports camera and interface behavior. `WorldMapUpdateInputTransitionFlag`
at `0x80090A18` derives a one-frame transition when a two-bit chord changes from
inactive to active.

The directional high nibble is interpreted as eight camera-relative headings:

| Direction value | Heading relative to camera yaw |
|---:|---:|
| `1` | `0` |
| `2` | `+0x400` |
| `3` | `+0x200` |
| `4` | `+0x800` |
| `6` | `+0x600` |
| `8` | `-0x400` |
| `9` | `-0x200` |
| `0xC` | `-0x600` |

Walking and Gear controls convert the selected heading to a unit X/Z vector.
Yggdrasil flight instead accumulates yaw velocity, roll, pitch, and forward or
reverse speed, producing smooth inertial controls.

## 3. Action Arbitration

The travel control functions return small action codes rather than directly
performing every transition:

| Result | Meaning in the receiving task |
|---:|---|
| `0` | Continue ordinary movement. |
| `1` | Accept the active exit and end the World loop. |
| `3` | Begin a boarding interaction detected through dynamic collision. |
| `4` | Begin Yggdrasil landing or Gear dismount behavior. |

An action request is gated by active exits, mode-transition exits, current
boarding collision, Menu eligibility, and vehicle motion. The task receiving the
result owns the state sequence: the leader handles walking exits, the leader
Gear coordinates boarding, and the Yggdrasil coordinates landing and vehicle
exits.

Post-dispatch arbitration applies additional shared gates:

- a task that has already cleared the World loop prevents a new pause, encounter,
  or Menu route from replacing its result;
- a pending Menu request suppresses encounter selection;
- an active exit prompt or secondary dialogue suppresses encounter selection and
  party-form toggling;
- Start enters the blocking pause loop through mask `0x0800` when no higher-level
  request is active;
- controller loss enters its own blocking loop under the same transition gates;
- an expired encounter opportunity is accepted only when the loop, Menu, prompt,
  dialogue, and transition state are all clear.

The opportunity flag is cleared after arbitration whether or not a Battle is
accepted. Menu requests in travel modes `1..3` use the in-place reconstruction
route; modes `4..7` select a World exit route instead.

## 4. Camera Layers

The ordinary camera uses two tasks:

| Task | Responsibility |
|---|---|
| World camera, slot 9 | Follow the active travel target through wrapped space and publish streaming-grid movement. |
| Camera controller, slot 10 | Interpolate orbit yaw, pitch, distance, zoom mode, view vectors, and shake. |

`WorldMapWorldCameraTaskUpdate` at `0x800914D0` selects the shortest wrapped delta
to its target. Its resulting world position drives `WorldMapSetGridUpdateMask`,
so streaming follows the camera-centered travel area instead of a raw unwrapped
coordinate.

The world-camera task separates target heading from followed position. In its
ordinary state it accepts discrete orbit input, turns in `0x40`-unit steps toward
a target `0x200` away, and smooths the published heading by one eighth of the
angular delta. This ordinary state wraps the delta only when its magnitude exceeds
`0xC00`, rather than at the shortest-path boundary `0x800`; deltas in
`0x801..0xC00` therefore retain their original direction. Scripted fast and slow
approach states use the conventional `0x800` boundary. Completing the fast
approach sends command `0x0B` to the Yggdrasil task.

Position following also has two rates. A wrapped X/Z displacement whose
one-eighth step is below `0x40` is applied in full; a larger displacement advances
the camera by one eighth and marks interpolation active. This avoids both a long
path across the world seam and a camera snap when the controlled vehicle moves
far enough in one frame.

`WorldMapCameraTaskUpdate` at `0x80091C18` approaches target angles and distance
over time. `WorldMapSelectCameraZoomLevel` at `0x80091FF8` checks candidate camera
distances against terrain and chooses a safe level. The final view is represented
as eye, target, and up vectors or as Euler rotation plus translation.

Four zoom profiles provide target distance and pitch pairs, with distances
`0x280000`, `0x370000`, `0x460000`, and `0x550000`. Before selecting a profile,
the camera projects a candidate view and samples a `6x6` terrain area. It tests
levels 0 through 2 against terrain and uses level 3 as the fallback. When the
candidate is nearer than the current level, the selector changes to it only when
the clearance difference exceeds `0x40`; otherwise it retains the current farther
level, preventing rapid oscillation near a marginal obstruction.
Commands 9 and 10 exchange profile tables, command `0x0E` resets automatic zoom,
and command `0x11` enters a fixed-distance interpolation used by a vehicle
transition.

## 5. Camera Matrix Paths

| Address | Function | Use |
|---:|---|---|
| `0x80096F18` | `WorldMapComputeCameraVector` | Derive eye, target, and up from camera state. |
| `0x80097070` | `WorldMapExtractCameraAngles` | Recover camera angles from view vectors. |
| `0x80097244` | `WorldMapBuildLookAtViewMatrix` | Build scripted/cinematic look-at view. |
| `0x80097440` | `WorldMapBuildViewMatrix` | Build ordinary Euler-based view. |

Ordinary travel primarily uses the Euler path. Scripted scenes use look-at
cameras, keyframed positions, target models, distance interpolation, and bounded
random shake while retaining the same wrapped terrain and model coordinates.

## 6. Radar And Minimap State

The active leader publishes a radar position and heading after accepted movement.
Walking uses slot 1, Gear travel uses slot 4, and Yggdrasil travel uses slot 7.
The radar renderer combines this pose with up to 32 configured marker positions
and a marker-enable mask.

`WorldMapMinimapYTaskUpdate` at `0x800922AC` animates the minimap's vertical
position during takeoff and landing. This keeps the interface transition aligned
with vehicle state rather than switching at the final state change.

## 7. Secondary Dialogue Task

The secondary dialogue task in slot 13 owns a window independent of the exit
prompt. Its initializer creates a window of UI class 4 and allocates both
ordering-table packet copies. The task then watches a shared signed message ID:

- `-1` means no message and clears an open window;
- a nonnegative ID resolves a string from archive section 6 and opens the window;
- changing the ID while open replaces the displayed string without recreating
  the task.

The task stores the currently displayed ID at `+0x50` and emits the active window
into the current ordering table every update. Ordinary teardown frees this window
separately from the exit-confirmation window.

## 8. Party Identity And Resources

The three persistent party slots determine which walking sprites and Gear bundles
are queued. `WorldMapAllocatePartySpriteResources` at `0x80071CDC` allocates only
the resources needed by populated party slots and available Gear assignments.
The corresponding task initializers create actors after relocation.

For each party slot, World keeps separate facts:

| Fact | Effect |
|---|---|
| Party slot populated | Create walking follower state and load its sprite. |
| Character owns an available Gear | Load and create a Gear actor. |
| `is_on_gear` flag set | Show Gear task and hide walking task. |
| Yggdrasil travel active | Hide party and Gear actors while retaining their task state. |

These facts remain separate across World entry and volatile reconstruction. The
in-place World Menu route reconciles only riding-form flags and poses; it does
not reload changed party identities or Gear assignments.

## 9. Following

Walking and Gear leaders append history only after movement is accepted by
terrain and dynamic collision. Followers sample delayed entries, compute wrapped
shortest-path deltas, update heading, and snap Y from terrain.

When a follower party slot is empty, its task remains disabled. During vehicle
travel the hidden walking members and Gears track the Yggdrasil's position or
retain independently saved positions, depending on the transition phase. On
disembark, leader history is reseeded before follower tasks resume.

## 10. Pause, Controller Loss, And Menu

`WorldMapPauseUntilStartPressed` at `0x8007634C` runs a presentation loop while
ordinary simulation is paused. `WorldMapPauseUntilControllerConnected` at
`0x80076594` similarly waits for a usable controller.

Both loops preserve the active display environment, install a pause presentation
environment, synchronize the GPU, drain every queued controller sample, and
restore the ordinary environment before returning. The first loop exits only
when Start appears in the accumulated pressed channel; the second polls controller
availability instead. Neither loop dispatches World tasks.

A Menu request enters the World-specific reconstruction route:

```text
record current party forms and wait for streaming
    -> release selected render-packet buffers
    -> preserve framebuffer and run Menu
    -> reopen World resources and rebuild graphics/packet buffers
    -> retain live simulation and reconcile riding forms and poses
```

`WorldMapReconcilePartyGearState` at `0x80075D4C` compares pre-Menu and post-Menu
party forms. A member changing to Gear receives the saved walking pose as its Gear
pose; a member changing to walking receives the saved Gear pose as its walking
pose. The active travel form is recalculated from the resulting party state.

Unlike a Battle return, this path does not save and restore the `0x2000`-byte
task snapshot. The task array, models, emitter/particle state, and resident
terrain chunks remain allocated while Menu runs.
