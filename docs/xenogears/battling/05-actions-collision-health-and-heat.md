# Actions, Collision, Health, And Heat

## 1. Action Model

Battling resolves attacks through a continuous transaction rather than a turn:

1. Controller or COM input appends a command to the fighter's 32-byte ring.
2. `BattlingUpdateActionState` consumes one eligible command and selects an
   animation or combo move descriptor.
3. Animation advancement records every integer frame crossed during the update.
4. Animation-frame ranges dispatch attack, effect, visibility, and reset events.
5. Attack events create one-frame swept volumes or persistent projectiles.
6. The opponent tests active geometry at the beginning of a later update.
7. Collision stages damage, hit type, hit position, hitstun, and reaction state.
8. Health processing drains queued damage and action processing resolves the
   staged reaction impulse.

The fighter runtime, movement pipeline, terrain, and update ordering are
documented in
[`04-fighter-runtime-movement-and-terrain.md`](04-fighter-runtime-movement-and-terrain.md).

All formulas use integer arithmetic. Division truncates toward zero unless a
formula explicitly implements ceiling division.

## 2. Commands And Action States

The command bytes consumed at `0x80077A9C` are:

| Command | Source | Action |
|---:|---|---|
| `0` | Empty ring | Clear the ring and return to ordinary state handling |
| `1` | Triangle | Follow the Triangle branch of the combo tree |
| `2` | Circle | Follow the Circle branch of the combo tree |
| `3` | R1 | Start the fighter-specific special/Ether animation |
| `4` | Square | Enter the airborne state and subtract `0x8C` from vertical velocity |
| `5` | Automatic close-range condition | Start animation `0x0F` and a six-update reaction phase |

R1 requires common fighter-record byte `+0x18` to be nonzero and is disabled for
fighter ID `0x29`. Its animation is `3` while grounded and `4` while airborne.
The action phase becomes `2`, which also suppresses ordinary guard and movement
state changes until the action settles.

`BattlingQueueCloseRangeReaction` at `0x800767C8` clears the command ring and
queues command `5` when all of these conditions hold:

- The controller-relative heading which entered the close-range branch is in
  the 12-bit interval `0x201..0x5FF`.
- Opponent distance is at most `0x200`.
- The fighter is not already in physical state `4`.
- The close-range latch is clear.
- Both fighters have ground-contact bit `0x00040000` set.
- Terrain class 1 is either absent or permitted by fighter behavior bit
  `+0x90A:0x02`.

## 3. Combo Tree And Move Descriptors

`BattlingAdvanceComboSelection` at `0x80077A38` traverses a complete binary tree
for up to three Triangle/Circle inputs. Selection zero is the root:

| Input sequence | Selection |
|---|---:|
| none | `0` |
| Triangle | `1` |
| Circle | `2` |
| Triangle, Triangle | `3` |
| Triangle, Circle | `4` |
| Circle, Triangle | `5` |
| Circle, Circle | `6` |
| Triangle, Triangle, Triangle | `7` |
| Triangle, Triangle, Circle | `8` |
| Triangle, Circle, Triangle | `9` |
| Triangle, Circle, Circle | `10` |
| Circle, Triangle, Triangle | `11` |
| Circle, Triangle, Circle | `12` |
| Circle, Circle, Triangle | `13` |
| Circle, Circle, Circle | `14` |

Both branches from selections `7..14` return to zero. The interleaved transition
table at `0x80091178` is therefore:

```text
selection 0: Triangle -> 1,  Circle -> 2
selection 1: Triangle -> 3,  Circle -> 4
selection 2: Triangle -> 5,  Circle -> 6
selection 3: Triangle -> 7,  Circle -> 8
selection 4: Triangle -> 9,  Circle -> 10
selection 5: Triangle -> 11, Circle -> 12
selection 6: Triangle -> 13, Circle -> 14
selection 7..14: either input -> 0
```

The selected four-byte descriptor is:

```text
move = move_table_80 + combo_selection * 4
```

Its consumed fields are:

