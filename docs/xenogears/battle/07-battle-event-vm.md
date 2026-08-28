# Battle Event VM

## 1. Identity And Boundary

Battle Event is the VM and resource family that runs scripted
scenes around and during turn-based battles. It cooperates with resident Battle
systems for rendering, combatants, dialogue, audio, and module transitions.

`BattleEventUpdate` at `0x801E879C` runs the scheduler. Its dispatch range is
`0x00..0x4B`, comprising 76 entries.

## 2. VM Components

The overlay owns the Event entity scheduler, bytecode dispatch, dialogue state,
Event-created sprites, and Event audio allocations. Opcode handlers call
resident Battle functions for camera motion, faders, combatant models, Gear
turn UI, animation cleanup, and indexed audio playback.

## 3. Disc Resource Family

Directory `0x20` groups the Event program with three data resources:

| File | Runtime role |
|---:|---|
| `0x02` | Relocated pointer table of compressed event-script and dialogue pairs. |
| `0x03` | Relocated resource/offset table used for Event-created sprite bundles. |
| `0x04` | Event sequence bank loaded during initialization. |

`BattleEventInitialize` (`0x801E5160`) loads files 2 and 3 together. It applies
the common pointer-relocation routine to each and selects two adjacent entries
from file 2:

```text
script_resource = decompress(file2[event_data_index * 2 + 0])
dialog_resource = decompress(file2[event_data_index * 2 + 1])
```

`event_data_index` is the selected Battle configuration byte at `0x8006F9DF`.
The compressed script and dialogue blocks are independent allocations.

File 3 is retained for opcode `0x35`. That handler reads a 32-bit offset from
`file3 + 4 + resource_index * 4`, follows it relative to the relocated file,
and decompresses the selected sprite animation bundle.

## 4. Script Resource Format

The Event VM reads this script-resource layout:

```text
+0x00  0x40-byte resource-container prefix
+0x40  u32                         entity_count
+0x44  entity_count * 0x10 bytes   eight u16 entry offsets per entity
...                                shared bytecode
```

The shared bytecode base is:

```text
bytecode_base = resource + 0x44 + entity_count * 0x10
entry(entity, entry_id) = bytecode_base +
                          u16(resource + 0x44 + entity * 0x10 + entry_id * 2)
```

The runtime has storage for 16 entities, while initialization consumes the
resource's `entity_count` without clamping it. Counts above 16 continue writing
past the entity storage. Each row physically contains eight entries. Packed
start operands use all five low bits as the entry tag and entry-table index;
indices `8..31` continue reading beyond that entity's eight-entry row.

Entry offsets, slot PCs, and jumps are unsigned 16-bit coordinates relative to
the shared bytecode base. Each entry is an independent offset into that shared
bytecode stream. Entry values and PCs are loaded as `u16`; for example,
`0xFFFF` fetches bytecode offset `65535`.

## 5. Dialogue Resource Format

The selected dialogue block uses the common message-bundle resolver:

```text
+0x00  u16 highest_message_id
+0x02  u16 zero
+0x04  u16 payload_offset[]
...    encoded dialogue payloads
```

The resolver returns:

```text
dialog_resource + u16(dialog_resource + 4 + message_id * 2)
```

The low half of the header stores the highest message index and the high half is
zero. Opcode message IDs select entries through the common resolver.

## 6. Runtime Allocation

`BattleEventInitialize` allocates and zeroes `0x828` bytes. Its script-relevant
layout is:

| Offset | Size | Meaning |
|---:|---:|---|
| `0x000` | `16 * 0x38` | Event script entities. |
| `0x380` | `8 * 2` | `decodedOperands`, the current decoded values. |
| `0x390` | 4 | Pointer to shared bytecode. |
| `0x394` | `512 * 2` | Script-defined signed 16-bit variable bank. |
| `0x794` | `0x10` | `u8[16]` entity execution-order array; `0xFF` marks unused entries. |
| `0x7A4` | `2 * 0x28` | Double-buffered portrait polygons. |
| `0x7F4` | 1 | Portrait packet parity. |
| `0x7F5` | 1 | Dialogue setup state initialized to 4. |
| `0x7F6` | `0x0A` | Dialogue X, Y, width, height, and flags. |
| `0x800` | 1 | Stop/end-event execution flag. |
| `0x801` | 1 | Pause/pass-control state. |
| `0x802` | 1 | Dialogue state initialized. |
| `0x803` | 1 | Alignment before the per-entity completion array. |
| `0x804` | `0x10` | Per-visual-entity asynchronous completion flags. |
| `0x814` | 4 | Loaded Event music pointer. |
| `0x818` | 4 | File-4 sequence bank pointer. |
| `0x81C` | 2 | Loaded music ID. |
| `0x81E` | 1 | Stored loaded-music volume. |
| `0x81F` | 1 | Event music allocation active. |
| `0x820` | 1 | File-4 sequence allocation active. |
| `0x821` | 7 | Tail alignment completing the `0x828`-byte allocation. |

