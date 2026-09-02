# Battling RNG

## 1. Scope

This chapter indexes every explicit random roll in Battling, the colosseum
Gear-fighting minigame, from the generator's side: what each roll consumes,
and where its formula is already documented in full. All rolls in this
chapter draw from the shared gameplay generator described in
[`01-generators-and-determinism.md`](01-generators-and-determinism.md),
through Battling's own overlay-local `rand` wrapper calls. None of them touch
the Movie or sound-modulator generators.

Every formula in this chapter is quoted from, not re-derived from, the cited
`battling/` chapter; this document does not restate their surrounding
context. Purely cosmetic rolls (camera placement, particle jitter, projectile
trail effects, victory-camera orbit, replay-camera orbit, ambient sound
variety) are deliberately not indexed here — they don't affect a bout's
outcome and are covered by the blanket mention in
[`01` §2.4](01-generators-and-determinism.md#24-consumers-across-modules).

## 2. Tactical Randomization

| Roll | Formula | Source |
|---|---|---|
| Tactical flag randomization | Compares fresh random bytes against four tuning weights to set bits `0x0100`/`0x0200`/`0x0400`/`0x0800`, randomly picks orientation bit `0x1000`, and stores a random low byte in the packed halfword | [`battling/06` §3](../battling/06-ai-practice-and-rubber-band.md#3-com-state-layout) (`BattlingAiTacticalFlagsRandomize`, `0x8008F7B8`) |
| Attack approval | Weighted comparison combining tuning weights, heat state, and both fighters' HP; random comparisons use the low byte of the random value | [`battling/06` §6](../battling/06-ai-practice-and-rubber-band.md#6-attack-approval-probabilities) (`BattlingAiAttackDecisionEvaluate`, `0x8008F5B4`) |
| Engagement approval | Distance-gated random test against `decision_weight_0`, or against `decision_weight_3` when Heat risk is high | [`battling/06` §6](../battling/06-ai-practice-and-rubber-band.md#6-attack-approval-probabilities) (`BattlingAiEngagementDecisionEvaluate`, `0x8008F720`) |
| Terrain-dependent action suppression | On a terrain-class-1 opponent, `random_mod_4 != 0` (75%) suppresses the whole call; otherwise a jump and/or special-action command is queued | [`battling/06` §5.6](../battling/06-ai-practice-and-rubber-band.md#56-terrain-dependent-action-gate) (`BattlingAiQueueTerrainDependentActions`, `0x8008F900`) |

## 3. State Machine Rolls

| Roll | Formula | Source |
|---|---|---|
| Attack cadence refresh | `burst_delay = (2 - difficulty) * 20 + random(0..19) + 1` | [`battling/06` §4](../battling/06-ai-practice-and-rubber-band.md#4-difficulty) (`BattlingAiBasicBurstRefresh`, `0x8008FF24`) |
| Initial/refreshed basic burst | `random_mod_10` picks a burst count of 1 (20%), 2 (30%), or 3 (50%); each queued action is `random_mod_2 + 1` | [`battling/06` §5.3](../battling/06-ai-practice-and-rubber-band.md#53-attack-state) (`BattlingAiRandomBasicBurstQueue`, `0x8008FE80`) |
| Attack-state transition roll | Ten weighted random branches select Retreat, Idle, queuing a burst/special, or Approach | [`battling/06` §5.3](../battling/06-ai-practice-and-rubber-band.md#53-attack-state) |
| Approach random heading | Closes distance with a randomized heading around `-0x300` | [`battling/06` §5.4](../battling/06-ai-practice-and-rubber-band.md#54-approach-state) |
| Approach extra-burst permission (Hard) | `random_byte < decision_weight_1` | [`battling/06` §5.4](../battling/06-ai-practice-and-rubber-band.md#54-approach-state) |
| Retreat entry | `target_distance = random(0..0x5FF) + 0x100`; `repetitions = random(0..4) + 3` | [`battling/06` §5.5](../battling/06-ai-practice-and-rubber-band.md#55-retreat-state) (`BattlingComAiRetreatStateEnter`, `0x80090894`) |

## 4. Practice Behaviors

| Roll | Formula | Source |
|---|---|---|
| RUN AWAY heading and timer | `heading = random(0..1535) + 1280`; `countdown = random(0..49) + 10` | [`battling/06` §8.1](../battling/06-ai-practice-and-rubber-band.md#81-give-chase-and-run-away) (`BattlingPracticeNearRangeMovementUpdate`, `0x8008F094`) |
| GIVE CHASE heading and timer | `heading = random(0..1535) - 768`; `countdown = random(0..119) + 10` | [`battling/06` §8.1](../battling/06-ai-practice-and-rubber-band.md#81-give-chase-and-run-away) (`BattlingPracticeFarRangeMovementUpdate`, `0x8008F17C`) |
| UP AND AT'EM / SLOWPOKE burst queue | Same `random_mod_10` burst-count draw as §3's basic burst, plus a short/long delay choice | [`battling/06` §8](../battling/06-ai-practice-and-rubber-band.md#8-practice-behaviors) (`BattlingPracticeRandomBasicActionsQueue`, `0x8008EF30`) |

## 5. Scripted-Scenario Climax

| Roll | Formula | Source |
|---|---|---|
| Attachment-point emitter rearm | Rearms after `8..23` updates; emits effect `9` on odd timer values, with additional random passes emitting effects `0x0B` and `8` | [`battling/06` §10](../battling/06-ai-practice-and-rubber-band.md#10-scripted-scenario-climax) (`BattlingUpdateScriptedScenarioClimax`, `0x80072170`) |

## 6. Not Indexed: Confirmed Non-Random

`BattlingStartRandomArenaReentryMotion` (`0x80070FD8`) — despite its name and
despite `battling/08`'s opcode table previously calling its result a
"randomized inward heading" — contains no call to `rand` or any wrapper. It
picks between two geometrically computed headings (`ratan2` toward the arena
center, or a shared side-to-side heading global) based on a fixed threshold.
See [`battling/08` §4](../battling/08-tutorial-and-attract-scripts.md#4-wait-and-movement-operations)
for the corrected description.

## 7. Function Index

| Address | Function |
|---|---|
| `0x8008F5B4` | `BattlingAiAttackDecisionEvaluate` |
| `0x8008F720` | `BattlingAiEngagementDecisionEvaluate` |
| `0x8008F7B8` | `BattlingAiTacticalFlagsRandomize` |
| `0x8008F900` | `BattlingAiQueueTerrainDependentActions` |
| `0x8008FE80` | `BattlingAiRandomBasicBurstQueue` |
| `0x8008FF24` | `BattlingAiBasicBurstRefresh` |
| `0x80090894` | `BattlingComAiRetreatStateEnter` |
| `0x8008F094` | `BattlingPracticeNearRangeMovementUpdate` |
| `0x8008F17C` | `BattlingPracticeFarRangeMovementUpdate` |
| `0x8008EF30` | `BattlingPracticeRandomBasicActionsQueue` |
| `0x80072170` | `BattlingUpdateScriptedScenarioClimax` |
| `0x80070FD8` | `BattlingStartRandomArenaReentryMotion` (non-random, see §6) |
