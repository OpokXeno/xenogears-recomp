# PlayStation SPU And Offline Rendering

## 1. SPU Overview

The original PlayStation SPU provides:

- 24 independent sample voices.
- 512 KiB of SPU RAM.
- PS1 ADPCM decoding.
- Per-voice pitch and Gaussian interpolation.
- Per-voice ADSR envelopes.
- Independent left/right voice volume.
- A shared hardware noise source.
- Paired pitch modulation, commonly called FM or PMON.
- A global reverb processor and per-voice reverb sends.
- Stereo CD/XA input.
- 44,100 Hz stereo output.

Xenogears performs music logic in software at 240 Hz, then stages SPU register
writes. The SPU itself produces audio samples at 44.1 kHz.

## 2. Offline Rendering Pipeline

The bundle reconstructs sequence audio in this order:

```text
SEDS/SMDS bytecode at 240 Hz
        |
        v
note events and parameter automation
        |
        v
WDS preset and PS1 ADPCM blocks
        |
        v
ADPCM decode and Gaussian interpolation
        |
        +---- optional global noise substitution
        |
        v
ADSR envelope
        |
        +---- capture mono source for paired FM
        |
        v
voice gain and left/right pan
        |
        +---- optional reverb-send buffer
        |
        v
sum all voices in a wide integer mix
        |
        v
mode-4 reverb, fixed main volume, signed 16-bit clamp
        |
        v
stereo PCM WAV at 44,100 Hz
```

The renderer does not normalize or apply a limiter after reconstruction. Values
are saturated where the emulated SPU path saturates.

## 3. Callback-To-Sample Timing

Sequence events use 240 Hz callback timestamps. Output uses 44,100 Hz frames.

Conversion is:

```text
frame = callback * 44100 // 240
```

Since `44100 / 240 = 183.75`, callback intervals alternate between 183 and 184
sample frames.

This conversion is used for:

- Voice starts.
- Key-offs.
- Pitch, gain, and pan automation.
- Noise enable and clock changes.
- FM enable changes.
- Reverb-send changes.
- Reverb-depth changes.

## 4. PS1 ADPCM Decode

Each 16-byte block produces 28 samples.

For every signed four-bit nibble:

```text
decoded = (signed_nibble << 12) >> shift
decoded += (history1 * filter0) >> 6
decoded += (history2 * filter1) >> 6
decoded = clamp(decoded, -32768, 32767)
```

The predictor products are truncated independently, matching the SPU model in
Beetle. The current sample becomes `history1`; the old `history1` becomes
`history2`.

Supported predictor pairs are:

```text
0:   0,   0
1:  60,   0
2: 115, -52
3:  98, -55
4: 122, -60
```

The low nibble of each packed byte is decoded before the high nibble.

For malformed data, the offline decoder treats shift values above 12 as 9 and
predictors above 4 as predictor zero. Normal retail streams use valid headers.

## 5. Sample End And Loop Behavior

The renderer latches a block carrying flag `0x04` as the effective loop start.

After all samples from an end block are consumed:

- `END | REPEAT` jumps to the repeat address.
- `END` without repeat ends the offline voice.

The physical SPU also sets its ENDX status bit. For a nonrepeating end it can
continue internal address bookkeeping while forcing the voice silent. The
offline renderer stops because later silent address movement does not change
the WAV in normal cases.

## 6. Pitch

The SPU pitch register uses `0x1000` as one decoded sample per output frame.

Examples:

| Musical note | Offline SPU pitch |
|---:|---:|
| 48 | `0x0800` |
| 60 | `0x1000` |
| 72 | `0x2000` |

Sequence pitch is Q8 semitones. Conversion is approximately:

```text
pitch = 0x1000 * 2 ** ((note_q8 - 60 * 256) / (12 * 256))
```

The implementation follows the driver's octave/range arithmetic and masks the
result to 14 bits with `& 0x3FFF`. The retail octave/semitone map has 117
generated entries followed by three zero-padding bytes. Higher masked negative
notes index the adjacent ratio-table bytes, behavior that some effects rely on
and that the renderer preserves.

The final note value includes:

- Sequence note.
- WDS preset tuning.
- Track pitch offset.
- Pitch slides and portamento.
- Pitch modulator output.
- Manager pitch.

Raw WDS preset WAVs use the equivalent middle-C rate in their WAV header. SEDS
and SMDS renders remain 44,100 Hz because they are final SPU output: their
per-voice WDS tuning is already represented by the pitch stepping and Gaussian
interpolation described here.

## 7. Gaussian Interpolation

Fractional pitch positions use four-tap Gaussian interpolation. Key-on first
preloads decoded data during the SPU's four-frame playback delay without
advancing ADSR or pitch phase. The bundle contains the 512 signed coefficients
in:

```text
psxrecomp/runtime/include/spu_gauss.h
```

For fractional index `i = (phase >> 4) & 0xFF`, the voice combines four nearby
decoded samples with these table regions:

```text
gauss[0x0FF - i]
gauss[0x1FF - i]
gauss[0x100 + i]
gauss[i]
```

The interpolation cursor addresses the first of four forward decoded samples.
The renderer keeps the last three samples from the previous ADPCM block so the
filter remains continuous across block boundaries. Initial history is zero at
key-on.

All products are accumulated before the final right shift. Shifting each tap
separately would produce different low bits.

## 8. ADSR Envelope

Every voice starts at envelope level zero and passes through:

1. Attack: rise toward `0x7FFF`.
2. Decay: exponential fall toward sustain level.
3. Sustain: rise or fall according to WDS mode bits.
4. Release: fall toward zero after key-off.