The variable bank is zeroed with the allocation and belongs to the active Event
script. Event bytecode assigns the content-facing meanings. Resident Battle
code injects these values at lifecycle boundaries:

| Index | Semantic name | Engine write |
|---:|---|---|
| `0` | `battleExitSignal` | Set to `0x00FF` when the central exit route reaches the Event handoff. |
| `16` | `party0ExitStatus` | `0x0000` or `0x8000`: merged bit 15 from party slot 0's live character and Gear status words. |
| `17` | `party1ExitStatus` | `0x0000` or `0x8000`: merged bit 15 from party slot 1's live character and Gear status words. |
| `18` | `party2ExitStatus` | `0x0000` or `0x8000`: merged bit 15 from party slot 2's live character and Gear status words. |

Enemy opcode `0x70` uses an 8-bit element index and can inject a value into
Event variable indices `0..255`. All other variable names remain properties of
each Event script.

## 7. Entity And Slot Layout

Each Event entity is `0x38` bytes:

```c
struct BattleEventEntity {          /* size 0x38 */
    uint16_t pc[8];                  /* +0x00 */
    int8_t  slot_state[8];           /* +0x10 */
    int8_t  entry_tag[8];            /* +0x18 */
    int8_t  current_slot;             /* +0x20 */
    int8_t  current_entry_tag;        /* +0x21 */
    int8_t  entity_priority;          /* +0x22 */
    int8_t  awaited_entry_tag;        /* +0x23 */
    int8_t  portrait_id;              /* +0x24 */
    uint8_t wait_alignment;            /* +0x25 */
    int16_t wait_frames_x2;           /* +0x26 */
    int8_t  wait_active;              /* +0x28 */
    uint8_t sprite_pointer_alignment[3]; /* +0x29 */
    void   *sprite_bundle;             /* +0x2C */
    void   *sprite_task;               /* +0x30 */
    int8_t  mecha_async_state;         /* +0x34 */
    int8_t  sprite_alive;              /* +0x35 */
    uint8_t tail_alignment[2];          /* +0x36 */
};
```

At initialization:

- All PCs are `0xFFFF`, and all tags are `-1`.
- Slot 0 becomes active with state 0, entry tag 0, and entry point 0.
- `entity_priority`, awaited tag, and portrait are `-1`.
- The execution-order array contains entity IDs `0..entity_count-1`; remaining
  entries stay `0xFF`.

`BattleEventGetBytecodeOffset` (`0x801E5768`) scans slots from 0 through 7 and
selects the highest-numbered active slot. It records that slot and tag as
current. Slot selection uses active state plus descending slot index; the
three-bit packed state is retained as entry metadata.

## 8. Entity Identifier Spaces

The VM uses three related identifier forms.

### Script entity ID

Opcodes `0x03..0x05` map their byte directly to the Event entity array without
clamping it to the 16 allocated entities.

### Packed entry byte

```text
bits 0..4 = entry tag/index
bits 5..7 = initial secondary-slot state
```

The low five bits become the caller's awaited tag and directly select an entry
relative to the target entity's eight-value entry row. The high three bits are
stored in the target slot.

### Battle visual character ID

`BattleEventResolveEntityIndex` (`0x801E5A98`) maps character-oriented operands:

| Input | Resolution |
|---:|---|
| `< 0x10` | Search the three active party character IDs; return matching party slot `0..2`. |
| `< 0x10`, fallback | Return slot 0. |
| `>= 0x10` | Return `input - 0x0D`; therefore `0x10` maps to enemy slot 3. |

Portrait opcode `0x1B` remaps every operand in `0xF3..0xFF` by reading byte
`operand - 0xF3` from the resident party-ID array base. Operands `0xF3..0xF5`
therefore select the three current party character IDs; `0xF6..0xFF` read the
resident bytes immediately following that three-byte array.

