# Actions, Damage, And Status

## 1. Overview

Battle actions proceed from calculation through resource
application, status changes, reactions, terminal-state detection, and
post-battle persistence. Combatant slots, readiness, AP, and target construction
are documented in
[`03-combatants-turns-and-targeting.md`](03-combatants-turns-and-targeting.md).

The formulas below use Battle's integer operations. All division truncates
toward zero.

## 2. Action Transaction

Battle separates selecting an action from applying its consequences:

1. Build a `u16` target mask and choose an action descriptor.
2. Initialize up to 32 action/result records with an actor, target mask, result
   code, and per-target values.
3. For each selected target, establish attacker, target, character/Gear mode,
   and action descriptor pointers.
4. Compute hit result, damage or recovery amount, optional status operation,
   and reaction records.
5. Postprocess reflection, protection, counters, and enemy-script reactions.
6. Apply HP, MP, Gear HP, or fuel changes.
7. Queue the corresponding result, status, and defeat animations.

The 32 records are initialized by `BattleInitializeAttackAnimationRecords` at
`0x800879A8`. Per-turn damage and status accumulators are cleared by
`BattleResetTurnDamageAccumulators` at `0x80085350`. Basic attacks enter the
calculator through `BattleComputeAttackAgainstTargets` at `0x80085CCC`; items
use `BattleComputeItemEffectsForTargets` at `0x80085C48`.

Animation records carry the completed transaction into presentation. Computed
results are present when `BattleApplyDamageAndResourceChanges` at `0x80085618`
commits them.

## 3. Action Families And Costs

| Family | Entry point | Cost and result data |
|---|---:|---|
| Basic attack/deathblow | `BattlePerformBasicAttack`, `0x80087AF0` | One, two, or three AP per input; action descriptor selected by the input sequence |
| Character ability | `BattleExecuteAbility`, `0x8008ADD0` | Checks and deducts MP, then uses character ability data |
| Character/Gear item | `BattleExecuteItemUse`, `0x8008B908` | Consumes a compacted inventory quantity after confirmation |
| Defend | `BattleApplyDefendStance`, `0x8009AA44` | Sets runtime flag `+0x15A:0x01` as a stance action |
| Gear attack | `BattleHandleGearAttackSelection`, `0x80083948` | Checks move availability and deducts the selected fuel cost |
| Gear special | `BattleTryUseGearSpecial`, `0x8008CDE4` | Checks restrictions and deducts fuel after target confirmation |
| Scripted enemy action | enemy script and attack dispatcher | Descriptor, targets, and resource mode come from encounter bytecode/data |

`BattleRewriteAbilityTargetModes` at `0x8009AB38` does not alter MP or fuel
costs. While character status `+0x88:0x0400` or Gear status
`+0x124:0x2000` is active, it rewrites descriptor targeting-mode field `+0x00`
to `1` for ten eligible character techniques or seven eligible Gear techniques.
The action therefore uses the rewritten targeting mode while retaining its
original resource cost. The companion routine at `0x8009AC48` restores the
affected descriptor fields to `0x2000` and clears the corresponding character
or Gear status bit when its second argument is zero or descriptor field
`+0x0A` contains `0x0100`. It returns without changing either field when both
conditions are false.

## 4. Damage Pipelines

Character-scale attackers use `BattleComputeStandardDamage` at `0x80094EE4`.
Gear attackers use the parallel Gear transaction at `0x8009CBC4`. The two paths
share elemental processing and the physical/ether core subtraction, but use
different hit resolvers, power and defence wrappers, variance, and guarded-hit
scaling.

The character-scale path calls:

| Address | Function | Output |
|---:|---|---|
| `0x80096AB8` | `BattleResolveAttackHitType` | Hit/result category |
| `0x80096FBC` | `BattleComputeAttackPower` | Effective power `P` |
| `0x80097610` | `BattleComputeDefenceValue` | Effective defence `D` |
| `0x80096494` | `BattleApplyElementalDamageModifiers` | Elemental scaling and possible absorb/result conversion |

The Gear path calls:

| Address | Function | Output |
|---:|---|---|
| `0x8009D3A0` | Gear hit resolver | Gear hit/result category |
| `0x8009D948` | `BattleComputeScaledAttackerPower` | Gear effective power `P` |
| `0x8009DA04` | Gear defence wrapper | Gear effective defence `D` |
| `0x80096494` | `BattleApplyElementalDamageModifiers` | Elemental scaling and possible absorb/result conversion |

