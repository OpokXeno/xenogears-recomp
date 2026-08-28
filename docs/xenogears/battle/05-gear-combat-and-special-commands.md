# Gear Combat And Special Commands

## 1. Combat Model

Gear combat retains the pilot and Gear in one live combatant record. Gear mode
selects Gear HP, fuel, statistics, attacks, Hyper state, and the Gear command
ring. Character Combo uses saved AP as the corresponding character-scale
attack-progress resource. Integer division truncates toward zero throughout the
formulas below.

## 2. Live Gear State

Each live combatant occupies a `0x170`-byte record at
`0x800CCCE8 + slot * 0x170`. The first `0xA4` bytes are the character-scale
record. Gear data and Battle-only state follow it, allowing one party slot to
retain the pilot and Gear state simultaneously.

Command and damage routines use these fields:

| Live offset | Width | Use |
|---:|---:|---|
| `+0x56` | 1 | Command-loadout/character type |
| `+0x62` | 1 | Character level used by the automatic Hyper gate |
| `+0xA0` | 1 | Associated Gear identifier; `0xFF` means no associated Gear |
| `+0xDC` | 2 | Current Gear fuel |
| `+0xDE` | 2 | Maximum Gear fuel |
| `+0xE0` | 1 | Gear output component used by physical power |
| `+0xE3` | 1 | Gear attack multiplier component |
| `+0x104` | 4 | Current Gear HP |
| `+0x108` | 4 | Maximum Gear HP |
| `+0x114` | 2 | Derived Gear physical defence component |
| `+0x116` | 2 | Derived Gear ether-defence component |
| `+0x120` | 2 | Gear condition flags, including `0x0080` Fuel Leak and `0x0400` Stop |
| `+0x124` | 2 | Gear action flags, including `0x4000` Hyper and `0x8000` Booster |
| `+0x126` | 2 | Protection/passive mirror for Gear positive-status group 1, paired with active field `+0x124` |
| `+0x13C` | 1 | Derived Gear agility |
| `+0x142` | 1 | Gear EtherAmp used by Gear ether power and Maria's Controls |
| `+0x148` | 1 | Current Attack Level, including `4` for Hyper |
| `+0x149` | 1 | Maximum ordinary Attack Level available to the Gear |
| `+0x15A` | 1 | Battle mode/stance flags; `0x80` selects Gear mode and `0x01` is defend/charge stance |
| `+0x162` | 1 | Remaining Hyper duration counter |

Current Gear HP is 32-bit and the Battle HUD renders five digits. Fuel and most
derived defence/output components are narrower. Damage and recovery still use
the shared result records: types `0`, `5`, `7`, and `8` damage Gear HP while
Gear mode is active; type `2` recovers Gear HP unless the item resource override
routes it to character HP; and types `10/11` damage or recover fuel.

### Derived power and defence

`BattleInitializePartyCombatStats` at `0x80097D5C` combines the persistent
character record, Gear record, and equipped component data before the first
turn. `BattleComputeAttackPower` at `0x80096FBC` calculates the standard Gear
physical base as:

```text
gear_physical_base = gear_output * gear_attack_multiplier

if (+0x124 | +0x126) has PWR UP 0x1000:
    gear_physical_base += 2 * gear_output
```

Action-specific weapon/input components can then be added before descriptor
power is applied. Billy's loadout uses separate displayed aggregate output and
per-action selected components.

For a Gear ether action, `BattleComputeScaledAttackerPower` at `0x8009D948`
starts from the character Ether calculation and applies:

```text
gear_ether_power = character_ether_power * EtherAmp / 4
```

If the EtherAmp byte is zero, or Gear mask `+0x7C:0x0100` disables the
amplifier, the routine substitutes `4`; that produces a neutral `4/4` scale
with full power. Gear `Eth effect UP` at `(+0x124 | +0x126):0x0040` adds another
one-half of the scaled result.

The common standard-damage subtraction remains `4P - 3D` for physical attacks
and `5P - 4D` for ether attacks. Gear mode supplies its own inputs and action
data to this shared subtraction transaction.

## 3. Battle Entry And Calling A Gear

### Loader restrictions