Event sprite and mecha actor aliases use `value - 0xF3` to select an Event
entity. Mecha target aliases use a separate Battle-combatant space:
`(value + 0x0D) & 0xFF` becomes the bit index in a resident combatant mask.
Thus `0xF3`, `0xF4`, and `0xF5` select resident combatant bits 0, 1, and 2 as
mecha targets rather than Event entities.

## 9. Variable And Operand Encoding

Variables are 512 signed 16-bit values. Direct destinations encode a `u16` byte
offset; bit zero is discarded:

```text
variable_index = (raw_offset & 0xFFFE) / 2
```

There are two value encodings.

### v15

Most Event control, media, sprite, and camera operations use:

```text
15              0
+---------------+
|I|   payload   |
+---------------+

I = 1: immediate value payload & 0x7FFF
I = 0: payload & 0x7FFE is a variable byte offset
```

The decoder is `BattleEventDecodeOperands` at `0x801E57F8` with mode 1.

### Type-mask operands

Conditional and arithmetic operations put two-byte operands first and a mask
byte last:

```text
operand     1  2  3  4  5  6  7  8
mask bit   80 40 20 10 08 04 02 01
```

A set bit means the corresponding `u16` is an immediate. A clear bit means it
is a variable byte offset. For opcode `0x02`, the same byte's low nibble is the
comparison selector.

The comparison function `BattleEventEvaluateCondition` (`0x801E58EC`) receives
semantic `leftValue` and `rightValue` operands:

| Low nibble | Test |
|---:|---|
| `0` | `leftValue == rightValue` |
| `1` | `leftValue != rightValue` |
| `2` | `(s16)rightValue < (s16)leftValue` |
| `3` | `(s16)leftValue < (s16)rightValue` |
| `4` | `(s16)rightValue <= (s16)leftValue` |
| `5` | `(s16)leftValue <= (s16)rightValue` |
| `6` | `(leftValue & rightValue) != 0` |
| `7` | `leftValue != rightValue` |
| `8` | `leftValue != 0 \|\| rightValue != 0` |
| `9` | Battle entity `rightValue` is present in target mask `leftValue` |
| `A` | Battle entity `rightValue` is absent from target mask `leftValue` |

Selectors `0xB..0xF` return false.

## 10. Scheduler

`BattleEventUpdate` is cooperative but unusual: one call may execute many
passes and render many Battle frames before returning.

```text
wait for animation sound loading
update character blinking

if stop_flag == 0:
    repeat passes:
        for order_index in 0 .. entity_count-1:
            entity = execution_order[order_index]
            render/update one Battle frame
            dispatch up to 4 instructions for that entity
        apply pause/pass-control state
```

For each dispatch:

1. Select the entity's highest active slot.
2. Fetch `bytecode[pc & 0xFFFF]`.
3. Route values `0x00..0x4B` through the dispatch table. Larger values skip the
   handler and reach the common PC update with the stale `s0` delta from the
   previous dispatch, or the caller-provided register value on the first
   dispatch.
4. Call the selected handler.
5. Add the handler's returned signed delta to the slot selected *after* the
   handler. Opcode `0x00` resets to slot 0 and resumes from its current PC.

The normal budget is four handlers per entity per pass. Opcode `0x00` forces
the remaining budget to end. A zero return suppresses only the scheduler's
post-handler PC increment. Polling handlers therefore remain in place, while
opcodes `0x00`, `0x01`, and the false path of `0x02` can replace the slot or PC
before returning zero.

Opcode `0x22` sets pass-control byte `+0x801` to 2. At the end of the pass, the
scheduler decrements it and returns when it becomes 1. This yields control to
the surrounding Event/battle transition after the current pass.

Opcode `0x2C` stores an entity priority byte. Value `-2` moves the current
entity to the front of the 16-byte
execution-order array while preserving all other entries' order.

## 11. Starting, Waiting, And Ending Scripts

`BattleEventFindFreeScriptSlot` (`0x801E57C4`) searches target slots `1..7` and
returns 8 when none is free.

| Opcode | Start/wait behavior |
|---:|---|
| `03` | Start in a free secondary slot; advance only if allocation succeeds. |
| `04` | Start once, then wait until the target reports the awaited entry as its current entry. |
| `05` | Start once, then wait for the awaited tag to leave the target's slots and current tag. |

The caller stores the low-five-bit entry tag in `awaited_entry_tag`; waiting
state retains the tag alone and is therefore tag-based.

Opcode `0x00` releases the selected slot, then resets that entity's primary slot
to entry 1 with slot state 7 and entry tag 1. `EndScript` installs the entity's
entry-1 program as its new primary script, normally an idle or wait loop.

