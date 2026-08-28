# Runtime Objects

## 1. Object Graph

Loading a map creates one compact `FieldActor` record per entity. Scriptable
actors also own an `ActorData` state object:

```text
FieldActor[entity_id]
    +0x00 -> model descriptor, when modeled
    +0x04 -> sprite object, when sprite-based
    +0x08 -> shadow quad
    +0x0C    current transform
    +0x2C    previous/cached transform
    +0x4C -> ActorData
```

`FieldActor` is used to locate render resources and transforms quickly.
`ActorData` contains the state changed by scripts and simulation.

## 2. FieldActor

Each `FieldActor` is `0x5C` bytes:

| Offset | Type | Meaning |
|---:|---|---|
| `0x00` | pointer | Model descriptor for a modeled entity |
| `0x04` | pointer | Sprite object |
| `0x08` | pointer | Actor-owned shadow quad and its two GPU packets |
| `0x0C` | `MATRIX` | Current model transform |
| `0x2C` | `MATRIX` | Previous or cached model transform |
| `0x4C` | `ActorData *` | Physical, graphical, dialogue, and VM state |
| `0x50` | `s16` | Model rotation X |
| `0x52` | `s16` | Model rotation Y |
| `0x54` | `s16` | Model rotation Z |
| `0x56` | `u16` | Rotation-vector padding/storage |
| `0x58` | `u16` | Runtime class, activation, visibility, and visual flags |
| `0x5A` | `u16` | Sprite ownership and replacement state |

Matrix translation components are at `+0x20/+0x24/+0x28` in the current matrix
and `+0x40/+0x44/+0x48` in the cached matrix.

Important `+0x58` masks are:

| Mask | Meaning |
|---:|---|
| `0x0020` | Actor hidden or visually suppressed |
| `0x0040` | Sprite-class visual entity |
| `0x0F00` | Runtime class bits required by the script scheduler |
| `0x2000` | Actor has dynamic model deformation storage |

## 3. ActorData

Each `ActorData` is `0x138` bytes. It combines several subsystems because Field
updates movement, collision, graphics, dialogue, and scripts through the same
actor identity.

### Physical and movement state

| Offset | Type | Meaning |
|---:|---|---|
| `0x000` | `u32` | Primary actor and script flags |
| `0x004` | `u32` | Physical, graphical, collision, attachment, and animation flags |
| `0x008` | `s16[4]` | Current triangle on each walkmesh layer |
| `0x010` | `s16` | Active walkmesh layer |
| `0x012` | `u16` | Walkmesh traversal scratch |
| `0x014` | `u32` | Current triangle material flags |
| `0x018` | `u16` | Interaction/collision half-width X |
| `0x01A` | `u16` | Interaction/collision height |
| `0x01C` | `u16` | Interaction/collision half-width Z |
| `0x01E` | `u16` | Solid/contact range |
| `0x020` | `VECTOR` | Fixed-point physical position |
| `0x030` | `VECTOR` | Physical delta for the current update |
| `0x040` | `VECTOR` | Accumulated or pending movement |
| `0x050` | `VECTOR` | Active surface normal |
| `0x060` | `SVECTOR` | Local movement or transition offset |
| `0x068` | `s16[3]` | Previous integer position |
| `0x06E` | `u16` | No-progress/contact response counter |
| `0x070` | `s16` | Object swivel angle |
| `0x072` | `s16` | Actor elevation anchor |
| `0x074` | `u8` | Actor currently touched/connected |
| `0x075` | `u8` | Parent actor; `0xFF` means none |
| `0x076` | `u16` | Movement speed |

Primary flag `0x00800000` suppresses automatic contact routine 3. It does not
disable explicit interaction routine 2. Primary bit 0 prevents ordinary VM
dispatch for that actor. Movement handlers use additional primary flags as
operation latches.

### VM and dialogue state

| Offset | Type | Meaning |
|---:|---|---|
| `0x078` | `u16[4]` | VM return-PC stack |
| `0x080` | `s8` | Dialogue portrait/face ID; `-1` means none |
| `0x081` | `s8` | Selected multichoice line |
| `0x082` | `u8` | Dialogue width |
| `0x083` | `u8` | Dialogue height |
| `0x084` | `u32` | Dialogue window flags and mode |
| `0x088` | `s16[2]` | Forced dialogue window position |
| `0x08C` | `ScriptSlot[8]` | Eight cooperative invocation records |
| `0x0CC` | `u16` | Working PC of the slot currently executing |
| `0x0CE` | `u8` | Selected local slot index |
| `0x0CF` | `u8` | Slot index in a remote actor awaited by a blocking start |

The return stack is shared by the actor's currently dispatched invocation. The
active depth is stored in `ActorData+0x12C` bits 6 through 8.

### Animation, visual, and effect state

