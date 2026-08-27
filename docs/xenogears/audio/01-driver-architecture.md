# Driver Architecture

## 1. Main Components

The Xenogears sound system has four main layers:

```text
WDS/SEDS/SMDS resources
        |
        v
sequence managers and logical tracks
        |
        v
staged voice state and global SPU masks
        |
        v
24 physical PlayStation SPU voices
        |
        v
stereo SPU output, reverb, and CD/XA input
```

The driver is part of the resident game executable. It is not code stored in
each song or effect.

## 2. Initialization

`InitializeSoundSystem` at `0x80037C80` performs the main setup:

1. Initialize SPU transfer and IRQ state.
2. Initialize SPU RAM allocation and loaded-bank lists.
3. Install the root-counter callback.
4. Create the permanent SEDS manager.
5. Clear voice ownership and pending SPU masks.
6. Initialize common volume state.
7. Select reverb mode 4 with depth zero.
8. Enable SPU reverb.

The permanent SEDS manager has 16 tracks. Each SEDS effect uses two tracks, so
the normal effect pool has eight two-channel slots.

## 3. Sequence Managers

A sequence manager is one playback context. Managers are linked through a
global list headed by `g_pSoundManagerListHead`.

A manager contains:

- Tempo and a 16.16 tick accumulator.
- Global manager volume, pitch, and pan adjustments.
- Bar/beat counters used by opcodes such as `0x97` and `0xF9`.
- The default WDS ID.
- A set of logical tracks.
- Optional SMDS percussion mappings.
- Loop and playback state.

The driver allocates approximately:

```text
0x94 + track_count * 0x158 bytes
```

Each track is approximately `0x158` bytes and contains sequence state,
automation state, four modulators, and staged voice data.

### SMDS managers

Starting an SMDS creates a dedicated manager sized for that score. The driver:

1. Reads the SMDS header.
2. Copies tempo/sequence defaults and the default WDS ID.
3. Installs every nonzero track script pointer.
4. Expands sparse percussion patches into a 96-entry lookup table.
5. Loads preset zero where possible.
6. Links the manager into the active-manager list.

SMDS logical track `n` is assigned physical voice index `n - 1`. Track zero has
physical index `0xFF` and does not directly start a voice. It is commonly used
as a conductor/control track.

### SEDS manager

SEDS banks are registered in a global bank list, but they do not each receive a
private manager. Effects run through the permanent 16-track SEDS manager.

Starting an effect:

1. Find the registered SEDS bank by ID.
2. Select the requested entry.
3. Read its volume and two channel script offsets.
4. Reserve or replace one pair of SEDS tracks.
5. Initialize both tracks with the bank's default WDS ID.
6. Activate the permanent manager.

SEDS track indices map to physical voices 8 through 23. These voices overlap
the range that a large SMDS can use. The ownership and priority system decides
which sound wins.

## 4. The 240 Hz Callback

`SoundSequencerCallback240Hz` at `0x8003C020` runs 240 times per second.

Its order is important:

1. Return early if global sound suspension flag `0x40` is set.
2. Increment the sound callback counter.
3. On alternating callbacks, update common-volume ramps and call
   `SpuSetCommonAttr`. This auxiliary work is 120 Hz.
4. Call `CommitPendingSpuVoiceWrites`. This writes staged voice registers and
   key-ons from the previous callback.
5. Walk every active manager and update manager fades and tempo state.
6. Subtract the manager tick increment from its accumulator.
7. For every emitted logical tick, call `AdvanceSequencerLogicalTick` and then
   `InterpretReadySequenceTracks`.
8. Walk managers again and update all track modulators at 240 Hz.
9. Call `StageTrackVoiceParameters` to calculate pitch, gain, pan, and dirty
   voice state.
10. Call `CommitPendingSpuVoiceKeyOffs`.
11. Complete pending SPU IRQ work and timing statistics.

This creates an important one-callback staging rule for the SPU interface:

- Most new voice parameters and key-ons generated during callback N reach the
  SPU at the beginning of callback N+1.
- Key-offs requested during callback N reach the SPU at the end of callback N.