`BattleLoaderLoadPartyResources` at `0x801E5384` copies the three selected
character records and their associated Gear records into live Battle state.
The Gear record is therefore already present even when the party member enters
on foot. Calling a Gear changes the combat mode and visual actor while using the
Gear HP and fuel loaded at Battle startup.

`BattleLoaderInitializeCommandsAndTargets` at `0x801E5014` expands each
character's command mask and applies these encounter restrictions:

| Condition | Consequence |
|---|---|
| Associated Gear ID is `0xFF` | Disable the Call Gear command |
| Encounter configuration bit `0x40` | Disable Call Gear for every party slot |
| Encounter configuration bit `0x80` | Disable Escape for every party slot |
| Character/status command masks | Disable their corresponding ring entries |

The `0x40` and `0x80` restrictions are installed in the command masks before
command selection.

### Transformation transaction

`BattleTransformCharacterToGear` at `0x800826CC` performs the interactive Call
Gear transition:

1. Relocate the entity to an available Gear formation position.
2. Clear character-scale status through `BattleClearCharacterStatusForGear` at
   `0x8009AEFC`.
3. Swap the visual backing to the associated Gear.
4. Set live `+0x15A:0x80` and switch the slot's command/HUD state to Gear mode.
5. Mark the party slot and matching resident party entry as being in a Gear.

The status reset is broad and exact:

```text
entity.+0x7C = 0
entity.+0x84 = 0
entity.+0x88 = 0
entity.+0x8C = 0
entity.+0x80 &= 0x2000
```

For command-loadout type `3`, the same routine also clears mask `0x0020` in a
status field for enemy slots `3..10`.

Character HP and MP remain in the character half of the record. Ordinary Gears
begin using the Gear HP/fuel state which was loaded separately. Chu-Chu's
character-ID-7 and grown-form-Gear-record-7 conversions are described in
Section 11.

Battle Event opcode `0x39`, `BattleEventOpcode39TransformToGear` at
`0x801E80F0`, performs a fixed slot-0 transformation. It assigns Gear ID `0` to
both the live slot field `+0xA0` and character 0's persistent associated-Gear
field at `0x8006D940` within the record based at `0x8006D8A0`, replaces the
slot-0 model with that Gear, sets Gear mode
`+0x15A:0x80`, selects Gear turn mode `2`, selects HUD display mode `2`, enables
the slot's Gear command state, and marks the transformed model active.

### Persistence boundary

`BattlePersistPartyCombatState` at `0x8009BE0C` writes character HP, MP, seven
deathblow proficiency counters, the prevariance lethal-match counter, Gear HP,
and fuel back to separate persistent records.
Status words retain Battle lifetime. For ordinary represented Gears the routine
clamps current HP and fuel to their maxima. A defeated Gear is written back at
one tenth maximum Gear HP:

```text
persisted_gear_hp = max_gear_hp / 10
```

Successful character or Gear escape calls this synchronization immediately.
Victory performs equivalent reconciliation in the Result image. Calling a Gear
keeps the pilot and Gear HP pools independent. Chu-Chu's explicit conversion in
Section 11 connects the pools for that character.

## 4. Gear Turn Parameters And Fuel

At the beginning of a Gear command turn,
`BattlePrepareGearTurnParameters` at `0x8009A2D4` copies the fuel costs from all
15 Gear attack descriptors into a turn-local array. The descriptors correspond
to three ordinary attacks, nine ordinary Gear deathblows, and three Hyper
attacks. The Attack command validates the selected entry against current fuel
and subtracts that exact descriptor cost only after the attack is accepted.

The same turn preparation computes Charge recovery, Hyper chance, displayed
Attack Level, and five condition bits. It also clears fuel-dependent state when
current fuel is zero. Fuel is persistent Gear state consumed by attack
descriptors and Gear specials, then restored by Charge or explicit result and
passive effects.

`BattleRewriteAbilityTargetModes` at `0x8009AB38` handles the character
targeting modifier at `+0x88:0x0400` and the Gear targeting modifier at
`+0x124:0x2000`. While the corresponding modifier is active, it writes target
mode `1` to offset `+0x00` of the eligible character or Gear ability
descriptors. `BattleRestoreAbilityTargetModes` at `0x8009AC48` writes those
target modes back to `0x2000` and clears the active modifier on the qualifying
execution path. These operations do not change character MP cost `+0x13` or
Gear fuel cost `+0x24` in the action descriptors.

