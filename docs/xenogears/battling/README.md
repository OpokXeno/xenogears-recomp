# Xenogears Battling System

Battling is Xenogears' real-time Gear arena system. It presents Gear selection,
one-player, two-player, and COM-controlled series, Practice, Tutorial, automatic
demonstrations, continuous movement and collision, animation-driven attacks,
replays, results, and the persistent opponent checklist.

Battling is a resident game state separate from the turn-based Battle system.
It owns two fighter runtimes, a fixed arena heightfield, its menus, simulation,
COM behavior, tutorial bytecode, and result handoff to Field.

## Reading Order

1. [`01-concepts-and-lifecycle.md`](01-concepts-and-lifecycle.md) defines
   fighters, activities, states, bouts, rounds, and series, then follows the
   subsystem from resident entry through menus, simulation, results, and exit.
2. [`02-resources-roster-and-fighter-data.md`](02-resources-roster-and-fighter-data.md)
   documents common archives, all 49 Gear packages, roster filtering, portraits,
   terrain, fighter hierarchies, configuration, animation events, and resource
   ownership.
3. [`03-modes-menus-options-and-input.md`](03-modes-menus-options-and-input.md)
   documents the menu graph, activities, Gear and controller selection,
   difficulty, match count, motion and frame-rate settings, pause menus,
   persistence, and menu input.
4. [`04-fighter-runtime-movement-and-terrain.md`](04-fighter-runtime-movement-and-terrain.md)
   maps the fighter runtime and follows controller input, acceleration, facing,
   terrain sampling, arena constraints, landing, and fighter separation.
5. [`05-actions-collision-health-and-heat.md`](05-actions-collision-health-and-heat.md)
   follows commands through combos, animation events, swept attack volumes,
   projectiles, guard, boost, heat overflow, damage, reactions, and knockout.
6. [`06-ai-practice-and-rubber-band.md`](06-ai-practice-and-rubber-band.md)
   documents COM state, difficulty, tactical decisions, Practice behaviors,
   rubber-band attraction, and the scripted climax.
7. [`07-bouts-replay-results-and-progression.md`](07-bouts-replay-results-and-progression.md)
   documents opening phases, knockout and draw handling, circular replay,
   victory presentation, exported result codes, the opponent checklist,
   Argento completion, and the resident option word.
8. [`08-tutorial-and-attract-scripts.md`](08-tutorial-and-attract-scripts.md)
   specifies the `00..22` script language, tutorial selection, dialog, camera
   and placement controls, scripted-scenario climax, and automatic
   demonstrations.

## Conventions

| Value | Base or representation |
|---|---|
| Fighter identity | Side `0` or `1`; runtime pointers identify the side during simulation |
| Gear identity | ID `0..48`; its canonical directory `0x31` package is file ID `Gear ID + 2`, with five identities resolved to shared package `0` or `1` during paired loading |
| Combo identity | Selection `0..14` in the shared Triangle/Circle tree |
| Action identity | ID `0..47` in fighter animation and event tables |
| Angle | 12-bit circle; `0x1000` is one revolution |
| Terrain coordinate | Eight fractional bits; cell index uses coordinate `>> 8` |
| Motion scale | `0x100` is neutral unless stated otherwise |
| Transform scale | `0x1000` is 1.0 |
| Health fraction | Usually an integer numerator over `0xFF` |
| Heat limit | `0x1000` |

Multibyte serialized and runtime values are little-endian. Integer division
truncates toward zero unless a formula states an explicit rounding rule.