## 12. Complete Dispatch Table

`Bytes` is the encoded instruction length. For polling instructions, the handler
returns zero while incomplete and returns the listed length only on completion.
Every value `0x00..0x4B` has a dispatch entry.
`0x32` and `0x34` are the two intentional one-byte no-ops.

| Op | Bytes | Handler | Function | Operands and behavior |
|---:|---:|---:|---|---|
| `00` | 1 logical | `0x801E5C1C` | `BattleEventOpcode00EndScript` | Release current slot, reset primary slot to entry 1, and end this entity's dispatch budget. |
| `01` | 3 | `0x801E5CE4` | `BattleEventOpcode01Jump` | Set the current slot PC to absolute `targetOffset:u16`. |
| `02` | 8 | `0x801E5D24` | `BattleEventOpcode02JumpUnless` | Evaluate typed `leftValue` and `rightValue`; continue on true or jump to `falseTarget:u16`. |
| `03` | 3 | `0x801E5DCC` | `BattleEventOpcode03StartScript` | Start `packedEntry` on `targetEntity` in a free secondary slot; poll while all seven slots are occupied. |
| `04` | 3 | `0x801E5EF8` | `BattleEventOpcode04StartScriptAndWaitForEntry` | Start `packedEntry` on `targetEntity`, then wait for its current-entry acknowledgement. |
| `05` | 3 | `0x801E5F8C` | `BattleEventOpcode05StartScriptAndWaitForCompletion` | Start `packedEntry` on `targetEntity`, then wait for the entry tag to leave the target. |
| `06` | 6 | `0x801E6084` | `BattleEventOpcode06SetVariable` | Store typed `sourceValue` in direct `destination`; the type mask is at `+5`. |
| `07` | 3 | `0x801E60E8` | `BattleEventOpcode07SetVariableTrue` | Store 1 in direct `destination`. |
| `08` | 3 | `0x801E6118` | `BattleEventOpcode08SetVariableFalse` | Store 0 in direct `destination`. |
| `09` | 6 | `0x801E6144` | `BattleEventOpcode09AddVariable` | Add typed `addend` to direct `destination`. |
| `0A` | 6 | `0x801E61B4` | `BattleEventOpcode0ASubtractVariable` | Subtract typed `subtrahend` from direct `destination`. |
| `0B` | 6 | `0x801E6224` | `BattleEventOpcode0BOrVariable` | Apply `destination \|= bitMask`. |
| `0C` | 6 | `0x801E6294` | `BattleEventOpcode0CClearVariableBits` | Apply `destination &= ~bitMask`. |
| `0D` | 3 | `0x801E6304` | `BattleEventOpcode0DIncrementVariable` | Increment direct `destination` with 16-bit wrapping. |
| `0E` | 3 | `0x801E633C` | `BattleEventOpcode0EDecrementVariable` | Decrement direct `destination` with 16-bit wrapping. |
| `0F` | 6 | `0x801E6374` | `BattleEventOpcode0FAndVariable` | Apply `destination &= bitMask`. |
| `10` | 6 | `0x801E63E4` | `BattleEventOpcode10OrVariable` | Apply `destination \|= bitMask`. |
| `11` | 6 | `0x801E6454` | `BattleEventOpcode11XorVariable` | Apply `destination ^= bitMask`. |
| `12` | 5 | `0x801E64C4` | `BattleEventOpcode12ShiftVariableLeft` | Shift direct `destination` left by `shiftCountVariable & 31`. |
| `13` | 5 | `0x801E6534` | `BattleEventOpcode13ShiftVariableRight` | Logically shift direct `destination` right by `shiftCountVariable & 31`. |
| `14` | 3 | `0x801E65A4` | `BattleEventOpcode14RandomVariable` | Store a random value in `0..0x7FFF` in direct `destination`. |
| `15` | 5 | `0x801E65FC` | `BattleEventOpcode15RandomRangeVariable` | Store a random value selected with fixed `upperBound:u16` in direct `destination`. |
| `16` | 6 | `0x801E6660` | `BattleEventOpcode16MultiplyVariable` | Store the low 16 bits of typed `leftValue * rightValue` in the first field's direct `destination`. |
| `17` | 6 | `0x801E66D8` | `BattleEventOpcode17DivideVariable` | Store `dividend / divisor` in the first field's direct `destination`; divisor zero stores `0xFFFF`. |
| `18` | 4 | `0x801E71D4` | `BattleEventOpcode18ShowDialog` | Display blocking `messageId:u16` with `dialogFlags` and the current entity's portrait. |
| `19` | 5 | `0x801E7230` | `BattleEventOpcode19ShowDialogWithPortrait` | Display blocking `messageId:u16` with explicit `portraitId` and `dialogFlags`. |
| `1A` | 11 | `0x801E7278` | `BattleEventOpcode1ASetDialogParameters` | Set v15 `x`, `y`, `width`, `height`, and `dialogFlags`; zero selects each geometry default. |
| `1B` | 2 | `0x801E7314` | `BattleEventOpcode1BSetPortrait` | Set current entity `portraitId`; operands `F3..FF` index bytes from the resident party-ID array base, with `F3..F5` selecting the three active party IDs. |
| `1C` | 1 | `0x801E7358` | `BattleEventOpcode1CSetCameraMode2` | Set central Battle mode to 2. |
| `1D` | 1 | `0x801E736C` | `BattleEventOpcode1DSetCameraMode1` | Set central Battle mode to 1. |
| `1E` | 3 | `0x801E7380` | `BattleEventOpcode1EFadeToWhite` | Create or retarget a white Battle fader for v15 `durationFrames`. |
| `1F` | 3 | `0x801E73D4` | `BattleEventOpcode1FFadeToBlack` | Create or retarget a black Battle fader for v15 `durationFrames`. |
| `20` | 1 | `0x801E746C` | `BattleEventOpcode20StopForFinalHandoff` | Allow the final Event handoff and stop the Event VM; central combat continues until a result or cancellation is set. |
| `21` | 1 | `0x801E748C` | `BattleEventOpcode21SetSilentResult` | Set the resident flag that selects silent Result presentation; reward and progression gates remain unchanged. |
| `22` | 1 | `0x801E74A0` | `BattleEventOpcode22PauseEvent` | Set pass-control state to 2 so update returns after this pass. |
| `23` | 7 | `0x801E74E0` | `BattleEventOpcode23SetupMecha` | Start asynchronous setup for Event entity `actorAlias - 0xF3`, resident target mask `1 << ((targetAlias + 0x0D) & 0xFF)`, and v15 `animationId`; poll state `0 -> 2 -> 1 -> 0`. |
| `24` | 5 | `0x801E7700` | `BattleEventOpcode24SetBattleTransition` | Set `battleInitializationMode + 1` and `transitionEffect`. |
| `25` | 1 | `0x801E775C` | `BattleEventOpcode25SetExitMode3Request` | Set the resident exit-mode request flag; central Battle selects mode 3 when result bit `0x80` is set, bit `0x40` is clear, and Battle Event is enabled. |
| `26` | 9 | `0x801E7770` | `BattleEventOpcode26SetFieldReturn` | Commit v15 `returnFieldId`, `cameraYaw`, `worldMapPositionId`, and `worldMapMode`. |
| `27` | 9 | `0x801E77E4` | `BattleEventOpcode27PlayMovie` | Set v15 `movieType \| 0x80`, `movieNumber`, `fadeParameter`, and `completionValue`, then select return mode 1. |
| `28` | 6 | `0x801E786C` | `BattleEventOpcode28CreateFader` | Call `BattleCreateFader(duration, mode, red, green, blue)` from five encoded bytes. |
| `29` | 9 | `0x801E78A8` | `BattleEventOpcode29SetCameraTarget` | Pass v15 `targetX`, `targetY`, `targetZ`, and `transitionFrames` to `BattleSetEventCameraTarget`. |
| `2A` | 5 | `0x801E79E0` | `BattleEventOpcode2APlaySpriteAnimation` | Apply v15 `animationId` to the Event sprite selected by `spriteEntityAlias`. |
| `2B` | 3 | `0x801E7B58` | `BattleEventOpcode2BWait` | Initialize the entity timer to `waitUnits * 2` and poll until frame updates reach zero. |
| `2C` | 3 | `0x801E7C0C` | `BattleEventOpcode2CSetPriority` | Store signed `priority`; `-2` moves the entity to queue front. |
| `2D` | 3 | `0x801E7E14` | `BattleEventOpcode2DLoadMusic` | Load v15 `musicId` with initial volume `0x7F`. |
| `2E` | 3 | `0x801E7E5C` | `BattleEventOpcode2ELoadMusicMuted` | Load v15 `musicId` with initial volume zero. |
| `2F` | 5 | `0x801E7EA4` | `BattleEventOpcode2FSetLoadedMusicVolume` | Set v15 `volume` over `transitionFrames` and save `volume` for restoration. |
| `30` | 3 | `0x801E7F08` | `BattleEventOpcode30SetLoadedMusicMuted` | Apply loaded-music `muted`: zero restores saved volume; nonzero sets volume to zero immediately. |
| `31` | 9 | `0x801E7F70` | `BattleEventOpcode31PlaySoundEffect` | Play v15 `effectId` with `volume`, `pan`, and `bankSelector` (`0` Event bank, nonzero resident bank). |
| `32` | 1 | `0x801E8074` | `BattleEventOpcode32NoOperation` | Intentional advancing no-op. |
| `33` | 1 | `0x801E807C` | `BattleEventOpcode33StopLoadedMusic` | Stop and free the Event-loaded music instance if active. |
| `34` | 1 | `0x801E80E8` | `BattleEventOpcode34NoOperation` | Second intentional advancing no-op. |
| `35` | 5 | `0x801E7914` | `BattleEventOpcode35CreateSprite` | Decompress file-3 `resourceIndex` and create the `spriteEntityAlias` sprite at the origin when its alive flag is clear. |
| `36` | 3 | `0x801E7B08` | `BattleEventOpcode36DestroySprite` | Destroy the `spriteEntityAlias` sprite and free its bundle. |
| `37` | 1 | `0x801E74B8` | `BattleEventOpcode37EndBattleSuccessfully` | Set result `0x01`, set cancellation byte `0x800D2FC4`, and stop the Event VM. |
| `38` | 7 | `0x801E75F0` | `BattleEventOpcode38PlayMechaAnimation` | Start v15 `animationId` for Event entity `actorAlias - 0xF3` against resident target mask `1 << ((targetAlias + 0x0D) & 0xFF)`. |
| `39` | 1 | `0x801E80F0` | `BattleEventOpcode39TransformPartyLeaderToGear` | Assign Gear ID 0 to party slot 0's live and persistent records, clear leader action state, relocate the slot to its Gear visual actor, release the character actor, and select Gear mode, command ring, HUD, status, and UI-refresh state. |
| `3A` | 5 | `0x801E818C` | `BattleEventOpcode3ASetEntityCameraAnimation` | Resolve `characterId` and bind v15 `cameraAnimationId` to its sprite. |
| `3B` | 3 | `0x801E81EC` | `BattleEventOpcode3BHideEntity` | Resolve `characterId` and apply the sprite's stashed idle animation. |
| `3C` | 3 | `0x801E823C` | `BattleEventOpcode3CShowEntity` | Resolve `characterId`, reset sprite frame/wait state, and clear hidden render bits. |
| `3D` | 3 | `0x801E828C` | `BattleEventOpcode3DClearAnimationSyncCounter` | Resolve `characterId` and clear the sprite actor's animation-script synchronization counter at `+0x9E`. |
| `3E` | 9 | `0x801E82DC` | `BattleEventOpcode3EMoveEntity` | Start linear movement of `characterId` to v15 `targetX`, `targetY`, `targetZ`, then poll completion. |
| `3F` | 9 | `0x801E83C0` | `BattleEventOpcode3FMoveEntityAlternate` | Start eased movement of `characterId` to v15 `targetX`, `targetY`, `targetZ`, then poll completion. |
| `40` | 3 | `0x801E7B2C` | `BattleEventOpcode40DestroySpriteAndReset` | Destroy the `spriteEntityAlias` sprite and reset camera/disposal state. |
| `41` | 7 | `0x801E7FF4` | `BattleEventOpcode41PlaySequenceEntry` | Play v15 `sequenceId` at `volume` from `bankSelector` (`0` Event bank, nonzero resident bank). |
| `42` | 1 | `0x801E86D0` | `BattleEventOpcode42InitializeGearTurnUi` | Call `BattleInitializeGearTurnUi(0)`. |
| `43` | 1 | `0x801E86F4` | `BattleEventOpcode43FreeTurnRenderWorkspace` | Call `BattleFreeTurnRenderWorkspace(0)`. |
| `44` | 1 | `0x801E8718` | `BattleEventOpcode44ClearResidentSpriteAliveFlags` | Clear the alive flag on each of the eleven resident sprite tasks. |
| `45` | 9 | `0x801E84A4` | `BattleEventOpcode45LoadCharacterSprite` | Resolve `destinationCharacterId` and `sourceCharacterId`, set `sourceSpriteMode`, asynchronously replace the destination sprite, bind `cameraAnimationId`, and poll completion. |
| `46` | 7 | `0x801E8600` | `BattleEventOpcode46SwapCharacterSprite` | Prepare `destinationCharacterId` with `swapEffectId`, then commit the state swap with `sourceCharacterId`. |
| `47` | 1 | `0x801E86AC` | `BattleEventOpcode47ClearAnimationData` | Call `BattleClearAnimationData` to finish disc transfer and release temporary animation resources. |
| `48` | 5 | `0x801E8750` | `BattleEventOpcode48PlayIndexedAudioClipBlocking` | Play `clipGroup` and `clipIndex` through `BattlePlayIndexedAudioClipBlocking`. |
| `49` | 3 | `0x801E7424` | `BattleEventOpcode49SetReturnMode` | Set v15 `fieldReturnMode`. |
| `4A` | 1 | `0x801E7660` | `BattleEventOpcode4AActivateSlot0GearHyper` | Call `BattleActivateGearHyperMode(0)` to set slot 0 AL to `4`, duration to `3`, and character 0's persistent Gear-special availability bit `0x4000`. |
| `4B` | 3 | `0x801E7684` | `BattleEventOpcode4BMarkCharacterDead` | Resolve `characterId` and set the low death/status bit in its combat state. |

