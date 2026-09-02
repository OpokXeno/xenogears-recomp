# Generators And Determinism

## 1. Three Independent Generators

Xenogears does not have one random number generator. It has three, each with
its own algorithm and persistent state, and none of them read from or write
to another's state:

| Generator | Algorithm | State | Domain |
|---|---|---|---|
| Gameplay | PsyQ `rand`/`srand`, linear congruential | One 32-bit word at `0x8005A1FC` | Battle, Field, World Map, menus and shops |
| Movie | Combined dual linear-congruential, XOR-mixed | Two 32-bit words at `0x80076EF4`/`0x80076EF8` | Movie-overlay CD self-test and background decoration |
| Sound modulator | Two-round xorshift | One 32-bit word at `0x800594E4` | SPU sequencer modulator waveforms 6 and 7 |

All three are fully deterministic across a cold boot: none of them is ever
seeded from a hardware timer, the VBlank clock, or any other source of
real-world entropy. Section 5 states the consequence precisely.

## 2. The Gameplay Generator

### 2.1 Core Algorithm And Seed

Resident `rand` at `0x8003FA38` implements the textbook ANSI C reference
generator:

```text
seed = seed * 0x41C64E6D + 12345
return (seed >> 16) & 0x7FFF
```

The 32-bit seed lives at `0x8005A1FC`. `0x41C64E6D` (1103515245 decimal) and
`12345` are the classic constants shared by many C library implementations;
this is not a Xenogears-specific algorithm. Resident `srand` at `0x8003FA68`
sits immediately after `rand` in memory and simply overwrites the seed word
with its argument.

`0x8005A1FC` falls outside the executable's loaded text/data range
(`0x80010000`-`0x80059800`, from the PS-X EXE header), so it is BSS: zeroed at
process start with no value carried in the disc image. Section 2.5 covers what
that means in practice.

### 2.2 Byte-Range Wrapper

Resident `RandomU8RangeInclusive` at `0x8001BD40` is the primary entry point
used across the game. Given inclusive bounds `min` and `max` (both treated as
bytes):

```text
if min == 0xFF: return 0xFF
if max == 0: return 0
if min == max: return min

range = max - min
if range < 255:
    return (min + (rand() & 0xFF) % (range + 1)) & 0xFF
else:                              # range == 255: the full byte span
    return rand() & 0xFF
```

The `0xFF`/zero short-circuits are the "explicit handling for fixed zero and
`0xFF` sentinel bounds" that its own annotation describes; callers can use
`0xFF` as a caller-side "no roll" signal without it reaching the modulo path.
The full-span case skips the modulo/offset arithmetic entirely once
`min == 0` and `max == 255`.

### 2.3 Sixteen-Bit Range Wrappers

Three further functions reproduce the same shape at 16 bits, each calling
`rand` directly rather than through `RandomU8RangeInclusive`:

- `BattleRandomU16InRange` (`0x80089B50`, battle-overlay)
- `GearShopMenuGetRandomRangeValue` (`0x801C511C`, gear-shop-menu-overlay,
  byte-identical compiled size to the battle-overlay copy)
- `FieldParticlesRandRange` (`0x800A987C`, field-overlay, a shorter
  direct-modulo form without the sentinel handling)

All three, and `RandomU8RangeInclusive`, resolve to the one shared `rand` and
its one shared seed word. Overlays cannot call each other directly, so each
overlay that needs range-random values carries its own compiled copy of the
wrapper, but every copy calls back into the same resident `rand`. There is
exactly one gameplay random stream, not one per overlay.

### 2.4 Consumers Across Modules

This single stream backs every gameplay-facing random roll found in this
survey:

- Battle: hit/evasion resolution, damage variance, crisis/critical rolls,
  stat growth, enemy-script target selection (the `BattleEnemyScriptSelect
  Random*` family), confused-turn target choice, Attack-Level/Deathblow
  variety selection, and Battle Event opcodes `0x14`
  (`BattleEventOpcode14RandomVariable`) and `0x15`
  (`BattleEventOpcode15RandomRangeVariable`), both of which call
  `BattleRandomU16InRange` — scripted Battle Events draw from the same stream
  as ordinary combat rolls.
