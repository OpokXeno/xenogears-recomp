# Battle RNG

## 1. Scope

This chapter indexes every explicit random roll in Central Battle from the
generator's side: what each roll consumes, and where its formula is already
documented in full. All rolls in this chapter draw from the shared gameplay
generator described in
[`01-generators-and-determinism.md`](01-generators-and-determinism.md) —
either directly through `rand`, or through `RandomU8RangeInclusive` /
`BattleRandomU16InRange`. None of them touch the Movie or sound-modulator
generators.

`random_mod_N` below means "a value in `0..N-1`, uniformly distributed,"
matching the notation used throughout
[`docs/xenogears/battle/`](../battle/README.md). Every formula in this
chapter is quoted from, not re-derived from, the cited battle chapter; this
document does not restate their surrounding context.

## 2. Turn And Readiness

| Roll | Formula | Source |
|---|---|---|
| Readiness reload variance | `reload = (165 - normalized_speed_score) - (random_mod_8 - 4)` | [`battle/03`](../battle/03-combatants-turns-and-targeting.md) |
| Initial turn-order tie-breaking | All eleven combatant slot indices are shuffled into a circular randomized order once at Battle load; ties in the readiness comparison fall back to each slot's position in that shuffled order | [`battle/03` §7](../battle/03-combatants-turns-and-targeting.md#7-one-complete-turn) (`BattleLoaderInitializeTurnOrder`, `0x801E4E7C`) |
| Confused party member's action choice | Cascading rolls (Defend chance, deathblow-tier draw with a learned-move check, basic-attack variant split) pick what a Confused character actually does; see the source chapter for the full cascade | [`battle/03` §9](../battle/03-combatants-turns-and-targeting.md#9-forced-and-enemy-turns) (`BattleSelectAutomatedCharacterAttack`, `0x8009BAC4`) |

## 3. Damage And Hit Resolution