## 5. Gear Attacks, Levels, And Deathblows

### Fifteen attack slots

The Gear attack selector maps T, S, and X into this fixed index space:

| Action indices | Required state | Input sequences |
|---:|---|---|
| `0..2` | Ordinary basic attacks | `T`, `S`, `X` |
| `3..5` | Attack Level 1 deathblows | `TT`, `TS`, `TX` |
| `6..8` | Attack Level 2 deathblows | `ST`, `SS`, `SX` |
| `9..11` | Attack Level 3 deathblows | `XT`, `XS`, `XX` |
| `12..14` | Attack Level 4/Hyper attacks | `T`, `S`, `X` |

`BattleCanSelectGearAttackInput` at `0x80086B88` checks the move's persistent
unlock bit and the tier implied by the partial input sequence. Outside Hyper,
the requested tier is bounded by current Attack Level. In Hyper, the first input
maps directly to indices `12..14`.

Against a character-scale target, a non-Hyper Gear uses alternate basic attack
data. At Hyper AL `4`, T/S/X map to attack indices `12..14` against both
character-scale and Gear-scale targets.

### Attack Level transitions

`BattlePostprocessGearAttackResult` at `0x8009C4B4` updates Attack Level after
the action:

```text
basic T/S/X (indices 0..2):
    AL = min(AL + 1, AL_limit)

level-1 deathblow (indices 3..5):
    AL = displayed_AL - 1

level-2 deathblow (indices 6..8):
    AL = displayed_AL - 2

level-3 deathblow (indices 9..11):
    AL = displayed_AL - 3
```

Using a lower-tier deathblow at a higher Attack Level subtracts that move's tier
from displayed AL. Hyper attacks `12..14` retain AL `4` until Hyper duration
expires.

The maximum ordinary AL is initialized from three groups of Gear-deathblow
unlock bits. The limit becomes `1`, then `2`, then `3` as the corresponding
higher groups become nonempty, so the highest available group wins. A Gear
with an empty set of those groups remains capped at AL `0`.

### Hyper Mode Points

Only ordinary Gear deathblows `3..11` add Hyper Mode Points:

| Deathblow | `TT` | `TS` | `TX` | `ST` | `SS` | `SX` | `XT` | `XS` | `XX` |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| HMP | 1 | 1 | 2 | 2 | 2 | 4 | 3 | 3 | 6 |

Basic and Hyper attacks add none. HMP is live Battle state at character offset
`+0x54`. Automatic Hyper activation leaves it unchanged; Hyper expiration resets
it to zero.

## 6. Hyper Mode

### Chance calculation

The automatic Hyper chance is computed on each Gear turn before commands are
shown. Let `slot` be party slot `0..2`, `HP` current Gear HP, and `MaxHP`
maximum Gear HP:

```text
if HP == MaxHP:
    HMM = slot + 1
else:
    HMM = (MaxHP - HP) / (MaxHP / 10)
    if low_byte(HMM) == 0:
        HMM += 1

raw = low_byte(HMM) * (HMP + 5)
chance = min(low_byte(raw), 99)
```

The formula first computes `max_hp / 10`, then divides `missing_hp` by that
integer result. At full HP the function uses the caller's party-slot value,
producing base modifiers `1`, `2`, and `3`.

Automatic chance is then gated:

| Gate | Result |
|---|---|
| Resident Battle flag `0x4000` clear | Chance `0` |
| Character level below `50` | Chance `0` |
| Associated Gear ID `3` | Chance `0` |
| Associated Gear ID `15` | Chance forced to `99` |
| Truncated chance above `99` | Clamp to `99` |

The product wraps to its low byte before the 99 clamp.

### Entry and duration

Automatic entry is tested only when current AL is `3`:

```text
if random_mod_100 < chance:
    set Gear status 0x4000
    Hyper duration = 3
    if character.+0x32 has 0x0040:
        Hyper duration = 6
    AL = 4
```

The `+0x32:0x0040` bit doubles Hyper duration. The roll occurs when
`BattlePrepareGearTurnParameters` prepares the Gear turn following the action
that leaves AL at three.
Consequently Item, Ether Machine, Booster, Charge, a failed Escape, or another
non-AL-spending action can leave AL `3` for the next entry check.

