# Xenogears Field System

The Field system runs the explorable maps and scripted scenes in Xenogears. It
loads one map bundle, creates its actors, executes their routines, and coordinates
input, movement, camera control, dialogue, menus, encounters, and transitions.

This documentation describes the USA Disc 1 Field implementation. All function
names are descriptive names assigned by this project.

## Reading Order

1. [`01-concepts-and-lifecycle.md`](01-concepts-and-lifecycle.md) defines maps,
   entities, actors, routines, slots, and invocations, then follows one map from
   loading to teardown.
2. [`02-resource-and-script-formats.md`](02-resource-and-script-formats.md)
   documents the map container, compression, script section, variables, and
   operand encodings.
3. [`03-runtime-objects.md`](03-runtime-objects.md) documents `FieldActor`,
   `ActorData`, script slots, global VM state, and persistence.
4. [`04-scheduler-and-vm.md`](04-scheduler-and-vm.md) explains how routines are
   started, selected, dispatched, suspended, resumed, and completed.
5. [`05-primary-opcodes.md`](05-primary-opcodes.md) catalogs all 256 primary
   dispatch entries.
6. [`06-extended-opcodes.md`](06-extended-opcodes.md) catalogs all 227 extended
   `FE` entries.
7. [`07-frame-input-player-and-party.md`](07-frame-input-player-and-party.md)
   follows one ordinary frame through controller sampling, actor passes, player
   control, party identity, resource staging, following, and mount state.
8. [`08-walkmesh-movement-and-collision.md`](08-walkmesh-movement-and-collision.md)
   explains runtime walkmesh queries, planar movement, edge sliding, actor/model
   collision, moving platforms, altitude, jumping, and gravity.
9. [`09-camera-control.md`](09-camera-control.md) documents tracked actors,
   follow and script modes, orbit-sector policy, smoothing, walkmesh clamping,
   scripted motion, completion, and shake.
10. [`10-triggers-contact-and-dialogue.md`](10-triggers-contact-and-dialogue.md)
    documents trigger quadrilaterals, contact routines, window slots, dialogue
    modes, choice input, ownership, waiting, and cleanup.
11. [`11-encounters-transitions-loading-and-persistence.md`](11-encounters-transitions-loading-and-persistence.md)
    documents encounter selection, map installation, module handoffs, in-place
    menus, fresh versus restored entry, runtime serialization, and teardown.

The Field documents stop at the logical simulation boundary. Stored walkmesh
geometry is defined in the graphics resource-format chapter; this directory
describes only how Field consumes it for collision and camera policy.

## Coordinate Systems

| Value | Base |
|---|---|
| Resource section offset | Start of the map container |
| Routine offset | Start of the shared bytecode area |
| Jump or call target | Start of the shared bytecode area |
| Slot PC | Start of the shared bytecode area |
| Opcode operand offset | First physical byte of that instruction |
| Runtime structure offset | Start of that runtime object |

Unless stated otherwise, multibyte values are little-endian. VM variables are
addressed by byte offset even though each stored value is 16 bits.

The documented Field image is loaded at `0x8006FAF0` and has SHA-256:

```text
38a1ce829a6f094c505f67143d6ace2d328418c65425a7383991179467e1fdfc
```