| Offset | Type | Meaning |
|---:|---|---|
| `0x0D0` | `s32[4]` | XYZ step toward a scripted target plus reserved fourth word |
| `0x0E0` | `s16` | Angular movement limit/step |
| `0x0E2` | `u8` | Door-state step |
| `0x0E3` | `u8` | Push/contact response timer |
| `0x0E4` | `s16` | Associated playable-character ID |
| `0x0E6` | `s16` | Default animation ID |
| `0x0E8` | `s16` | Current visual animation ID |
| `0x0EA` | `s16` | Forced animation ID; `0xFF` releases it |
| `0x0EC` | `s16` | Cached elevation |
| `0x0EE` | `s16` | Secondary visual/shadow vertical offset |
| `0x0F0` | `s32` | Fixed-point movement/contact response accumulator |
| `0x0F4` | `s16` | Actor scale X |
| `0x0F6` | `s16` | Actor scale Y |
| `0x0F8` | `s16` | Actor scale Z |
| `0x0FA` | `u16` | Scale subsystem scratch |
| `0x0FC` | `u8[3]` | Primary actor RGB color |
| `0x0FF` | `u8[3]` | Secondary actor RGB color |
| `0x102` | `s16` | Movement phase/remaining update counter |
| `0x104` | `s16` | Target rotation |
| `0x106` | `s16` | Current physical rotation |
| `0x108` | `s16` | Rotation applied to sprite/rendering |
| `0x10A` | `s16` | Positional sound ID |
| `0x10C` | `u8` | Positional sound volume |
| `0x10D` | `u8` | Positional sound mode; `0xFF` is inactive |
| `0x10E` | `u16` | Alignment storage |

### Owned allocations and packed state

| Offset | Type | Meaning |
|---:|---|---|
| `0x110` | pointer | Twelve-byte attachment/platform snapshot |
| `0x114` | pointer | Four XZ vertices of a scripted movement boundary |
| `0x118` | pointer | Dynamic model-deformation intensities |
| `0x11C` | `s16` | Heading saved by a movement override |
| `0x11E` | `s16` | Rotation interpolation step; initialized to `0x200` |
| `0x120` | pointer | Loaded special-animation buffer |
| `0x124` | `s16` | Special-animation file ID; `-1` means none |
| `0x126` | `u8` | Graphic package selector and resource-family bit |
| `0x127` | `u8` | Sprite/graphic variant ID |
| `0x128` | `u16` | Packed model-animation parameter |
| `0x12A` | `u16` | Graphics subsystem scratch |
| `0x12C` | `u32` | VM depth, swivel mode, attachment, mecha, dialogue, and ownership bits |
| `0x130` | `u32` | Restorable sprite/CLUT configuration |
| `0x134` | `u32` | Graphics allocation ownership and render configuration |

Relevant `+0x12C` bit groups are:

| Bits | Meaning |
|---:|---|
| 0..1 | Object swivel axis: X, Y, or Z |
| 2..4 | Dialogue portrait cache slot |
| 5 | Door operation active |
| 6..8 | VM call depth |
| 9..11 | Contact/interaction direction |
| 12 | Ownership of movement-boundary allocation |
| 13..15 | Mecha index |
| 16..17 | Restorable graphic mode |
| 18..27 | Graphics and movement operation latches |
| 28..31 | Actor state preserved across ordinary reinitialization |

Bits 18 through 27 are not one shared enumeration. They are independent latches
owned by specific movement and graphics handlers.

## 4. ScriptSlot

The eight slots occupy `ActorData+0x8C..0xCB`. Each slot is exactly eight bytes:

| Offset | Type | Meaning |
|---:|---|---|
| `+0x00` | `u16` | Saved PC; initialized to `0xFFFF` |
| `+0x02` | `u8` | Timed-wait counter |
| `+0x03` | `u8` | Routine ID; `0xFF` after release |
| `+0x04` bits 0..15 | 16 bits | Handler-owned remaining-step counter |
| `+0x04` bits 16..17 | 2 bits | Blocking-operation state |
| `+0x04` bits 18..21 | 4 bits | Scheduler priority; `0xF` is inactive |
| `+0x04` bit 22 | 1 bit | Protected remote invocation awaited by another actor |
| `+0x04` bits 23..24 | 2 bits | Walk/movement handler mode |
| `+0x04` bits 25..31 | 7 bits | Independent handler latches |

A slot can be allocated when its priority is `0xF` and it is not protected by a
blocking caller. The scheduler selects the lowest priority value; equal values
select the later slot.

## 5. Global Runtime State

| Address | Meaning |
|---:|---|
| `0x800ADBF8` | Current decompressed `ScriptsFile` |
| `0x800ADBFC` | Routine-row count |
| `0x800ADC00` | Shared bytecode base |
| `0x800AFB0C` | Runtime entity count read from the map header |
| `0x800AFB10` | `FieldActor[]` base |
| `0x800B0078` | Current `ActorData` context |
| `0x800C3A68` | Active 1024-value VM memory |
| `0x8006EF64` | Persistent 512-value Field state inside game state |

`0x800ADBFC` and `0x800AFB0C` are deliberately separate. The former controls
script-row traversal; the latter controls initial actor-array allocation.

## 6. Ownership And Teardown

| Resource | Owner | Release path |
|---|---|---|
| `FieldActor[]` | Loaded map | Field teardown |
| Model descriptor | Modeled actor | Actor/map teardown |
| Sprite object | Actor when ownership bit is set | Actor resource release |
| Shadow quad | Actor | Actor resource release |
| `ActorData` | Actor | Actor resource release |
| Attachment snapshot | ActorData ownership bit | Actor resource release |
| Movement boundary | ActorData ownership bit | Boundary-free opcode or actor release |
| Deformation storage | Deformable actor | Actor resource release |
| Special animation | ActorData while file ID is valid | Special-animation free or actor release |

## 7. Persistence Boundary

The lower 512 VM values, selected map/player/camera state, and selected actor
state are copied into persistent or serialized storage. A complete live Field
state additionally depends on:

- The eight slots and their PCs.
- Current working PC and call stack.
- Loaded model, sprite, animation, and audio resources.
- Active dialogue and menu state.
- Camera and effect interpolations.
- Archive and streaming I/O phase.
- Current transition state.

The 512 persistent variables alone are therefore not a VM snapshot.