- Field: random-encounter countdown expiration and formation selection
  (`FieldRandomEncounterUpdate`), encounter timer initialization, scripted
  random turns, and the `RandVariable`/`MulVariableWithRand` Field script
  opcodes.
- World Map: `WorldMapSelectRandomEncounter` and the warp/shake camera tasks.
- Menus and shops: `GearShopMenuGetRandomRangeValue` (`0x801C511C`) is a real,
  correctly-implemented range-roll helper, but exhaustive static analysis
  found no caller for it anywhere — no direct call in any shard or overlay,
  no computed/indirect call within its own overlay, and no reference to its
  address in that overlay's raw binary as data. It appears to be dead code in
  the retail build rather than an active consumer of this stream; see
  [`menu/11`](../menu/11-gear-shop-tuning-and-preview.md#25-function-index) for
  where it's cataloged.
- Battling (the colosseum minigame) draws on the same `rand` for camera
  placement, particle jitter, and COM AI tactical randomization, all through
  its own overlay-local wrapper calls; the gameplay-affecting rolls are
  indexed in [`06-battling-rng.md`](06-battling-rng.md).
- The resident sprite animation VM shared by Field, Battle, and other
  sprite-driven modules has five dedicated random opcodes (`AC`, `C0`, `C1`,
  `C4`, `E5` — random angular/radial displacement, random position, and
  assign-random-below-immediate); see
  [`graphics/04` §10](../graphics/04-models-sprites-and-animation.md#10-sprite-animation-vm)
  for the full opcode catalog.

This list characterizes the stream's reach; it is not an exhaustive per-site
catalog of every roll and its exact formula (drop tables, individual
encounter weights, and similar detail belong in a follow-up reference, not
here).

### 2.5 Determinism

No call to `srand` (`0x8003FA68`) exists anywhere in the recompiled game —
not in the main executable, and not in any overlay. This was checked
exhaustively (every generated call site for that address, across every
shard and every overlay's compiled output) rather than sampled. Combined with
the BSS zero-initialization from 2.1, the gameplay seed begins every cold
boot at `0`, and its entire subsequent sequence is a pure function of how
many times, and in what order, code calls `rand`. Two runs that perform the
same sequence of actions from power-on produce the same sequence of rolls.

## 3. The Movie Generator

### 3.1 Combined Algorithm And State

`MovieRandom` at `0x80074AF0` (movie-overlay) is a self-contained generator
with no call into resident `rand`. It carries two independent 32-bit words,
`A` at `0x80076EF4` and `B` at `0x80076EF8`:

```text
A = 5*A + 1
B = 7*B + 3
combined = (A ^ B) + 1
A = combined
return abs(combined)
```

Both state words are BSS (also outside the loaded text/data range), so both
start at `0`. Unlike a plain single-term LCG, the additive constants on each
term prevent the all-zero degenerate state: the first call from a zero seed
returns `abs((5*0+1) ^ (7*0+3) + 1) = abs(1 ^ 3 + 1) = 3`, not `0`.

No seed-setting counterpart to this function exists; nothing outside
`MovieRandom` itself writes to either state word.

### 3.2 Consumers

Fourteen call sites, all within the movie-overlay:

- `MovieUpdateCdReadStressTest` uses it to pick sectors for a CD read-speed
  self-test — an engineering diagnostic, not a gameplay or presentation
  system.
- `MovieInitDarkAnimatedQuads`, `MovieUpdateDarkAnimatedQuad`,
  `MovieInitBrightAnimatedQuads`, and `MovieUpdateBrightAnimatedQuad` use it
  to drive the animated decorative quads shown around FMV playback.

### 3.3 Determinism

Same conclusion as Section 2.5: zero-initialized state, no external seed
call, fully deterministic from power-on.

## 4. The Sound Modulator Generator

### 4.1 Algorithm And Seed

Resident `GenerateSoundRandom15` at `0x8003F43C` is a two-round xorshift over
a single 32-bit state word at `0x800594E4`:

```text
x ^= x << 17
x ^= x >> 15        # arithmetic shift
state = x
return x & 0x7FFF
```

Resident `SeedSoundModulatorRandom` at `0x8003F42C` overwrites the state word
directly with its argument.

### 4.2 Boot Seeding

Unlike the other two generators, this one's storage location falls inside the
executable's loaded range, and it is explicitly seeded exactly once: resident
`InitializeSoundSubsystem` at `0x80037B88` writes the literal constant
`0x12345678` straight into `0x800594E4` as part of sound-engine startup, via
an inlined store rather than a call to `SeedSoundModulatorRandom` (which
itself has no callers anywhere). `0x12345678` is a conventional placeholder
constant, not a derived or time-based value — the boot-time seed is fixed,
not random.

### 4.3 Consumers

This generator backs modulator waveforms `6` and `7` documented in
[`docs/xenogears/audio/03-sequence-bytecode.md`](../audio/03-sequence-bytecode.md#6-modulator-model)
("Positive random value at period boundary" and "Bipolar random value at
period boundary"), implemented by `SoundModulatorTickUnipolarRandomHold`
(`0x8003F354`) and `SoundModulatorTickBipolarRandomHold` (`0x8003F3C0`). That
document describes the waveform's musical behavior; this is the generator
underneath it.

## 5. Determinism Summary

No generator in this survey draws on any source of real-world entropy — no
VBlank/VSync count, no controller-timing jitter, no CD-read latency. All
three begin every cold boot from a fixed state (zero for two of them, a fixed
literal constant for the third) and evolve purely as a function of how many
times, and in what order, the running game calls into them. This means:

- A tool driving the game through an identical sequence of inputs from
  power-on reproduces identical rolls in all three domains, including
  battle outcomes, encounter selection, and audio modulation.
- The three streams never interact. Consuming extra rolls from one (for
  example, opening a menu that happens to call `GearShopMenuGetRandomRangeValue`)
  never perturbs the other two.
- There is no in-game mechanism (found in this pass) that reseeds any of the
  three during play. Whatever call-order divergence exists between two
  runs is the only thing that can make their rolls diverge.

## 6. Function Index

| Address | Function |
|---|---|
| Resident `0x8003FA38` | `rand` — advance and return from the gameplay LCG |
| Resident `0x8003FA68` | `srand` — overwrite the gameplay LCG seed (never called anywhere) |
| Resident `0x8001BD40` | `RandomU8RangeInclusive` — sentinel-aware inclusive byte range over `rand` |
| Resident `0x8003F42C` | `SeedSoundModulatorRandom` — overwrite the sound xorshift state (never called anywhere) |
| Resident `0x8003F43C` | `GenerateSoundRandom15` — advance the sound xorshift and return its low 15 bits |
| Resident `0x8003F354` | `SoundModulatorTickUnipolarRandomHold` — sample-and-hold consumer of the sound generator |
| Resident `0x8003F3C0` | `SoundModulatorTickBipolarRandomHold` — bipolar sample-and-hold consumer |
| Resident `0x80037B88` | `InitializeSoundSubsystem` — seeds the sound generator with `0x12345678` at boot |
| `0x80089B50` | `BattleRandomU16InRange` (battle-overlay) — sentinel-aware inclusive 16-bit range over `rand` |
| `0x801C511C` | `GearShopMenuGetRandomRangeValue` (gear-shop-menu-overlay) — same wrapper, independently compiled |
| `0x800A987C` | `FieldParticlesRandRange` (field-overlay) — direct-modulo 16-bit range over `rand` |
| `0x801E65A4` | `BattleEventOpcode14RandomVariable` (battle-event-overlay) — stores a raw `BattleRandomU16InRange` roll |
| `0x801E65FC` | `BattleEventOpcode15RandomRangeVariable` (battle-event-overlay) — stores a bounded `BattleRandomU16InRange` roll |
| `0x80074AF0` | `MovieRandom` (movie-overlay) — advance the combined dual-LCG Movie generator |
