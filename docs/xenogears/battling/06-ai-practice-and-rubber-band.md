# AI, Practice, And Rubber-Band Mode

## 1. Simulation Roles

Battling can assign either Gear to a controller or to COM control. Fighter
control bytes `0x80099D9D` and `0x80099D9E` are copied into fighter flag
`+0xD0:0x00000040` when a bout begins:

| Value | Controller |
|---:|---|
| `0` | Player input |
| `1` | COM input |

Normal COM control uses the four-state tactical machine beginning at
`0x80090CC0`. Practice mode instead dispatches the selected practice behavior
through `BattlingPracticeAiUpdate` at `0x8008F280`; three named battle
selections continue into the normal COM machine, which reloads the current
global `GAME LEVEL` difficulty.

Bout initialization and the replay/result phases are described in
[`07-bouts-replay-results-and-progression.md`](07-bouts-replay-results-and-progression.md).
Scripted tutorial control is described in
[`08-tutorial-and-attract-scripts.md`](08-tutorial-and-attract-scripts.md).

## 2. Fighter Fields Used By AI

The COM machine reads and writes these fighter fields:

| Offset | Size | Meaning |
|---:|---:|---|
| `+0x00/+0x04/+0x08` | 4 each | World X, Y, and Z position. |
| `+0x48` | 4 | Requested/normalized movement magnitude, `0..0x100`. |
| `+0x58` | 4 | Requested movement heading. |
| `+0xB4` | 2 | Current HP. |
| `+0xB6` | 2 | Current Heat. |
| `+0xBC` | 2 | Maximum HP. |
| `+0xBE` | 2 | Standard Heat increase. |
| `+0xC4` | 1 | Physical state. |
| `+0xC5` | 1 | Action phase. |
| `+0xCE` | 2 | Desired facing offset. |
| `+0xD0` | 4 | Main fighter flags. |
| `+0xD8` | 4 | Opponent fighter pointer. |
| `+0x8F4` | 4 | Distance from this fighter's nearest projectile to its opponent. |
| `+0x911` | 1 | Special action availability. |
| `+0x15FC` | 4 | Pointer to this fighter's COM state. |
| `+0x1600` | 4 | Pointer to the common fighter record containing COM tuning. |
| `+0x1668` | 2 | Received damaging-hit counter. |

Shared horizontal and three-dimensional distances are maintained at
`0x8009284C` and `0x80092850`. Shared heading from side 1 to side 0 is
stored at `0x80092934`.

### 2.1 Health fraction test

`BattlingHealthAboveFractionTest` at `0x8008F4F4` evaluates:

```text
current_hp > maximum_hp * fraction / 255
```

