# Bouts, Replay, Results, And Progression

## 1. Terms And State

Battling uses four nested time scales:

| Term | Meaning |
|---|---|
| Display interval | One presentation pass paced by the display loop. |
| Simulation update | One gameplay transaction; bout update count is `0x8009294C`. |
| Bout | One fight ending when either fighter reaches knockout state or both do so together. |
| Round | The ordinal displayed when a new bout is initialized. The one-based counter is `0x80092950`. |
| Series | Consecutive bouts governed by the number-of-matches option and accumulated side wins. |

The principal runtime fields are:

| Address | Size | Meaning |
|---:|---:|---|
| `0x80092638` | 4 | Bout-finished latch. |
| `0x8009263C` | 4 | Opening countdown. |
| `0x80092640` | 4 | Post-knockout delay. |
| `0x80092890` | 4 | Winner side: `0`, `1`, or draw `2`. |
| `0x800928D4` | 1 | Live bout update enabled. |
| `0x800928FC` | 1 | Result-side input selector. |
| `0x80092918` | 4 | Draw count. |
| `0x8009294C` | 4 | Current bout update count. |
| `0x80092950` | 4 | Current one-based round number. |
| `0x8009881E` | 2 | Side 0 wins. |
| `0x80097102` | 2 | Side 1 wins. |

AI, Practice, and rubber-band behavior are documented in
[`06-ai-practice-and-rubber-band.md`](06-ai-practice-and-rubber-band.md).
Tutorial-controlled bouts are documented in
[`08-tutorial-and-attract-scripts.md`](08-tutorial-and-attract-scripts.md).

## 2. Field Handoff And Initial Activity

Field extended opcode `FE BF`, `SetupBattling`, stores six evaluated operands at
`0x8005061C..0x80050621` before requesting the Battling module. Field opcode
`FE C0`, `WriteBattlingMatchResultCode`, later copies byte `0x80050622` to a
Field script variable. The Field instruction table is cataloged in
[`Extended FE Opcodes`](../field/06-extended-opcodes.md).

| Address | Initial use |
|---:|---|
| `0x8005061C` | Session/scenario selector: `1` enables progression filtering; `2` enables the scripted scenario. |
| `0x8005061D` | Initial activity selector. |
| `0x8005061E` | Side 0 selected Gear ID. |
| `0x8005061F` | Side 1 selected Gear ID. |
| `0x80050620` | Number-of-matches setting. |
| `0x80050621` | COM difficulty. |

Initial activity selector `0`, `1`, or `2` starts activity `1`, Practice activity
`4`, or Tutorial activity `5`. Selected Gear IDs are copied to
`0x80092798/0x8009279C`; number of matches and difficulty are copied to
`0x80099D9F/0x80099D98`.

## 3. Bout Initialization

`BattlingInitializeBoutRuntime` at `0x80079B44` initializes both fighters,
effects, camera, UI, input, replay, and result state. It performs these bout
boundaries:

1. Set initial fighter headings to `+0x400` and `-0x400`.
2. Initialize each fighter through `0x80078F00`.
3. Reset effects, camera bounds, result state, input, and line queues.
4. Clear exported result byte `0x80050622`.
5. Clear the scripted-scenario climax latch.
6. Set opening countdown `0x8009263C = 90`.
7. Clear bout update count and knockout delay.
8. Increment round counter `0x80092950`.
9. Close any prior tutorial/result dialog.

Arena-variant rotation is gated by `0x8005061C`. Startup clears that byte before
the first bout, and the in-session progression path does not restore it, so a
normal Field-launched series retains variant `0`. If the gate is set, the latent
branch selects from `(round - 1) % 5`: rounds 4 and 5 use variants `1` and `2`,
while the other rounds use variant `0`.

## 4. Opening Countdown

`BattlingBoutStateUpdate` at `0x800751C8` decrements the opening counter once per
simulation update and draws a phase label:

| Counter | Ordinary bout | Practice | Bout control |
|---:|---|---|---|
| `90..60` | `ROUND n` | `PRACTICE` | Paused |
| `59..30` | `READY` | `START` | Paused |
| `29..1` | `FIGHT` | `FIGHT` | `0x800928D4 = 1` |
| `0` | No countdown label | No countdown label | Live |

