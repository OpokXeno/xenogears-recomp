# Sequence Bytecode And Opcodes

## 1. Bytecode Model

SEDS channel scripts and SMDS track scripts use the same bytecode.

- Bytes `0x00..0x7F` begin a note.
- Bytes `0x80..0xFF` are commands.
- Commands use fixed encoded sizes from the retail driver table at
  `0x80050824`.
- A track executes untimed commands immediately until it reaches a note, rest,
  tie, transfer, or end.

## 2. Status Names Used Below

| Status | Meaning |
|---|---|
| Full | The offline renderer models the relevant behavior |
| Bookkeeping | The driver changes musical counters that do not directly feed PCM |
| Partial | The renderer accepts the command but omits some driver behavior |
| Context | Correct execution needs state outside the isolated resource |
| Unsupported | The command has a known size but the renderer has no handler |
| Driver no-op | The retail handler only consumes bytes; renderer support is unnecessary for current content |
| Reserved | The extractor rejects the command intentionally |

The `Uses` column is a static reachable-instruction census over the current 63
SMDS files and 3,042 active SEDS entries. Counted loop iterations are not
multiplied. A zero means the command is not used by the current retail corpus.

## 3. Note Encoding: `0x00..0x7F`

A note is encoded as:

```text
byte 0: velocity, 0x00..0x7F
byte 1: selector = semitone * 19 + duration_index
byte 2: explicit duration, only when duration_index is zero
```

The selector must be below 228.

```text
semitone       = selector // 19
duration_index = selector % 19
```

The encoded size is two bytes for table durations and three bytes for an
explicit duration.

There are 54,778 distinct reachable note instructions in the current corpus.

### Duration table

| Index | Logical ticks | Index | Logical ticks |
|---:|---:|---:|---:|
| 0 | explicit byte | 10 | 18 |
| 1 | 192 | 11 | 16 |
| 2 | 144 | 12 | 12 |
| 3 | 96 | 13 | 9 |
| 4 | 72 | 14 | 8 |
| 5 | 64 | 15 | 6 |
| 6 | 48 | 16 | 4 |
| 7 | 36 | 17 | 3 |
| 8 | 32 | 18 | 2 |
| 9 | 24 |  |  |

Normal note pitch is:

```text
(track_octave + semitone) * 256
```

This is Q8 semitone notation. WDS preset tuning and track pitch state are added
later.

## 4. Timing, Gate, And Tie Rules

For every timed note/rest/tie:

```text
duration = base_duration + signed_duration_adjust
```

If the result is nonpositive, the driver adds the base duration again and
updates the persistent adjustment. Final duration is at least one tick.

Gate selection is:

| Condition | Gate |
|---|---:|
| Tie follows or persistent hold is active | `0x7FFF` |
| Gate mode `0x0F` | `max(1, duration - 1)` |
| Gate mode `0x10` | `duration` |
| Other gate mode | `max(1, duration * mode >> 4)` |

Duration and gate decrement once per logical tick. Gate zero requests key-off.

The driver looks ahead through commands and loop paths:

- If the next timed event is `0x81`, it holds the current note.
- If the next timed event is another note, it sets linear release rate 6 one
  tick before the next note.

The corpus contains 9,126 distinct note-to-tie sources and 24,587 distinct
note-to-note sources.

## 5. Complete Opcode Table

### `0x80..0x8F`

| Op | Bytes | Uses | Status | Meaning |
|---:|---:|---:|---|---|
| `80` | 2 | 28,922 | Full | Rest for operand ticks; key off current voice immediately |
| `81` | 2 | 14,853 | Full | Tie/wait without starting a new note |
| `82` | 1 | 0 | Reserved | Reserved one-byte command |
| `83` | 1 | 0 | Reserved | Reserved one-byte command |
| `84` | 1 | 0 | Reserved | Reserved one-byte command |
| `85` | 1 | 0 | Reserved | Reserved one-byte command |
| `86` | 1 | 0 | Reserved | Reserved one-byte command |
| `87` | 1 | 0 | Reserved | Reserved one-byte command |
| `88` | 1 | 0 | Reserved | Reserved one-byte command |
| `89` | 1 | 0 | Reserved | Reserved one-byte command |
| `8A` | 1 | 0 | Driver no-op | Retail handler consumes no operands and returns |
| `8B` | 1 | 0 | Reserved | Reserved one-byte command |
| `8C` | 1 | 0 | Reserved | Reserved one-byte command |
| `8D` | 2 | 0 | Unsupported | Conditional update of saved sequence position |
| `8E` | 4 | 0 | Driver no-op | Retail handler skips three operands |
| `8F` | 1 | 0 | Driver no-op | Retail handler returns |