While AL is `4`, each Gear-turn preparation decrements the duration byte. When
it reaches zero, the code clears Gear status `0x4000`, sets AL to `0`, and
clears HMP. Gear-turn preparation is the Hyper duration clock.

`BattleActivateGearHyperMode` at `0x8009C0E0` is a separate direct entry path.
It sets AL `4`, duration `3`, and ORs `0x4000` into character 0's persistent
Gear-special availability word at `0x8006ED0E`. The direct path uses a fixed
three-turn duration and does not set the automatic path's live Gear status
`+0x124:0x4000`.

### Condition flags

`BattleGetGearConditionFlags` at `0x8009C050` returns a compact UI/state mask:

| Result bit | Condition |
|---:|---|
| `0x01` | Gear status `+0x120:0x0400` is active |
| `0x02` | Current Gear HP is below `MaxHP / 8` and `+0x36:0x0001` is clear |
| `0x04` | Attack Level equals `4` |

These bits describe Gear conditions. Bit `0x02` is a critical-HP indicator;
Hyper chance uses the separate HMM formula.

## 7. Defend And Charge

`BattleApplyDefendStance` at `0x8009AA44` is shared by character Defend and Gear
Charge. It sets live runtime flag `+0x15A:0x01`. In Gear mode it also removes a
status group when Gear mask `+0x126:0x0010` is set:

```text
entity.+0x120 &= 0xFE4F
entity.+0x7C  &= 0xEFFF
```

`BattleClearDefendStance` at `0x8009AB00` clears the stance at the start of the
combatant's next command turn.

For attacks which reach the standard hit resolver, the stance bypasses the
ordinary hit/evade comparison and rolls:

```text
95%: result category 2, which halves the computed amount
 5%: result category 1, which keeps the normal amount
```

The 95-percent guard reduction uses result category `2`; the defence statistic
keeps its original value.

### Charge recovery

Charge first applies that stance, then adds the amount prepared at
`0x8009A2D4` to current fuel and clamps to maximum fuel. Let `charger` be the
Gear byte at Gear-relative `+0x57`, `T_cost` be the first Gear attack
descriptor's fuel cost, and `AL` be current Attack Level:

```text
if charger == 0:
    charge_fuel = 30
else:
    charge_fuel = charger * T_cost

if AL in 1..3:
    charge_fuel += 20 * AL
else if AL == 4:
    charge_fuel *= 10
```

For charger values `0`, `5`, `10`, `20`, and `50` with `T_cost = 10`, the
result is:

| AL | None | A Charger | S Charger | X Charger | Z Charger |
|---:|---:|---:|---:|---:|---:|
| `0` | 30 | 50 | 100 | 200 | 500 |
| `1` | 50 | 70 | 120 | 220 | 520 |
| `2` | 70 | 90 | 140 | 240 | 540 |
| `3` | 90 | 110 | 160 | 260 | 560 |
| `4`/Infinity | 300 | 500 | 1000 | 2000 | 5000 |

Hyper multiplies the base charge by ten instead of first adding the ordinary
AL increment. Charge recovery above the missing amount is discarded by the
maximum-fuel clamp.

## 8. Booster

`BattleHandleGearBoosterCommand` at `0x800830A8` toggles Gear status
`+0x124:0x8000` and its active mirror `+0x84:0x8000`. Enabling it clears
`+0x120:0x0020` and `+0x7C:0x1000`; disabling it clears both Booster bits. The
command is target-free and commits the turn immediately.

`BattleTimeProgress` at `0x8007171C` advances readiness as follows:

```text
ordinary readiness decrement = 1
if (+0x84 | +0x86) has 0x8000:
    readiness decrement = 2
```

Active Booster doubles each readiness decrement from one to two while retaining
the stored readiness interval.

The end-of-turn periodic-change path applies Booster's fuel debit as result type
`10`:

```text
booster_fuel_damage = max_fuel / 50
```

Fuel Leak contributes a second separately rounded `max_fuel / 50` term, so both
active effects stack to twice that amount. `BattlePrepareGearTurnParameters`
clears both Booster bits when current fuel is zero. A periodic debit that
reduces fuel to zero leaves Booster active through the following readiness
interval; the bits are cleared when that Gear's next command turn is prepared.