### Power and defence inputs

Character-scale physical power starts from Attack at `+0x58`; ether power
starts from Ether at `+0x5B`. Character attack descriptors can add selected
weapon/input components before action-rate scaling. Physical defence begins
with Defence at `+0x59` and the equipment-derived component at `+0x2D`; ether
defence begins with Ether Defence at `+0x5C`.

Gear physical power starts from the Gear output and attack-multiplier channels.
Gear ether power wraps the character Ether calculation with Gear EtherAmp
scaling. Against Gear-scale defence, the Gear wrapper incorporates the derived
physical and Ether Defence fields at Gear `+0x70/+0x72`, the Gear
Defence-reduction percentage at Gear `+0x99`, and applicable Gear status flags.

The character-scale transaction applies these opposing Attack/Defence tiers:

| Owner and displayed status | Field and mask | Arithmetic |
|---|---:|---:|
| Attacker, Atk UP-Def DOWN strong tier | `(+0x88 \| +0x8A) & 0x0008` | `P += P / 5` |
| Attacker, Atk UP-Def DOWN weak tier | `(+0x88 \| +0x8A) & 0x0002` | `P += P / 10` |
| Attacker, Def UP-Atk DOWN strong tier | `(+0x88 \| +0x8A) & 0x0004` | `P -= P / 5` |
| Attacker, Def UP-Atk DOWN weak tier | `(+0x88 \| +0x8A) & 0x0001` | `P -= P / 10` |
| Target, Def UP-Atk DOWN strong tier | `(+0x88 \| +0x8A) & 0x0004` | `D += D / 5` |
| Target, Def UP-Atk DOWN weak tier | `(+0x88 \| +0x8A) & 0x0001` | `D += D / 10` |
| Target, Atk UP-Def DOWN strong tier | `(+0x88 \| +0x8A) & 0x0008` | `D -= D / 5` |
| Target, Atk UP-Def DOWN weak tier | `(+0x88 \| +0x8A) & 0x0002` | `D -= D / 10` |

The Gear transaction performs the same ordered arithmetic using Gear status
pair `(+0x124 | +0x126)` for both the attacker and the target. Because each row
uses the value produced by preceding rows, integer truncation occurs at every
individual addition or subtraction.

The character-scale power and defence calculators also apply:

| State | Field and mask | Arithmetic |
|---|---:|---:|
| Atk UP | `(+0x84 \| +0x86) & 0x2000` | Add one to the physical quarter-scale factor |
| Atk DOWN | `+0x7C & 0x0200` | Subtract one from the physical quarter-scale factor |
| Eth effect UP | `(+0x88 \| +0x8A) & 0x8000` | Add one to the ether quarter-scale factor |
| Eth effect DOWN | `+0x80 & 0x0400` | Subtract one from the ether quarter-scale factor |
| Ether power doubling | `(+0x88 \| +0x8A) & 0x2000` | Ether `P = P * 2` |
| Physical defence boost | `(+0x84 \| +0x86) & 0x0100` | Physical `D = D * 3 / 2` |
| Eth Def UP | `(+0x88 \| +0x8A) & 0x4000` | Ether `D = D * 3 / 2` |
| One-use attack reduction | Attacker `+0x80 & 0x0080` | `P = P * 3 / 4`, then clear the bit |
| One-use defence reduction | Target `+0x80 & 0x0040` | `D = D * 3 / 4`, then clear the bit |

The opposing UP and DOWN effects are combined before multiplication:

```text
physical_factor = 4 + has_Atk_UP - has_Atk_DOWN
physical_P = physical_P * physical_factor / 4

ether_factor = 4 + has_Eth_effect_UP - has_Eth_effect_DOWN
ether_P = ether_P * ether_factor / 4
```

Consequently simultaneous UP and DOWN cancel to factor `4/4`; they are not
sequential `5/4` and `3/4` multiplications. Ether power doubling is applied
separately after the quarter-scale factor.

### Elemental scaling

The action descriptor supplies up to six elemental bits. An attacker-derived
element supplies the element when the descriptor field is zero. Target affinity
and protection fields adjust a base multiplier of ten:

```text
P = P * elemental_multiplier / 10
```