| Move offset | Width | Gameplay use |
|---:|---:|---|
| `+0x00` | 1 | Animation selector; zero marks the move unavailable |
| `+0x01` | 1 | Nonzero adds `0x40` forward movement each action update |
| `+0x02` | 1 | Enables the move's attachment effect path |

An available combo starts animation:

```text
animation_id = move.byte_0 + 0x12
```

An unavailable descriptor sets cooldown `+0x914` to `15` and clears the command
ring. The move pointer is stored at runtime `+0x84`.

Attack strength at runtime `+0x644` uses the common fighter record:

```text
base_power = signed16(common_fighter_record + 0x00)
percentage = unsigned8(common_fighter_record + 0x09 + combo_selection)

attack_strength = base_power * percentage / 100
```

R1/Ether uses common fighter-record byte `+0x18` as its projectile damage rather
than this combo percentage.

## 4. Animation Progress And Event Ranges

`BattlingRecordReplayFrameAndAdvanceAnimation` at `0x80074BA4` stores animation
time in signed halfword `+0x99E` with four fractional bits:

```text
old_subframe = animation_subframe
animation_subframe += base_step_4F + movement_step_52

crossed_frames = (animation_subframe >> 4) - (old_subframe >> 4)
```

The crossed count is stored at `+0x99A`. Every crossed integer frame is passed
to `BattlingProcessCrossedAnimationFrameEvents` at `0x80074678`.

An animation selects its event list through a signed 16-bit offset table:

```text
event_list_offset = signed16(event_offset_table_900
                           + animation_id * 2)
event_list = fighter_resource_8FC + event_list_offset
```

Each range is four bytes:

```c
struct BattlingAnimationEventRange {
    uint8_t start_frame;
    uint8_t end_frame;
    int16_t event_offset;
};
```

`start_frame == 0xFF` terminates the list. Both endpoints are inclusive. The
event record is:

```text
event = fighter_resource + event_offset
```

Event type is `event[0]`:

| Type | Gameplay operation |
|---:|---|
| `0` | Emit an attack volume, projectile, or Ether projectile |
| `1` | Start the two event sound/effect identifiers once for the crossed range |
| `2` | Dispatch an attachment-based point, trail, or segment effect |
| `3` | Snap the root to the current body anchor and reset action state |
| `4` | Enable the selected model part |
| `5` | Disable the selected model part |

For type 0, reaching `start_frame` sets attack-emission flag `0x04000000`.
Reaching `end_frame` clears it after processing that frame. The event dispatcher
receives the flag's current value, so geometry is emitted only during the
active range.

## 5. Type-0 Attack Events

The common type-0 prefix is:

| Event offset | Width | Gameplay use |
|---:|---:|---|
| `+0x00` | 1 | Event type, zero |
| `+0x01` | 1 | Attack behavior selector |
| `+0x02` | 1 | First model attachment |
| `+0x03` | 1 | Second model attachment |
| `+0x04` | 2 | Signed point index for the first attachment |
| `+0x06` | 2 | Signed point index for the second attachment |

`BattlingResolveModelAttachmentPosition` at `0x80073B7C` transforms each
attachment point into arena coordinates. Equal attachment/index pairs create a
single-point attack. Distinct pairs create a directed segment.

`BattlingProcessAnimationAttackEvent` at `0x800740E4` interprets behavior byte
`+0x01`:

| Behavior | Result |
|---:|---|
| `0x20` | Charge standard Ether Heat and spawn projectile type `0` |
| `0x04` | Spawn projectile type `1` |
| `0x21..0x26` | Spawn projectile type `1..6` |
| Other values | Emit or update a swept attack volume; the byte becomes its hit type |

The Ether path clears the fighter's command ring after either spawning or
rejecting the projectile. A successful spawn records the attack label as
`ETHER`, uses common fighter-record byte `+0x18` as damage, and marks the current
replay sample. A rejected Heat charge starts the failed-action response and
does not create the projectile.

## 6. Swept Attack Volumes

Each fighter owns sixteen records beginning at runtime `+0x104`. Record size is
`0x54`:

| Record offset | Width | Gameplay use |
|---:|---:|---|
| `+0x00` | `0x10` | Current endpoint A |
| `+0x10` | `0x10` | Previous endpoint A |
| `+0x20` | `0x10` | Current endpoint B |
| `+0x30` | `0x10` | Previous endpoint B |
| `+0x43` | 1 | First attachment identifier |
| `+0x44` | 1 | Collision and continuation flags |
| `+0x45` | 1 | Lifecycle state |
| `+0x46` | 1 | Hit type |
| `+0x47` | 1 | Damage |
| `+0x48` | 1 | Emission generation |
| `+0x4C` | 4 | Attachment/trail key |
| `+0x50` | 4 | Move descriptor pointer |

`BattlingInitializeSweptAttackVolume` at `0x80073CEC` copies each current
endpoint to its previous slot, installs the two new endpoints, copies the low
byte of current attack strength to damage `+0x47`, and sets lifecycle state `2`.
An existing compatible volume is updated in place; otherwise the first inactive
slot is claimed. Emission is discarded when all sixteen slots are occupied.

`BattlingCommitCurrentFrameAttackVolumes` at `0x80073CA4` applies:

```text
state 2 -> state 1
state 0 -> state 0
state 1 -> state 0
```

Thus a newly emitted volume becomes active after event processing, remains
available for the next collision pass, and is then retired unless the event
updates it into a new generation.

### 6.1 Horizontal collision

Attack collision uses the target's body circle centered on the interpolated
line between anchors `+0x92C` and `+0x93C`. Radius is fighter-resource byte
`+0x13`.

`BattlingTestSegmentCircleIntersection2D` at `0x80075750` normalizes a segment
direction to 12-bit fixed point. For segment start `S`, end `E`, circle center
`C`, radius `r`, and segment length `L`, it derives longitudinal and
perpendicular components:

```text
direction = normalize_12(E - S)
along = dot(C - S, direction) >> 12
perpendicular = cross(C - S, direction) >> 12
```

The segment hits when:

```text
along >= 0
along <= L + r
perpendicular^2 < r^2
```

The radius comparison is strict. `BattlingFindSegmentCircleImpact2D` at
`0x80075888` performs the same test and stores the near-side impact point.
`BattlingTestQuadrilateralEdgesAgainstCircle` at `0x80075A4C` tests all four
edges of the quadrilateral formed by the current and previous endpoints.

### 6.2 Vertical collision

The target body center is interpolated at the attack's Y. A volume qualifies
only when its representative Y lies strictly between the two body-anchor Ys.
Ordinary swept volumes use the average Y of their four current/previous
endpoints. The point-segment flag uses current endpoint A's Y.

On a hit:

```text
damage = volume.damage

if target has effective guard:
    damage = damage / 2

hitstun_add = damage * 6 / 5
```

Damage is staged at target `+0x970`; it is not subtracted from HP inside the
collision loop. The strongest nonzero hit supplies the pending hit type and
impact origin. A successful swept hit retires the attacker's active volumes.

## 7. Projectile Pool

Each fighter owns eight `0x44`-byte projectile records beginning at runtime
`+0x64C`:

| Record offset | Width | Gameplay use |
|---:|---:|---|
| `+0x00` | `0x10` | Current position |
| `+0x10` | `0x10` | Previous position |
| `+0x20` | 6 | Scaled movement XYZ, three signed halfwords |
| `+0x26` | 2 | Alignment and working storage |
| `+0x28` | 6 | Normalized steering XYZ, three signed halfwords |
| `+0x2E` | 2 | Alignment and working storage |
| `+0x30` | 1 | Active flag |
| `+0x31` | 1 | Speed scale |
| `+0x32` | 1 | Movement behavior |
| `+0x33` | 1 | Lifetime-dependent behavior |
| `+0x34` | 4 | Current distance to target |
| `+0x38` | 4 | Attachment/trail key |
| `+0x3C` | 2 | Damage supplied by the active attack |
| `+0x3E` | 2 | Lifetime and steering interpolation value |
| `+0x40` | 2 | Type-table parameter |

`BattlingSpawnAttackProjectile` at `0x80073424` claims the first inactive slot.
If no slot among the eight is free, the spawn is discarded. Direction is either
the explicit difference between the event's two endpoints or the vector from
the event origin to the opponent with target Y offset by `-0x90`.