### `0x90..0x9F`: control flow

| Op | Bytes | Uses | Status | Meaning |
|---:|---:|---:|---|---|
| `90` | 1 | 7,053 | Full | Jump to saved `91` point; end track if none exists |
| `91` | 1 | 1,237 | Full | Save next PC and current octave as song-repeat point |
| `92` | 1 | 0 | Reserved | Reserved |
| `93` | 1 | 0 | Reserved | Reserved |
| `94` | 2 | 10,105 | Full | Set octave/base note to operand times 12 |
| `95` | 1 | 3,397 | Full | Raise octave by 12 semitones |
| `96` | 1 | 3,286 | Full | Lower octave by 12 semitones |
| `97` | 3 | 82 | Bookkeeping | Configure manager bar/beat timing values, including `192 / operand1` |
| `98` | 2 | 7,032 | Full | Begin counted loop; maximum nesting depth is four |
| `99` | 1 | 7,032 | Full | Repeat or end counted loop; restore loop-start octave |
| `9A` | 1 | 67 | Full | Final-iteration loop break/alternate ending |
| `9B` | 1 | 0 | Reserved | Reserved |
| `9C` | 4 | 0 | Partial | Calls a manager/global sound routine; renderer records approximation |
| `9D` | 4 | 0 | Partial | Calls a manager/global sound routine; renderer records approximation |
| `9E` | 4 | 0 | Context | Transfer to external group `u16` and entry `u8` |
| `9F` | 1 | 0 | Reserved | Reserved |

`0x98 count` stores `(count - 1) & 0xFF`. Positive `count` values execute the
body that many times. Count zero wraps and requests 256 iterations.

`0x9A` exits only on the final iteration, using the exit learned from an earlier
pass through `0x99`.

### `0xA0..0xAF`: manager state and instruments

| Op | Bytes | Uses | Status | Meaning |
|---:|---:|---:|---|---|
| `A0` | 2 | 495 | Full | Set tempo byte immediately |
| `A1` | 2 | 0 | Partial | Relative tempo-state change; renderer records approximation |
| `A2` | 3 | 15 | Full | Tempo slide `[duration, target]` over logical ticks |
| `A3` | 1 | 0 | Reserved | Reserved |
| `A4` | 2 | 0 | Unsupported | Set manager state field |
| `A5` | 2 | 0 | Unsupported | Set manager state field |
| `A6` | 2 | 0 | Full | Set manager volume to `operand << 24` |
| `A7` | 3 | 0 | Partial | Manager-volume interpolation; renderer records approximation |
| `A8` | 1 | 0 | Reserved | Reserved |
| `A9` | 2 | 244 | Full | Set gate mode |
| `AA` | 2 | 0 | Partial | Release/reassign physical voice; renderer records approximation |
| `AB` | 1 | 0 | Reserved | Reserved |
| `AC` | 2 | 5,002 | Full | Select and load preset from current WDS |
| `AD` | 2 | 415 | Full | Add signed duration adjustment; zero resets it |
| `AE` | 1 | 175 | Full | Enable percussion mapping |
| `AF` | 1 | 47 | Full | Disable percussion mapping |

### `0xB0..0xBF`: hold, FM, noise, and reverb

| Op | Bytes | Uses | Status | Meaning |
|---:|---:|---:|---|---|
| `B0` | 1 | 1,021 | Full | Enable persistent note hold |
| `B1` | 1 | 226 | Full | Disable persistent note hold |
| `B2` | 1 | 1,676 | Full | Enable SPU pitch modulation/FM for eligible voice pair |
| `B3` | 1 | 180 | Full | Disable FM |
| `B4` | 2 | 410 | Full | Set global noise clock and enable noise on track |
| `B5` | 2 | 453 | Full | Add to global noise clock modulo 64 and enable noise |
| `B6` | 1 | 0 | Full | Enable noise without changing clock |
| `B7` | 1 | 21 | Full | Disable noise |
| `B8` | 4 | 16 | Partial | Set global depth, delay, feedback while keeping reverb mode |
| `B9` | 1 | 0 | Reserved | Reserved |
| `BA` | 1 | 4,814 | Full/Context | Enable current voice's reverb send |
| `BB` | 1 | 342 | Full | Disable current voice's reverb send |
| `BC` | 4 | 0 | Driver no-op | Retail handler skips three operands |
| `BD` | 1 | 0 | Driver no-op | Retail handler returns |
| `BE` | 1 | 0 | Driver no-op | Retail handler returns |
| `BF` | 1 | 0 | Reserved | Reserved |