| Roll | Formula | Source |
|---|---|---|
| Character-scale damage variance | `if damage < 10: += random_mod_2` else `+= random_mod_(damage/10 + 2)` | [`battle/04` §4](../battle/04-actions-damage-and-status.md) |
| Gear-scale damage variance | `if damage < 15: += random_mod_3` else `+= random_mod_(damage/15 + 2)` | [`battle/04` §4](../battle/04-actions-damage-and-status.md) |
| Hit/evade resolution | Modulo-100 test against attacker hit%, action accuracy modifier, and target evade% | [`battle/04` §4](../battle/04-actions-damage-and-status.md) (`BattleResolveAttackHitType`, `0x80096AB8`) |
| Critical hit | `random_mod_100 <= threshold`; threshold `10`, or `60` with the stronger critical trait | [`battle/04` §5](../battle/04-actions-damage-and-status.md) (`BattleApplyCrisisAndCriticalPowerModifiers`, `0x8009B46C`) |
| Player counterattack | `random_mod_100 <= threshold`; threshold `50`, or `60` for one command-loadout type | [`battle/04` §5](../battle/04-actions-damage-and-status.md) (`BattleTriggerPlayerCounterattack`, `0x800968C0`) |
| Gear attack failure (Ether-based) | `fail unless random_mod_100 < gear_ether_stat + action_modifier`, checked before the attack resolves at all | [`battle/05`](../battle/05-gear-combat-and-special-commands.md#ether-based-attack-failure) (`BattleRollGearAttackFailure`, `0x80096824`) |

## 4. Status Effects

| Roll | Formula | Source |
|---|---|---|
| Status application | `random_mod_100 <= descriptor_chance`, chance `0` still passes on residue `0` | [`battle/04` §8](../battle/04-actions-damage-and-status.md) (`BattleApplyStatusWithResistance`, `0x80097964`) |

### Status dispel

[`battle/04` §9](../battle/04-actions-damage-and-status.md) names two dispel
functions but only calls this "a chance roll" with no threshold given. Both
were traced here, and their mechanisms differ from each other:

- **`BattleDispelSelectedStatusGroups`** (`0x800958D8`) draws **one**
  `rand() % 100` residue and requires it to satisfy **two** independent
  thresholds (an AND, not two separate rolls) before doing anything. Passing
  both clears status groups `5`, `7`, and `9` together — both the active and
  protection words for each — gated by a bitmask of which of those groups
  were actually requested. Failing either threshold leaves everything
  unchanged, silently.
- **`BattleRemoveConfiguredStatuses`** (`0x80095BAC`) draws its own
  `rand() % 100` residue against a **single** threshold. On failure it writes
  an explicit "no effect" result code and returns immediately. On success it
  clears a wider, more varied set of individual status bits (spanning groups
  `0` and `2`, plus at least one field not present in `battle/04`'s status
  table at all) selected by flag bits rather than further rolls — one roll
  gates the whole operation.

Both functions read their threshold bytes and group/bit masks through the
same kind of indirection: a shared pointer that gets pointed at a small
parameter block by whichever action is currently executing (confirmed by
finding at least five distinct, already-documented action-resolution
functions — `BattleComputeAttackResults`, `BattleTriggerPlayerCounterattack`,
and others — that each set this pointer before the dispel call). The actual
chance values therefore live inside each dispel-capable action's own data,
not in one static lookup table, so there is no single small table to extract
the way there was for enemy drops. Enumerating every action whose data
carries a nonzero dispel component was scoped and set aside: the pointer's
source data is itself indexed through a further, not-yet-bottomed-out
indirection, and it did not look like a bounded task comparable to the enemy
drop or field encounter tables.

## 5. Escape

| Roll | Formula | Source |
|---|---|---|
| Character or Gear Escape | `random_mod_100 < 50` | [`battle/04` §11](../battle/04-actions-damage-and-status.md), [`battle/05` §9](../battle/05-gear-combat-and-special-commands.md), [`battle/08` §12](../battle/08-results-progression-and-persistence.md) all cite the same `BattleAttemptEscape` (`0x8009A9D0`) |

## 6. Hyper Mode

| Roll | Formula | Source |
|---|---|---|
| Automatic Hyper entry | `random_mod_100 < chance`, where `chance` is derived from Gear HP fraction and party slot (full formula in the source chapter) | [`battle/05` §6](../battle/05-gear-combat-and-special-commands.md) (`BattlePrepareGearTurnParameters`, `0x8009A2D4`) |

## 7. Results, Progression, And Drops

| Roll | Formula | Source |
|---|---|---|
| Stat growth (Attack/Defence/etc., `next_level < 100`) | `score = trunc(((target-current)*100)/(100-next_level)) - 50 + random_mod_100`; grows on `score >= 50` | [`battle/08` §8](../battle/08-results-progression-and-persistence.md) |
| Maximum HP growth (`next_level < 100`) | `gain = trunc((random_mod_100 * (hp_target_1+99-next_level-max_hp)) / ((100-next_level)*100)) + 2` | [`battle/08` §8](../battle/08-results-progression-and-persistence.md) (`BattleResultRollMaxHpGrowth`, `0x801E3700`) |
| Maximum HP growth (`next_level >= 100`) | `gain = random_mod_100 * q * 2 + 2`, `q = trunc((hp_target_2-max_hp)/((201-next_level)*100))` | same |
| Maximum MP growth | Same two-branch shape as maximum HP, own targets | [`battle/08` §8](../battle/08-results-progression-and-persistence.md) (`BattleResultRollMaxMpGrowth`, `0x801E38CC`) |
| Enemy item drop | Two independent `random_mod_100` tests against per-enemy primary/secondary chance bytes; see Section 8 | [`battle/08` §10](../battle/08-results-progression-and-persistence.md) (`BattleResultRollEnemyDrops`, `0x801E42C4`) |

## 8. Enemy Drop Rolls In Detail

This is the one battle roll whose per-subject data (each enemy's actual drop
chances and items) is worth a dedicated exhaustive table, since — unlike a
hit-chance formula — the interesting content is the data, not the formula.
The formula itself, quoted from
[`battle/08` §10](../battle/08-results-progression-and-persistence.md):

```text
if random_mod_100 < primary_chance or trader_card_equipped:
    award primary
else if fresh_random_mod_100 < secondary_chance:
    award secondary
else:
    emit an empty drop entry
```

Each enemy's primary/secondary chance and item data live at fixed offsets in
its live `0x170`-byte entity record (`+0x150`..`+0x155`), which
[`battle/01`](../battle/01-concepts-and-lifecycle.md#7-static-input-and-runtime-realization)
already establishes is a straight copy of the enemy's static definition
record made at Battle load. That means every enemy's real drop chances and
item IDs are recoverable directly from the enemy definition data without
touching the disassembly further — a data-extraction task, not a
reverse-engineering one. The complete table for all 76 enemy sets, with item
IDs resolved to real item names, is in
[`04-enemy-drop-tables.md`](04-enemy-drop-tables.md).

## 9. Function Index

| Address | Function |
|---|---|
| `0x801E4E7C` | `BattleLoaderInitializeTurnOrder` |
| `0x8009BAC4` | `BattleSelectAutomatedCharacterAttack` |
| `0x80096AB8` | `BattleResolveAttackHitType` |
| `0x8009B46C` | `BattleApplyCrisisAndCriticalPowerModifiers` |
| `0x800968C0` | `BattleTriggerPlayerCounterattack` |
| `0x80096824` | `BattleRollGearAttackFailure` |
| `0x80097964` | `BattleApplyStatusWithResistance` |
| `0x800958D8` | `BattleDispelSelectedStatusGroups` |
| `0x80095BAC` | `BattleRemoveConfiguredStatuses` |
| `0x8009A2D4` | `BattlePrepareGearTurnParameters` |
| `0x8009A9D0` | `BattleAttemptEscape` |
| `0x801E3700` | `BattleResultRollMaxHpGrowth` |
| `0x801E38CC` | `BattleResultRollMaxMpGrowth` |
| `0x801E42C4` | `BattleResultRollEnemyDrops` |