A matching weakness raises the multiplier from 10 to at least 15, or 18 when a
stronger affinity flag is also active. Matching resistance reduces it in steps
of three or six. Elemental absorption changes the result to HP recovery. The
multiplier is clamped to at least one before application.

`BattleApplyAttackMaskResistance` at `0x8009DB54` is a separate Gear/enemy
resistance path. It selects the highest set bit in a 16-bit attack mask and
scales damage as:

```text
damage = damage * (20 - resistance) / 20   when resistance < 20
damage = 0                                 when resistance >= 20
```

### Core subtraction and action rate

After modifiers, standard damage uses:

```text
physical = 4 * P - 3 * D
ether    = 5 * P - 4 * D
```

When `D` is zero, the physical path is `4 * P` and the ether path is `5 * P`.
Descriptor modes can then scale the result by an action rate over 20; a
descriptor flag forces that rate to 20. Both paths convert nonpositive results
to zero before variance.

### Character-scale variance and results

The character-scale transaction adds positive variance as:

```text
if damage < 10:
    damage += random_mod_2
else:
    damage += random_mod_(damage / 10 + 2)
```

The character hit resolver compares attacker hit percentage at `+0x5E`, the
signed action accuracy modifier, and target evade percentage at `+0x5F`.
Status and runtime flags can add or subtract 30 or 50 points, force a category,
or replace the ordinary roll. The random tests use values modulo 100.

`BattleResolveAttackHitType` returns these operational categories:

| Resolver result | Category | Calculation and result-record effect |
|---:|---|---|
| `1` | Full hit | Normal amount with a minimum of one; result type `0` |
| `2` | Guarded hit | Half amount; result type `5` |
| `3` | Miss or attack negation | Zero amount; result type `4` |
| `4` | Elemental absorption | Computed amount becomes HP recovery; result type `2` |
| `5` | Special damage hit | Computed amount remains damage; result type `7` |

### Gear variance and results

The Gear transaction adds positive variance with a different threshold and
range:

```text
if damage < 15:
    damage += random_mod_3
else:
    damage += random_mod_(damage / 15 + 2)
```

When the action descriptor supplies a nonzero 16-bit attack mask,
`BattleApplyAttackMaskResistance` then applies the selected per-mask resistance.
The Gear resolver categories are:

| Resolver result | Category | Calculation and result-record effect |
|---:|---|---|
| `1` | Full hit | Keep the computed amount, including zero; result type `0` |
| `2` | Guarded hit | Apply Gear guard scaling; result type `5` |
| `3` | Miss or attack negation | Zero amount; result type `4` |
| `4` | Elemental absorption | Computed amount becomes HP recovery; result type `2` |

For Gear guarded category `2`, let `G` be the target Gear byte at Gear `+0x9C`,
clamped to nine:

```text
G = min(target_gear.byte_0x9C, 9)
damage = damage * (10 - G) / 20
```

Thus Gear guarding is not a fixed one-half reduction. Both damage transactions
finally clamp the amount to `0..9999`. A multi-target ether mode can divide a
nonzero result by three after hit/result processing.

## 5. Crisis, Criticals, And Counterattacks

`BattleApplyCrisisAndCriticalPowerModifiers` at `0x8009B46C` is used by
eligible party attacks. It counts qualifying low-HP party conditions: below
one-half maximum adds one step and below one-quarter adds another. For every
step, power increases by half of its current base value:

```text
P += crisis_steps * (P / 2)
```

It then performs a critical roll. The ordinary threshold is 10 percent and a
trait can raise it to 60 percent. A successful critical multiplies power by
`3/2`, or by `2` with the stronger critical trait. The command-loadout type
selects the party attacks that receive this modifier.

`BattleTriggerPlayerCounterattack` at `0x800968C0` uses these conditions:

- The target's Stop and defeated mask `+0x7C:0xA000` and Sleep
  `+0x80:0x1000` are clear.
- The target is party slot `0..2`, as enforced by the caller.
- The attacker is an enemy slot, or the party target carries Confusion at
  `+0x80:0x2000`.
- The target carries Counter UP at `+0x88:0x1000`.
- The action descriptor selects physical damage, enables counters, and uses a
  reaction class other than `2`.
- The attacker is in character scale.