The seven eight-byte type rows at `0x800910F4` are:

| Type | Initial `+0x3E` | Movement `+0x32` | Parameter `+0x40` | Speed `+0x31` | Behavior `+0x33` | Sound |
|---:|---:|---:|---:|---:|---:|---:|
| `0` | `0x0600` | `0` | `0x2D` | `0x40` | `1` | `0x0B` |
| `1` | `0x0600` | `2` | `0x19` | `0xC0` | `3` | `0` |
| `2` | `0x0300` | `2` | `0x19` | `0xC0` | `0` | `0` |
| `3` | `0x0000` | `3` | `0x20` | `0x80` | `2` | `0` |
| `4` | `0x0000` | `2` | `0x20` | `0x80` | `2` | `0` |
| `5` | `0x0000` | `4` | `0x20` | `0x80` | `2` | `0` |
| `6` | `0x0000` | `5` | `0x20` | `0x80` | `2` | `0` |

Damage is not stored in this table. Type 0 receives Ether damage from common
fighter-record byte `+0x18`; other spawned attacks pass the current attack
strength.

`BattlingUpdateAttackProjectiles` at `0x80073644` performs this sequence for
each active slot:

1. Decrement `+0x3E`; retire the slot when it becomes `-1`.
2. Sample terrain at the current projectile X/Z and retire on floor contact.
3. Preserve current position as previous position.
4. Measure and normalize the vector to the opponent's body center.
5. Retain this fighter's projectile nearest to the opponent at
   `+0x8F4/+0x8F8`.
6. Interpolate the steering vector toward the opponent using `+0x3E` and
   `0x1000 - +0x3E`.
7. Scale it by speed byte `+0x31` and apply movement behavior `+0x32`.
8. Apply lifetime-dependent behavior `+0x33`.

### 7.1 Projectile collision

`BattlingProcessIncomingAttackCollisions` at `0x80075B50` tests the opponent's
eight projectiles before its swept volumes. Projectile Y must cross the target's
vertical body span, or its distance path must select the nearest body endpoint.
Horizontal collision uses the segment from previous to current projectile
position against the same resource-defined body radius at `+0x13`.

Rear impact is determined by:

```text
relative = (projectile_heading - target_facing) & 0x0FFF
rear_hit = 0x0601 <= relative <= 0x09FF
```

Damage processing is ordered:

```text
damage = signed16(projectile.damage)

if rear_hit:
    damage *= 2
    pending_hit_type = 3
else if target has effective guard:
    damage /= 2
```

Rear-hit doubling therefore takes precedence over guard reduction. Projectile
reaction accumulation is:

```text
hitstun_add = damage * 2 / 3
feedback_timer = damage / 3 + 12
```

The projectile is retired after impact. The collision loop stops after the first
projectile hit in that pool, then continues to the swept-volume phase.

## 8. Boost And Guard

### 8.1 Cross boost

Frame preparation clears boost-request bit `0x00008000`; held Cross restores it
during controller processing. A boost attempt requires:

```text
boost request is set
action phase C5 == 0
physical state C4 is 0 or 1
final_speed_40 > 0x30
secondary failure bit D4:0x40 is clear
```

The attempt calls `BattlingAccumulateHeatAndQueueHealthDamage` with addition
`0x20` and classification `2`. On success:

```text
final_speed_40 *= 2
movement_animation_step_52 *= 2
```

On failure, the update clears the boost request and sets secondary bit `0x40`.
That bit prevents another attempt until frame preparation clears the transient
state. State `0`, a failed charge, or an ineligible action phase also clears the
request before the next movement update.

### 8.2 Guard startup

L1 defense uses a request bit, a three-bit startup phase, and a separate
effective-guard bit. Frame preparation at `0x800764CC` clears request bit
`0x00000002`; held L1 restores it during controller processing. Released L1
clears phase bits `0x38`.

At the beginning of action-state processing, effective guard bit `0x4` is
cleared. For physical state `0`, action phase `0`, and held L1, animation
`0x11` is selected and the phase advances:

