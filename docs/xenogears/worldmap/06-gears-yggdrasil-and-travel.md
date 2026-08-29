# Gears, Yggdrasil, And Travel

## 1. Travel Forms

`WorldMapSetupTravelMode` at `0x80073300` combines persistent vehicle state with
the three party `is_on_gear` flags to derive the active travel form. The caller
handles World entry mode and other mode parameters separately. Runtime tasks
preserve all three travel representations even when only one is visible.

| Form | Active leader | Exit group | Typical speed field |
|---|---|---:|---:|
| On foot | Task 1 | 0 | `8` |
| Gear | Task 4 | 1 | `12` |
| Yggdrasil | Task 7 | 2 plus internal exits | Variable flight speed |

## 2. Walking Party

`WorldMapPlayerTravelTaskInit` at `0x8008A2C8` creates the leader sprite, restores the
saved World position and heading, samples terrain altitude, and fills the
32-sample follower history. Empty party slots disable their follower tasks.

`WorldMapPlayerTaskUpdate` at `0x8008A72C`:

1. Converts input to a camera-relative heading and unit step.
2. Selects idle or walking animation.
3. Resolves terrain, model, and dynamic collision.
4. Commits accepted position and follower history.
5. Selects a walking exit and publishes radar state.
6. Saves integer World position and heading to persistent travel fields.

When the leader is on a Gear, its walking task hides the sprite and mirrors the
leader Gear pose. During Yggdrasil travel it mirrors the vehicle pose.

## 3. Gear Creation And Availability

`WorldMapCreateGear` at `0x8008C364` combines party occupancy, character Gear
assignment, Field-return flags, and `is_on_gear` state. An available Gear receives
its party-specific sprite bundle, scale, animation, saved pose, and heading.

Each Gear has a separate saved X/Z position and heading. This allows a character
to walk away from a parked Gear, return from Field, or change party composition
without collapsing walking and Gear positions into one value.

## 4. Gear Movement

`WorldMapPlayerGearTaskUpdate` at `0x8008C844` uses the same camera-relative
eight-direction input as walking, with a larger speed and a Gear travel-mode
permission row. Accepted movement updates follower history, ground-contact
effects, Gear exits, radar position, and persistent Gear position.

Ground effects are selected from terrain class. The leader clears or replaces
the active particle layer when movement, material, or travel state changes.
Follower Gears consume delayed history and use wrapped target stepping.

## 5. Boarding The Yggdrasil

Boarding begins when a walking character or Gear action intersects the
Yggdrasil's dynamic cylinder. The leader Gear becomes coordinator:

```text
detect vehicle owner
    -> command walking/Gear follower tasks
    -> choose wrapped direction toward boarding target
    -> move each visible Gear into the vehicle
    -> signal one completion to Yggdrasil task
    -> hide completed party representation
```

The Yggdrasil's occupancy counter at task `+0x74` is compared with the active
party count. Once all members have reported, party and Gear tasks switch to
vehicle travel, the radar target becomes the Yggdrasil, and the alternate flight
music and takeoff state begin.

## 6. Yggdrasil Runtime State

The Yggdrasil task reuses the common record as follows:

| Offset | Vehicle meaning |
|---:|---|
| `+0x20` | Major state: grounded, flying, takeoff, landing, or transition. |
| `+0x28` | Live fixed-point position. |
| `+0x38` | Normalized movement direction. |
| `+0x48` | Heading/yaw. |
| `+0x4A` | Speed limit. |
| `+0x58` | Accumulated heading phase. |
| `+0x5C` | Heading velocity. |
| `+0x60` | Forward/reverse speed. |
| `+0x68` | Target landing altitude. |
| `+0x6C` | Pitch-control state. |
| `+0x70` | Fixed-point pitch. |
| `+0x74` | Number of boarded party members. |
| `+0x78` | Chosen disembark/landing heading. |
| `+0x7C` | Landing-effect gate. |

