# Fighter Runtime, Movement, And Terrain

## 1. Runtime Model

Battling simulates two Gear fighters in a continuous arena. Each fighter owns
position, velocity, action, collision, command, projectile, replay, and
controller state inside one `0x171C`-byte record. The two records are adjacent
and point to one another:

| Side | Runtime address | Opponent pointer |
|---:|---:|---:|
| `0` | `0x8009872C` | Runtime `+0xD8` points to `0x80097010` |
| `1` | `0x80097010` | Runtime `+0xD8` points to `0x8009872C` |

The distance between the two bases is exactly `0x171C`. Functions accept a
fighter-runtime pointer rather than a side index, so the same movement and
terrain paths operate on both sides.

Actions, attack geometry, projectiles, health, heat, reactions, and knockout
are documented in
[`05-actions-collision-health-and-heat.md`](05-actions-collision-health-and-heat.md).
The serialized heightfield and packed terrain flags are documented in
[`../graphics/03-resource-formats.md#8-battling-heightfield-and-texture-flags`](../graphics/03-resource-formats.md#8-battling-heightfield-and-texture-flags).

All angles in this chapter use Battling's 12-bit circle unless stated
otherwise:

```text
one revolution = 0x1000
half turn       = 0x0800
quarter turn    = 0x0400
```

Integer division truncates toward zero. Signed divisions whose inputs may be
negative receive explicit rounding corrections before shifts where shown by the
gameplay functions.

## 2. Fighter Runtime Layout

### 2.1 Position and motion prefix

The beginning of each record holds root transforms and the velocity channels
combined during one simulation update:

| Offset | Width | Gameplay use |
|---:|---:|---|
| `+0x000` | `0x10` | Current root position; X, Y, and Z are `s32` values at `+0x00`, `+0x04`, and `+0x08` |
| `+0x010` | `0x0C` | Movement velocity XYZ |
| `+0x020` | `0x0C` | Recoil and launch velocity XYZ |
| `+0x030` | `0x0C` | Move/lunge velocity derived from `+0x44` |
| `+0x040` | 4 | Final horizontal movement speed |
| `+0x044` | 4 | Move-imposed forward displacement accumulator |
| `+0x048` | 4 | Requested/normalized movement magnitude from controller or COM, `0..0x100` |
| `+0x04C` | 1 | Requested animation ID |
| `+0x04E` | 1 | Previous animation ID; `0xFF` forces a restart |
| `+0x04F` | 1 | Base animation subframe step |
| `+0x050` | 2 | Last animation frame submitted to event processing |
| `+0x052` | 1 | Movement-derived animation step |
| `+0x054` | 4 | Current root/fighter heading |
| `+0x058` | 4 | Requested movement heading |
| `+0x05C` | 4 | Model hierarchy pointer |
| `+0x060` | 4 | Model root pointer |
| `+0x06C` | `0x10` | Previous root position retained for swept movement |
| `+0x080` | 4 | Fifteen-entry combo move table |
| `+0x084` | 4 | Current four-byte move descriptor |

The four directional acceleration accumulators are independent:

| Offset | Digital direction |
|---:|---|
| `+0x088` | Left |
| `+0x08C` | Up |
| `+0x090` | Right |
| `+0x094` | Down |

Their tuning values are:

| Offset | Width | Use | Initial value |
|---:|---:|---|---:|
| `+0x098` | 4 | Acceleration per update | `0x10` |
| `+0x09C` | 4 | Deceleration per update | `0x10` |
| `+0x0A8` | 4 | Motion limit used by supporting action logic | `0x60` |
| `+0x0B0` | 4 | Current terrain floor height | Calculated |

### 2.2 Action, terrain, and collision state

The movement loop shares these fields with action and collision processing:

| Offset | Width | Gameplay use |
|---:|---:|---|
| `+0x0C3` | 1 | Short reaction phase timer |
| `+0x0C4` | 1 | Physical state |
| `+0x0C5` | 1 | Action phase |
| `+0x0C6` | 1 | Previous physical state |
| `+0x0CC` | 2 | Base controller heading |
| `+0x0CE` | 2 | Camera-relative controller heading |
| `+0x0D0` | 4 | Primary fighter flags |
| `+0x0D4` | 4 | Secondary and transition flags |
| `+0x0D8` | 4 | Opponent fighter-runtime pointer |
| `+0x0FC` | 4 | Smoothed facing heading |
| `+0x104` | `16 * 0x54` | Sixteen swept attack-volume records |
| `+0x64C` | `8 * 0x44` | Eight projectile records |
| `+0x8F4` | 4 | Distance from this fighter's nearest active projectile to its opponent |
| `+0x8F8` | 4 | Pointer to that owned projectile |
| `+0x8FC` | 4 | Fighter animation and collision resource |
| `+0x900` | 4 | Animation-event offset table |
| `+0x908` | 1 | Resource-defined actor property |
| `+0x909` | 1 | Fighter/Gear ID |
| `+0x90A` | 1 | Fighter-specific movement and terrain behavior bits |
| `+0x92C` | `0x10` | Lower body anchor |
| `+0x93C` | `0x10` | Upper body anchor |
| `+0x94C` | `0x10` | Derived body center |
| `+0x95C` | `0x10` | Previous body center |

Primary flag uses which directly affect movement are:

| Mask | Gameplay meaning |
|---:|---|
| `0x00000002` | L1 defensive request for the current update |
| `0x00000004` | Guard is effective for the current update |
| `0x00000040` | COM control |
| `0x00000100` | Defensive movement suppression |
| `0x00008000` | Cross boost request for the current update |
| `0x00040000` | Ground contact |
| `0x00080000` | Motion-orientation transition |
| `0x00800000` | Knockout |
| `0x01000000` | Close-range reaction latch |
| `0x08000000` | Fighter side |
| `0x60000000` | Terrain class in bits `29..30` |

### 2.3 Command, replay, and tuning tail

| Offset | Width | Gameplay use |
|---:|---:|---|
| `+0x998` | 2 | Animation frame supplied to event processing |
| `+0x99A` | 2 | Number of animation frames crossed this update |
| `+0x99E` | 2 | Signed animation subframe accumulator |
| `+0x9A0` | `0x20` | 32-byte command ring |
| `+0x9C0` | 1 | Command-ring write counter |
| `+0x9C1` | 1 | Command-ring read counter |
| `+0x9C2` | 1 | Command count |
| `+0x9C3` | 1 | Current combo-tree selection, `0..14` |
| `+0x9CC` | `256 * 0x0C` | Replay samples |
| `+0x15CC` | 4 | Current replay-sample pointer |
| `+0x15D0` | 4 | Previous or alternate replay-sample pointer |
| `+0x15D4` | 4 | Fighter effect color/state |
| `+0x15D8` | `7 * 2` | Motion-control halfwords |
| `+0x15F0` | 2 | Resource-defined movement-speed factor |
| `+0x15F2` | 2 | Effective movement and animation scale |
| `+0x15F4` | 2 | Base movement and animation scale |
| `+0x15F6` | 2 | Auxiliary scale, initialized to `0x100` |
| `+0x15F8` | 2 | Auxiliary state, initialized to zero |
| `+0x15FC` | 4 | COM state pointer |
| `+0x1600` | 4 | Common fighter-record pointer |

`BattlingInitializeMotionTuning` at `0x80078ED4` initializes the seven
halfwords at `+0x15D8` as:

```text
[0x0100, 0x0000, 0x0010, 0x0000, 0x0000, 0x0030, 0x0030]
```

`BattlingInitializeBattlerRuntime` at `0x80078F00` takes `+0x15F0` from fighter
resource byte `+0x0F`. It copies fighter resource halfword `+0x0C` to both
`+0x15F2` and `+0x15F4`, sets `+0x15F6` to `0x100`, and clears `+0x15F8`.

## 3. Simulation Update Order

`BattlingBoutSimulationUpdate` at `0x80079DF0` executes one complete live
update in this order:

1. Select the current 12-byte replay slot for each fighter.
2. Update side 0's eight projectiles.
3. Update side 1's eight projectiles.
4. Clear transient speed, hit, boost, and attack-display state.
5. Test side 0 against side 1's active projectiles and attack volumes.
6. Test side 1 against side 0's active projectiles and attack volumes.
7. Poll controllers while the opening countdown and activity permit player input.
8. Run COM control for each COM-owned fighter.
9. Clear movement and commands after a bout-ending knockout.
10. Advance bout state, countdowns, and fighter-to-fighter headings.
11. Drain fighter timers, Heat, pending damage, and health.
12. Consume commands and update each fighter's action state.
13. Build both fighters' facing, movement, gravity, and recoil velocities.
14. Integrate side 1 against terrain, then side 0.
15. Resolve body overlap between the two fighters.
16. Synchronize model roots to the resulting transforms.
17. Record replay state and advance both animations.
18. Refresh body anchors and state transitions.
19. Process animation events crossed during the update.
20. Advance shared effects and optional rubber-band contact effects.

The global update hold at `0x80092664` skips this sequence while nonzero and
decrements itself instead. Collision therefore consumes projectiles and swept
volumes which were already active at the beginning of the update. Attack events
crossed in step 19 create geometry for a subsequent update.

## 4. Controller Inputs And Command Ring

`BattlingProcessControllerMovement` at `0x80076884` uses these logical button
masks:

| Mask | Button | Immediate gameplay operation |
|---:|---|---|
| `0x0004` | L1 | Request defense, clear queued commands, and suppress acceleration |
| `0x0008` | R1 | Enqueue command `3` on the press edge |
| `0x0010` | Triangle | Enqueue command `1` on the press edge |
| `0x0020` | Circle | Enqueue command `2` on the press edge |
| `0x0040` | Cross | Request boost while held; clear commands on the press edge |
| `0x0080` | Square | Clear the ring and enqueue command `4` on the press edge |
| `0x1000` | Up | Increase accumulator `+0x08C` |
| `0x2000` | Right | Increase accumulator `+0x090` |
| `0x4000` | Down | Increase accumulator `+0x094` |
| `0x8000` | Left | Increase accumulator `+0x088` |

Triangle and Circle also increment the combined counter at `+0x1660`; R1
increments `+0x165C`. Cross and L1 are held-state controls and are not ordinary
command bytes.

The command ring is managed by:

| Address | Function | Operation |
|---:|---|---|
| `0x8007639C` | `BattlingEnqueueFighterCommand` | Append one command unless count is already 32 |
| `0x800763E4` | `BattlingDequeueCommand` | Return zero when empty, otherwise remove the oldest command |
| `0x80076424` | `BattlingClearCommandQueue` | Clear counters, count, and combo selection |

Its exact operations are:

```text
enqueue(command):
    if count < 32:
        commands[write_counter & 0x1F] = command
        write_counter++
        count++

dequeue():
    if count == 0:
        return 0
    command = commands[read_counter & 0x1F]
    read_counter++
    count--
    return command

clear():
    write_counter = 0
    read_counter = 0
    count = 0
    combo_selection = 0
```

The unmasked counters are bytes; only storage access masks them with `0x1F`.
Enqueue silently drops a command when all 32 entries are occupied.

## 5. Directional Acceleration

### 5.1 Digital input

Each held digital direction increases its accumulator by `+0x98` and clamps it
to `0x100`. A released direction decreases by `+0x9C` and clamps to zero.
Battling forms the signed components:

```text
forward_component = accumulator_up - accumulator_down
side_component    = accumulator_right - accumulator_left

raw_magnitude = sqrt(forward_component^2 + side_component^2)
movement_input = min(raw_magnitude, 0x100)

if raw_magnitude < 0x30:
    movement_input = 0
```

When movement input is nonzero, the requested heading is:

```text
requested_heading = atan2(side_component, forward_component)
                  + camera_relative_heading
```

The heading is retained modulo `0x1000` by its consumers.

### 5.2 Analog input

Analog controller bytes are centered on `0x80` and doubled:

```text
analog_x = 2 * (raw_x - 0x80)
analog_y = 2 * (raw_y - 0x80)

analog_magnitude = sqrt(analog_x^2 + analog_y^2)
```

When no digital direction is held, analog magnitude below `0x30` discards the
analog heading. Outside that dead zone, the signed axes limit their matching
directional accumulators rather than bypassing the accumulator model.

### 5.3 Defensive deceleration

While L1 is held, no directional accumulator can increase. The update first
multiplies the base deceleration by four, then uses two thirds of that value:

```text
guard_deceleration = (4 * base_deceleration) * 2 / 3
```

With the initial deceleration `0x10`, this evaluates to `42`. All four
accumulators move toward zero using this amount.

Terrain class and fighter resource bits can halve the ordinary acceleration:

| Terrain class | Fighter behavior test | Acceleration |
|---:|---|---:|
| `3` | Resource byte `+0x90A` bit `0x04` clear | `base / 2` |
| `1` | Resource byte `+0x90A` bit `0x04` set | `base / 2` |
| Other combinations | - | `base` |

## 6. Motion Scale And Speed

Before input is applied, `BattlingPrepareFrameStateAndAttackDisplay` at
`0x800764CC` rebuilds the effective scale:

```text
effective_scale_15F2 = base_scale_15F4 * global_motion_scale / 0x100
```

The standalone `MOTION SPEED` option selects the global scale:

| Displayed setting | Internal index | Global scale |
|---:|---:|---:|
| `1` | `0` | `0x0060` |
| `2` | `1` | `0x0080` |
| `3` | `2` | `0x00BB` |
| `4` | `3` | `0x0100` |
| `5` | `4` | `0x0180` |
| `6` | `5` | `0x0200` |
| `7` | `6` | `0x0300` |
| `8` | `7` | `0x0400` |

Setting 4 is initialized as the neutral `0x100` scale. The final horizontal
speed computed by `BattlingUpdateActionState` at `0x80077A9C` is:

```text
final_speed = movement_input * movement_factor_15F0
            * effective_scale_15F2 >> 16
```

For locomotion state `1`, the movement-derived animation increment is:

```text
animation_step_52 = movement_input * effective_scale_15F2 >> 12
```

Eligible move descriptors can add `0x40` to `+0x44` for an attack lunge. Cross
boost doubles `final_speed` and the movement-derived animation step after its
Heat charge succeeds. Heat and boost rejection are specified in the next
chapter.

## 7. Physical State And Facing

Physical state byte `+0xC4` uses these operational values:

| Value | Movement behavior |
|---:|---|
| `0` | Grounded with zero movement input |
| `1` | Grounded with nonzero movement input |
| `2` | Airborne form of state `0` |
| `3` | Airborne form of state `1` |
| `4` | Heavy knockback; normal input speed is forced to zero |
| `5` | Knockout; health and action updates return immediately |

Outside heavy knockback, bit zero is rebuilt from whether `+0x48` is zero. Bit
one is cleared when root Y is within `0x30` of the current floor. Square sets bit
one, subtracts `0x8C` from vertical movement velocity at `+0x14`, and leaves the
resulting state as `2` or `3` according to its prior locomotion bit.

`BattlingAccumulateVelocityAndFacing` at `0x80078194` obtains the shared
fighter-to-fighter heading and adds `0x800` for the opposite side. Facing is
interpolated toward that heading in steps controlled by `0x100`, except during
states which lock the heading. The movement vector uses:

```text
velocity_x = -(sin(facing) * final_speed) >> 12
velocity_z = -(cos(facing) * final_speed) >> 12
```

Gravity is applied every live update:

```text
velocity_y += 10
```

Recoil velocity at `+0x20/+0x28` is then added. Its horizontal retention factor
comes from terrain class bits in `+0xD0`:

| Class | Retained recoil |
|---:|---:|
| `0` | `255 / 256` |
| `1` | `128 / 256` |
| `2` | `0` |
| `3` | `0` |

Physical state `4` forces `255 / 256` retention regardless of terrain class.

## 8. Heightfield Coordinates

Battling's terrain resource is a fixed `128 * 128` array of four-byte cells.
Runtime X and Z use eight fractional bits for cell selection:

```text
cell_x = x >> 8
cell_z = z >> 8
cell = grid[cell_z * 128 + cell_x]
```

Each cell stores a signed height and packed flags. Serialized height is
multiplied by `12` during initialization. See
[`../graphics/03-resource-formats.md#8-battling-heightfield-and-texture-flags`](../graphics/03-resource-formats.md#8-battling-heightfield-and-texture-flags)
for the resource layout and non-gameplay texture fields.

`BattlingGetArenaTerrainCell` at `0x800828C4` returns the packed cell word.
Terrain class is packed in flag bits `8..9` and becomes fighter primary-flag
bits `29..30` during integration.

## 9. Floor Sampling

`BattlingSampleArenaFloorHeightAndNormal` at `0x80082488` loads the four corners
of the containing cell square:

```text
(x0, z0)         (x0 + 0x100, z0)
(x0, z0 + 0x100) (x0 + 0x100, z0 + 0x100)
```

The point's signed position relative to the square diagonal chooses one of two
triangles. The function computes that triangle's plane normal and evaluates the
plane at the supplied X and Z. Floor Y is therefore planar within a triangle,
not a nearest-cell or bilinear height lookup.

When collision-adjusted sampling is requested, each corner's terrain class
modifies its height before the triangle plane is built:

| Terrain class | Added height |
|---:|---:|
| `0` | `0` |
| `1` | `0x100` |
| `2` | `0` |
| `3` | `0x40` |

The class-1 addition is performed as `+0xC0` followed by the shared `+0x40`
branch. Class 3 receives only the shared `+0x40`.

## 10. Arena Boundary

The playable arena is constrained around:

```text
center_x = 0x3F80
center_z = 0x3F80
radius   = 16000 = 0x3E80
```

`BattlingConstrainArenaMovementToRadius` at `0x800828F8` first evaluates:

```text
candidate_x = root_x + velocity_x
candidate_z = root_z + velocity_z

distance = sqrt((candidate_x - 0x3F80)^2
              + (candidate_z - 0x3F80)^2)
```

If `distance` exceeds `16000`, it rotates the candidate displacement into a
boundary-relative frame and iteratively adjusts that displacement until the
candidate lies on or inside the radius. It rewrites the movement vector rather
than clamping the stored root directly, preserving tangential movement along the
edge.

The initial opposing placements lie around the arena center:

```text
side 0: x = 0x3E80, z = 0x3F80
side 1: x = 0x4080, z = 0x3F80
```

## 11. Terrain Integration And Landing

`BattlingIntegrateTerrainMovement` at `0x80078D20` performs the fighter's terrain
transaction:

1. Sample collision-adjusted floor Y at the current X/Z.
2. Store the terrain class in primary-flag bits `29..30`.
3. For class 3, initialize the resource-specific contact timer at `+0x90B` to
   `0x0F`.
4. Constrain horizontal velocity to the arena radius.
5. Clear ground-contact bit `0x00040000`.
6. Compare candidate `root_y + velocity_y` to floor Y.
7. On contact, set the ground bit, resolve bounce or landing, and snap root Y to
   the floor.
8. Add final XYZ velocity to the root.

The Y convention makes larger values move toward or below the floor. Contact is
recognized when:

```text
root_y + velocity_y > floor_y
```

Ordinary contact sets vertical velocity to zero. During heavy-knockback state
`4`, downward velocity of at least `0x40` bounces as:

```text
velocity_y = -velocity_y / 3
```

The first such heavy landing also starts its contact response and sets secondary
flag `0x10`. Smaller heavy-knockback velocities settle to zero.

## 12. Fighter Body Separation

`BattlingResolveBodyOverlap` at `0x80078920` prevents the two moving body hulls
from passing through one another. It uses the previous roots, current roots, and
resource-defined body radii at fighter resource byte `+0x03`.

The routine first requires the vertical intervals between each fighter's lower
and upper body-anchor Ys to overlap. It then constructs a horizontal separating
line from the two previous roots, selects one hull endpoint per fighter through
`BattlingSelectCollisionHullEndpoint` at `0x80078704`, transforms those
endpoints from previous-root space into current-root space, and computes:

```text
combined_radius = radius_0 + radius_1
penetration = combined_radius - endpoint_distance
```

No displacement occurs when `endpoint_distance >= combined_radius`. On overlap,
let `m0` and `m1` be the two horizontal movement magnitudes:

```text
if m0 + m1 != 0:
    side0_share = penetration * m1 / (m0 + m1)
    side1_share = penetration * m0 / (m0 + m1)
else:
    side0_share = penetration / 2
    side1_share = penetration / 2
```

The shares are converted to opposing X/Z displacements with 12-bit sine and
cosine. The implementation carries an additional eight fractional bits during
share calculation, so its final trigonometric products shift by `20`.

## 13. Function Map

| Address | Function | Gameplay responsibility |
|---:|---|---|
| `0x8007639C` | `BattlingEnqueueFighterCommand` | Append a command to the 32-byte ring |
| `0x800763E4` | `BattlingDequeueCommand` | Remove the oldest command |
| `0x80076424` | `BattlingClearCommandQueue` | Reset command and combo state |
| `0x800764CC` | `BattlingPrepareFrameStateAndAttackDisplay` | Clear transient state and derive effective motion scale |
| `0x80076884` | `BattlingProcessControllerMovement` | Convert controller state into commands, acceleration, speed, and heading |
| `0x80077A9C` | `BattlingUpdateActionState` | Consume commands and derive final speed and animation movement |
| `0x80078194` | `BattlingAccumulateVelocityAndFacing` | Build facing, movement, gravity, and recoil velocity |
| `0x80078704` | `BattlingSelectCollisionHullEndpoint` | Select the body-hull endpoint used for separation |
| `0x80078920` | `BattlingResolveBodyOverlap` | Separate overlapping swept fighter hulls |
| `0x80078D20` | `BattlingIntegrateTerrainMovement` | Constrain, land, bounce, and integrate a fighter |
| `0x80078E94` | `BattlingSynchronizeModelTransform` | Copy gameplay root and facing into the fighter model |
| `0x80078ED4` | `BattlingInitializeMotionTuning` | Install default motion-control values |
| `0x80078F00` | `BattlingInitializeBattlerRuntime` | Initialize one complete fighter record |
| `0x8007920C` | `BattlingUpdateAnchorsAndStateEffects` | Refresh body anchors and movement-state transitions |
| `0x800796B8` | `BattlingUpdateSharedArenaCamera` | Derive camera-relative side, midpoint, distance, and headings |
| `0x80079DF0` | `BattlingBoutSimulationUpdate` | Execute one ordered Battling simulation update |
| `0x80082488` | `BattlingSampleArenaFloorHeightAndNormal` | Select a heightfield triangle and evaluate its plane |
| `0x800828C4` | `BattlingGetArenaTerrainCell` | Fetch one packed terrain cell |
| `0x800828F8` | `BattlingConstrainArenaMovementToRadius` | Keep candidate movement within radius `16000` |
