# Xenogears Battle System

The Battle system runs Xenogears' turn-based character and Gear encounters. An
encounter begins with a formation selected by Field, World Map travel, a Battle
Event, or setup state. Battle loads the arena and combatants, advances
readiness, resolves actions and statuses, presents the result, and returns the
updated party state to the resident game flow.

## Reading Order

1. [`01-concepts-and-lifecycle.md`](01-concepts-and-lifecycle.md) introduces
   encounter origins, combatant identities, startup gates, the frame and turn
   lifecycle, outcomes, persistence, and teardown ownership.
2. [`02-loading-resources-and-formations.md`](02-loading-resources-and-formations.md)
   follows Field and World Map encounter selection into the 32-byte formation,
   then follows enemy, arena, party, Event, and Result resources through staged
   loading.
3. [`03-combatants-turns-and-targeting.md`](03-combatants-turns-and-targeting.md)
    maps the shared `0x170`-byte live entity envelope and follows readiness
    countdowns, turn order, AP, character attacks, forced turns, and target
    masks.
4. [`04-actions-damage-and-status.md`](04-actions-damage-and-status.md) follows
   an action through hit testing, power, defence, elements, variance, resource
   changes, statuses, durations, reactions, and outcomes.
5. [`05-gear-combat-and-special-commands.md`](05-gear-combat-and-special-commands.md)
   covers calling Gears, Gear statistics and fuel, Attack Level, Hyper Mode,
   Charge, Booster, Ether Machine, Special Options, Combo, and
   character-specific commands.
6. [`06-enemy-ai-and-action-scripts.md`](06-enemy-ai-and-action-scripts.md)
    documents the four-byte enemy script language, its 114 native lower
    opcodes, 14 raw-action encodings, 28 condition opcodes, per-enemy variable
    banks, staged actions, and native action dispatch.
7. [`07-battle-event-vm.md`](07-battle-event-vm.md) specifies Battle Event
   resources, entities, slots, variables, operand encodings, scheduling,
   dialogue, media, sprites, and Battle handoff.
8. [`08-results-progression-and-persistence.md`](08-results-progression-and-persistence.md)
   documents reward eligibility, EXP distribution, both level tracks, stat
   growth, deathblow and ability unlocks, drops, inventory reconciliation,
   character and Gear persistence, and resident return routing.

## Encounter Origins

Field and World Map travel each maintain their own encounter schedule and
formation table. Field random encounters use map-local countdowns and a
16-entry weight row, while explicit Field instructions select a requested
battle directly. World Map movement advances a travel schedule, selects one of
four progress-sensitive weight rows for the current region, and records the
chosen formation in the resident Battle request state. Every route converges on
the same selected 32-byte formation consumed by the Battle loader.

## Conventions

| Value | Base or representation |
|---|---|
| Combatant identity | Slot `0..10`; party `0..2`, enemies `3..10` |
| Target identity | Bit `1 << combatant_slot` in a `u16` mask |
| Live entity offset | Start of that slot's `0x170`-byte record |
| Formation offset | Start of the selected 32-byte record |
| Enemy instruction PC | Pointer into the selected four-byte-record stream |
| Battle Event PC/jump | Offset from the Event shared-bytecode base |
| Battle Event variable | 16-bit value selected by an encoded byte offset |
| Fixed-point values | Scale stated with each field or formula |

Multibyte serialized and runtime values are little-endian unless a chapter
states a different encoding.