`WorldMapYggdrasilSubModelsTaskUpdate` at `0x800907F4` independently accelerates
or decelerates the two attached engine rotations during takeoff and landing.

### Alternate Vehicle Reconstruction

`WorldMapYggdrasilPCTaskInit` at `0x8008E4F4` is the reconstruction initializer
used when the task already contains a live transform. Rather than loading the
ordinary persistent vehicle pose again, it copies task position and visibility
to root model 0, selects reconstruction behavior from numeric travel modes
`1..7`, resamples terrain altitude where required, and synchronizes visibility
for additional model records 1 through 3. It then rebuilds the transforms for
main models 0 and 1 from negative pitch and 12-bit heading.

The matching `WorldMapYggdrasilSubModelsPCTaskInit` at `0x800907C4` attaches model
records 2 and 3 to root model 0 before the ordinary submodel updater resumes.
This path preserves task-owned flight state while recreating model hierarchy and
matrices that could not survive resource teardown.

## 7. Flight Controls

`WorldMapUpdateYggdrasilFlightControls` at `0x80090FB4` applies inertia:

- Left/right input approaches a target roll and yaw velocity.
- Heading accumulates the current yaw velocity and wraps to 12 bits.
- Pitch approaches a bounded positive or negative target while held and returns
  toward level when released.
- Throttle changes forward or reverse speed within the task's limit and decays
  toward zero when released.
- Heading and pitch produce a normalized three-dimensional movement direction.

The resulting speed and direction are passed to
`WorldMapCheckYggdrasilPosition`. Flight clamps altitude relative to terrain and
a global ceiling, applies model and terrain collision, then tests dynamic
objects and vehicle exits.

## 8. Terrain Proximity Effects

While flying near terrain, the vehicle compares its altitude with sampled ground
height. Water and sand classes select different effect layers beneath the ship.
Moving away from the ground clears both layers. Takeoff and landing select an
additional material-dependent burst at the contact position.

The vehicle models use a shared transform built from negative pitch, heading,
and roll. Both main model records receive the same position and orientation;
attached engine models compose their private rotation afterward.

## 9. Landing Validation

Landing validation depends on the Yggdrasil's major state. In state 2, a request
continues only when the current travel-mode-2 permission lookup is nonzero, the
same allowed polarity used by ordinary traversal. In state 3, this current-cell
test is omitted. Both states then call `WorldMapFindYggdrasilLandingDirection` at
`0x8008E0F0`, which tries headings in `0x100` increments around the full turn and
accepts the first point `0x68000` units away that passes mode-2 movement.

The landing state clears active exits and flight effects, stores target terrain
altitude, and descends by fixed increments. At contact it:

1. Snaps the vehicle to terrain altitude.
2. Changes party and Gear tasks to disembark states.
3. Marks every populated party slot as riding; Gear creation later resolves
   actual Gear availability.
4. Resets occupancy and vehicle-flight flags.
5. Reinitializes the encounter schedule. The state-`0x10` completion path also
   switches to ground-travel music; state `0x14` does not perform that switch.

## 10. Disembarking

The leader Gear starts at the saved vehicle position, faces the chosen landing
heading, and moves toward a point outside the vehicle. When it reaches that
point, it becomes the active leader, reseeds all follower history, and changes
the travel form to Gear. Follower Gears emerge through coordinated task commands.

Gear dismount enters the sequence only when the walking-row permission lookup is
nonzero, matching ordinary walking traversal. A zero lookup leaves the Gear
active.

## 11. Vehicle-Owned Exits

`WorldMapSetupYggdrasilInternalExit` at `0x8008E078` recognizes selected dynamic
vehicle owners and selects one of two static records. Owner `0x0F` selects Field
`0x138`, World mode 1, and prompt `0x0E`; owner `0x10` selects Field `0x1B8`,
World mode 1, and prompt `0x1D`. These records obtain their trigger from dynamic
collision rather than the configuration rectangle table.

The vehicle exit flow stores Yggdrasil position and heading independently so the
ship remains at the same wrapped location when World travel resumes.