The retail `0xB8` handler uses all three operands. The offline mode-4 renderer
currently automates only signed depth. Every retail `0xB8` found in the current
corpus has zero delay and feedback, so this partial implementation does not
change current corpus output.

SEDS still needs inherited initial reverb state even when it contains `0xBA` or
later `0xB8` commands.

### `0xC0..0xCF`: ADSR and mode controls

| Op | Bytes | Uses | Status | Meaning |
|---:|---:|---:|---|---|
| `C0` | 1 | 572 | Full | Reload current preset from current WDS |
| `C1` | 4 | 0 | Full | Replace packed mode fields from three nibbles/bytes |
| `C2` | 2 | 3,413 | Full | Set attack rate, ADSR bits `0..6` |
| `C3` | 2 | 0 | Full | Set decay rate, ADSR bits `8..11` |
| `C4` | 2 | 5,189 | Full | Set sustain rate, ADSR bits `16..22` |
| `C5` | 2 | 155 | Full | Set release rate, ADSR bits `24..28` |
| `C6` | 2 | 0 | Full | Set sustain level, ADSR bits `12..15` |
| `C7` | 3 | 198 | Full | Set decay rate and sustain level together |
| `C8` | 2 | 0 | Full | Replace mode bits `0..2` |
| `C9` | 2 | 410 | Full | Replace mode bits `4..6` |
| `CA` | 2 | 144 | Full | Replace mode bits `8..10` |
| `CB` | 1 | 0 | Reserved | Reserved |
| `CC` | 1 | 0 | Reserved | Reserved |
| `CD` | 1 | 0 | Reserved | Reserved |
| `CE` | 1 | 0 | Reserved | Reserved |
| `CF` | 1 | 0 | Reserved | Reserved |

These commands update track instrument state for later note starts. Reloading a
preset through `0xAC`, `0xC0`, or `0xFC` restores ADSR/modes from WDS and can
overwrite earlier command overrides.

### `0xD0..0xDF`: pitch and pitch modulation

| Op | Bytes | Uses | Status | Meaning |
|---:|---:|---:|---|---|
| `D0` | 2 | 289 | Full | Set signed pitch offset in 1/8-semitone steps |
| `D1` | 2 | 908 | Full | Add signed pitch offset in 1/8-semitone steps |
| `D2` | 2 | 192 | Full | Add signed pitch offset in 1/32-semitone steps |
| `D3` | 3 | 130 | Full | Add signed 16-bit Q8 pitch delta, high byte first |
| `D4` | 3 | 9,969 | Full | Relative pitch slide `[duration, signed semitones]` |
| `D5` | 1 | 2 | Full | Toggle continuous pitch-slide mode |
| `D6` | 2 | 9 | Full | Set future-note portamento duration |
| `D7` | 2 | 145 | Full | Set pitch-modulator ramp-in speed |
| `D8` | 4 | 131 | Full | Configure standard pitch modulation in slot 0 |
| `D9` | 4 | 1,987 | Full | Configure general pitch modulation in slot 0 |
| `DA` | 1 | 6 | Full | Enable pitch modulator slot 0 |
| `DB` | 1 | 296 | Full | Disable pitch modulator slot 0 |
| `DC` | 1 | 1 | Full | Cancel pitch slide and portamento |
| `DD` | 1 | 0 | Reserved | Reserved |
| `DE` | 1 | 0 | Reserved | Reserved |
| `DF` | 1 | 0 | Reserved | Reserved |

`0xD0` and `0xD1` multiply the signed operand by 32 Q8 units. Since one
semitone is 256 Q8 units, this is one eighth of a semitone.

`0xD2` multiplies by 8 Q8 units, or one thirty-second of a semitone.

`0xD0` through `0xD3` only change the persistent offset used when a later note
is constructed. They mark pitch state dirty, so retail can rewrite the same SPU
pitch for an already-owned voice, but they do not retune the currently sounding
note. Active-note movement instead comes from slides, portamento, and pitch
modulation.

### `0xE0..0xEF`: volume, pan, and modulators