## 9. Ether Machine, Specials, Items, And Escape

### Ether Machine

The Gear Ether command enters the same ability menu and executor used by the
pilot's character abilities. `BattleTryUseAbility` at `0x8008B224` selects the
Gear-side ability descriptor and unlock mask while Gear mode is active, but it
still validates and subtracts the pilot's current MP after target confirmation.
The pilot's MP remains the resource charged for these abilities.

Damage and healing behavior comes from the selected Gear-side descriptor. For
ordinary offensive ether, the pilot's Ether input receives the `EtherAmp / 4`
scale described in Section 2. Gear healing uses an explicit Gear-compatible
result path. `BattleComputeGearHealing` at `0x8009BD94` uses:

```text
gear_hp_recovery = effect_power * max_gear_hp / 20
```

### Special Options

`BattleRunGearSpecialSelectionMenu` at `0x8008CFB8` enumerates up to four
Gear-specific Special Options. Each option occupies Gear action descriptor
`0x25 + option_index` and provides its own name, target flags, effect data, and
16-bit fuel cost.

`BattleTryUseGearSpecial` at `0x8008CDE4` requires all of the following:

- Current fuel is at least the descriptor cost.
- The option's persistent availability test succeeds.
- The option slot's live command-restriction bit is clear.
- Target selection is confirmed.

Only after confirmation does it execute:

```text
current_fuel -= option_fuel_cost
```

Fuel is subtracted after all availability and confirmation checks succeed.
Special effects and the number of populated option slots come from the Gear's
descriptors.

### Items and Billy ammunition

`BattleLoaderBuildBattleInventory` at `0x801E4CD0` compacts up to 48 usable
ordinary consumables with IDs `1..48` from the persistent 150-entry item bank.
It separately scans the 100-entry character-weapon bank for IDs `0x32..0x48`
and builds the secondary Billy-ammunition list.

`BattleExecuteItemUse` at `0x8008B908` consumes one ordinary-item quantity only
after use is committed. During item result application it enables the item
resource override, so result type `2` restores character HP at `+0x4C` even
when the recipient is in Gear mode. Ordinary items do not restore Gear HP;
Gear-HP healing uses the Gear action behavior at `0x8009BD94` described under
Ether Machine.

The secondary list receives immediate Billy-ammunition handling rather than
ordinary target selection. `BattleIsBillyAmmoReward` and
`BattleRegisterBillyAmmoReward` at `0x8009A7E4` and `0x8009A854` match its
identifiers against Billy's equipped stocks, consume the selected ammunition
quantity, and copy the ammunition fields into persistent and live equipped
slots. Ammunition and Gear fuel are separate resources.

### Escape

Character Escape at `0x80082504` and Gear Escape at `0x80083580` both call
`BattleAttemptEscape` at `0x8009A9D0`:

```text
success = random_mod_100 < 50
```

On success, Battle synchronizes character HP/MP, progression counters, and Gear
HP/fuel before returning result `0x40`; statuses end with the Battle instance.
Availability is determined earlier by the encounter command mask. A failed Gear
escape spends the turn and retains AL, so AL `3` remains eligible for the next
automatic Hyper check.

## 10. Character Combo And Saved AP

Remaining character AP is accumulated after a committed attack sequence. The
saved value is capped at `0x1C` (28). Cancelling before the first attack input
leaves the saved value unchanged; ending a partially used sequence saves the
remainder.

The Combo command uses this saved value as a budget. `BattleDrawCircleMenuComboSelect`
at `0x8008C81C` enumerates the first seven learned character deathblows, records
each selected deathblow's AP cost, and permits up to seven selections:

```text
select move only if move_cost <= remaining_saved_AP
remaining_saved_AP -= move_cost

remove last selection:
    remaining_saved_AP += removed_move_cost
```

On target confirmation it writes the remaining budget back to the slot's saved
AP byte. `BattleExecuteSelectedCombo` at `0x8008C4A8` then resolves each chosen
deathblow in order against the confirmed target. Cancellation restores command
selection and the original saved-AP budget. Confirmation commits the edited
budget.