The envelope advances once per 44.1 kHz output frame.

The shaped mono sample is:

```text
shaped = raw_sample * envelope_level >> 15
```

The rate algorithm uses an increment and a divider. In simplified form:

```text
increment = 7 - (speed & 3)
```

Low speed values increase increment size. High values reduce divider frequency.
Exponential decrease scales the increment by current envelope level.
Exponential attack slows above level `0x6000`.

Key-off enters Release without clearing the current level.

Sequence ADSR commands and preset reloads also update the registers of an
already active voice. The renderer applies these changes one callback after the
command, matching the driver's staged SPU writes. This is especially important
for ambient sounds that lower the sustain rate during a tie so the sample fades
before its eventual key-off.

The sequence driver can force release rate 6 and linear mode before a new note,
at track end, or when a voice is stolen.

## 9. Gain And Pan

Track gain combines:

```text
manager volume
* note velocity
* track volume after modulation
```

It is clamped to `0x3FFF` before pan conversion.

Pan uses `0x0000..0x7F00`, with `0x4000` near center. The driver computes left
and right coefficients with constants `0x7F00`, `0x5A00`, and `0x2500`.

For the left half:

```text
left  = 0x7F00 - (pan * 0x2500 >> 14)
right = pan * 0x5A00 >> 14
```

For the right half it mirrors the distance from `0x8000`.

The PS1 SPU has no abstract pan register. Xenogears converts pan into separate
left and right voice volumes before writing hardware.

## 10. Physical Voice Mapping

The offline simulator keeps the recovered physical mapping:

```text
SMDS track n -> physical voice n - 1
SEDS track n -> physical voice 8 + n
```

SMDS track zero maps to `-1` and does not render a voice.

Different managers can target the same physical voice in the game. The live
driver resolves ownership and priority. Offline resources are normally rendered
independently, so cross-manager stealing is not reproduced unless that context
is provided separately.

## 11. Noise

The SPU has one shared pseudo-random noise generator and one global noise clock.
Each voice has an enable bit selecting noise instead of its ADPCM/Gaussian
sample.

Xenogears commands:

```text
B4 clock  -> set clock and enable this track
B5 delta  -> add to clock and enable this track
B6        -> enable this track
B7        -> disable this track
```

The offline renderer generates one shared mono noise stream. Every enabled voice
reads the same stream at the same absolute frame. Noise replaces the raw sample,
but ADSR, volume, pan, and reverb send still apply.

## 12. Frequency Modulation / PMON

The SPU can modulate one voice's pitch using the output of the immediately
preceding physical voice.

The renderer enables this only for positive odd-numbered physical voices. For
example, voice 9 uses voice 8 as its source.

The source is captured after ADSR but before stereo voice volume. Target pitch
is:

```text
modulated_pitch = base_pitch * (0x8000 + source_sample) >> 15
modulated_pitch = clamp(modulated_pitch, 0, 0x3FFF)
```

Correct FM therefore depends on physical voice mapping and simultaneous source
voice playback.

## 13. Reverb Mode 4

All currently extracted SMDS scores use PSYQ-style reverb mode 4.

The bundle uses:

```text
work-area base: 0xF204
register count: 32
internal rate: 22,050 Hz
```

Only voices with reverb send enabled are added to the stereo send buffer. The
send is post-ADSR and post-voice-volume.

Processing stages are:

1. Buffer 44.1 kHz stereo send samples.
2. FIR downsample to 22.05 kHz.
3. Same-side and crossed-side IIR reflections.
4. Four comb-filter taps.
5. Two all-pass filters.
6. Circular work-RAM writes.
7. FIR reconstruction to 44.1 kHz.
8. Signed depth scaling.
9. Saturating addition to the dry mix.

The Python reference lives in `extract_disc_audio.py`. The equivalent optimized
C implementation is `../../spu_reverb_fast.c`.

At first use, the script hashes and compiles the C source into a temporary
shared library with:

```text
cc -O3 -std=c99 -shared -fPIC
```

If compilation or loading fails, it automatically uses Python. Set
`XENOGEARS_DISABLE_FAST_REVERB=1` to force Python.

Depth uses a signed volume derived from:

```text
output_volume = depth << 8
wet = output_volume * reverb_sample >> 15
```

The renderer begins with cleared reverb RAM. Real gameplay may begin with a tail
left by earlier music or effects.

## 14. Mixing And Main Volume

The offline order is:

```text
sum dry voices in signed 32-bit storage
sum reverb-send voices separately
run mode-4 reverb
add wet to dry with saturation
apply main volume 0x3FFF
clamp to signed 16-bit stereo PCM
```

WAV output is little-endian, two channels, 16 bits per channel, 44,100 frames
per second.

## 15. Release And Reverb Tails

When all sequence tracks end normally, the renderer allows up to five extra
seconds for ADSR release.

Mode-4 reverb also receives up to five seconds after dry input ends. The output
is trimmed to the last frame where wet magnitude is greater than one, but never
shorter than the dry source.

The real SPU has no fixed five-second cutoff. It continues while the game keeps
rendering. The offline limits prevent unbounded files and should be treated as a
practical rendering policy.

## 16. Streamed CD/XA Mixing

On hardware, sequence voices are not the only source. The SPU can also mix a
stereo CD/XA stream:

```text
24 voices
+ CD/XA left and right
+ reverb return
-> main left/right volume
-> output
```

CD/XA can also be sent to reverb. This bundle does not currently provide that
stream while rendering WDS/SEDS/SMDS, so extracted WAVs contain only the
sequence system.
