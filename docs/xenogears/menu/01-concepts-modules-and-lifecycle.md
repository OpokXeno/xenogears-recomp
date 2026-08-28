# Concepts, Modules, And Lifecycle

## 1. System Boundary

The Menu subsystem is a resident coordinator plus one selected Menu module:

```text
Field, World, or resident state dispatcher
                  |
                  v
        Resident MenuMain / MenuExecute
        mode, heap, input, graphics, dispatch
                  |
                  v
      General | Member | Name | Shop | Gear Shop
                  |
                  +---- Gear Helper for Gear Shop
                  |
                  v
       persistent state and return status
                  |
                  v
       caller restoration or state change
```

Resident services provide allocation, controller queues, sound, font handling,
graphics environments, GTE state, and access to persistent game state. The
selected module owns menu policy, page state, mode-specific allocations, frame
composition, and teardown. Gear Helper owns model, skeleton, animation, trail,
and deformable-mesh services used by Gear Shop previews.

## 2. Runtime Components

| Component | Contract |
|---|---|
| `MenuMain` | Creates `SystemMenu`, initializes display state, and enters `MenuExecute`. |
| `MenuExecute` | Dispatches mode `0..6` to the matching module entry. |
| `SystemMenu` | Shared root record containing graphics environments, command state, common selection state, and owner pointers. |
| General Menu | Runs the normal menu, load-game selection, and disc-change presentation. |
| Member Change | Edits active and benched party formation. |
| Enter Name | Edits one selected character or Gear name. |
| Shop | Runs ordinary purchase and sale transactions. |
| Gear Shop | Runs Gear purchases, tune-up transactions, and model previews. |
| Gear Helper | Manages Gear preview models and their animation resources. |

## 3. Entry Points

| Module | Entry | Contract |
|---|---:|---|
| General Menu | Menu `0x801C62A8` | Initialize common presentation, dispatch modes 0, 2, or 6, and shut down. |
| Member Change | Member Change `0x801CB0A8` | Run the complete party-formation session. |
| Enter Name | Enter Name `0x801CBDBC` | Run the complete name-editing session. |
| Shop | Shop `0x801CCD28` | Run the complete ordinary-shop session. |
| Gear Shop | Gear Shop `0x801CE024` | Run the complete Gear-shop session. |
| Gear Helper | Gear Helper `0x801E738C` | Initialize helper pools and ten model slots. |
| Gear Helper | Gear Helper `0x801E7FD4` | Release all model slots and helper pools. |

## 4. Resident Setup

Resident `MenuMain` at `0x8001C634` performs the following sequence:

1. Allocate and clear the `0x1E98`-byte `SystemMenu` root.
2. Publish its pointer at resident `0x800625A0`.
3. Store command `8`, the idle command, at `SystemMenu+0x325`.
4. Select heap user 2 for Menu allocations.
5. Select the second embedded graphics environment at `SystemMenu+0x120`.
6. Initialize frame counters, transition state, and draw control.
7. Build two 320 by 224 `DRAWENV`/`DISPENV` pairs and initialize the Menu view.
8. Install both graphics environment pairs and enable display output.
9. Call resident `MenuExecute` at `0x8001C1A8`.
10. Complete resident post-return state handling.

`MenuMain` creates the root, while the selected module releases it. This split
keeps resident setup common and lets each module tear down its children in the
correct dependency order.

## 5. Complete Lifecycle

1. Field, World, or the resident dispatcher chooses mode `0..6` and publishes
   the mode-specific argument.
2. Field or World pauses simulation and preserves the framebuffer and volatile
   presentation state needed after return.
3. The selected Menu module and its common resources are prepared for execution.
4. Gear Shop also prepares Gear Helper and preview support storage.
5. Resident `MenuMain` creates `SystemMenu`, initializes graphics state, and
   calls `MenuExecute`.
6. `MenuExecute` calls the selected module entry.
7. The module creates managers, resource records, windows, cursors, selection
   state, and page-specific records.
8. `MenuDraw` translates controller state into one command and presents the page
   state from before that command. The page loop consumes the published command
   after `MenuDraw` returns, updates logical state, and makes the visual effect
   visible in a later composition.
9. Accepted operations update persistent game state or publish a return value.
10. The module stops input-driven creation, disables drawing, drains GPU-visible
    work, and releases children in reverse dependency order.
11. The module releases `SystemMenu` and returns through `MenuExecute`.
12. Modes 2 and 6 request resident game state 1 after General Menu shutdown.
13. Field or World rebuilds its volatile state and resumes when control returns
    through its modal handoff.

## 6. Ownership Boundary

`SystemMenu` is the lifetime root, but every child has one explicit owner field.
The selected module publishes an owner before a child becomes reachable.
Manager disable paths release the allocation without writing `NULL` to the owner
field, so that field can remain stale until `SystemMenu` is released and must not
be reused after disable. GPU-visible packet records remain alive through the
last frame that can reference them. Gear Shop shuts down all Gear Helper model
slots before releasing helper pools and then continues ordinary module teardown.

The detailed record map and release sequence are specified in
[`04-runtime-objects-allocations-and-ownership.md`](04-runtime-objects-allocations-and-ownership.md).

## 7. Lifecycle Invariants

1. Dispatch activates exactly one of the five Menu modules.
2. Mode selection and entry dispatch refer to the same module.
3. `SystemMenu` exists before a module entry runs.
4. Every child is disabled before its owner storage is released.
5. Current and previous GPU parity storage remain distinct.
6. Gear Helper is active only while Gear Shop preview services require it.
7. `SystemMenu` is the final Menu allocation released.
8. Caller restoration begins after module teardown completes.

## 8. Function Index

| Address | Function |
|---:|---|
| Resident `0x8001BDDC` | Initialize one Menu graphics environment. |
| Resident `0x8001BE14` | Initialize both Menu graphics environments. |
| Resident `0x8001BEEC` | Initialize Menu view and transition state. |
| Resident `0x8001C1A8` | Dispatch the selected Menu mode. |
| Resident `0x8001C634` | Create `SystemMenu` and run the resident Menu lifetime. |
| Menu `0x801C62A8` | Run General Menu modes 0, 2, and 6. |
| Member Change `0x801CB0A8` | Run Member Change. |
| Enter Name `0x801CBDBC` | Run Enter Name. |
| Shop `0x801CCD28` | Run Shop. |
| Gear Shop `0x801CE024` | Run Gear Shop. |
| Gear Helper `0x801E738C` | Initialize Gear Helper. |
| Gear Helper `0x801E7FD4` | Shut down Gear Helper. |