| Op | Bytes | Uses | Status | Meaning |
|---:|---:|---:|---|---|
| `E0` | 2 | 5,370 | Full | Set track volume; cancel current volume slide |
| `E1` | 2 | 37 | Full | Add signed track-volume delta; cancel slide |
| `E2` | 3 | 4,948 | Full | Volume slide `[duration, signed target]` |
| `E3` | 2 | 132 | Full | Set volume-modulator ramp-in speed |
| `E4` | 4 | 6 | Full | Configure standard volume modulation in slot 1 |
| `E5` | 4 | 922 | Full | Configure general volume modulation in slot 1 |
| `E6` | 1 | 2 | Full | Enable volume modulator slot 1 |
| `E7` | 1 | 138 | Full | Disable volume modulator slot 1 |
| `E8` | 2 | 2,353 | Full | Set pan; cancel current pan slide |
| `E9` | 2 | 1 | Full | Add signed pan delta |
| `EA` | 3 | 230 | Full | Pan slide; reproduces retail terminal-delta quirk |
| `EB` | 2 | 65 | Full | Set pan-modulator ramp-in speed |
| `EC` | 4 | 21 | Full | Configure standard pan modulation in slot 2 |
| `ED` | 4 | 464 | Full | Configure general pan modulation in slot 2 |
| `EE` | 1 | 0 | Full | Enable pan modulator slot 2 |
| `EF` | 1 | 83 | Full | Disable pan modulator slot 2 |

The retail `0xEA` handler stores `(target - current) << 8` as both the source of
the step and the terminal value. This looks unusual, but the renderer preserves
that behavior rather than snapping to `target << 8`.

### `0xF0..0xFF`: general modulators and bank controls

| Op | Bytes | Uses | Status | Meaning |
|---:|---:|---:|---|---|
| `F0` | 4 | 89 | Full | Select/configure modulator `[slot, config, target]` |
| `F1` | 4 | 89 | Full | Set selected modulator period and signed amplitude |
| `F2` | 3 | 4 | Full | Set selected modulator delay and ramp-in |
| `F3` | 1 | 0 | Reserved | Reserved |
| `F4` | 1 | 0 | Reserved | Reserved |
| `F5` | 2 | 0 | Driver no-op | Retail handler skips one operand |
| `F6` | 2 | 89 | Full | Reinitialize and enable selected slot |
| `F7` | 2 | 2 | Full | Disable selected slot |
| `F8` | 4 | 220 | Full | Configure per-note volume sweep `[start, duration, target]` |
| `F9` | 3 | 6 | Bookkeeping | Update manager bar/beat counters |
| `FA` | 1 | 0 | Reserved | Reserved |
| `FB` | 1 | 0 | Reserved | Reserved |
| `FC` | 3 | 8,230 | Full | Set WDS and preset, then load immediately |
| `FD` | 2 | 0 | Full | Set tempo scale to `operand << 8`; zero is no-op |
| `FE` | 2 | 274 | Full | Change selected WDS ID without loading a preset |
| `FF` | 1 | 0 | Unsupported | Check live SPU envelope and possibly release/deactivate voice |

## 6. Modulator Model

Every track has four modulator slots. Known targets are:

| Target | Destination |
|---:|---|
| 0 | Pitch |
| 1 | Volume attenuation |
| 2 | Pan |

Modulators run on every 240 Hz callback, not only on logical musical ticks.

Period byte conversion is:

```text
period = value + value * value // 64
```

Delay units are four callbacks. A modulator can restart on every note or keep
its phase, depending on configuration bit `0x10`.

Implemented waveform arithmetic is:

| Wave | Behavior |
|---:|---|
| 0 | Alternate zero and positive amplitude |
| 1 | Alternate positive and negative amplitude |
| 2 | Linear ramp with direction reversal |
| 3 | Piecewise/asymmetric ramp with alternating phase lengths |
| 4 | Rising ramp reset at period boundary |
| 5 | Same implemented arithmetic as waveform 4 |
| 6 | Positive random value at period boundary |
| 7 | Bipolar random value at period boundary |
| 8..15 | Disable and produce zero |

These descriptions name the arithmetic, not official musical waveform names.

## 7. Instrument Lifetime

The current selected WDS ID and the currently loaded instrument WDS ID are
separate state.

```text
AC preset       -> load from selected WDS
C0              -> reload from selected WDS
FC wds, preset  -> select and load both
FE wds          -> select only
```

Therefore:

```text
AC 1
FE 8
note
```

still uses preset 1 from the old loaded bank. A later load command moves the
instrument to WDS 8.

## 8. Current Corpus Coverage

No reachable retail sequence currently uses any opcode that remains unsupported
or context-dependent:

```text
8A 8D 8E 8F 9C 9D 9E A1 A4 A5 A7 AA BC BD BE F5 FF
```

The byte values may still appear inside operands or ADPCM data. The statement
above refers only to decoded, reachable sequence instructions.

Opcodes `0x97` and `0xF9` do occur, but their driver effects are musical meter
bookkeeping and do not directly alter generated PCM.