Opcodes above `0x4B` skip handler dispatch and apply the stale `s0` delta through
the common PC-update path.

## 13. Timers And Asynchronous Completion

Zero-return polling is used by:

| Opcode | Completion source |
|---:|---|
| `03` | A free secondary slot becomes available. |
| `04`, `05` | Target entity slot/tag state changes. |
| `18`, `19` | Dialogue state machine closes and tears down. |
| `23` | Mecha setup callback changes entity `+0x34` from 2 to 1. |
| `2B` | Central Battle input mode 2 decrements entity `+0x26` while graphics-control byte `+0x56` is nonzero. |
| `3E`, `3F` | Sprite move callback clears per-entity async byte `+0x804`. |
| `45` | Character sprite replacement callback clears the same async byte. |

For `0x2B`, first dispatch stores `waitUnits * 2` and marks the timer active.
When graphics-control byte `+0x56` is nonzero, `BattleUpdateInputs_mode2`
decrements active timers once per Battle input/frame update and clamps negative
values to zero. A zero-unit wait advances during its initial dispatch; a
nonzero wait advances on a later VM dispatch after the timer reaches zero.
Timer storage therefore uses twice the script's wait units.

## 14. Dialogue Lifecycle

Default dialogue parameters are:

```text
x      = 0x7FFF (auto-center)
y      = 0x7FFF (auto top/bottom)
width  = 16
height = 8
flags  = 0x01F0
```