The test is `random_mod_100 <= threshold`, where the threshold is 50, or 60 for
one target command-loadout type. These inclusive comparisons accept 51 or 61 of
the 100 possible residues.

On success the routine:

1. Swaps attacker and target contexts.
2. Rebinds the action descriptor to the countering entity.
3. Sets the original target's incoming amount to zero.
4. Records result code `7` and schedules reaction animation `0x34`.

The counter replaces the incoming damage transaction.

## 6. Applying Resource Changes

`BattleApplyDamageAndResourceChanges` interprets one result type per target:

| Type | Effect |
|---:|---|
| `0`, `5`, `7`, `8` | Damage current HP, or Gear HP in Gear mode |
| `1`, `9` | Damage MP |
| `2` | Recover character HP on foot or under the item resource override; otherwise recover Gear HP in Gear mode |
| `3` | Recover character MP on foot or under the item resource override; otherwise ignore the change in Gear mode |
| `10` | Damage Gear fuel |
| `11` | Recover Gear fuel |

Damage floors the selected resource at zero. Recovery caps it at the matching
maximum. HP reaching zero sets defeat state; enemy slots can then be removed by
`BattleRemoveDefeatedEntityFromSlot` at `0x800883AC`. Character and Gear HP use
different widths, but both are applied through this result-type dispatch.
Global item resource override `0x800C2050` changes routing for both recovery
types. Type `2` updates character HP at `+0x4C/+0x4E` instead of Gear HP, and
type `3` updates character MP at `+0x50/+0x52`, even when the target is in Gear
mode. Without the override, a Gear-mode target routes type `2` to Gear HP and
skips type `3`.

`BattleComputeScriptedDamageAmount` at `0x80096018` supports descriptor-selected
damage formulas:

- Current HP divided by an action rate.
- Current HP minus one.
- Attacker's missing HP.
- Target MP multiplied by ten.
- Exactly one point.
- Current HP.
- Copy the target's current maximum HP into the amount. If that maximum exceeds
  `9999`, subsequently replace the target's maximum-HP field with `9999`; the
  already computed amount is not capped. Result staging retains its low 16
  bits.
- For an on-foot target, set physical-effect block `+0x7C:0x0001` and
  Ether-effect block `+0x80:0x0001`. A Gear-mode target receives failure result
  type `6` instead.

`BattleComputeDrainTransfer` at `0x80095D4C` records matching target damage and
attacker recovery. Its amount is normally a selected maximum resource times an
action rate divided by ten. The current-HP-minus-one mode leaves one HP.

## 7. Items And Periodic Changes

`BattleApplyItemEffectToTarget` at `0x80098D2C` interprets item descriptor flags.
Items can perform fixed HP or MP recovery, clear complete status groups, revive
to a fraction of maximum HP, apply timed masks, modify command state, alter
progression counters, or invoke special inventory effects. Gear-mode targets
receive item effects whose descriptors enable Gear use.

The Battle inventory is a compact working copy. `BattleExecuteItemUse` decrements
the selected quantity only after a target operation is accepted and clears the
entry when its quantity reaches zero. The result overlay handles post-battle
item drops.

`BattleBuildPeriodicResourceChanges` at `0x8009ADA0` computes three independent
resource changes for the acting entity:

```text
HP damage = max_HP / 20
    when Poison (+0x7C:0x0800) is active

MP damage = max_MP / 20
    when MP drain (+0x80:0x0200) or Gear EP DOWN (+0x120:0x0200) is active

fuel damage = (max_fuel / 50) * enabled_source_count
    enabled sources: Fuel leak (+0x120:0x0080) and Booster (+0x124:0x8000)
```

The two fuel sources stack, so both produce twice the individually rounded
`max_fuel / 50` term. The caller commits these as damage result types `8`, `9`,
and `10`, respectively. Active entities receive the changes through the common
resource machinery before the second win-condition check.

## 8. Status Storage

Character-scale status state is organized into alternating active and
protection/resistance words:

| Logical group | Active field | Protection/resistance field |
|---:|---:|---:|
| `0` | `+0x7C` | `+0x7E` |
| `2` | `+0x80` | `+0x82` |
| `5` | `+0x84` | `+0x86` |
| `7` | `+0x88` | `+0x8A` |
| `9` | `+0x8C` | `+0x8E` |