`BattleExecuteSelectedCombo` neither injects reaction variables nor dispatches
an enemy reaction program. Ordinary attacks inject the variables through
`BattleUpdateMonsterScriptAttackVars` at `0x80079840` and then call
`BattleExecuteMonsterAttackedScript` at `0x80079AB0`. Abilities and items perform
only the variable injection. Combo bypasses both operations, spends saved AP,
and is available from the character command ring while the slot is on foot.

## 11. Character-Specific Exceptions

### Billy, command-loadout type 4

Billy has explicit type-4 branches in both stat initialization and attack-power
calculation. Ordinary Attack display and damage paths can use equipped weapon
and ammunition components instead of the same `+0x58` Attack byte used by most
characters. His Gear attack records likewise select weapon components for
individual T/S/X actions, while deathblows use their own descriptors.

Ammo registration copies the ammo descriptor's attack byte directly into the
equipped slot's live `+0x04` component. The type-4 Attack display is the sum of
the components at `+0x04` and `+0x1C`. Let `C` be the component term selected by
an action's component mask after component modifiers, `B` its character or Gear
base physical term, and `Q` descriptor power byte `+0x11`. Power mode `2`
normalizes Billy's selected components before applying descriptor power:

```text
C_normalized = floor(3 * C / 5)
P_physical   = floor((C_normalized + B) * Q / 20)
P_ether      = floor(Billy_Ether * Q / 20)  if descriptor flag 0x0100 is set
```

Power mode `0` uses `C + B` directly. Ammo registration performs no inverse
normalization; the `3/5` conversion belongs to mode-2 attack power.

### Chu-Chu, character ID 7 and grown-form Gear record 7

Chu-Chu occupies two identifier spaces with the same numeric value. Live Battle
initialization, character-to-Gear conversion, and persistence recognize
character ID `7` at entity `+0x56`; Result updates grown-form Gear record `7`.
These paths provide the explicit conversion logic in
`BattleInitializePartyCombatStats`, `BattleClearCharacterStatusForGear`, and
`BattlePersistPartyCombatState`.

The HP conversion is:

```text
grown_current_hp = min(character_current_hp * 50, 99999)
grown_max_hp     = min(character_max_hp * 50, 99999)

character_hp_after_sync = max(1, (grown_current_hp + 1) / 50)
```

The grown-form initialization also writes:

```text
Gear output component  = character Attack
Gear physical defence  = character Defence * 12
Gear ether defence     = character Ether Defence * 6
Gear agility           = character Agility
```

The enlarged-form attack multiplier remains record-derived. Let `M_seed` be
Gear-relative byte `+0x74` and `M_part` be the equipment multiplier bonus at
Gear-relative byte `+0x56`:

```text
grown_attack_multiplier = M_seed + M_part
grown_physical_base      = character Attack * grown_attack_multiplier
```

`BattleInitializePartyCombatStats` stores this sum at live `+0xE3`, then the
character-ID-7 branch stores character Attack as output at live `+0xE0`. Gear PWR UP
therefore adds `2 * character Attack` to this base. The enlarged form's Gear
ether defence is exactly `character Ether Defence * 6`.

### Maria, command-loadout type 8

Maria's Controls remain character-scale and use ether-classified action
descriptors. Let `E` be Maria's current Ether power term, `Q` descriptor power
byte `+0x11`, and `A` the associated Gear's EtherAmp byte at live `+0x142`.
`BattleComputeAttackPower` calculates a mode-2 Control as:

```text
P0 = floor(E * Q / 20)
P_character_target = floor(P0 * A / 4)
character_target_damage_before_rate =
    5 * P_character_target - 4 * target_ether_defence
```

The associated Gear contributes `A`; `Q` supplies the action-specific power.
Against a Gear-scale target, `BattleComputeScaledAttackerPower` at `0x8009D948`
applies the Gear ether scale again. Let `A2` equal `A`, except that zero or Gear
mask `+0x7C:0x0100` selects the neutral value `4`:

```text
P_gear_target = floor(P_character_target * A2 / 4)

if (+0x124 | +0x126) has Eth effect UP 0x0040:
    P_gear_target += floor(P_gear_target / 2)

damage_before_rate = 5 * P_gear_target - 4 * target_gear_ether_defence
```

Once Maria enters her Gear, the ordinary Gear attack and deathblow paths use the
Gear descriptors and fuel costs.