`BattleEventDisplayDialog` (`0x801E6CE8`) is a polling state machine:

1. Resolve auto X/Y and clamp visible text height to at most four lines.
2. Optionally load a 64 by 64 portrait TIM from common directory 4.
3. Allocate Battle dialogue window 0 and wait while rendering until initialized.
4. Initialize the shared text decoder on the selected dialogue payload.
5. Render and expose the animated continuation cursor when text is ready.
6. Accept Battle input value 4 to request close.
7. Free text memory, clear portrait/window state, and restore default parameters.

Dialogue flag uses are:

| Bit | Use |
|---:|---|
| `0` | Select mirrored portrait table/geometry and corresponding text placement. |
| `1` | Suppress portrait. |
| `2` | With automatic Y, select lower placement at `0x8C`; upper placement is `0x10`. |
| `3` | Suppress normal window/portrait/cursor creation and clearing. |
| `4` | Select the alternate window style passed to the common allocator. |

The portrait lookup has 90 normal/mirrored pairs. Most pairs are identical;
some characters have distinct mirrored artwork IDs.

## 15. Music And Audio

Initialization always loads directory `0x20`, file 4 as an Event sequence bank.
Opcodes `0x2D` and `0x2E` load an Event music instance:

```text
OP2D/OP2E music ID N -> read directory 0x20 file N + 4
```