The group number is the operation selector accepted by status and item data.
Groups contain conditions, stat modifiers, and mutually exclusive elemental
tiers.

### Character status cues

`BattleSelectCharacterStatusCue` at `0x8009B684` maps a status group and mask to
the displayed status cue:

| Group | Mask | Displayed cue |
|---:|---:|---|
| `0` | `0x2000` | `Stop` |
| `0` | `0x1000` | `Slow` |
| `0` | `0x0800` | `Poison` |
| `0` | `0x0400` | `ACC DOWN` |
| `0` | `0x0200` | `Atk DOWN` |
| `0` | `0x0002` | `Play Dead` |
| `0` | `0x0001` | `Block PHY effects` |
| `2` | `0x2000` | `Confusion` |
| `2` | `0x1000` | `Sleep` |
| `2` | `0x0800` | `Can't use Eth Ability` |
| `2` | `0x0400` | `Eth effect DOWN` |
| `2` | `0x0020` | `Bart taunts enemy` |
| `2` | `0x0001` | `Block ETH effects` |
| `5` | `0x8000` | `Speed UP` |
| `5` | `0x4000` | `Def UP` |
| `5` | `0x2000` | `Atk UP` |
| `5` | `0x1800` | `ACC-EV UP` |
| `5` | `0x1000` | `ACC UP` |
| `5` | `0x0800` | `EV UP` |
| `7` | `0x0002`, `0x0008` | `Atk UP-Def DOWN` |
| `7` | `0x0001`, `0x0004` | `Def UP-Atk DOWN` |
| `7` | `0x8000` | `Eth effect UP` |
| `7` | `0x4000` | `Eth Def UP` |
| `7` | `0x1000` | `Counter UP` |
| `9` | `0x8000` | `Add Wind to Atk` |
| `9` | `0x4000` | `Add Earth to Atk` |
| `9` | `0x2000` | `Add Fire to Atk` |
| `9` | `0x1000` | `Add Water to Atk` |
| `9` | `0x0800` | `Block Wind-Weak point Earth` |
| `9` | `0x0400` | `Block Earth-Weak point Wind` |
| `9` | `0x0200` | `Block Fire-Weak point Water` |
| `9` | `0x0100` | `Block Water-Weak point Fire` |

`BattleApplyStatusWithResistance` assigns the `Play Dead` cue directly for
`0:0x0002` when the party-disabling guard accepts the status.

The scheduler uses `Slow` (`+0x7C:0x1000`), `Stop`
(`+0x7C:0x2000`), `Speed UP` (`(+0x84 | +0x86):0x8000`), `Sleep`
(`+0x80:0x1000`), and `Confusion` (`+0x80:0x2000`).

### Gear status cues and equivalents

Gear status storage is a separate three-pair layout inside the Gear subrecord:

| Gear selector | Active field | Protection/resistance field |
|---:|---:|---:|
| `0` | entity `+0x120` (Gear `+0x7C`) | entity `+0x122` (Gear `+0x7E`) |
| `1` | entity `+0x124` (Gear `+0x80`) | entity `+0x126` (Gear `+0x82`) |
| `3` | entity `+0x128` (Gear `+0x84`) | entity `+0x12A` (Gear `+0x86`) |

`BattleApplyGearStatus` at `0x8009DBFC` writes these words and
`BattleSelectGearStatusCue` at `0x8009E868` maps them to displayed cues.

| Gear selector | Mask | Displayed cue |
|---:|---:|---|
| `0` | `0x0400` | `Stop` |
| `0` | `0x0200` | `EP DOWN` |
| `0` | `0x0100` | `Stop Eth Engine` |
| `0` | `0x0080` | `Fuel leak` |
| `0` | `0x0040` | `Def DOWN` |
| `0` | `0x0020` | `Slow` |
| `0` | `0x0010` | `ACC-EV DOWN` |
| `0` | `0x0004` | `PWR loss` |
| `1` | `0x1000` | `PWR UP` |
| `1` | `0x0800` | `ACC UP` |
| `1` | `0x0400` | `EV UP` |
| `1` | `0x0040` | `Eth effect UP` |
| `1` | `0x0020` | `Eth Def UP` |
| `1` | `0x0002`, `0x0008` | `Atk UP-Def DOWN` |
| `1` | `0x0001`, `0x0004` | `Def UP-Atk DOWN` |
| `3` | `0x8000` | `Add Wind to Atk` |
| `3` | `0x4000` | `Add Earth to Atk` |
| `3` | `0x2000` | `Add Fire to Atk` |
| `3` | `0x1000` | `Add Water to Atk` |
| `3` | `0x0800` | `Block Wind-Weak point Earth` |
| `3` | `0x0400` | `Block Earth-Weak point Wind` |
| `3` | `0x0200` | `Block Fire-Weak point Water` |
| `3` | `0x0100` | `Block Water-Weak point Fire` |

