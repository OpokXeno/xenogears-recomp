# Scheduler And VM Execution

## 1. Cooperative Execution

Field scripting is cooperative. An opcode handler runs to completion, but it can
request that the VM stop dispatching the current invocation until a later
scheduler pass.

Each eligible actor normally receives one selected slot and an eight-dispatch
budget per scheduler pass. Eight slots do not mean eight parallel threads.

## 2. Starting A Routine

Actor-to-actor start opcodes encode:

```text
actor selector
packed routine byte:
    bits 0..4 = routine ID 0..31
    bits 5..7 = initial scheduler priority
```

The start path:

1. Resolves the target actor selector.
2. Checks whether the requested routine already has an invocation registered.
3. Finds a slot whose priority is `0xF` and whose remote-wait protection bit is
   clear.
4. Reads the target actor's routine offset.
5. Stores the offset as the slot PC.
6. Stores routine ID, priority, wait state, and counters.

The three start forms are:

| Opcode | Behavior |
|---:|---|
| `07` | Start asynchronously; the caller continues immediately. |
| `08` | Start synchronously and wait until the remote invocation has entered execution or has already completed. |
| `09` | Start synchronously and wait until the remote invocation has completely finished. |

Blocking forms protect the remote slot with bit 22. The caller stores the remote
slot index in its own `ActorData+0xCF` and advances through blocking states 0, 1,
and 2. Completion clears the protection and advances the caller.

## 3. Contact And Interaction Routines

Actor contact dispatch tests distance, vertical overlap, facing, input, and
collision bounds.

| Event | Routine |
|---|---:|
| Explicit interaction input | 2 |
| Automatic physical contact | 3 |

Actor flag `0x00800000` suppresses only automatic routine 3. Opcodes `CD` and
`CE` disable and re-enable that automatic contact path.

## 4. Ordinary Scheduler Pass

`FieldRunActorScriptScheduler` performs:

```text
for each script row / actor:
    reject actors not eligible for script updates
    current ActorData = actor.ActorData

    selected_priority = 0xF
    for slot 0..7:
        if slot.priority <= selected_priority:
            selected slot = slot
            selected_priority = slot.priority

    if selected_priority == 0xF:
        slot 0.pc = row[actor][routine 1]
        slot 0.priority = 7
        selected slot = 0

    working_pc = selected_slot.saved_pc
    run VM with budget 8
    selected_slot.saved_pc = working_pc
```

Because comparison is inclusive, a later slot wins a priority tie.

A special director-only mode limits traversal to actor 0. Actor primary flag bit
0 prevents ordinary dispatch while preserving the actor and its slot state.

## 5. Entry Routines

Map entry paths run actor 0 synchronously:

1. Copy its complete `0x138`-byte `ActorData`.
2. Reset the eight slots.
3. Resolve the selected entry routine.
4. Run with budget `0xFFFF`.
5. Restore the saved actor state.

Routine 2 is used by a restored-field setup path and routine 3 by a reload path.
These entry executions are temporary bootstrap contexts, not ordinary persistent
slots.

## 6. Primary Dispatch

The working PC indexes the shared bytecode:

```text
opcode  = bytecode[working_pc]
handler = primary_table[opcode]
handler()
```

The handler owns the PC. It can:

- Add its completed instruction length.
- Replace the PC with a jump or call target.
- Preserve or rewind the PC to retry later.
- Release the current slot.
- Request the current VM cycle to stop.
- Trigger a Field or module transition that ends scheduling.

The primary table at `0x800AE2A0` contains 256 pointers.

## 7. Extended Dispatch

Primary opcode `FE` is a prefix:

```text
working_pc += 1
subopcode = bytecode[working_pc]
extended_table[subopcode]()
```

The table at `0x800AE6A0` has 227 entries, `00..E2`. Extended handler PC deltas
are measured from the subopcode byte, so total physical size includes the `FE`
prefix.

`FE 00` and `FE 78..FE 7E` return without advancing the subopcode PC. The prefix
has already been consumed, so the second byte is decoded again as a primary
opcode. `FE E3..FE FF` have no table entries and are invalid script input.

## 8. Dispatch Limits

`FieldScriptVMRun` applies:

| Limit | Effect |
|---|---|
| Caller budget | Ordinary scheduler passes use 8 |
| Entry budget | Synchronous entry routines use `0xFFFF` |
| Hard watchdog | More than `0x400` consecutive handlers reports an event-loop error |

Loading, transition, and module-state gates can return before the budget is
consumed. Opcode `C6` adds `0x20` to the current budget and yields; it is not a
32-frame sleep.

## 9. Jumps

Opcode `01` loads its following `u16` as the new PC.

Opcode `02` is eight bytes:

```text
+0  opcode
+1  lhs u16
+3  rhs u16
+5  operand-type and condition byte
+6  false-target u16
```

The high nibble selects variable/immediate interpretation for both operands. The
low nibble implements:

| Value | Expression |
|---:|---|
| `0x0` | `a == b` |
| `0x1` | `a != b` |
| `0x2` | `b < a` |
| `0x3` | `a < b` |
| `0x4` | `a >= b` |
| `0x5` | `b >= a` |
| `0x6` | `(a & b) != 0` |
| `0x7` | `a != b` |
| `0x8` | `(a | b) != 0` |
| `0x9` | `(a & b) != 0` |
| `0xA` | `((~a) & b) != 0` |

