# Entry Modes, Handoffs, And Coordination

## 1. Mode Dispatch

Resident `MenuExecute` at `0x8001C1A8` treats mode as the complete dispatch
selector. Modes 0, 2, and 6 use the General Menu entry; the remaining modes use
their specialized module entry.

| Mode | Operation | Entry | Post-entry resident action |
|---:|---|---:|---|
| 0 | Normal Menu | Menu `0x801C62A8` | Return to caller. |
| 1 | Member Change | Member Change `0x801CB0A8` | Return to caller. |
| 2 | Title Menu / Load Game | Menu `0x801C62A8` | Request resident game state 1. |
| 3 | Enter Name | Enter Name `0x801CBDBC` | Return to caller. |
| 4 | Shop | Shop `0x801CCD28` | Return to caller. |
| 5 | Gear Shop | Gear Shop `0x801CE024` | Return to caller. |
| 6 | Disc Change | Menu `0x801C62A8` | Request resident game state 1. |

## 2. General Menu Branches

Menu `0x801C62A8` performs common presentation setup and branches by mode:

| Mode | General Menu path |
|---:|---|
| 0 | Initialize normal content at Menu `0x801D2D38`, then run the seven-entry menu at Menu `0x801C55A0`. |
| 2 | Run the three-entry load/save title selection at Menu `0x801C58EC` and process its result. |
| 6 | Run disc-change load/save control at Menu `0x801C57A4`. |

All three paths converge on Menu `0x801C5FE4` for shutdown. Resident state
changes for modes 2 and 6 occur after this shutdown returns.

## 3. Request Arguments

| Operation | Input contract |
|---|---|
| Normal Menu | One-bit opening argument; Field opcode `FE 99` stores its inverse for the next mode-0 request. |
| Member Change | Resident party membership, availability, and formation state. |
| Title Menu / Load Game | Title-menu state and result byte at resident `0x800594D0`. |
| Enter Name | Selected character or Gear name-record index in the range `0..30`. |
| Shop | Eight-bit shop inventory selector. |
| Gear Shop | Gear-shop selector plus resident party and Gear state. |
| Disc Change | Zero-based disc selector `0..1`. |

Each operation reads its own resident inputs. These values are separate
coordination fields rather than one serialized argument record.

## 4. Field Request Surface

Field scripts publish requests and yield to Field coordination:

| Opcode | Field function | Effect |
|---:|---:|---|
| `FE 55` | Field `0x80093740` | Queue mode 0 with its configured argument. |
| `FE 56` | Field `0x80093930` | Store the evaluated selection and queue mode 1. |
| `FE 57` | Field `0x800937E0` | Queue mode 2. |
| `FE 58` | Field `0x80093824` | Queue mode 3 for the selected name record. |
| `FE 59` | Field `0x800939A0` | Queue mode 4 for the selected shop. |
| `FE 5A` | Field `0x80093A04` | Queue mode 5 for the selected Gear shop. |
| `FE 87` | Field `0x800936E4` | Wait while a Menu request remains active. |
| `FE 99` | Field `0x8008848C` | Set the inverse mode-0 opening argument. |
| `FE CF` | Field `0x80093888` | Save Field context, install destination values, and queue mode 1. |
| `FE DA` | Field `0x80093790` | Queue mode 6 with argument 1. |

The waiter resumes after Field restoration finishes, so script execution never
observes intermediate Field presentation state.

## 5. Normal Menu Gate

Field `0x800798BC` refreshes normal-menu availability during actor control. It
blocks a Triangle request while the controlled actor is absent or while current
material state has bit `0x40` or `0x80`; a non-`0xFF` override replaces that
result. A newly pressed Triangle queues mode 0 only after this gate passes.

Once Menu starts, input comes from resident controller queues. Field's actor
gate no longer participates in Menu navigation.

## 6. Field Handoff

Field `0x800799D4` runs an in-place modal handoff:

1. Stop ordinary Field presentation progression.
2. Preserve framebuffer content and transient stream state.
3. Prepare common Menu resources and the selected module.
4. Prepare Gear Helper when mode 5 is selected.
5. Rearrange the VRAM rectangles used by Menu and preserve Field texture pages
   that Menu replaces.
6. Release or suspend volatile Field rendering resources.
7. Publish mode, party mount state, and Menu scratch storage.
8. Call resident `MenuMain` at `0x8001C634`.
9. Consume the mode-2 result when Load Game was selected.
10. Rebuild Field display environments, framebuffer content, streamed assets,
    party resources, GTE view state, and per-frame services.
11. Clear the request to `0xFF` and resume Field.

Field remains the active coordinator throughout this sequence and continues its
existing main loop after restoration.

## 7. Load-Game Return

Resident byte `0x800594D0` coordinates General Menu and Field:

| Value | Field action |
|---:|---|
| `0` | Install the no-load title transition, including Field ID `4`. |
| `1` | Enter the idle-attract route and replay the opening sequence. |
| `2` | Restore the saved Field destination and persistent values. |

The memory-card operation state machine is specified in
[`07-memory-card-save-load-and-disc-flows.md`](07-memory-card-save-load-and-disc-flows.md).

For result `2`, the immediate Field restoration reloads the compact party
resource IDs retained before Menu rather than deriving new resource IDs from
the party bytes restored by the save. Those restored IDs can first reach party
skin initialization on a later eligible Field transition, subject to the
reentry and body-type guards of that transition.

## 8. World Handoff

World uses its own modal preparation and restoration pair:

| Address | Contract |
|---:|---|
| World `0x800758C0` | Snapshot travel state, release volatile rendering resources, and preserve the framebuffer. |
| World `0x80075B58` | Rebuild graphics and resource buffers, recreate effects, and reconcile party Gear state. |

```text
World runtime
    -> prepare for Menu
    -> Resident MenuMain
    -> selected Menu module
    -> restore World
    -> resume World runtime
```

Field and World share resident Menu services while retaining separate restore
procedures for their simulation and rendering state.

## 9. Resident State Boundary

Resident `ChangeGameState` at `0x8001996C` uses this state table:

| State | Module |
|---:|---|
| 0 | Kernel menu |
| 1 | Field |
| 2 | Battle |
| 3 | World Map |
| 4 | Battling |
| 5 | Menu |
| 6 | Movie |

Field and World can invoke Menu as an in-place modal service. A top-level state-5
transition reaches the same resident `MenuMain` through the state dispatcher.

## 10. Coordination Invariants

1. Dispatch exposes only the selected Menu module.
2. Common resources and module code are ready before dispatch.
3. Mode 5 initializes Gear Helper before creating a model preview.
4. Field or World simulation remains paused while Menu owns volatile graphics
   state.
5. The selected module releases all page-owned work before returning.
6. The caller consumes return state before clearing the request.
7. A Field script waiter resumes after complete restoration.
8. Modes 2 and 6 request resident state 1 after General Menu shutdown.

## 11. Function Index

| Address | Function |
|---:|---|
| Resident `0x8001996C` | Request a resident state change. |
| Resident `0x8001C1A8` | Dispatch the selected Menu mode. |
| Resident `0x8001C634` | Run resident Menu setup and dispatch. |
| Field `0x800798BC` | Refresh normal-menu availability. |
| Field `0x800799D4` | Run the complete Field handoff and restoration. |
| Field `0x800936E4` | Wait for Menu close and Field restoration. |
| World `0x800758C0` | Prepare World for Menu. |
| World `0x80075B58` | Restore World after Menu. |
| Menu `0x801C55A0` | Run normal seven-entry selection. |
| Menu `0x801C57A4` | Run mode-6 disc-change control. |
| Menu `0x801C58EC` | Run mode-2 load-game selection. |
| Menu `0x801C5FE4` | Shut down General Menu resources. |
| Menu `0x801C62A8` | Dispatch General Menu modes. |