Loading replacement music stops/frees the prior instance, reads and initializes
the new sequence, records ID/volume, and creates a sound instance. `0x2F`
sets its volume over a transition period, `0x30` mutes or restores the saved
volume immediately, and `0x33` stops and frees it.

`0x31` and `0x41` can select between the Event sequence bank and resident Battle
sequence state. `0x48` synchronously loads and plays an indexed audio clip
through `BattlePlayIndexedAudioClipBlocking`, rendering during load/playback and
releasing temporary resources afterward.

## 16. Sprites, Cameras, And Movement

Event-created sprite ownership is attached to an Event entity:

```text
entity +0x2C -> decompressed animation bundle
entity +0x30 -> sprite task
entity +0x35 -> alive flag
```

`0x35` creates only when the alive flag is clear. `0x36` and `0x40` unregister
callbacks, free the task and bundle, and clear ownership; `0x40` additionally
resets camera/disposal state. `0x44` clears the resident alive bytes while
retaining the tasks and bundles.

Visual manipulation opcodes `0x3A..0x3F` resolve a character ID to one of the
eleven Battle visual slots. Movement opcodes set an asynchronous flag, start a
native sprite movement helper, and preserve their PC until the helper clears
that flag. The normal and alternate helpers use different interpolation paths;
the script-visible distinction is linear versus eased movement.

