# Battle Event Objects And Namespaces

Battle Event runs alongside central Battle. It does not own combat: it borrows
resident Battle systems for rendering, combatant sprites and Gears, the camera,
faders, dialogue windows and audio. Each namespace groups operations by the
object they affect. See [`OPERATION_CATALOG.md`](OPERATION_CATALOG.md) for every
operand.

## 1. Identifier Spaces

Several operand families look alike but address different objects:

| Operand family | Used by | Resolution |
|---|---|---|
| Event entity ID | `flow.start_script*` | Index into the Event entity array (`0..entity_count-1`). |
| Event sprite alias | `sprite.*` `sprite` argument | `value - 0xF3` selects the Event entity that owns the sprite; `0xF3` is entity 0. |
| Combatant alias | `actor.play_mecha_animation*` `actor` and `target` arguments | `value - 0xF3` is a combatant slot: `0xF3..0xF5` are party slots 0..2, `0xF6` is enemy slot 3. `play_mecha_animation_wait` also records completion in the Event entity with the same index. |
| Battle visual character | `actor.move/move_eased/play_animation/restart_animation/reset_frame/clear_animation_sync/load_sprite/swap_sprite/suppress_critical_pose` | Values below `0x10` are character IDs searched among the three active party members (party slot 0 when absent); values `0x10` and above map to visual slot `value - 0x0D`, so `0x10` is enemy slot 3. |
| Portrait | `dialogue.set_portrait`, `dialogue.show_with_portrait` | A portrait ID; `0xF3..0xFF` read the resident party-ID array, so `0xF3..0xF5` are the active party's portraits. |
| Variable offset | variable operands | Byte offset into the 512-entry signed 16-bit bank. |

The decompiler renders these arguments in hexadecimal so aliases stay visible.

## 2. Namespaces

### `flow`

Scheduling and control: `stop`, `goto`, conditional jumps, starting scripts in
other entities' slots (optionally waiting until they begin or finish),
`flow.wait(units)` (each unit is two Battle input updates), entity priority,
`flow.yield_to_battle()` (return to Battle after the current pass) and two
no-ops. `flow.wait` counts down only in cutscene mode.
Polling operations hold their PC until complete, so a waiting entity keeps its
slot while others run.

### `state`

The Event variable bank: assignment, arithmetic, bit operations, shifts,
random numbers, multiply and divide. Values are signed 16-bit and wrap.
Resident Battle writes `engine.*` variables at exit, and Enemy AI opcode `0x70`
can write indices `0..255`; other meanings belong to each script.

### `dialogue`

Blocking message windows. `dialogue.set_window(x, y, width, height, flags)`
configures the next message (zero selects each geometry default and
`0x7FFF` auto-placement); `dialogue.show(message_id, flags)` uses the current
entity's portrait, set with `dialogue.set_portrait`. Messages come from the
paired `dialog.bin`. Flag bits: `0` mirrored layout, `1` no portrait, `2` lower
automatic placement, `3` suppress window/portrait/cursor management, `4`
alternate window style.

### `camera`

`camera.set_target(x, y, z, frames)` moves the Event camera target.
`actor.play_mecha_animation_wait` hands the camera to the mecha animation while
it plays.

### `visual`

Screen faders. The Battle fader draws a full-screen rectangle whose colour moves
from its current value to a target over `duration * 2` frames; `mode` is the GPU
blend mode (`0` average, `1` add, `2` subtract, `3` add a quarter). A fader whose
colour reaches zero removes itself. `visual.fade_out(duration)` subtracts toward
white, darkening the screen to black; `visual.fade_in(duration)` returns the
subtraction to zero. `visual.create_fader(mode, red, green, blue, duration)`
sets the mode and colour explicitly.

### `audio`

Event music (`load_music`, `load_music_muted`, `set_music_volume`,
`set_music_muted`, `stop_music`), sound effects and sequence entries from the
Event bank (`bank` 0) or resident Battle bank (nonzero), and blocking indexed
clips. `audio.set_sound_volume` changes the volume of an effect that is already
playing; scripts ramp it in loops to fade effects in and out. Music ID `N` loads
directory `0x20` file `N + 4`.

### `sprite`

Event-created sprites owned by an Event entity: create from a file-3 resource
index, play an animation, destroy (optionally resetting camera state), and clear
the resident sprites' alive flags.

### `actor`

Combatant visuals: mecha animations against a resident target (with Event
camera control and waiting, or without either), character sprite animations,
frame resets, linear and eased moves (polled until complete), sprite replacement
and swaps, clearing the mechas' Event-animation flags, and suppressing a mecha's
critical-HP idle pose.

### `battle`

Central Battle mode (cutscene or combat) and outcome: end the battle
successfully, silent Result, turning a party defeat into an Event victory,
queueing a follow-up battle, Field/World return, return mode, movie playback,
leader Gear transformation, slot 0 Hyper Mode, Gear turn UI and render workspace
helpers, and animation data cleanup.

### `event`

`event.stop_for_final_handoff()` permits the final Event handoff and stops the
Event VM while central combat continues.

## 3. Common Patterns

**Cutscene conductor.** Entity 0's `start` program runs the scene: it creates
Event sprites, plays mecha animations, fades in, and starts dialogue held by
other entities with `flow.start_script_wait_finished(entity, entry, 3)`. Each
speaking entity sets its portrait in `start` and keeps one message per entry.
Scenes that hand back to combat end with `battle.enter_combat_mode();
flow.yield_to_battle();`.

**Exit dispatcher.** Five retail scripts branch on
`engine.battle_exit_signal` in entity 0's `idle` program, which runs after the
`start` program stops: each comparison jumps to the code for one exit value, and
the unreachable `goto` placed after each case appears as orphan code. No retail
script reads the `party*_exit_status` variables.

**Parallel motion.** Several entities each own one mecha or sprite; the conductor
starts their entries without waiting, then waits on the last one.