The COM machine uses fractions `0x20`, `0x50`, `0x80`, and `0xC0` for tactical
decisions. The result-code path additionally uses `0x10` and `0xE0`; see
[`Result-code selection`](07-bouts-replay-results-and-progression.md#8-result-code-selection).

### 2.2 Heat safety

The standard safe heat limit is calculated by `BattlingHeatSafeLimitCompute`
at `0x8008F570`:

```text
safe_heat_limit = 0x1000 - standard_heat_increase
```

`BattlingHeatIncreaseSafetyTest` at `0x8008F530` has two policies selected by
its second argument. Argument `0`, used by ordinary AI decisions, requires the
next increase to remain strictly below the limit:

```text
current_heat < 0x1000 - standard_heat_increase
```

A nonzero argument calls the overflow preflight at `0x80073DE4`, accepting an
increase that remains below the limit or produces survivable overflow damage.
The HUD warning path evaluates the strict policy first and then the survivable
policy to choose its warning color.

`BattlingHeatRiskClassify` at `0x8008F580` compares current heat with the
fighter-specific limit cached in COM state `+0x24`:

| Result | Condition |
|---:|---|
| `0` | `current_heat > cached_limit` |
| `1` | `cached_limit - 0x200 < current_heat <= cached_limit` |
| `2` | `current_heat <= cached_limit - 0x200` |

## 3. COM State Layout

`BattlingComAiInitialize` at `0x80090CC0` selects one of two side-specific
records, stores its pointer at fighter `+0x15FC`, loads fighter tuning, and
chooses an initial Idle, Attack, or Retreat state.

```c
struct BattlingComState {
    BattlingFighter *fighter;       /* +0x00 */
    int16_t timer;                  /* +0x04 */
    uint8_t initial_heading_span;   /* +0x06, initialized to 0x10 */
    uint8_t initial_speed;          /* +0x07, initialized to 10 */
    uint8_t state;                  /* +0x08 */
    uint8_t repetitions;            /* +0x09 */
    int16_t desired_heading;        /* +0x0A */
    int16_t desired_speed;          /* +0x0C */
    uint8_t boost_requested;        /* +0x0E */
    uint8_t difficulty;             /* +0x0F */
    uint32_t decision_weight_0;     /* +0x10 */
    uint32_t decision_weight_1;     /* +0x14 */
    uint32_t decision_weight_2;     /* +0x18 */
    uint32_t decision_weight_3;     /* +0x1C */
    uint32_t phase;                 /* +0x20 */
    int32_t safe_heat_limit;        /* +0x24 */
    int32_t target_distance;        /* +0x28 */
    uint16_t tactical_flags;        /* +0x2C */
    uint8_t close_action_count;     /* +0x2E */
    uint8_t padding_2F;             /* +0x2F */
    int16_t received_hit_snapshot;  /* +0x30 */
};
```

The four decision weights come from bytes `+0x06..+0x09` of the fighter's
record at `fighter.+0x1600`. `BattlingAiTacticalFlagsRandomize` at `0x8008F7B8`
compares fresh random bytes with those weights, sets tactical bits
`0x0100`, `0x0200`, `0x0400`, and `0x0800`, randomly selects orientation bit
`0x1000`, stores a random low byte in the packed halfword at `+0x2C`, and
snapshots the fighter's received damaging-hit counter `+0x1668` at `+0x30`.

## 4. Difficulty

Difficulty is stored at `0x80099D98` and copied to COM state `+0x0F` every
update:

| Value | Label | Idle delay | Attack repetitions | Pending-action interruption |
|---:|---|---:|---:|---|
| `0` | Easy | `150` | `1..4` | Once every 6 updates |
| `1` | Normal | `120` | `1..6` | On odd updates |
| `2` | Hard | `90` | `1..8` | Every update |

The Idle delay formula in `BattlingComAiIdleStateEnter` at `0x8008FC7C` is:

```text
idle_delay = (2 - difficulty) * 30 + 90
```

Attack cadence is also refreshed by `BattlingAiBasicBurstRefresh` at
`0x8008FF24`:

```text
burst_delay = (2 - difficulty) * 20 + random(0..19) + 1
```

## 5. State Machine

`BattlingComAiUpdate` at `0x80090E10` runs only for a fighter carrying COM flag
`+0xD0:0x40`. It dispatches the current state, maps `boost_requested` to fighter
flag `0x8000`, maps tactical flag `0x2000` to fighter action flag `0x0002`, and
then applies heading and speed through `BattlingAiSteeringApply` at
`0x8008EE1C`.

| State | ID | Enter routine | Update routine |
|---|---:|---:|---:|
| Idle | `0` | `0x8008FC7C` | `0x8008FCC8` |
| Attack | `1` | `0x80090174` | `0x8009031C` |
| Retreat | `2` | `0x80090894` | `0x80090990` |
| Approach | `3` | `0x80090504` | `0x80090580` |

### 5.1 Scripted-scenario boundary override

Idle, Attack, and Retreat call `BattlingScriptedScenarioBoundaryRecoveryUpdate` at
`0x8008FACC` before their ordinary behavior; Approach does not. The override is
active only while the scripted-scenario flag is set and the controlled fighter
has the side-1 marker. If that fighter is outside the arena, it receives an
inward heading and full movement. If only its opponent is outside, side 1
applies the recovery or waiting branch until both participants return to the
arena.

### 5.2 Idle state

Idle entry clears phase `+0x20`, selects the difficulty delay, and refreshes
tactical flags.

Idle update clears heading, speed, and boost before applying reactions:

1. At horizontal distance below `0x200`, orientation flag `0x1000` selects
   immediate Attack; otherwise Normal and Hard can enter Retreat.
2. If the opponent is executing action phase `+0xC5 == 2`, pending actions can be
   interrupted at the difficulty cadence.
3. If `signed(received_hits) + 2 < signed(received_hit_snapshot)`, the AI enters
   Attack.
4. If the opponent's nearest owned projectile is within `0x400` of the
   controlled fighter, the AI enters Retreat and requests boost.
5. After the idle timer expires, phase changes from `0` to `1`. The first phase
   may queue a tactical burst; the second waits for a safe heat increase, then
   enters Approach.

The received-hit comparison is the exact signed comparison executed by the
game. The counter normally increases from the value copied into the snapshot,
so this condition does not represent "three new hits"; absent other state
changes, it becomes true only across signed 16-bit wraparound.

### 5.3 Attack state

Attack entry queues an initial basic burst and chooses the difficulty-dependent
repetition count. It clears movement and boost and refreshes tactical flags.

`BattlingAiAttackSequenceChoose` at `0x80090258` selects between an ordinary
burst and an available special. Specials can be selected while fighter
`+0x911` is nonzero; at horizontal distance up to `0x1000`, a viable special is
queued through `BattlingAiAvailableSpecialQueue` at `0x8008FFEC`.

Attack update applies these transitions:

| Condition | Transition or action |
|---|---|
| Boundary recovery takes control | Remain under boundary override. |
| Opponent enters action phase `2` | Interrupt pending actions at the difficulty cadence. |
| Incoming projectile distance is `0x201..0x5FF` and difficulty is Normal or Hard | Interrupt pending actions. |
| Horizontal distance is at most `0x300` | Continue the current attack sequence. |
| Random branch 0 passes weight `+0x14` | Enter Retreat. |
| Random branch 1 | Enter Idle. |
| Random branch 2 | Queue the special when available; otherwise enter Idle. |
| Random branches 3 through 5 | Queue a burst or special when approved; otherwise enter Idle. |
| Random branches 6 through 9 | Enter Approach. |
| Repetition counter reaches zero | Enter a randomized Retreat variant. |

### 5.4 Approach state

Approach entry chooses repetitions `1..4`, clears its timer, refreshes tactical
flags, and decides whether boost is desirable.

Approach update closes distance with a randomized heading around `-0x300` and
full speed `0xFF`. Timer spans are `10..109` updates, or `10..129` when tactical
flag `0x0400` is set. It also:

- enters Attack below horizontal distance `0x180`;
- on Normal or Hard, evades when the nearest incoming projectile has lifetime
  behavior `+0x33 == 1` and is within `0x500`, choosing heading
  `-0x400` or `+0x400` from orientation bit `0x1000`;
- permits extra attack bursts on Hard when `random_byte < decision_weight_1`;
- clears boost when heat no longer permits it;
- chooses a fresh attack sequence when an approach repetition expires;
- returns to Idle when the selected attack sequence declines to continue.

### 5.5 Retreat state

Retreat entry chooses:

```text
target_distance = random(0..0x5FF) + 0x100
repetitions     = random(0..4) + 3
```

It refreshes tactical flags, may queue an immediate evasive action, chooses
boost from heat and engagement state, and resets the close-action counter.

Retreat update uses a heading in `0x500..0xAFF`, maintaining speed `0xFF` while
the current horizontal distance remains below `target_distance`. Timer spans
are `10..69` updates, or `10..49` with tactical flag `0x0400`. The state returns
to Idle after passing its target distance, or immediately when three-dimensional
fighter separation exceeds `0x4B0` while rubber-band mode is active. At close range it
can face `-0x400` or `+0x400`, request boost, and queue evasive bursts.

## 6. Attack Approval Probabilities

`BattlingAiAttackDecisionEvaluate` at `0x8008F5B4` combines the tuning weights,
heat state, and both fighters' HP. Random comparisons use the low byte of the
random value.

The principal weighted thresholds are:

```text
threshold_0_strong = decision_weight_0 * 0x140 >> 4
threshold_3_strong = decision_weight_3 * 0x140 >> 4
threshold_3_weak   = decision_weight_3 * 0x0C0 >> 4
```

The decision favors attacks when the next heat increase is safe, the controlled
fighter remains above `0xC0/0xFF` HP, or the opponent is sufficiently weak.
When the controlled fighter falls below `0x80/0xFF`, its HP is also compared
directly with the opponent's current HP. Weight `+0x18` gates a branch that
rejects an opponent in physical state `4` unconditionally. For other physical
states, an opponent above `0x20/0xFF` HP passes that branch immediately; a
weaker opponent proceeds to the relative-HP comparison.

`BattlingAiEngagementDecisionEvaluate` at `0x8008F720` uses distance to gate its
first random test. Horizontal distance beyond `0x5FF` bypasses that initial
`decision_weight_0` rejection, but high Heat risk still requires a random value
below `decision_weight_3`; lower Heat-risk classes permit engagement. At shorter
distance, both gates can reject the decision.

## 7. Range Constants

| Value | Use |
|---:|---|
| `0x100` | Minimum randomized retreat target; tutorial close-range threshold begins near this scale. |
| `0x180` | Approach-to-Attack threshold. |
| `0x200` | Idle close-range transition threshold. |
| `0x300` | Attack-state close-range continuation threshold. |
| `0x400` | Incoming-projectile distance that provokes Retreat. |
| `0x4B0` | Three-dimensional rubber-band fighter-separation threshold that cancels Retreat. |
| `0x500` | Normal/Hard reaction range for an incoming behavior-1 projectile. |
| `0x600` | Maximum ordinary engagement threshold and retreat random span. |
| `0x800` | Boundary-recovery and projectile-threat policy threshold. |
| `0x1000` | Special-selection, projectile-threat, and boundary policy threshold. |

## 8. Practice Behaviors

Practice activity sets `DAT_800928C8 = 4` and runs side 1 through
`BattlingPracticeAiUpdate`. Practice selection is `0x80099DA2`, range `0..13`.
Changing the selection clears movement, pending action flags, tactical flag
`0x2000`, the behavior timer, and the current speed.

| Index | Label | Behavior |
|---:|---|---|
| `0` | `BYSTANDER` | Stands without scheduling an action; polls once per update. |
| `1` | `ON GUARD` | Toggles automated movement and chooses the corresponding delay. |
| `2` | `CONTROLLER2` | Clears side 1 COM control and accepts controller 2 input. |
| `3` | `UP AND AT'EM` | Queues one to three randomized basic actions with an aggressive orientation. |
| `4` | `SLOWPOKE` | Queues the same randomized basic action family with the alternate orientation. |
| `5` | `MAGIC FIRER` | Queues action `3` and waits 60 updates. |
| `6` | `MAGIC JUMPER` | Queues actions `4` and `3`, then waits 60 updates. |
| `7` | `KANGAROO` | Queues action `4` and waits 60 updates. |
| `8` | `GIVE CHASE` | Runs far-range pursuit, periodically choosing full-speed headings. |
| `9` | `RUN AWAY` | Runs near-range evasion and stops after opening sufficient distance. |
| `10` | `BACK DASH` | Queues a facing-dependent action and waits 60 updates. |
| `11` | `EASY BATTLE` | Continues through the normal COM machine using the current `GAME LEVEL`. |
| `12` | `NORMAL BATTLE` | Continues through the normal COM machine using the current `GAME LEVEL`. |
| `13` | `HARD BATTLE` | Continues through the normal COM machine using the current `GAME LEVEL`. |

The dispatcher briefly writes `0`, `1`, or `2` for these three labels, but
`BattlingComAiUpdate` immediately overwrites COM state `+0x0F` from global byte
`0x80099D98`. The labels therefore do not override the configured difficulty.

The movement and queued-action behaviors call `BattlingAiSteeringApply` after
updating desired heading and speed, so they use the same acceleration and
turning rules as normal COM control.

## 9. Rubber-Band Mechanics

Rubber-band mode is selected by byte `0x80092884`. While enabled,
`BattlingAccumulateVelocityAndFacing` at `0x80078194` applies an attraction term
whenever the fighters' three-dimensional distance at `0x80092850` exceeds
`0x280`:

```text
velocity.x += (opponent.x - fighter.x) / 0x30
velocity.y += (opponent.y - fighter.y) / 0x30
velocity.z += (opponent.z - fighter.z) / 0x30
```

The term is applied independently to both fighters each simulation update. It
therefore behaves as a symmetric long-range spring rather than teleporting or
clamping either Gear. The horizontal AI machine also reacts to rubber-band
distance: Retreat returns to Idle when three-dimensional distance exceeds
`0x4B0`.

During the bout and replay, rubber-band mode emits line-burst pattern `0x13`
between paired fighter attachment points. `BattlingLineBurstOpcodeDispatch` at
`0x8007D65C` maps this to pattern 2, which builds three seven-segment paths with
alternating red and white segment colors.

## 10. Scripted-Scenario Climax

Launch selector `2` enables scripted-scenario flag `0x800928C4` and clears the
ordinary rubber-band option at `0x80092884` during startup.
`BattlingStartScriptedScenarioClimaxIfInactive` at `0x800720D4` starts the one-shot
climax when all of these conditions hold:

```text
scripted_scenario_enabled
and (side0.hp <= side0.max_hp * 0x60 / 0xFF
     or side1.hp <= side1.max_hp * 0x60 / 0xFF)
and side0 is inside the arena
and side1 is inside the arena
and climax_latch == 0
```

The start routine selects script `0x800910C4`, sets latch `0x8009293C`, pauses
ordinary bout control through `0x800928D4 = 0`, clears both COM-control bytes,
resets camera and dialog state, and initializes three effect emitters.

`BattlingUpdateScriptedScenarioClimax` at `0x80072170` then runs the script described
in [`Tutorial And Attract Scripts`](08-tutorial-and-attract-scripts.md#11-scripted-scenario-climax).
Its local presentation substates are:

| State | Behavior |
|---:|---|
| `0` | Script-controlled setup. |
| `0x0B` | Plays sound effects `0x2A`, `0x2B`, and `0x2C`, then enters state `1`. |
| `1` | Updates three randomized attachment-point emitters. Each emitter rearms after `8..23` updates and emits effect `9` on odd timer values. Random passes additionally emit effects `0x0B` and `8`. |
| `2` | Sets side 0 current HP to `1`, initializes fade value `0xFF`, and falls through to state `3`. |
| `3` | Draws a closing fade and subtracts `8` from the fade value each update, clamped at zero. |
| `4` | Draws an opening fade and adds `3` each update. At completion it exports result code `0x7F` and returns to Field. |

`BattlingResetScriptedScenarioClimax` at `0x800720C4` clears the one-shot latch when a
new bout runtime is initialized.