Any analysis of live voice state must account for this one-callback separation
between staging and SPU commit.

## 5. Logical Ticks

The callback rate is fixed, but the musical tick rate depends on tempo.

The manager uses:

```text
accumulator -= tick_increment
while accumulator < 0:
    accumulator += 0x10000
    process one logical tick
```

Default values are:

```text
tempo       = 0x66
tempo scale = 0x100
increment   = 0x6600
```

The default rate is approximately 95.625 logical ticks per second:

```text
240 * 0x6600 / 65536
```

Tempo opcodes change the increment, not the 240 Hz callback frequency.

On each logical tick the driver updates slides, decrements duration and gate
counters, requests key-off when a gate reaches zero, and interprets tracks whose
duration reaches zero.

## 6. Track Interpretation

`InterpretReadySequenceTracks` at `0x8003C6E8` handles ready tracks.

It repeatedly consumes untimed commands until it reaches one of these:

- A note below `0x80`.
- Rest `0x80`.
- Tie `0x81`.
- Track termination.
- A command that transfers execution elsewhere.

The interpreter dispatches `0x80..0xFF` through a 128-entry function-pointer
table near `0x80050624`. Encoded sizes come from the byte table at `0x80050824`.

The track supports:

- One saved song-repeat point.
- A four-level counted-loop stack.
- Octave restoration across loops and repeat jumps.
- Lookahead to the next timed event.
- Duration and gate adjustment.
- Four independent 240 Hz modulators.

### Lookahead

After creating a timed event, the driver scans forward through untimed commands
and known loop paths.

It asks two questions:

1. Is the next timed event a tie (`0x81`)? If so, hold the current note.
2. Is the next timed event another note? If so, force linear release rate 6 one
   tick before re-articulation.

This behavior is required for correct note lengths. A note followed by a tie is
one continuous SPU voice, not a note that is released and restarted.

## 7. WDS Loading And SPU RAM

The WDS loading path at `0x80037FD8`:

1. Validate the WDS header.
2. Allocate or select an SPU RAM region.
3. Upload the ADPCM payload asynchronously.
4. Copy the WDS header and preset records into driver-owned main RAM.
5. Record the actual SPU address.
6. Link the bank into the loaded-WDS list.

The transfer queue contains eight entries. It uses `SpuSetTransferStartAddr`
and asynchronous SPU writes.

Loaded banks are looked up by numeric WDS ID. A sequence does not contain a
filename or disc path.

`LoadInstrumentVoiceParameters` at `0x8003E5BC` reads a 16-byte preset record
and stages:

- ADPCM start address.
- Repeat address.
- Pitch/tuning adjustment.
- ADSR rates.
- ADSR shape modes.

The retail driver does not consistently compare an instrument index with the
declared preset count. At least one retail sequence intentionally reads a
coherent record beyond the normal preset table.

## 8. Voice Ownership And Priority

The SPU has 24 physical voices. The driver keeps a 24-entry owner table.

`AssignAndKeyOnSpuVoice` at `0x8003EF04` performs arbitration:

1. Reject physical indices outside 0 through 23.
2. Check the current owner, if any.
3. Compare priority.
4. If replacement is allowed, request fast release for the previous owner.
5. Install the new owner pointer.
6. Set the pending key-on bit.

Default priorities recovered from initialization are:

```text
SMDS track: 0x0100
SEDS track: 0x0200
```

This allows an effect to replace a music voice when both target the same
physical channel.

`ScheduleSpuVoiceKeyOff` only acts if the requesting track still owns the
voice. This prevents an old track from keying off a newer sound after stealing
or replacement.

Stolen voices use release rate 6 before key-off. This reduces clicks and stale
tails.

## 9. Voice Register Staging

Tracks do not write SPU registers directly for every command.

`StageTrackVoiceParameters` at `0x8003EBF0` calculates:

- Final pitch from note, pitch modulation, and manager pitch.
- Final gain from manager volume, velocity, track volume, and volume modulation.
- Left/right voice volume from pan and pan modulation.
- Key-on and key-off requests.