## 12. Function Map

| Address | Function | Subsystem role |
|---:|---|---|
| `0x801E4AC0` | `BattleLoaderInitializePartyDisplayState` | Select character AP, Gear fuel, or hidden HUD mode |
| `0x801E5014` | `BattleLoaderInitializeCommandsAndTargets` | Install command layouts and no-Gear/no-escape restrictions |
| `0x801E5384` | `BattleLoaderLoadPartyResources` | Copy character, Gear, and action records into Battle |
| `0x801E80F0` | `BattleEventOpcode39TransformToGear` | Assign Gear ID 0 and transform party slot 0 |
| `0x8007171C` | `BattleTimeProgress` | Advance Booster readiness at double rate |
| `0x800826CC` | `BattleTransformCharacterToGear` | Execute interactive character-to-Gear transition |
| `0x80096FBC` | `BattleComputeAttackPower` | Compute descriptor, Billy, Chu-Chu, and Maria power branches |
| `0x8009AEFC` | `BattleClearCharacterStatusForGear` | Reset status and invoke Chu-Chu's character-ID-7 HP conversion |
| `0x8009B104` | `BattleScaleCharacterHpForGear` | Scale Chu-Chu HP by 50 with saturation |
| `0x80097D5C` | `BattleInitializePartyCombatStats` | Derive character/Gear combat statistics and AL limit |
| `0x8009A2D4` | `BattlePrepareGearTurnParameters` | Load costs, calculate Charge and Hyper, advance duration |
| `0x8009A7E4` | `BattleIsBillyAmmoReward` | Match a reward to Billy's equipped ammunition |
| `0x8009A854` | `BattleRegisterBillyAmmoReward` | Copy ammunition fields into persistent and live slots |
| `0x8009ADA0` | `BattleBuildPeriodicResourceChanges` | Build Fuel Leak and Booster fuel debits |
| `0x80086B88` | `BattleCanSelectGearAttackInput` | Validate Gear move unlock and AL tier |
| `0x80086F98` | `BattleAppendGearAttackInputAndBuildMoveHints` | Build Gear input sequence and choose move |
| `0x80083948` | `BattleHandleGearAttackSelection` | Validate and deduct attack fuel |
| `0x8009C4B4` | `BattlePostprocessGearAttackResult` | Update AL and HMP after a Gear attack |
| `0x8009C050` | `BattleGetGearConditionFlags` | Build critical/Hyper condition bits |
| `0x8009C0E0` | `BattleActivateGearHyperMode` | Enter direct three-turn Hyper state and set character 0's Gear-special availability bit |
| `0x8009D948` | `BattleComputeScaledAttackerPower` | Apply Gear ether scaling for Gear-target damage |
| `0x8009AA44` | `BattleApplyDefendStance` | Apply character Defend or Gear Charge stance |
| `0x8009AB00` | `BattleClearDefendStance` | Clear stance on the next command turn |
| `0x8009AB38` | `BattleRewriteAbilityTargetModes` | Rewrite eligible character or Gear ability target modes to `1` |
| `0x8009AC48` | `BattleRestoreAbilityTargetModes` | Restore rewritten target modes to `0x2000` and clear the modifier |
| `0x800830A8` | `BattleHandleGearBoosterCommand` | Toggle Booster speed state |
| `0x80082D4C` | `BattleHandleGearChargeCommand` | Apply guard, recover fuel, and clamp it |
| `0x80082F7C` | `BattleHandleGearEtherCommand` | Open and execute the Gear-side ability menu |
| `0x8008CDE4` | `BattleTryUseGearSpecial` | Validate option masks and deduct fuel on confirmation |
| `0x8008CFB8` | `BattleRunGearSpecialSelectionMenu` | Enumerate four Gear Special Options |
| `0x8008C81C` | `BattleDrawCircleMenuComboSelect` | Build a Combo within saved AP |
| `0x8008C4A8` | `BattleExecuteSelectedCombo` | Resolve the selected deathblow sequence |
| `0x8009A9D0` | `BattleAttemptEscape` | Roll shared 50-percent escape and synchronize state |
| `0x8009BE0C` | `BattlePersistPartyCombatState` | Persist character/Gear resources and Chu-Chu's character-ID-7 conversion |