Rubber-band mode adds `RUBBER BAND BATTLE` or `RUBBER BAND MODE` to the
ordinary or Practice presentation.

The bout update count starts advancing with the bout update and is retained for
replay length selection.

## 5. Knockout And Draw

Fighter flag `+0xD0:0x00800000` marks knockout. The first terminal update sets
the finished latch and classifies the outcome:

```text
if side0.KO and side1.KO:
    winner = 2
    draws += 1
else if side1.KO:
    winner = 0
    side0_wins += 1
else if side0.KO:
    winner = 1
    side1_wins += 1
```

The update also clears the opening label, selects result-side input, exports a
result code for a decisive outcome, and starts post-knockout counter
`0x80092640`.

During the delay, a decisive bout displays `KNOCK OUT!!`; a simultaneous
knockout displays `DRAW GAME`. When the old counter value exceeds `60`, on the
62nd post-knockout update, a draw enters state `2`, while a decisive bout enters
replay state `4`.

### 5.1 Opponent checklist update

When side 0 defeats a COM-controlled side 1 outside COM-vs-COM activity `3`,
the defeated fighter's Gear ID at `+0x909` is marked in the persistent checklist.
The checklist and Argento completion rule are specified in
[`Roster And Opponent Progression`](#10-roster-and-opponent-progression).

## 6. Replay History

Each fighter owns a 256-entry circular history at fighter `+0x09CC`. Every
entry is 12 bytes, so the history occupies exactly `0xC00` bytes through
`+0x15CB`.

```c
struct BattlingReplaySample {
    int16_t x;                     /* +0x00 */
    int16_t y;                     /* +0x02 */
    int16_t z;                     /* +0x04 */
    uint16_t facing_and_flags;     /* +0x06 */
    uint8_t animation_id;          /* +0x08 */
    uint8_t animation_advance;     /* +0x09 */
    uint16_t animation_state;      /* +0x0A */
};                                 /* size 0x0C */
```

| Fighter offset | Meaning |
|---:|---|
| `+0x15CC` | Current position/replay sample pointer. |
| `+0x15D0` | Successor sample used for animation state. |

`BattlingBoutSimulationUpdate` at `0x80079DF0` calculates the sample address:

```text
sample = fighter + 0x09CC + replay_cursor * 12
replay_cursor = u8(replay_cursor + 1)
```

`BattlingRecordReplayFrameAndAdvanceAnimation` at `0x80074BA4` stores position,
low 12 heading bits, animation ID, animation advance, event-frame state, and
the packed flags after advancing live animation.

### 6.1 Packed flags

| Sample field | Bit | Fighter state restored |
|---|---:|---|
| `facing_and_flags` | `0x1000` | Force animation restart on this sample. |
| `facing_and_flags` | `0x2000` | Fighter `+0xD0:0x00008000`. |
| `facing_and_flags` | `0x4000` | Fighter `+0xD0:0x00080000`. |
| `animation_state` | `0x0100` | Fighter `+0xD0:0x00000004`. |

The remaining low 12 bits of `facing_and_flags` hold fighter heading. Low byte
of `animation_state` holds the saved animation event/frame state.

## 7. Replay Rewind And Restoration

`BattlingBeginReplayRewind` at `0x8007A21C` receives the bout update count. For a
bout shorter than 255 updates, it rewinds the circular cursor by the complete
bout length and sets replay countdown to `bout_updates - 2`. For a bout of 255
updates or more, the replay countdown is capped at `0xFF`, selecting the latest
256-sample circular-history window.

Before playback it saves:

- both fighters' live positions and `0x100`-neutral display scales;
- two auxiliary world transforms;
- camera-related positions used by the result presentation.

`BattlingReplayUpdate` at `0x8007A344` advances the circular cursor, sets both
sample pointers, restores animation-restart markers at the oldest replay frame,
updates both fighter models, decodes packed flags, and draws `REPLAY` when the
replay display flag is enabled. Replay input can request early transition to
state `8`.

`BattlingRestoreReplayFrame` at `0x80074AB4` restores:

1. Signed X, Y, and Z.
2. Heading from the low 12 bits of `+0x06`.
3. Animation ID from `+0x08`.
4. Animation restart and visibility state.
5. Saved frame advance and animation events.

When the replay countdown reaches zero, state `8` calls
`BattlingRestorePostReplayArena` at `0x8007AC3C`. That routine restores all
saved live positions, restores display scales to their neutral `0x100`, mirrors the auxiliary
transforms into both render copies, selects the winner, and initializes the
victory camera.

## 8. Result-Code Selection

The exported byte at `0x80050622` is reset to zero for every bout.
`BattlingSelectResultCode` at `0x80075060` writes it after a decisive knockout.

### 8.1 Side 0 victory

When side 0 wins, the code depends only on side 0 remaining HP:

```text
if current_hp > maximum_hp * 0xE0 / 0xFF:
    result = 2
else if current_hp > maximum_hp * 0x10 / 0xFF:
    result = 1
else:
    result = 3
```

### 8.2 Side 1 victory

The side 1 victory path uses four result values at
`0x80099D80..0x80099D8C`, side 0 maximum HP, and side 1 remaining HP.
Using `C80`, `C84`, `C88`, and `C8C` for those four values:

```text
if C8C == 0 and C88 == 0:
    result = 0x88
else if C8C != 0 and C88 == 0:
    result = 0x82
else if C80 > side0.maximum_hp * 0xB0 / 0xFF:
    result = 0x83
else if C84 > side0.maximum_hp * 0xA0 / 0xFF:
    result = 0x84
else if C80 + C84 > side0.maximum_hp * 0xB0 / 0xFF:
    result = 0x85
else if side1.current_hp > side1.maximum_hp * 0xE0 / 0xFF:
    result = 0x86
else if side1.current_hp > side1.maximum_hp * 0x10 / 0xFF:
    result = 0x81
else:
    result = 0x87
```

Scripted-scenario climax completion exports `0x7F` instead of using this
classifier.

## 9. Victory Presentation And Showcase

`BattlingPrepareVictoryPresentation` at `0x8007A884` resets shared visual and
audio effects, selects result-message style, and restores fighter display
scales. `BattlingResultsUpdate` at `0x8007AE10` displays one of:

| Activity/outcome | Text |
|---|---|
| Player 1 wins activity `1` | `YOU WERE VICTORIOUS` |
| Side 1 COM wins activity `1` | `YOU WERE DEFEATED` and `BY <Gear>` |
| Player 1 wins activity `2` | `1PLAYER VICTORY` |
| Player 2 wins activity `2` | `2PLAYER VICTORY` |
| Side 0 wins activity `3` | `COM1 VICTORY` |
| Side 1 wins activity `3` | `COM2 VICTORY` |

The update selects a timed winner animation through `0x8007A6D0`, applies the
defeated pose through `0x8007A730`, and maintains the orbiting victory camera at
`0x8007A958`. Confirmation can leave the result state early. If the configured
result presentation repeats, its `0x96`-update timer restarts before another
cycle.

Series-end state `6` calls `BattlingInitializeWinnerShowcase` at `0x800725B0`.
The showcase selects the side with the greater accumulated win count and draws:

- `WINNER`;
- the winning Gear name;
- difficulty `LEVEL` outside two-player and COM-vs-COM activities;
- `MATCHES` and the current series values;
- a rotating Gear model using per-Gear camera, light, and pose values.

## 10. Roster And Opponent Progression

`BattlingBuildGearRoster` at `0x8007EEE8` scans 49 descriptors at `0x80091964`.

```c
struct BattlingGearDescriptor {
    uint32_t id;                   /* +0x00 */
    const char *resource_name;     /* +0x04 */
    const char *display_name;      /* +0x08 */
};                                 /* size 0x0C */
```

Runtime Gear metadata uses stride `0x20` from the pointer at `0x80092874`.
Signed halfword `+0x04` is the roster progression requirement. In a
progression-gated Field session, a descriptor is included when:

```text
gear_metadata.required_progress <= *(uint16_t *)0x8006EF64
```

Menu and standalone selection can construct the complete 49-entry roster.
Selected descriptor pointers are written to the array at `0x800928EC`; count is
stored at `0x80092888`.

### 10.1 Gear IDs

| ID | Gear | ID | Gear | ID | Gear |
|---:|---|---:|---|---:|---|
| `0` | WELTALL | `17` | GANADOR | `34` | WORKER |
| `1` | VIERGE | `18` | TITAN | `35` | DOZER |
| `2` | HEIMDAL | `19` | WSHAVER | `36` | DEATH |
| `3` | BRIGANDIER | `20` | FIREWHEEL | `37` | MERMAN |
| `4` | RENMAZUO | `21` | SILVERSTAR | `38` | SALVAGER |
| `5` | STIER | `22` | ARGENTO | `39` | TROOPER |
| `6` | BLADEGASH | `23` | MUSHA | `40` | TWINBURNER |
| `7` | SIEBZEHN | `24` | HATAMOTO | `41` | S-TROOPER |
| `8` | CRESCENS | `25` | BACKFIRER | `42` | S-TRIPPER |
| `9` | CHU-CHU | `26` | SHINOBI | `43` | SUFAL |
| `10` | WELTALL-2 | `27` | WYRM | `44` | EG-GUNNER |
| `11` | XENOGEARS | `28` | TIN ROBO | `45` | EG-ARMOR |
| `12` | EL-REGRS | `29` | RANKAR | `46` | PEDESTAL |
| `13` | EL-FENRIR | `30` | ETONE1 | `47` | EDIN |
| `14` | EL-ANDVARI | `31` | ETONE2 | `48` | EG-BLADE |
| `15` | EL-RENMAZUO | `32` | GOLEM |  |  |
| `16` | EL-STIER | `33` | FIXBOT |  |  |

### 10.2 Opponent checklist

The persistent opponent checklist is eight bytes at
`0x8006F978..0x8006F97F`. `BattlingMarkOpponentDefeated` at `0x800888B0` and
`BattlingIsOpponentDefeated` at `0x800888E4` use:

```text
byte_index = gear_id >> 3
bit_mask   = 1 << (gear_id & 7)
```

Completion checks IDs `0..48`, skipping ID `22`. When all other 48 bits are
set, `BattlingCheckOpponentChecklistCompletion` at `0x800889C8` sets completion bit
`0x00010000` in `0x8006F980` and appends descriptor `0x80091A6C`, Argento, to
the progression-gated roster.

When a completed save is loaded, the completion bit remains set and the eight
checklist bytes are cleared for a new opponent cycle. Argento remains available
because roster append tests the completion bit rather than the current cycle
bits.

## 11. Resident Option Word

An option-format payload and Argento completion share word `0x8006F980`:

| Bits | Meaning |
|---:|---|
| `0..3` | Format marker, value `1`. |
| `4` | Controller 1 vibration. |
| `5` | Controller 2 vibration. |
| `6..12` | Number-of-matches value. |
| `13..15` | COM difficulty. |
| `16` | Argento completion. |
| `17..31` | Preserved resident state. |

`BattlingOptionsSave` at `0x80088A40` packs the low halfword:

```text
packed = 1
       | (vibration_1 & 1) << 4
       | (vibration_2 & 1) << 5
       | (number_of_matches & 0x7F) << 6
       | (difficulty & 7) << 13
```

An option word whose low nibble differs from `1` receives these defaults:

| Option | Default |
|---|---:|
| Controller 1 vibration | `0` |
| Controller 2 vibration | `0` |
| Number of matches | `2` |
| Difficulty | `0`, Easy |

Packing and loading the low option fields are gated by Field-session byte
`0x8005061C`. Startup consumes and clears that byte before ordinary activity.
On exit, `BattlingExitToField` at `0x800851D4` invokes the guarded packer while
the byte is zero, requests resident module `1`, then sets the byte to `1`.
Consequently menu changes do not rewrite the low option fields in that exit
transaction. The opponent-checklist completion path updates bit `16`
independently.

## 12. Related Formats

Battling arena cells, texture flags, and the fixed `128 * 128` heightfield are
specified in
[`Resource Formats`](../graphics/03-resource-formats.md#8-battling-heightfield-and-texture-flags).
Battling HUD, model, render-tree, and frame submission are specified in
[`Battle, UI, And Movies`](../graphics/06-battle-ui-and-movies.md#5-battling-arena).