Dirty flags mark fields that need hardware writes. The commit path handles:

- Left and right volume.
- Pitch.
- ADPCM start address.
- Repeat address.
- ADSR registers.
- FM enable mask.
- Noise enable mask.
- Reverb-send mask.

The final masks are 24-bit values because the SPU has 24 voices.

## 10. Reverb State

Reverb mode, depth, delay, feedback, and work RAM are global SPU state. They are
not private to a song, effect, manager, or voice.

`SetReverbModeDepthDelayFeedback` at `0x80038934` applies this state:

- Mode `-1` keeps the current mode.
- Mode `-2` makes no mode/depth change.
- Changing mode reallocates and clears the reverb work area.
- Delay and feedback are meaningful to the configurable PSYQ-style modes that
  use them.

An SMDS header stores initial mode, depth, delay, and feedback bytes. A song can
therefore establish global reverb state.

SEDS has no equivalent header. SEDS inherits the current state. Opcode `0xB8`
changes global depth/delay/feedback while keeping the current mode. Opcodes
`0xBA` and `0xBB` only enable or disable one voice's send to the global reverb.

## 11. Noise And FM Are Global Hardware Features

The SPU noise clock is global. Individual voices only choose whether to use the
noise source.

FM, called pitch modulation by the SPU, pairs voices. A modulated voice uses the
output of the immediately preceding physical voice. Voice assignment therefore
matters; FM cannot be reconstructed correctly from a logical track alone.

## 12. CD/XA Input

The SPU mixer has a separate stereo CD input. Xenogears uses this path for
streamed XA/movie audio.

CD audio can have:

- Independent left/right volume.
- Optional send to SPU reverb.
- Mixing with all 24 sequence voices.

WDS, SEDS, and SMDS do not contain that stream. XA sectors and movie audio use
the separate CD/XA input path and are outside the resource formats described
here.

## 13. Important Functions

| Address | Reverse-engineering name or role |
|---|---|
| `0x80037C80` | `InitializeSoundSystem` |
| `0x80037FD8` | Allocate/upload/register WDS |
| `0x80038428` | Validate/register SEDS |
| `0x80038934` | `SetReverbModeDepthDelayFeedback` |
| `0x80039850` | Create SMDS manager |
| `0x8003B148` | Allocate fixed-track manager |
| `0x8003B644` | Start a two-channel SEDS effect |
| `0x8003BFA0` | SPU transfer/IRQ bookkeeping |
| `0x8003C020` | `SoundSequencerCallback240Hz` |
| `0x8003C4C4` | `AdvanceSequencerLogicalTick` |
| `0x8003C6E8` | `InterpretReadySequenceTracks` |
| `0x8003CC84` | `ApplyPercussionNoteMapping` |
| `0x8003D070` | Handle external sequence opcode `0x9E` |
| `0x8003D4E4` | Handle reverb parameters `0xB8` |
| `0x8003D53C` | Enable reverb send `0xBA` |
| `0x8003D59C` | Disable reverb send `0xBB` |
| `0x8003E5BC` | Load WDS instrument parameters |
| `0x8003E83C` | Release physical voice ownership |
| `0x8003E900` | Commit pending SPU voice writes |
| `0x8003EB5C` | Commit pending key-offs |
| `0x8003EBF0` | Stage track voice parameters |
| `0x8003EEA0` | Convert Q8 semitone to SPU pitch |
| `0x8003EF04` | Assign and key on physical voice |
| `0x8003EFA0` | Schedule physical voice key-off |
| `0x8003EFE4` | Update track modulators |

## 14. What A Resource Does Not Capture

Even a valid resource does not record the complete live sound state. It omits
some or all of:

- Loaded-bank registry and duplicate WDS selection.
- Other active managers.
- Physical voice owners and priorities.
- Current noise clock.
- Current FM and reverb-send masks.
- Current global reverb mode/depth.
- Existing reverb RAM history.
- Current CD/XA stream.
- Exact phase within the 240 Hz callback.

This is why an isolated SEDS effect can be structurally correct but still differ
from the same effect heard during gameplay.