`0x29` passes a signed XYZ target plus `transitionFrames` to
`BattleSetEventCameraTarget` (`0x800B3658`). `0x3A` changes a sprite-bound camera
animation. Camera effects use central Battle camera state and sprite animation
callbacks.

## 17. Battle And Module Lifecycle

The central Battle configuration flag `0x20` enables the Event phase. The
central entry points are:

| Address | Function | Role |
|---:|---|---|
| `0x80070E2C` | `BattleEventOverlayLoad` | Activate Battle Event and call initialization. |
| `0x80070EB0` | `BattleEventOverlayUpdate` | Call `BattleEventUpdate` when enabled. |
| `0x80070EDC` | `BattleEventOverlayShutdown` | Call Event shutdown and restore resident Battle music when appropriate. |

`BattleMain` calls Event update once immediately after load. Event bytecode can
then:

- Configure transition and return state with `0x24`, `0x26`, and `0x49`.
- End or skip combat successfully with `0x37`.
- Pause an Event phase with `0x22`.
- Stop Event execution and permit the final Event handoff with `0x20`.
- Enter movie mode with `0x27`.

During ordinary turns and enemy end-turn scripts, central Battle calls the Event
update hook after staged action sequences. Enemy opcode `0x70` can write Event
variables between those updates.

When the central exit route reaches the Event handoff, it first writes:

```text
event_variable[0] = 0x00FF
```

Central Battle performs the final Event update only when the Event phase is
enabled and either the Event end flag is set or the Battle cancellation/start
flag is clear. On that path it clears the Event stop flag, writes variables
`16..18`, and calls Event update again:

```text
event_variable[16 + slot] =
    (party_entity[slot].status_7C & 0x8000) |
    (party_entity[slot].gear_status_120 & 0x8000)
```

Each merged party value is `0x0000` or `0x8000`. The final update lets bytecode
choose the return path. The engine-defined names for these writes appear in the
variable-bank table; each script defines the remaining story-level meanings.

`BattleEventShutdown` (`0x801E563C`) frees the runtime, dialogue state, script
resource, and retained file-3 resource. It stops/frees Event music and releases
the file-4 sequence bank when active. The central
shutdown path resumes resident Battle music when Event replacement ownership is
clear and the resident music state requests playback.

## 18. Main Functions

| Address | Function |
|---:|---|
| `0x801E5160` | `BattleEventInitialize` |
| `0x801E563C` | `BattleEventShutdown` |
| `0x801E5768` | `BattleEventGetBytecodeOffset` |
| `0x801E57C4` | `BattleEventFindFreeScriptSlot` |
| `0x801E57F8` | `BattleEventDecodeOperands` |
| `0x801E58EC` | `BattleEventEvaluateCondition` |
| `0x801E5A98` | `BattleEventResolveEntityIndex` |
| `0x801E6CE8` | `BattleEventDisplayDialog` |
| `0x801E7CD0` | `BattleEventLoadMusic` |
| `0x801E7DE4` | `BattleEventSetLoadedMusicVolume` |
| `0x801E879C` | `BattleEventUpdate` |
| `0x801E93E8` | `BattleEventSpriteAnimationComplete` |
| `0x801E9430` | `BattleEventSetSpriteCameraAnimation` |
| `0x801E95E4` | `BattleEventStartSpriteMove` |
| `0x801E9694` | `BattleEventStartSpriteMoveAlternate` |
| `0x801E9894` | `BattleEventStartCharacterSpriteLoad` |
| `0x801E9978` | `BattleEventCreateSpriteEntity` |
| `0x801E9AD4` | `BattleEventDestroySpriteTask` |