True advances to `PC+8`; false loads the target at `+6`.

## 10. Calls And Returns

Calls stay inside the current actor and use its four-entry return stack:

| Opcode | Saved return PC | Destination |
|---:|---:|---|
| `05` | `PC + 3` | `u16` offset from the shared bytecode base |
| `06` | `PC + 5` | `u16` offset from the shared bytecode base |

Opcode `06` leaves two inline bytes between its destination operand and return
continuation. Opcode `0D` decrements the depth and restores the saved PC.

Call depth five and return at depth zero report stack errors. Return underflow
also releases the current slot and ends its VM cycle.

A call is not an actor-to-actor routine start: calls share the current actor,
slot, priority, and working PC context.

## 11. Suspension And Completion

| Class | PC and slot behavior |
|---|---|
| Complete instruction | Advance to the following instruction and continue within budget |
| Yield | Advance PC, stop this VM cycle, resume at the next instruction later |
| Polling wait | Preserve or rewind PC, stop this cycle, retry the same instruction later |
| Timed wait | Store a timer/state in the slot and resume after scheduler updates |
| End invocation | Set routine ID to `0xFF`, make the slot inactive, stop this cycle |
| Transition exit | Stop because Field loading or another game module takes control |

Polling waits cover movement, animation, dialogue, camera, sound, music, video,
menus, archive I/O, special animation, mecha loading, and remote routines.

The encoded length is determined by the completed path. A temporary rewind while
waiting does not make the instruction shorter.

Opcode `26` uses the selected slot's one-byte timer. On its first dispatch it
stores the low byte of the evaluated operand. Every dispatch yields; later
dispatches decrement the timer, and the PC advances when it reaches zero.
Consequently timer value `N` occupies `N+1` scheduler selections. Value zero is
a real one-selection yield, not an instruction that continues immediately.

Opcode `5B` is a different kind of wait: it clears movement state and yields
without changing the PC or releasing the slot. Repeated selection therefore
parks that invocation on `5B` indefinitely.

## 12. Input State And Dialogue Sequencing

The VM exposes two input histories:

| Opcode | Source |
|---:|---|
| `31` | Buttons currently held |
| `32` | Input bits accumulated since the last clear |
| `33` | Clear accumulated input |

Opcodes `31` and `32` contain a `u16` mask followed by a `u16` false target.
They fall through when any requested bit is set and branch otherwise.

Clearing accumulated input does not release a currently held button. An
interaction that immediately opens dialogue can therefore expose the same held
button to later UI logic. A deterministic sequence clears accumulated input,
polls `31` until the activating button is physically released, yields once,
and only then opens the next dialogue window.

## 13. Event Timer

Opcodes `94..96` control the event timer stored at VM offset `0x0A`:

| Opcode | Effect |
|---:|---|
| `94` | Pack two evaluated bytes into the timer, pause it, and reset its update divider |
| `95` | Configure pause with bit 7 and countdown direction with bit 2 |
| `96` | Pause the timer and reset its update divider |

This timer is separate from each script slot's one-byte timed-wait counter.

## 14. Object Swivel

Opcodes `D7`, `D8`, and `D9` select X, Y, or Z in `ActorData+0x12C` bits 0..1
and store an angle in `ActorData+0x70`. The selected swivel is applied when
building object collision transforms.

## 15. Reserved Entries

| Entries | Behavior |
|---|---|
| `13`, `FD`, `FF` | Same one-byte advancing NOP |
| `D1`, `E4` | Return with the PC unchanged; executing them repeatedly reaches the watchdog |
| `FE 00`, `FE 78..FE 7E` | Consume `FE`, then reinterpret the second byte as primary |

## 16. Main Functions

| Address | Function |
|---:|---|
| `0x8008399C` | Detect contact/interaction and start routines |
| `0x800869B8` | Extended `FE` dispatch |
| `0x8009EB48` | Test whether an actor routine is registered |
| `0x8009EB78` | Asynchronous actor-routine start |
| `0x8009ED68` | Start-sync actor-routine state machine |
| `0x8009F0A0` | End-sync actor-routine state machine |
| `0x800A1730` | Call with return `PC+5` |
| `0x800A17F4` | Call with return `PC+3` |
| `0x800A18B8` | Return |
| `0x800A19B0` | Reset all actor slots |
| `0x800A1A8C` | Stop and redirect waiting invocations |
| `0x800A1B70` | End current invocation |
| `0x800A1BD0` | Conditional jump |
| `0x800A1E74` | Unconditional jump |
| `0x800A1E9C` | Add 32 to budget and yield |
| `0x800A1EC8` | VM dispatch loop |
| `0x800A2030` | Actor script scheduler |
| `0x800A22AC` | Synchronous entry-routine runner |
| `0x800A28D4` | Initialize or restore actor routines |
| `0x800A3018` | Typed variable read |
| `0x800A3074` | Variable write |
| `0x800A3090` | Resolve entity/routine entry |
| `0x800ACDEC` | Resolve `v80` operand |