Character and Gear status fields use the same displayed names for `Stop`,
`Slow`, the opposing Attack/Defence modifiers, and the elemental families.

`BattleApplyStatusWithResistance` at `0x80097964` performs the common
character-scale operation:

1. Dispatch character-scale targets through this path and Gear-mode targets
   through the Gear/enemy path.
2. Accept the probability test when `random_mod_100 <= descriptor_chance`,
   including residue zero at chance zero.
3. Apply bits whose corresponding protection/resistance bits are clear.
4. Resolve opposing pairs or mutually exclusive tier groups.
5. For a party-disabling bit, call
   `BattleCanApplyPartyDisablingStatus` at `0x80099498`.
6. OR the surviving mask into the active word and choose a result cue.
7. Initialize a duration when the mask maps to a timed slot.

The party-disabling guard preserves at least one available member in the active
party as a rule of status application.

Group `7` contains two opposing pairs: masks `0x0001/0x0004` cancel
active `0x0002/0x0008`, and vice versa. Group `9` treats `0xF000` and `0x0F00`
as mutually exclusive tier families. A clear protection family allows the new
tier to replace the active family.

## 9. Status Durations And Expiration

`BattleInitializeStatusDuration` at `0x800995A0` maps named statuses to the
timer bytes beginning at entity offset `+0x15C`:

| Status | Group and mask | Timer index |
|---|---:|---:|
| Stop | `0:0x2000` | `0` |
| Slow | `0:0x1000` | `1` |
| Sleep | `2:0x1000` | `2` |
| Can't use Eth Ability | `2:0x0800` | `3` |
| Speed UP | `5:0x8000` | `4` |
| Def UP | `5:0x4000` | `5` |
| Eth effect UP | `7:0x8000` | `9` |
| Eth Def UP | `7:0x4000` | `10` |
| Counter UP | `7:0x1000` | `11` |
| Add Wind/Earth/Fire/Water to Atk | `9:0xF000` | `12` |
| Block element-Weak point element | `9:0x0F00` | `13` |

Duration comes from the action descriptor. An attacker modifier can double it.
For groups `5`, `7`, and `9`, active Def UP-Atk DOWN tiers
`+0x88:0x0001/0x0004` double the duration, while Atk UP-Def DOWN tiers
`+0x88:0x0002/0x0008` halve it. The target's duration-doubling trait at
`+0x32:0x0040` doubles those same duration classes.

`BattleUpdateEntityStatusTimers` at `0x80099890` runs at the start of the
entity's turn and clears each mapped bit when its byte reaches zero. Gear-mode
entities use the Gear timer map in `BattleDecrementSecondaryStatusTimers` at
`0x80099CF0`. That map uses a subset of the bank through index `12`; Gear Hyper
reuses byte `+0x162`. Bytes `+0x16A..+0x16F` do not participate in either
status-duration map. `BattleTimeProgress` continuously decrements the Stop
`+0x7C:0x2000` timer.

Status-removal actions can clear selected masks or whole words.
`BattleDispelSelectedStatusGroups` at `0x800958D8` clears chosen active groups
after a chance roll. `BattleRemoveConfiguredStatuses` at `0x80095BAC` supports
broader group clearing. `BattleReviveEntityAndClearStatuses` at `0x80085AC4`
restores current HP to maximum and clears the five active status words at
`+0x7C`, `+0x80`, `+0x84`, `+0x88`, and `+0x8C`.

## 10. Reactions And Postprocessing

Several mechanisms can add or rewrite action results after the initial target
calculation:

| Address | Function | Behavior |
|---:|---|---|
| `0x8008AC88` | `BattleQueueProtectedEnemyReaction` | Retains protected enemies and queues reaction code `0xF3` |
| `0x800968C0` | `BattleTriggerPlayerCounterattack` | Cancels incoming damage and schedules a role-swapped counter |
| `0x8009C4B4` | `BattlePostprocessGearAttackResult` | Advances attack level and applies recovery, variance, reflection, and MP protection |
| `0x80079840` | `BattleUpdateMonsterScriptAttackVars` | Injects the attacker mask and current descriptor fields before reaction-program processing |

The target's damage-variance trait at `+0x32:0x0080` randomly changes successful
damage to either one-half or three-halves of its computed amount. Damage
reflection at `+0x32:0x0020` copies the target's amount and result type back to
the attacker. MP protection at `+0x8A:0x0200` sets incoming MP damage to zero.

Status animations and defeat animations consume computed state.
`BattleStartNewDefeatAnimations` at `0x800C0314` marks newly defeated targets as
processed and waits for their presentation to settle. Resource application
sets defeat state when HP reaches zero.

## 11. Victory, Defeat, And Escape

`BattleCheckWinConditions` at `0x8007252C` rebuilds a living-entity mask. An
occupied party slot enters the mask when character state `+0x7C:0xC000` is
clear and its visual/lifecycle state is active. An occupied enemy ordinarily
enters when its mode-selected character `+0x7C` or Gear `+0x120` state has
`0xC000` clear and its visual and per-enemy lifecycle gates are active.

Enemy slots have one explicit retention exception. When an enemy carries
mode-selected terminal state `0xC000` but its slot bit is present in special
retention mask `0x800C3608`, the checker keeps that enemy in the living mask and
bypasses the ordinary visual/lifecycle gate for that slot. After constructing
the mask, it tests the two fixed sides:

```text
if (living_mask & 0x07F8) == 0:
    battle_result = 0x01       # victory

if (living_mask & 0x0007) == 0:
    battle_result = 0x81       # defeat
```

Because the party test occurs second, simultaneous absence of both sides ends
as defeat. Sleep at `+0x80:0x1000` does not alter either side-exhaustion test.
Only when neither terminal result was selected does the checker make a temporary
copy of the living mask, remove every sleeping participant from that copy, and
test whether anyone remains awake. If the temporary awake mask is zero, it
clears Sleep from the first sleeping entity in the original living mask and
continues Battle.

`BattleAttemptEscape` at `0x8009A9D0` succeeds when `random_mod_100 < 50`. On
success it immediately calls `BattlePersistPartyCombatState` at `0x8009BE0C`.
The command handler then sets result `0x40`. The persistence helper copies party
HP, MP, progression counters, Gear HP, and fuel back to persistent records,
clamps resources to maxima, and applies defeat-specific corrections.

The top-level `BattleMain` at `0x80070F40` distinguishes ordinary victory,
defeat, escape, and scripted result modes before entering Result.

## 12. Results And Persistence

The Result phase has a separate lifetime. Its simulation-facing order is:

| Address | Function | Responsibility |
|---:|---|---|
| `0x801E2280` | `BattleResultProcessRewards` | Total encounter experience and currency and run result presentation |
| `0x801E2794` | `BattleResultApplyPostBattleProgression` | Coordinate experience, unlock, drop, and persistent updates |
| `0x801E2888` | `BattleResultSyncPartyState` | Copy represented occupied party members and associated Gear resources to persistent state; defeated or removed characters persist at one HP |
| `0x801E2ACC` | `BattleResultDistributeExperience` | Divide earned experience among eligible active party members |
| `0x801E308C` | `BattleResultApplyLevelUps` | Advance both experience tracks and apply all resulting levels |
| `0x801E3A18` | `BattleResultUpdateAbilityUnlocks` | Evaluate and propagate newly unlocked abilities |
| `0x801E403C` | `BattleResultUpdateApLevels` | Raise AP levels when progression thresholds are reached |
| `0x801E42C4` | `BattleResultRollEnemyDrops` | Roll eligible enemy drop tables |
| `0x801E1444` | `BattleResultApplyItemDrops` | Commit aggregated drops within inventory capacity |
| `0x801E252C` | `BattleResultFinalize` | Finish post-battle processing and release Battle resources |

Ordinary victory applies EXP, levels, unlocks, and AP updates before synchronizing
live combat resources into the persistent records. Updated persistent maxima
participate in the final HP/MP clamp. Escape uses the resident persistence
helper. Defeat dispatches the defeat result path.