| Consecutive eligible L1 update | Phase bits `0x38` on entry | Result |
|---:|---:|---|
| 1 | `0x00` | Advance to phase 1 (`0x08`) |
| 2 | `0x08` | Advance to phase 2 (`0x10`) |
| 3 | `0x10` | Set effective guard bit `0x04` |
| 4 and later | `0x10` | Set effective guard bit `0x04` again |

Guard therefore becomes effective on the third consecutive eligible update.
Releasing L1 resets the startup phase. Attacking, reacting, airborne state, and
other nonzero action phases prevent the state-0 guard branch from restoring the
effective bit.

Effective guard applies these gameplay changes:

| Incoming source | Guard result |
|---|---|
| Swept volume | Damage divided by two |
| Front/side projectile | Damage divided by two |
| Rear projectile | Rear-hit doubling; guard branch is bypassed |
| Pending reaction | Guard-specific feedback and reduced rumble path |

## 9. Heat

Runtime halfword `+0xB6` is the Heat gauge. Its limit is:

```text
HEAT_LIMIT = 0x1000
```

`BattlingInitializeBattlerRuntime` at `0x80078F00` initializes:

| Offset | Value | Use |
|---:|---:|---|
| `+0xB6` | `0` | Current Heat |
| `+0xBA` | `0` | Queued Heat-overflow HP damage |
| `+0xBE` | `0x480` | Standard Ether Heat cost |

Cross boost attempts to add `0x20` on every eligible boosted update. Ether
attempts to add the fighter's `+0xBE` value.

### 9.1 Overflow preflight

`BattlingCanAccumulateHeatWithoutFatalDamage` at `0x80073DE4` computes:

```text
prospective = signed16(current_heat) + addition

if prospective <= 0x1000:
    return true

required_hp = (prospective - 0x0FF1) / 0x14
return required_hp < current_hp
```

For values above the limit, let `overflow = prospective - 0x1000`. Subtracting
`0x0FF1` implements:

```text
required_hp = floor((overflow + 15) / 20)
```

This equals actual floor-divided overflow damage or exceeds it by one when the
remainder is at least five. The strict HP comparison rejects an action when the
preflight requirement reaches current HP.

### 9.2 Accumulation and queued damage

`BattlingAccumulateHeatAndQueueHealthDamage` at `0x80073E2C` applies:

```text
prospective = unsigned16(current_heat) + addition

if prospective <= 0x1000:
    current_heat = prospective
    return true

overflow_damage = (prospective - 0x1000) / 0x14

if current_hp <= overflow_damage:
    restore previous Heat
    queued_overflow_damage = 0
    return false

current_heat = 0x1000
queued_overflow_damage = overflow_damage
return true
```

Actual queued overflow uses unadjusted floor division. An overflow smaller than
`20` therefore caps Heat at `0x1000` while queuing zero HP damage.

Classification argument `1` adds accepted Ether overflow damage to runtime
counter `+0x1654`. Classification `2` adds accepted boost overflow damage to
`+0x1658`.

### 9.3 Activity exceptions

Heat accumulation has two complete activity bypasses:

| Battling activity | Behavior |
|---:|---|
| `4` | Return success without changing Heat or queuing overflow damage |
| `6` | Return success without changing Heat or queuing overflow damage |

Consequently Practice boost retains its speed effect without increasing Heat,
and Ether events do not pay the ordinary `+0xBE` cost in these activities.

### 9.4 Heat decay

`BattlingUpdateHealthHitstunAndKnockout` at `0x80077038` drains nonzero Heat:

| Physical/terrain condition | Decay per update |
|---|---:|
| Even physical state (`0`, `2`, or `4`) | `8` |
| Odd state on terrain class `0` or `2` | `4` |
| Odd state on terrain class `1` | `6` |
| Odd state on terrain class `3`, fighter behavior bit `+0x90A:0x04` clear | `2` |
| Odd state on terrain class `3`, fighter behavior bit `+0x90A:0x04` set | `8` |

Decay clamps the gauge to zero.

## 10. Health And Damage Drainage

The health fields are:

| Offset | Width | Gameplay use | Initial value |
|---:|---:|---|---:|
| `+0xB4` | 2 | Current HP | `300` |
| `+0xB6` | 2 | Current Heat | `0` |
| `+0xB8` | 2 | HP value from the preceding health change | `300` |
| `+0xBA` | 2 | Queued Heat-overflow damage | `0` |
| `+0xBC` | 2 | Maximum HP | `300` |
| `+0xC0` | 1 | Current normalized health amount | `0x7F` |
| `+0xC1` | 1 | Previous normalized health amount | `0x7F` |
| `+0xC2` | 1 | Health-change transition timer | `0xF0` |
| `+0x970` | 4 | Pending direct-hit damage | `0` |

One health update subtracts both pending channels in order:

```text
current_hp -= signed16(queued_overflow_damage)
current_hp -= signed16(low16(pending_direct_hit_damage))
current_hp = max(current_hp, 0)

queued_overflow_damage = 0
pending_direct_hit_damage = 0
```

When HP changes, normalized health is rebuilt with ceiling division:

```text
normalized = (current_hp * 0x80 + maximum_hp - 1) / maximum_hp

if normalized == 1:
    normalized = 2
```

The prior normalized value moves to `+0xC1`, the new value moves to `+0xC0`,
and timer `+0xC2` resets to `0xF0`. The timer decreases by `0x10` each update
until zero.

The scripted-scenario health path tests side 1 at or below
`maximum_hp * 80 / 255`. It sets:

```text
current_hp = maximum_hp * 0x50 / 0x100
```

before the knockout test. With maximum HP 300, integer rounding can change 94 HP
to 93 at this boundary.

## 11. Pending Hit Transaction

Collision stages the strongest hit in these fields:

| Offset | Width | Use |
|---:|---:|---|
| `+0x0E8` | 4 | Action interruption/hitstun timer |
| `+0x0F0` | 2 | Repeated-reaction phase count |
| `+0x100` | 4 | Pending hit type plus one; zero means none |
| `+0x916` | 2 | Escalating hitstun total |
| `+0x918` | 1 | Hit/guard response intensity |
| `+0x91C` | `0x10` | Impact position |
| `+0x970` | 4 | Pending direct damage |
| `+0x974` | `0x10` | Strongest hit origin |
| `+0x1668` | 2 | Received damaging-hit counter |

`+0xE8` is capped to `20` by the health update, then decremented toward zero.
`+0x916` and unavailable-move cooldown `+0x914` each decrement by one while
nonzero. When `+0xC8` reaches zero, repeated-reaction count `+0xF0` is reset.

After collision has selected a strongest nonzero hit:

```text
pending_hit = strongest_hit_type + 1
pending_damage = strongest_damage
received_damaging_hit_count++
```

On the action update, a nonzero pending hit calls
`BattlingResolvePendingHitImpulse` at `0x80077770` and returns without consuming
a command.

## 12. Reactions And Knockback

`BattlingResolvePendingHitImpulse` increments the repeated-reaction phase and
selects a response from hit type, guard state, current knockback timer, impact
height, and accumulated hitstun.

Impact height is compared with the body span between anchors `+0x92C` and
`+0x93C`. The two-thirds point of that span separates upper and lower reaction
selection. Ordinary reactions use animations `5`, `6`, and `7`; a hit during
animation `9` with active knockback time selects animation `0x0C`.

Heavy knockback begins when either condition holds:

```text
accumulated_hitstun_916 > 0x25
repeated_reaction_phase_F0 > 3
```

Hit type `3` also enters a dedicated heavy launch with horizontal shift `11`
and vertical amount `0x80`. Escalation heavy launch uses shift `10` and vertical
amount `0xA0`.

`BattlingApplyDirectionalLaunchImpulse` at `0x80077584` applies 12-bit
trigonometric recoil:

```text
recoil_x += sin(angle) >> shift
recoil_z += cos(angle) >> shift

if velocity_y > -0x8C:
    velocity_y -= vertical_amount

if velocity_y < -0x82:
    velocity_y = -0x82
```

`BattlingBeginHeavyKnockback` at `0x8007762C` installs:

```text
animation_id       = 8
previous_animation = 0xFF
knockback_timer_CA = 0x3C
hitstun_916        = 0x28
interruption_E8    = 0x28
physical_state_C4  = 4
action_phase_C5    = 0
```

It also clears incompatible reaction/guard state and applies the launch impulse.
When physical state remains `4` and `+0xCA` reaches zero, the health update
returns the fighter to physical state `0`, action phase `0`.

## 13. Knockout

After both damage channels are drained and HP is clamped, ordinary activities
enter
knockout when all conditions hold:

```text
activity != 4
activity != 6
current_hp == 0
KO flag is clear
```

The knockout transition is:

```text
animation_id       = 0x0D
previous_animation = 0xFF
physical_state_C4  = 5
action_phase_C5    = 0
velocity_y        -= 0x50
KO flag           |= 0x00800000
pending_hit         = 0
rumble_duration     = 0x14
```

Horizontal recoil is added relative to the shared fighter-to-fighter heading.
Fighter side selects whether the sine and cosine contributions are added or
subtracted; both use a shift of `9`.

Physical state `5` causes subsequent health and action updates to return
immediately. Bout processing detects the two KO flags independently, allowing
side 0 victory, side 1 victory, or a draw when both flags are set.

## 14. Function Map

| Address | Function | Gameplay responsibility |
|---:|---|---|
| `0x80073424` | `BattlingSpawnAttackProjectile` | Claim and initialize one of eight projectile slots |
| `0x80073644` | `BattlingUpdateAttackProjectiles` | Advance lifetime, steering, movement, and terrain contact |
| `0x80073B7C` | `BattlingResolveModelAttachmentPosition` | Transform an event attachment into arena coordinates |
| `0x80073CA4` | `BattlingCommitCurrentFrameAttackVolumes` | Promote new swept volumes and retire old ones |
| `0x80073CEC` | `BattlingInitializeSweptAttackVolume` | Install current/previous attack endpoints and damage |
| `0x80073DE4` | `BattlingCanAccumulateHeatWithoutFatalDamage` | Apply the biased floor-divided overflow preflight |
| `0x80073E2C` | `BattlingAccumulateHeatAndQueueHealthDamage` | Charge Heat and convert overflow to queued HP damage |
| `0x80073F34` | `BattlingDispatchAnimationEffectEvent` | Dispatch attachment-based action effects |
| `0x800740E4` | `BattlingProcessAnimationAttackEvent` | Emit swept geometry, projectiles, or Ether |
| `0x80074678` | `BattlingProcessCrossedAnimationFrameEvents` | Dispatch every event range crossed this update |
| `0x80074BA4` | `BattlingRecordReplayFrameAndAdvanceAnimation` | Advance animation subframes and count crossed frames |
| `0x80075750` | `BattlingTestSegmentCircleIntersection2D` | Test a directed horizontal segment against a circle |
| `0x80075888` | `BattlingFindSegmentCircleImpact2D` | Test a segment and record its near-side impact point |
| `0x80075A4C` | `BattlingTestQuadrilateralEdgesAgainstCircle` | Test a swept attack quadrilateral against a body circle |
| `0x80075B50` | `BattlingProcessIncomingAttackCollisions` | Resolve projectile and swept-volume collisions |
| `0x800767C8` | `BattlingQueueCloseRangeReaction` | Queue automatic command `5` at close range |
| `0x80077038` | `BattlingUpdateHealthHitstunAndKnockout` | Drain timers, Heat, damage, HP, and enter knockout |
| `0x80077584` | `BattlingApplyDirectionalLaunchImpulse` | Add angle-based horizontal and clamped vertical recoil |
| `0x8007762C` | `BattlingBeginHeavyKnockback` | Install heavy-knockback animation, timers, and state |
| `0x80077770` | `BattlingResolvePendingHitImpulse` | Select reaction animation and launch response |
| `0x80077A38` | `BattlingAdvanceComboSelection` | Traverse the Triangle/Circle combo tree |
| `0x80077A9C` | `BattlingUpdateActionState` | Consume commands, start actions, guard, jump, and boost |
