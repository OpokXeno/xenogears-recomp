#!/usr/bin/env python3
"""Inventory and extract Xenogears sequence and wave-bank resources."""

from __future__ import annotations

import argparse
import array
import concurrent.futures
import copy
import ctypes
import functools
import hashlib
import json
import math
import multiprocessing
import os
import re
import struct
import subprocess
import sys
import tempfile
import wave
from dataclasses import dataclass, field, replace
from pathlib import Path

from census_disc_overlays import (
    DIRECTORY_COUNT,
    DIRECTORY_LBA,
    FAT_LBA,
    FAT_SECTORS,
    SECTOR_SIZE,
    map_physical_routes,
    parse_directory_table,
    parse_fat_table,
)
from extract_disc_overlays import open_disc


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DISC = ROOT / "game" / "disc1.cue"
MANIFEST_SCHEMA = "xenogears-disc-audio/v1"
NAME_MAP_SCHEMA = "xenogears-audio-name-map/v1"
WDS_MAGIC = b"wds "
SEDS_MAGIC = b"seds"
SMDS_MAGIC = b"smds"
MAGICS = (WDS_MAGIC, SEDS_MAGIC, SMDS_MAGIC)
WAV_SAMPLE_RATE = 44100
SEQUENCER_CALLBACK_RATE = 240
SEQUENCE_RELEASE_TAIL_SECONDS = 5
SEQUENCE_REVERB_TAIL_SECONDS = 5
# Exact-loop renders are not bounded by the user-facing preview duration. Keep
# a generous internal guard for malformed sequences that never reach a boundary.
SEQUENCE_LOOP_SAFETY_CALLBACKS = SEQUENCER_CALLBACK_RATE * 24 * 60 * 60
DEFAULT_SEDS_REVERB_DEPTH: int | None = None
SPU_MAIN_VOLUME = 0x3FFF
SPU_REVERB_MODE4_BASE = 0xF204
SPU_REVERB_MODE4_REGISTERS = (
    0x00E3,
    0x00A9,
    0x6F60,
    0x4FA8,
    0xBCE0,
    0x4510,
    0xBEF0,
    0xA680,
    0x5680,
    0x52C0,
    0x0DFB,
    0x0B58,
    0x0D09,
    0x0A3C,
    0x0BD9,
    0x0973,
    0x0B59,
    0x08DA,
    0x08D9,
    0x05E9,
    0x07EC,
    0x04B0,
    0x06EF,
    0x03D2,
    0x05EA,
    0x031D,
    0x031C,
    0x0238,
    0x0154,
    0x00AA,
    0x8000,
    0x8000,
)
SPU_GAUSS_HEADER = ROOT / "psxrecomp" / "runtime" / "include" / "spu_gauss.h"
SPU_REVERB_FAST_SOURCE = ROOT / "tools" / "spu_reverb_fast.c"
SEQUENCE_DURATION_TICKS = (
    0,
    192,
    144,
    96,
    72,
    64,
    48,
    36,
    32,
    24,
    18,
    16,
    12,
    9,
    8,
    6,
    4,
    3,
    2,
)

INTERNAL_WDS_NAMES = {
    0x00: "main_se",
    0x01: "bat_se",
    0x02: "gear_se",
    0x03: "ambi",
    0x04: "ambi2",
    0x05: "ambi3",
    0x06: "ambi4",
    0x07: "movie14",
    0x20: "minato",
    0x21: "lahan",
    0x22: "jyukai",
    0x23: "shitan",
    0x24: "musi",
    0x25: "church",
    0x26: "battle2",
    0x27: "chuchu",
    0x28: "over",
    0x29: "orgel",
    0x2A: "battle3",
    0x2B: "ajito",
    0x2C: "emerada",
    0x2D: "ellie",
    0x2E: "world",
    0x2F: "sad",
    0x30: "ave",
    0x31: "ellie2",
    0x32: "balto",
    0x33: "dajil",
    0x34: "maria1",
    0x35: "maria2",
    0x36: "heshu",
    0x37: "kaisou",
    0x38: "pinch",
    0x39: "porgan",
    0x3A: "babel",
    0x3B: "solachu",
    0x3C: "shinnyu",
    0x3D: "inbou",
    0x3E: "ido",
    0x3F: "takeoff",
    0x40: "glaerf",
    0x41: "last",
    0x42: "shebat",
    0x43: "dungeon",
    0x44: "lastbat",
    0x45: "solaris",
    0xB5: "vomaria",
    0xB6: "melmv",
    0xB7: "yugumv",
    0xB8: "zoharumv",
    0xB9: "vomagic5",
    0xBA: "vomagic4",
    0xBB: "vomagic3",
    0xBC: "voivent3",
    0xBD: "voivent2",
    0xBE: "vobossm",
    0xBF: "vobossl",
    0xC0: "vochu6",
    0xC1: "vomagic2",
    0xC2: "vomagic1",
    0xC3: "movie15",
    0xC4: "movie16",
    0xC5: "movie18",
    0xC6: "voivent",
    0xC7: "damage",
    0xC8: "vofei",
    0xC9: "vofei1",
    0xCA: "vofei2",
    0xCB: "vofei3",
    0xCC: "vofei4",
    0xCD: "vofei5",
    0xCE: "vofei6",
    0xCF: "voellie",
    0xD0: "voellie1",
    0xD1: "voellie2",
    0xD2: "voellie3",
    0xD3: "voellie4",
    0xD4: "voellie5",
    0xD5: "voellie6",
    0xD6: "voellie7",
    0xD7: "voellie8",
    0xD8: "voshita",
    0xD9: "voshita1",
    0xDA: "voshita2",
    0xDB: "voshita3",
    0xDC: "voshita4",
    0xDD: "voshita5",
    0xDE: "voshita6",
    0xDF: "vobaluto",
    0xE0: "vobalu1",
    0xE1: "vobalu2",
    0xE2: "vobalu3",
    0xE3: "vobalu4",
    0xE4: "vobalu5",
    0xE5: "vobalu6",
    0xE6: "vobalu7",
    0xE7: "vorico",
    0xE8: "vorico1",
    0xE9: "vorico2",
    0xEA: "vorico3",
    0xEB: "vorico4",
    0xEC: "vorico5",
    0xED: "vobilly",
    0xEE: "vobilly1",
    0xEF: "vobilly2",
    0xF0: "vobilly3",
    0xF1: "vobilly4",
    0xF2: "vobilly5",
    0xF3: "voeme",
    0xF4: "voeme1",
    0xF5: "voeme2",
    0xF6: "voeme3",
    0xF7: "voeme4",
    0xF8: "voeme5",
    0xF9: "vochu",
    0xFA: "vochu1",
    0xFB: "vochu2",
    0xFC: "vochu3",
    0xFD: "vochu4",
    0xFE: "vochu5",
}

# Total encoded sizes from the game's opcode table at 0x80050824. Reserved
# opcodes still occupy one byte in the runtime's default handler.
SEQUENCE_OPCODE_LENGTHS = (
    2,
    2,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    2,
    4,
    1,
    1,
    1,
    1,
    1,
    2,
    1,
    1,
    3,
    2,
    1,
    1,
    1,
    4,
    4,
    4,
    1,
    2,
    2,
    3,
    1,
    2,
    2,
    2,
    3,
    1,
    2,
    2,
    1,
    2,
    2,
    1,
    1,
    1,
    1,
    1,
    1,
    2,
    2,
    1,
    1,
    4,
    1,
    1,
    1,
    4,
    1,
    1,
    1,
    1,
    4,
    2,
    2,
    2,
    2,
    2,
    3,
    2,
    2,
    2,
    1,
    1,
    1,
    1,
    1,
    2,
    2,
    2,
    3,
    3,
    1,
    2,
    2,
    4,
    4,
    1,
    1,
    1,
    1,
    1,
    1,
    2,
    2,
    3,
    2,
    4,
    4,
    1,
    1,
    2,
    2,
    3,
    2,
    4,
    4,
    1,
    1,
    4,
    4,
    3,
    1,
    1,
    2,
    2,
    2,
    4,
    3,
    1,
    1,
    3,
    2,
    2,
    1,
)
RESERVED_SEQUENCE_OPCODES = frozenset(
    (
        *range(0x82, 0x8A),
        0x8B,
        0x8C,
        0x92,
        0x93,
        0x9B,
        0x9F,
        0xA3,
        0xA8,
        0xAB,
        0xB9,
        0xBF,
        *range(0xCB, 0xD0),
        *range(0xDD, 0xE0),
        0xF3,
        0xF4,
        0xFA,
        0xFB,
    )
)


@dataclass(frozen=True)
class WdsPreset:
    index: int
    start_units: int
    repeat_units: int
    pitch: int
    adsr: int
    modes: int
    reserved: int


@dataclass(frozen=True)
class AudioResource:
    kind: str
    payload: bytes
    metadata: dict


@dataclass(frozen=True)
class ResourceOccurrence:
    disc_index: int
    disc_name: str
    fat_index: int
    lba: int
    container_size: int
    container_offset: int
    routes: list[dict]


@dataclass(frozen=True)
class SequenceInstruction:
    offset: int
    size: int
    opcode: int | None
    operands: tuple[int, ...]
    velocity: int | None = None
    semitone: int | None = None
    duration_index: int | None = None
    explicit_duration: int | None = None


@dataclass(frozen=True)
class SequenceLoopFrame:
    remaining: int
    start_offset: int
    start_octave: int
    exit_offset: int | None = None
    exit_octave: int | None = None


@dataclass(frozen=True)
class SequenceWalkResult:
    terminal: str
    instructions: tuple[SequenceInstruction, ...]


@dataclass
class SequenceVoiceEvent:
    track_index: int
    physical_voice_index: int
    start_callback: int
    wds_id: int
    preset_index: int
    note_q8: int
    gain: int
    pan_q8: int
    adsr: int
    modes: int
    reverb_send: bool = False
    noise_enabled: bool = False
    frequency_modulation: bool = False
    key_off_callback: int | None = None
    stop_callback: int | None = None
    automation: list[tuple[int, int, int, int]] = field(default_factory=list)
    reverb_automation: list[tuple[int, bool]] = field(default_factory=list)
    noise_automation: list[tuple[int, bool]] = field(default_factory=list)
    frequency_modulation_automation: list[tuple[int, bool]] = field(
        default_factory=list
    )
    adsr_automation: list[tuple[int, int, int]] = field(default_factory=list)
    release_automation: list[tuple[int, int, bool]] = field(default_factory=list)


@dataclass(frozen=True)
class SequenceSimulationResult:
    events: tuple[SequenceVoiceEvent, ...]
    callbacks: int
    logical_ticks: int
    ended_tracks: int
    approximated_opcodes: tuple[int, ...]
    unresolved_voices: tuple[tuple[int, int], ...]
    reverb_depth: int | None
    reverb_depth_automation: tuple[tuple[int, int], ...]
    noise_clock_automation: tuple[tuple[int, int], ...]
    external_reverb_context_required: bool


@dataclass(frozen=True)
class SequenceRenderResult:
    samples: array.array
    callbacks: int
    logical_ticks: int
    note_count: int
    ended_tracks: int
    approximated_opcodes: tuple[int, ...]
    unresolved_voices: tuple[tuple[int, int], ...]
    external_reverb_context_required: bool
    assumed_reverb_depth: int | None


@dataclass
class _SequenceModulator:
    waveform: int = 0
    target: int = 0
    flags: int = 0
    current: int = 0
    increment: int = 0
    amplitude: int = 0
    phase_count: int = 0
    phase_reload: int = 0
    delay: int = 0
    delay_reload: int = 0
    ramp: int = 0
    ramp_step: int = 0


@dataclass
class _SequenceTrackState:
    index: int
    pc: int
    wds_id: int
    physical_voice_index: int
    active: bool = True
    saved_offset: int | None = None
    saved_octave: int = 60
    loops: list[SequenceLoopFrame] = field(default_factory=list)
    octave: int = 60
    duration: int = 0
    gate: int = 0
    next_event_is_note: bool = False
    duration_adjust: int = 0
    gate_mode: int = 0x0F
    velocity: int = 0x6000
    preset_index: int = 0
    pitch_offset_q8: int = 0
    volume_q24: int = 0x7F000000
    pan_q8: int = 0x4000
    percussion: bool = False
    hold_notes: bool = False
    reverb_send: bool = False
    noise_enabled: bool = False
    frequency_modulation: bool = False
    adsr: int = 0
    modes: int = 0
    instrument_wds_id: int | None = None
    current_event: int | None = None
    current_note_q24: int = 60 * 256 << 16
    pitch_slide_step: int = 0
    pitch_slide_ticks: int = 0
    pitch_slide_active: bool = False
    pitch_slide_continuous: bool = False
    volume_slide_step: int = 0
    volume_slide_ticks: int = 0
    pan_slide_step: int = 0
    pan_slide_ticks: int = 0
    pan_slide_target: int = 0x4000
    note_volume_start_q24: int = 0
    note_volume_step: int = 0
    note_volume_ticks: int = 0
    glide_duration: int = 0
    glide_ticks: int = 0
    glide_step: int = 0
    previous_note_q8: int = 60 * 256
    pitch_mod: int = 0
    volume_mod: int = 0
    pan_mod: int = 0
    selected_modulator: int = 0
    modulators: list[_SequenceModulator] = field(
        default_factory=lambda: [_SequenceModulator() for _ in range(4)]
    )


def _u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def _u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def parse_wds(data: bytes, offset: int = 0) -> AudioResource | None:
    if (
        offset < 0
        or offset + 0x30 > len(data)
        or data[offset : offset + 4] != WDS_MAGIC
    ):
        return None
    if _u32(data, offset + 0x0C) != 0x101:
        return None

    header_size = _u32(data, offset + 0x10)
    adpcm_size = _u32(data, offset + 0x14)
    adpcm_offset = _u32(data, offset + 0x18)
    preset_count = _u16(data, offset + 0x1C) + 1
    total_size = adpcm_offset + adpcm_size
    if (
        header_size != adpcm_offset
        or header_size != 0x30 + preset_count * 16
        or adpcm_size == 0
        or adpcm_size % 16
        or total_size > len(data) - offset
    ):
        return None
    if (
        _u32(data, offset + 0x08) != header_size
        or sum(
            struct.unpack_from("<I", data, offset + word_offset)[0]
            for word_offset in range(0, header_size, 4)
        )
        & 0xFFFFFFFF
        != 0
    ):
        return None

    presets = []
    for index in range(preset_count):
        record_offset = offset + 0x30 + index * 16
        start_units, repeat_units, pitch, adsr, modes, reserved = struct.unpack_from(
            "<IHhIHH", data, record_offset
        )
        start_byte = start_units * 8
        repeat_byte = start_byte + repeat_units * 8
        if (
            start_byte >= adpcm_size
            or start_byte % 16
            or repeat_byte >= adpcm_size
            or repeat_byte % 16
        ):
            return None
        presets.append(
            WdsPreset(index, start_units, repeat_units, pitch, adsr, modes, reserved)
        )

    payload = data[offset : offset + total_size]
    return AudioResource(
        "wds",
        payload,
        {
            "wds_id": _u16(data, offset + 0x20),
            "header_size": header_size,
            "adpcm_data_size": adpcm_size,
            "adpcm_data_offset": adpcm_offset,
            "preset_count": preset_count,
            "configured_spu_address": _u32(data, offset + 0x28),
            "presets": presets,
        },
    )


def _resolve_wds_preset(resource: AudioResource, preset_index: int) -> WdsPreset | None:
    presets = resource.metadata["presets"]
    if 0 <= preset_index < len(presets):
        return presets[preset_index]
    if preset_index < 0:
        return None

    # The original driver does not compare instrument IDs with the WDS count.
    # Suijyo uses WDS 3 instrument 23, whose record lies inside the ADPCM area.
    record_offset = 0x30 + preset_index * 16
    if record_offset + 16 > len(resource.payload):
        return None
    start_units, repeat_units, pitch, adsr, modes, reserved = struct.unpack_from(
        "<IHhIHH", resource.payload, record_offset
    )
    start_units &= 0xFFFF  # SPU start/repeat address registers are 16-bit.
    adpcm = resource.payload[
        resource.metadata["adpcm_data_offset"] : resource.metadata["adpcm_data_offset"]
        + resource.metadata["adpcm_data_size"]
    ]
    start_byte = start_units * 8
    repeat_byte = ((start_units + repeat_units) & 0xFFFF) * 8
    if start_byte + 16 > len(adpcm) or repeat_byte >= len(adpcm):
        return None

    # Reject arbitrary out-of-range reads unless they begin a coherent SPU ADPCM
    # stream. The known WDS 3/23 stream reaches an ordinary non-looping end block.
    cursor = start_byte
    while cursor + 16 <= len(adpcm):
        header = adpcm[cursor]
        flags = adpcm[cursor + 1]
        if header & 0x0F > 12 or header >> 4 > 4 or flags & ~7:
            return None
        if flags & 1:
            return WdsPreset(
                preset_index,
                start_units,
                repeat_units,
                pitch,
                adsr,
                modes,
                reserved,
            )
        cursor += 16
    return None


def parse_seds(data: bytes, offset: int = 0) -> AudioResource | None:
    if (
        offset < 0
        or offset + 0x20 > len(data)
        or data[offset : offset + 4] != SEDS_MAGIC
    ):
        return None
    total_size = _u32(data, offset + 8)
    effect_count = _u16(data, offset + 0x12)
    volume_offset = _u32(data, offset + 0x18)
    if (
        _u32(data, offset + 0x0C) != 0x101
        or effect_count == 0
        or volume_offset != 0x20 + effect_count * 4
        or volume_offset + effect_count > total_size
        or total_size > len(data) - offset
    ):
        return None

    script_offsets = struct.unpack_from(f"<{effect_count * 2}H", data, offset + 0x20)
    script_start = volume_offset + effect_count
    if any(
        script_offset != 0 and not script_start <= script_offset < total_size
        for script_offset in script_offsets
    ):
        return None

    volumes = data[offset + volume_offset : offset + volume_offset + effect_count]
    events = [
        {
            "index": index,
            "volume": volumes[index],
            "channel_script_offsets": [
                script_offsets[index * 2],
                script_offsets[index * 2 + 1],
            ],
        }
        for index in range(effect_count)
    ]
    return AudioResource(
        "seds",
        data[offset : offset + total_size],
        {
            "seds_id": _u16(data, offset + 0x14),
            "default_wds_id": _u16(data, offset + 0x16),
            "effect_count": effect_count,
            "volume_table_offset": volume_offset,
            "events": events,
        },
    )


def parse_smds(data: bytes, offset: int = 0) -> AudioResource | None:
    if (
        offset < 0
        or offset + 0x22 > len(data)
        or data[offset : offset + 4] != SMDS_MAGIC
    ):
        return None
    total_size = _u32(data, offset + 8)
    track_count = data[offset + 0x14]
    patch_count = data[offset + 0x15]
    track_table_end = 0x22 + track_count * 2
    if (
        data[offset + 0x12 : offset + 0x14] != b"\x02\x01"
        or track_count == 0
        or total_size < track_table_end
        or total_size > len(data) - offset
    ):
        return None

    track_offsets = struct.unpack_from(f"<{track_count}H", data, offset + 0x22)
    if any(
        track_offset != 0 and not track_table_end <= track_offset < total_size
        for track_offset in track_offsets
    ):
        return None

    auxiliary_table_offset = _u16(data, offset + 0x1E)
    patch_table_offset = _u16(data, offset + 0x20)
    if auxiliary_table_offset and auxiliary_table_offset >= total_size:
        return None
    if patch_count:
        if (
            patch_table_offset < track_table_end
            or patch_table_offset + patch_count * 5 > total_size
        ):
            return None
        patches = [
            {
                "index": data[offset + patch_table_offset + index * 5],
                "value": _u32(data, offset + patch_table_offset + index * 5 + 1),
            }
            for index in range(patch_count)
        ]
        if any(patch["index"] >= 96 for patch in patches):
            return None
    else:
        patches = []

    internal_name = None
    if auxiliary_table_offset:
        name_limits = [
            candidate
            for candidate in (patch_table_offset, *track_offsets, total_size)
            if candidate > auxiliary_table_offset
        ]
        name_limit = min(name_limits, default=total_size)
        encoded_name = data[
            offset + auxiliary_table_offset : offset + name_limit
        ].split(b"\0", 1)[0]
        if encoded_name and all(0x20 <= character < 0x7F for character in encoded_name):
            internal_name = encoded_name.decode("ascii")

    return AudioResource(
        "smds",
        data[offset : offset + total_size],
        {
            "declared_size": total_size,
            "seds_id": _u16(data, offset + 0x10),
            "track_count": track_count,
            "patch_count": patch_count,
            "default_wds_id": _u16(data, offset + 0x16),
            "sequence_state": _u16(data, offset + 0x18),
            "reverb_mode": struct.unpack_from("<b", data, offset + 0x1A)[0],
            "reverb_depth": data[offset + 0x1B],
            "reverb_parameters": list(data[offset + 0x1C : offset + 0x1E]),
            "auxiliary_table_offset": auxiliary_table_offset,
            "internal_name": internal_name,
            "patch_table_offset": patch_table_offset,
            "patches": patches,
            "track_offsets": list(track_offsets),
        },
    )


def decode_sequence_instruction(data: bytes, offset: int) -> SequenceInstruction:
    if not 0 <= offset < len(data):
        raise ValueError(f"sequence offset 0x{offset:X} is out of bounds")

    first = data[offset]
    if first < 0x80:
        if offset + 2 > len(data):
            raise ValueError(f"truncated note at sequence offset 0x{offset:X}")
        selector = data[offset + 1]
        if selector >= 12 * 19:
            raise ValueError(
                f"invalid note selector 0x{selector:02X} at sequence offset 0x{offset:X}"
            )
        duration_index = selector % 19
        size = 3 if duration_index == 0 else 2
        if offset + size > len(data):
            raise ValueError(f"truncated note duration at sequence offset 0x{offset:X}")
        explicit_duration = data[offset + 2] if size == 3 else None
        return SequenceInstruction(
            offset,
            size,
            None,
            (),
            velocity=first,
            semitone=selector // 19,
            duration_index=duration_index,
            explicit_duration=explicit_duration,
        )

    size = SEQUENCE_OPCODE_LENGTHS[first - 0x80]
    if first in RESERVED_SEQUENCE_OPCODES:
        raise ValueError(
            f"reserved opcode 0x{first:02X} at sequence offset 0x{offset:X}"
        )
    if offset + size > len(data):
        raise ValueError(
            f"truncated opcode 0x{first:02X} at sequence offset 0x{offset:X}"
        )
    return SequenceInstruction(
        offset, size, first, tuple(data[offset + 1 : offset + size])
    )


def walk_sequence(
    data: bytes, entry_offset: int, *, max_steps: int = 1_000_000
) -> SequenceWalkResult:
    pc = entry_offset
    octave = 0
    saved_offset = None
    saved_octave = None
    loops: tuple[SequenceLoopFrame, ...] = ()
    visited = set()
    instructions = []

    for _ in range(max_steps):
        state = (pc, octave, saved_offset, saved_octave, loops)
        if state in visited:
            return SequenceWalkResult("cycle", tuple(instructions))
        visited.add(state)

        instruction = decode_sequence_instruction(data, pc)
        instructions.append(instruction)
        next_pc = pc + instruction.size
        opcode = instruction.opcode

        if opcode is None:
            pc = next_pc
        elif opcode == 0x90:
            if saved_offset is None:
                return SequenceWalkResult("end", tuple(instructions))
            pc = saved_offset
            octave = saved_octave
        elif opcode == 0x91:
            saved_offset = next_pc
            saved_octave = octave
            pc = next_pc
        elif opcode == 0x94:
            octave = instruction.operands[0] * 12
            pc = next_pc
        elif opcode == 0x95:
            octave += 12
            pc = next_pc
        elif opcode == 0x96:
            octave -= 12
            pc = next_pc
        elif opcode == 0x98:
            if len(loops) == 4:
                raise ValueError(f"loop stack overflow at sequence offset 0x{pc:X}")
            loops += (
                SequenceLoopFrame(
                    (instruction.operands[0] - 1) & 0xFF,
                    next_pc,
                    octave,
                ),
            )
            pc = next_pc
        elif opcode == 0x99:
            if not loops:
                raise ValueError(f"loop stack underflow at sequence offset 0x{pc:X}")
            frame = loops[-1]
            remaining = (frame.remaining - 1) & 0xFF
            if remaining == 0xFF:
                loops = loops[:-1]
                pc = next_pc
            else:
                loops = loops[:-1] + (
                    SequenceLoopFrame(
                        remaining,
                        frame.start_offset,
                        frame.start_octave,
                        next_pc,
                        octave,
                    ),
                )
                octave = frame.start_octave
                pc = frame.start_offset
        elif opcode == 0x9A:
            if not loops:
                raise ValueError(f"loop break outside loop at sequence offset 0x{pc:X}")
            frame = loops[-1]
            if frame.remaining == 0:
                if frame.exit_offset is None:
                    raise ValueError(
                        f"loop break has no resolved exit at sequence offset 0x{pc:X}"
                    )
                loops = loops[:-1]
                pc = frame.exit_offset
                octave = frame.exit_octave
            else:
                pc = next_pc
        elif opcode == 0x9E:
            return SequenceWalkResult("external", tuple(instructions))
        else:
            pc = next_pc

    raise ValueError(f"sequence exceeded the {max_steps}-instruction limit")


def _signed_byte(value: int) -> int:
    return value - 0x100 if value & 0x80 else value


def _signed_word(low: int, high: int) -> int:
    value = low | high << 8
    return value - 0x10000 if value & 0x8000 else value


def _trunc_div(numerator: int, denominator: int) -> int:
    quotient = abs(numerator) // abs(denominator)
    return -quotient if (numerator < 0) ^ (denominator < 0) else quotient


def _i16(value: int) -> int:
    value &= 0xFFFF
    return value - 0x10000 if value & 0x8000 else value


def _i32(value: int) -> int:
    value &= 0xFFFFFFFF
    return value - 0x100000000 if value & 0x80000000 else value


def _sound_random(state: int) -> tuple[int, int]:
    value = (state ^ state << 17) & 0xFFFFFFFF
    state = (value ^ _i32(value) >> 15) & 0xFFFFFFFF
    return state & 0x7FFF, state


def _scale_modulator_amplitude(amplitude: int, period: int, waveform: int) -> int:
    if period == 0:
        return amplitude
    if waveform in (2, 3):
        return _trunc_div(amplitude, period)
    if waveform in (4, 5) and period != 1:
        return _trunc_div(amplitude, period - 1)
    return amplitude


def _initialize_modulator(modulator: _SequenceModulator) -> None:
    modulator.phase_count = 1
    modulator.current = 0
    modulator.flags &= ~0x0C
    modulator.delay = modulator.delay_reload
    modulator.ramp = modulator.ramp_step


def _configure_modulator(
    track: _SequenceTrackState,
    slot: int,
    target: int,
    waveform: int,
    period_value: int,
    amplitude: int,
    delay: int,
    retrigger: bool,
) -> None:
    if period_value == 0 or amplitude == 0:
        return
    period = period_value + period_value * period_value // 64
    modulator = track.modulators[slot]
    modulator.waveform = waveform & 0x0F
    modulator.target = target
    modulator.flags = 1 | (2 if retrigger else 0)
    modulator.amplitude = _scale_modulator_amplitude(
        amplitude, period, modulator.waveform
    )
    modulator.phase_reload = period
    modulator.delay_reload = delay * 4
    modulator.ramp_step = 0x400
    _initialize_modulator(modulator)


def _restart_note_modulators(track: _SequenceTrackState) -> None:
    for modulator in track.modulators:
        if modulator.flags & 3 == 3:
            _initialize_modulator(modulator)


def _run_modulator_waveform(
    modulator: _SequenceModulator, random_state: int
) -> tuple[int, int]:
    waveform = modulator.waveform
    if waveform >= 8:
        modulator.flags &= ~1
        return 0, random_state
    if waveform == 6:
        _, random_state = _sound_random(random_state)

    modulator.phase_count = (modulator.phase_count - 1) & 0xFFFF
    if waveform == 0:
        if modulator.phase_count == 0:
            modulator.phase_count = modulator.phase_reload
            modulator.current = modulator.amplitude if modulator.current == 0 else 0
    elif waveform == 1:
        if modulator.phase_count == 0:
            modulator.phase_count = modulator.phase_reload
            modulator.current = (
                -modulator.amplitude if modulator.flags & 8 else modulator.amplitude
            )
            modulator.flags ^= 8
    elif waveform == 2:
        if modulator.phase_count == 0:
            modulator.phase_count = modulator.phase_reload
            modulator.increment = (
                -modulator.amplitude if modulator.flags & 8 else modulator.amplitude
            )
            modulator.flags ^= 8
        modulator.current = _i32(modulator.current + modulator.increment)
    elif waveform == 3:
        if modulator.phase_count == 0:
            modulator.phase_count = modulator.phase_reload * (
                2 if modulator.flags & 4 else 1
            )
            modulator.increment = (
                -modulator.amplitude if modulator.flags & 8 else modulator.amplitude
            )
            modulator.flags = (modulator.flags | 4) ^ 8
        modulator.current = _i32(modulator.current + modulator.increment)
    elif waveform in (4, 5):
        if modulator.phase_count == 0:
            modulator.phase_count = modulator.phase_reload
            modulator.current = 0
        else:
            modulator.current = _i32(modulator.current + modulator.amplitude)
    elif waveform == 6:
        if modulator.phase_count == 0:
            modulator.phase_count = modulator.phase_reload
            random_value, random_state = _sound_random(random_state)
            modulator.current = _i32((modulator.amplitude >> 15) * random_value)
    elif waveform == 7 and modulator.phase_count == 0:
        modulator.phase_count = modulator.phase_reload
        random_value, random_state = _sound_random(random_state)
        modulator.current = _i32(
            (modulator.amplitude >> 14) * random_value - modulator.amplitude
        )
    return modulator.current, random_state


def _update_track_modulators(track: _SequenceTrackState, random_state: int) -> int:
    track.pitch_mod = 0
    track.volume_mod = 0
    track.pan_mod = 0
    for modulator in track.modulators:
        if not modulator.flags & 1:
            continue
        if modulator.delay:
            modulator.delay = (modulator.delay - 1) & 0xFFFF
            continue
        raw, random_state = _run_modulator_waveform(modulator, random_state)
        ramp = _i16(modulator.ramp)
        if ramp < 0x400:
            raw = _i32((raw >> 10) * ramp)
            modulator.ramp = (modulator.ramp + modulator.ramp_step) & 0xFFFF
        value = _i16(raw >> 16)
        if modulator.target == 0:
            track.pitch_mod = _i16(track.pitch_mod + value)
        elif modulator.target == 1:
            track.volume_mod = _i16(track.volume_mod + value)
        elif modulator.target == 2:
            track.pan_mod = _i16(track.pan_mod + value)
    return random_state


def _load_track_preset(
    track: _SequenceTrackState, wds_by_id: dict[int, AudioResource]
) -> WdsPreset | None:
    resource = wds_by_id.get(track.wds_id)
    if resource is None:
        return None
    preset = _resolve_wds_preset(resource, track.preset_index)
    if preset is None:
        return None
    track.adsr = preset.adsr
    track.modes = preset.modes
    track.instrument_wds_id = track.wds_id
    return preset


def _get_track_preset(
    track: _SequenceTrackState, wds_by_id: dict[int, AudioResource]
) -> WdsPreset | None:
    if track.instrument_wds_id is None:
        return None
    resource = wds_by_id.get(track.instrument_wds_id)
    if resource is None:
        return None
    return _resolve_wds_preset(resource, track.preset_index)


def _track_gain(track: _SequenceTrackState, manager_volume_q24: int) -> int:
    track_volume = max(0, min(0x7FFF, track.volume_q24 >> 16))
    track_volume = max(
        0,
        min(
            0x7FFF,
            track_volume - ((track_volume * _i16(track.volume_mod)) >> 15),
        ),
    )
    manager_volume = max(0, min(0x7FFF, manager_volume_q24 >> 16))
    gain = manager_volume * ((track.velocity * track_volume) >> 15) >> 16
    return max(0, min(0x3FFF, gain))


def _track_note_q8(track: _SequenceTrackState) -> int:
    return _i16((track.current_note_q24 >> 16) + track.pitch_mod)


def _track_pan_q8(track: _SequenceTrackState) -> int:
    return max(0, min(0x7F00, _i16(track.pan_q8) + track.pan_mod))


def _capture_voice_automation(
    tracks: list[_SequenceTrackState],
    events: list[SequenceVoiceEvent],
    manager_volume_q24: int,
    callback: int,
) -> None:
    for track in tracks:
        if track.current_event is None:
            continue
        event = events[track.current_event]
        if event.stop_callback is not None and event.stop_callback <= callback:
            continue
        state = (
            callback + 1,
            _track_note_q8(track),
            _track_gain(track, manager_volume_q24),
            _track_pan_q8(track),
        )
        previous = (
            event.automation[-1]
            if event.automation
            else (event.start_callback, event.note_q8, event.gain, event.pan_q8)
        )
        if state[1:] != previous[1:]:
            event.automation.append(state)


def _capture_event_adsr(
    track: _SequenceTrackState,
    events: list[SequenceVoiceEvent],
    callback: int,
) -> None:
    if track.current_event is None:
        return
    event = events[track.current_event]
    state = (callback + 1, track.adsr, track.modes)
    previous = (
        event.adsr_automation[-1]
        if event.adsr_automation
        else (event.start_callback, event.adsr, event.modes)
    )
    if state[1:] != previous[1:]:
        event.adsr_automation.append(state)


def _set_event_key_off(
    track: _SequenceTrackState,
    events: list[SequenceVoiceEvent],
    callback: int,
    forced_release_rate: int | None = None,
) -> None:
    if track.current_event is None:
        return
    event = events[track.current_event]
    if event.key_off_callback is None:
        event.key_off_callback = callback
    if forced_release_rate is not None:
        event.release_automation.append((callback, forced_release_rate, False))


def _set_track_reverb_send(
    track: _SequenceTrackState,
    events: list[SequenceVoiceEvent],
    callback: int,
    enabled: bool,
) -> None:
    if track.reverb_send == enabled:
        return
    track.reverb_send = enabled
    if track.current_event is not None:
        events[track.current_event].reverb_automation.append((callback + 1, enabled))


def _set_track_noise(
    track: _SequenceTrackState,
    events: list[SequenceVoiceEvent],
    callback: int,
    enabled: bool,
) -> None:
    if track.noise_enabled == enabled:
        return
    track.noise_enabled = enabled
    if track.current_event is not None:
        events[track.current_event].noise_automation.append((callback + 1, enabled))


def _set_track_frequency_modulation(
    track: _SequenceTrackState,
    events: list[SequenceVoiceEvent],
    callback: int,
    enabled: bool,
) -> None:
    if (
        track.physical_voice_index <= 0
        or not track.physical_voice_index & 1
        or track.frequency_modulation == enabled
    ):
        return
    track.frequency_modulation = enabled
    if track.current_event is not None:
        events[track.current_event].frequency_modulation_automation.append(
            (callback + 1, enabled)
        )


def _next_timed_event_kind(
    sequence_data: bytes, track: _SequenceTrackState, pc: int
) -> str:
    """Mirror the driver's lookahead when selecting the current event's gate."""
    loops = list(track.loops)
    visited: set[tuple[int, tuple[SequenceLoopFrame, ...]]] = set()
    while True:
        state = (pc, tuple(loops))
        if state in visited:
            return "other"
        visited.add(state)
        instruction = decode_sequence_instruction(sequence_data, pc)
        opcode = instruction.opcode
        next_pc = pc + instruction.size
        if opcode is None:
            return "note"
        if opcode in (0x80, 0xB0, 0xB1):
            return "other"
        if opcode == 0x81:
            return "tie"
        if opcode == 0x90:
            if track.saved_offset is None:
                return "other"
            pc = track.saved_offset
            continue
        if opcode == 0x99:
            if not loops:
                return "other"
            loop = loops[-1]
            if loop.remaining:
                pc = loop.start_offset
            else:
                loops.pop()
                pc = next_pc
            continue
        if opcode == 0x9A and loops and loops[-1].remaining == 0:
            loop = loops.pop()
            if loop.exit_offset is None:
                return "other"
            pc = loop.exit_offset
            continue
        pc = next_pc


def _start_timed_event(
    track: _SequenceTrackState, base_duration: int, hold: bool
) -> None:
    duration = base_duration + track.duration_adjust
    if duration <= 0:
        duration += base_duration
        track.duration_adjust = _signed_byte(
            (track.duration_adjust + base_duration) & 0xFF
        )
    track.duration = max(1, duration)
    if hold or track.hold_notes:
        track.gate = 0x7FFF
    elif track.gate_mode == 0x0F:
        track.gate = max(1, track.duration - 1)
    elif track.gate_mode == 0x10:
        track.gate = track.duration
    else:
        track.gate = max(1, track.duration * track.gate_mode >> 4)


def _reset_track_for_general_loop(
    track: _SequenceTrackState, initial_track: _SequenceTrackState
) -> None:
    track.__dict__.clear()
    track.__dict__.update(copy.deepcopy(initial_track.__dict__))


def simulate_sequence(
    resource: AudioResource,
    wds_by_id: dict[int, AudioResource],
    *,
    entry_index: int = 0,
    max_seconds: float = 120.0,
    ignore_loops: bool = False,
    loop_count: int | None = None,
) -> SequenceSimulationResult:
    """Simulate a sequence at 240 Hz, optionally stopping after exact cycles."""
    if loop_count is not None and loop_count < 1:
        raise ValueError("loop_count must be at least 1")
    if loop_count is not None and ignore_loops:
        raise ValueError("loop_count cannot be used with ignore_loops")
    loop_target = loop_count if loop_count is not None else 1 if ignore_loops else None
    if loop_count is None and max_seconds <= 0:
        raise ValueError("max_seconds must be positive")
    if resource.kind == "seds":
        events_metadata = resource.metadata["events"]
        if not 0 <= entry_index < len(events_metadata):
            raise ValueError(f"SEDS effect index {entry_index} is out of range")
        entry = events_metadata[entry_index]
        entry_offsets = entry["channel_script_offsets"]
        physical_voice_base = 8
        manager_volume_q24 = (0x7FFF * entry["volume"] >> 7) << 16
        percussion_map = {}
        reverb_depth = None
    elif resource.kind == "smds":
        if entry_index != 0:
            raise ValueError(
                "SMDS resources contain one score; entry_index must be zero"
            )
        entry_offsets = resource.metadata["track_offsets"]
        physical_voice_base = -1
        manager_volume_q24 = 0x7F000000
        percussion_map = {
            patch["index"]: patch["value"] for patch in resource.metadata["patches"]
        }
        reverb_depth = resource.metadata["reverb_depth"]
    else:
        raise ValueError("sequence simulation requires an SEDS or SMDS resource")

    tracks = [
        _SequenceTrackState(
            index,
            offset,
            resource.metadata["default_wds_id"],
            physical_voice_base + index,
            active=bool(offset),
        )
        for index, offset in enumerate(entry_offsets)
    ]
    for track in tracks:
        _load_track_preset(track, wds_by_id)
    initial_tracks = [copy.deepcopy(track) for track in tracks]

    sequence_data = resource.payload
    voice_events: list[SequenceVoiceEvent] = []
    approximated: set[int] = set()
    unresolved_voices: set[tuple[int, int]] = set()
    accumulator = 0x10000
    tempo_q16 = 0x660000
    tempo_scale_q8 = 0x100
    tick_increment = 0x6600
    tempo_slide_step = 0
    tempo_slide_ticks = 0
    tempo_slide_target = 0x66
    logical_ticks = 0
    random_state = 0x12345678
    reverb_depth_automation: list[tuple[int, int]] = []
    external_reverb_context_required = False
    noise_clock = 0
    noise_clock_automation: list[tuple[int, int]] = []
    max_callbacks = (
        SEQUENCE_LOOP_SAFETY_CALLBACKS
        if loop_count is not None
        else max(1, math.ceil(max_seconds * SEQUENCER_CALLBACK_RATE))
    )
    callbacks = max_callbacks
    cycle_track_indices = {track.index for track in tracks if track.active}
    completed_cycle_counts = {track.index: 0 for track in tracks if track.active}
    cycle_boundary_callbacks = {track.index: -1 for track in tracks if track.active}
    stop_after_cycle = False

    for callback in range(max_callbacks):
        if not any(track.active for track in tracks):
            callbacks = callback
            break
        accumulator -= tick_increment
        while accumulator < 0:
            accumulator += 0x10000
            logical_ticks += 1
            if tempo_slide_ticks:
                if tempo_slide_ticks == 1:
                    tempo_q16 = tempo_slide_target << 16
                else:
                    tempo_q16 += tempo_slide_step
                tempo_slide_ticks -= 1
                tick_increment = (tempo_q16 >> 16) * tempo_scale_q8

            for track in tracks:
                if not track.active or track.duration == 0:
                    continue
                if track.volume_slide_ticks:
                    track.volume_slide_ticks -= 1
                    track.volume_q24 += track.volume_slide_step
                if track.pitch_slide_active:
                    if not track.pitch_slide_continuous:
                        track.pitch_slide_ticks -= 1
                        if track.pitch_slide_ticks == 0:
                            track.pitch_slide_active = False
                    track.current_note_q24 += track.pitch_slide_step
                if track.glide_ticks:
                    track.glide_ticks -= 1
                    track.current_note_q24 += track.glide_step
                if track.pan_slide_ticks:
                    track.pan_slide_ticks -= 1
                    if track.pan_slide_ticks == 0:
                        track.pan_q8 = track.pan_slide_target
                    else:
                        track.pan_q8 = _i16(
                            (track.pan_q8 + track.pan_slide_step) & 0xFFFF
                        )
                track.duration -= 1
                track.gate -= 1
                if track.duration == 1 and track.next_event_is_note:
                    if track.current_event is not None:
                        voice_events[track.current_event].release_automation.append(
                            (callback, 6, False)
                        )
                if track.gate == 0:
                    _set_event_key_off(track, voice_events, callback)

            for track in tracks:
                if stop_after_cycle:
                    break
                if not track.active or track.duration:
                    continue
                for _ in range(1_000_000):
                    instruction = decode_sequence_instruction(sequence_data, track.pc)
                    next_pc = track.pc + instruction.size
                    opcode = instruction.opcode
                    operands = instruction.operands

                    if opcode is None:
                        current_event = (
                            voice_events[track.current_event]
                            if track.current_event is not None
                            else None
                        )
                        # A tie or persistent hold prevents gate expiry, so the SPU
                        # voice keeps playing and the next note only changes pitch.
                        voice_continues = (
                            current_event is not None
                            and current_event.key_off_callback is None
                        )
                        if current_event is not None and not voice_continues:
                            current_event.stop_callback = callback + 1
                        duration = (
                            instruction.explicit_duration
                            if instruction.duration_index == 0
                            else SEQUENCE_DURATION_TICKS[instruction.duration_index]
                        )
                        if duration is None:
                            raise AssertionError("note duration was not decoded")
                        track.velocity = instruction.velocity << 8
                        note_q8 = (track.octave + instruction.semitone) * 256
                        if track.percussion:
                            mapping = percussion_map.get(
                                track.octave + instruction.semitone, 0
                            )
                            track.preset_index = mapping & 0xFF
                            note_q8 = (mapping >> 8 & 0xFF) * 256
                            track.pan_q8 = (mapping >> 24 & 0xFF) << 8
                            _load_track_preset(track, wds_by_id)
                        if track.note_volume_ticks:
                            track.volume_q24 = track.note_volume_start_q24
                            track.volume_slide_ticks = track.note_volume_ticks
                            track.volume_slide_step = track.note_volume_step
                        if track.glide_duration and note_q8 != track.previous_note_q8:
                            track.current_note_q24 = (
                                track.previous_note_q8 + track.pitch_offset_q8
                            ) << 16
                            track.glide_ticks = track.glide_duration
                            track.glide_step = _trunc_div(
                                (note_q8 - track.previous_note_q8) << 16,
                                track.glide_duration,
                            )
                        else:
                            track.current_note_q24 = (
                                note_q8 + track.pitch_offset_q8
                            ) << 16
                            track.glide_ticks = 0
                        track.previous_note_q8 = note_q8
                        preset = _get_track_preset(track, wds_by_id)
                        if preset is None:
                            unresolved_voices.add(
                                (
                                    track.instrument_wds_id
                                    if track.instrument_wds_id is not None
                                    else track.wds_id,
                                    track.preset_index,
                                )
                            )
                            if not voice_continues:
                                track.current_event = None
                        elif track.physical_voice_index >= 0 and not voice_continues:
                            voice_events.append(
                                SequenceVoiceEvent(
                                    track.index,
                                    track.physical_voice_index,
                                    callback + 1,
                                    track.instrument_wds_id,
                                    track.preset_index,
                                    _track_note_q8(track),
                                    _track_gain(track, manager_volume_q24),
                                    _track_pan_q8(track),
                                    track.adsr,
                                    track.modes,
                                    track.reverb_send,
                                    track.noise_enabled,
                                    track.frequency_modulation,
                                )
                            )
                            track.current_event = len(voice_events) - 1
                        next_event_kind = _next_timed_event_kind(
                            sequence_data, track, next_pc
                        )
                        track.next_event_is_note = next_event_kind == "note"
                        _start_timed_event(track, duration, next_event_kind == "tie")
                        _restart_note_modulators(track)
                        track.pc = next_pc
                        break
                    if opcode == 0x80:
                        _set_event_key_off(track, voice_events, callback)
                        track.next_event_is_note = (
                            _next_timed_event_kind(sequence_data, track, next_pc)
                            == "note"
                        )
                        _start_timed_event(track, operands[0], True)
                        track.pc = next_pc
                        break
                    if opcode == 0x81:
                        next_event_kind = _next_timed_event_kind(
                            sequence_data, track, next_pc
                        )
                        track.next_event_is_note = next_event_kind == "note"
                        _start_timed_event(track, operands[0], next_event_kind == "tie")
                        track.pc = next_pc
                        break
                    if opcode == 0x90:
                        if loop_target is not None:
                            if cycle_boundary_callbacks[track.index] == callback:
                                break
                            completed_cycle_counts[track.index] = min(
                                loop_target, completed_cycle_counts[track.index] + 1
                            )
                            cycle_boundary_callbacks[track.index] = callback
                            if all(
                                completed_cycle_counts[index] >= loop_target
                                for index in cycle_track_indices
                            ):
                                _set_event_key_off(track, voice_events, callback, 6)
                                track.active = False
                                track.pc = next_pc
                                stop_after_cycle = True
                                break
                            if track.saved_offset is None:
                                _set_event_key_off(track, voice_events, callback, 6)
                                if (
                                    completed_cycle_counts[track.index]
                                    >= loop_target
                                ):
                                    track.active = False
                                    track.pc = next_pc
                                    break
                                _reset_track_for_general_loop(
                                    track, initial_tracks[track.index]
                                )
                            else:
                                track.pc = track.saved_offset
                                track.octave = track.saved_octave
                            continue
                        if track.saved_offset is None:
                            _set_event_key_off(track, voice_events, callback, 6)
                            track.active = False
                            track.pc = next_pc
                            break
                        track.pc = track.saved_offset
                        track.octave = track.saved_octave
                        continue
                    if opcode == 0x91:
                        track.saved_offset = next_pc
                        track.saved_octave = track.octave
                    elif opcode == 0x94:
                        track.octave = operands[0] * 12
                    elif opcode == 0x95:
                        track.octave += 12
                    elif opcode == 0x96:
                        track.octave -= 12
                    elif opcode == 0x97:
                        pass
                    elif opcode == 0x98:
                        if len(track.loops) == 4:
                            raise ValueError(
                                f"loop stack overflow in track {track.index}"
                            )
                        track.loops.append(
                            SequenceLoopFrame(
                                (operands[0] - 1) & 0xFF, next_pc, track.octave
                            )
                        )
                    elif opcode == 0x99:
                        if not track.loops:
                            raise ValueError(
                                f"loop stack underflow in track {track.index}"
                            )
                        else:
                            loop = track.loops[-1]
                            remaining = (loop.remaining - 1) & 0xFF
                            if remaining == 0xFF:
                                track.loops.pop()
                            else:
                                track.loops[-1] = SequenceLoopFrame(
                                    remaining,
                                    loop.start_offset,
                                    loop.start_octave,
                                    next_pc,
                                    track.octave,
                                )
                                track.pc = loop.start_offset
                                track.octave = loop.start_octave
                                continue
                    elif opcode == 0x9A:
                        if not track.loops:
                            raise ValueError(
                                f"loop break outside loop in track {track.index}"
                            )
                        else:
                            loop = track.loops[-1]
                            if loop.remaining == 0:
                                if loop.exit_offset is None or loop.exit_octave is None:
                                    raise ValueError(
                                        f"unresolved loop exit in track {track.index}"
                                    )
                                track.loops.pop()
                                track.pc = loop.exit_offset
                                track.octave = loop.exit_octave
                                continue
                    elif opcode == 0x9E:
                        target_group = operands[0] | operands[1] << 8
                        target_index = operands[2]
                        raise ValueError(
                            "opcode 0x9E requires external sequence manager context "
                            f"(group {target_group}, index {target_index})"
                        )
                    elif opcode == 0xA0:
                        tempo_q16 = operands[0] << 16
                        tick_increment = operands[0] * tempo_scale_q8
                    elif opcode == 0xA2:
                        duration, target = operands
                        tempo_slide_target = target
                        if duration and target << 16 != tempo_q16:
                            tempo_slide_ticks = duration
                            tempo_slide_step = _trunc_div(
                                (target << 16) - tempo_q16, duration
                            )
                    elif opcode == 0xA6:
                        manager_volume_q24 = operands[0] << 24
                    elif opcode == 0xA9:
                        track.gate_mode = operands[0]
                    elif opcode == 0xB0:
                        track.hold_notes = True
                    elif opcode == 0xB1:
                        track.hold_notes = False
                    elif opcode == 0xB2:
                        _set_track_frequency_modulation(
                            track, voice_events, callback, True
                        )
                    elif opcode == 0xB3:
                        _set_track_frequency_modulation(
                            track, voice_events, callback, False
                        )
                    elif opcode == 0xB4:
                        noise_clock = operands[0]
                        noise_clock_automation.append(
                            (callback, min(noise_clock, 0x3F))
                        )
                        _set_track_noise(track, voice_events, callback, True)
                    elif opcode == 0xB5:
                        noise_clock = (noise_clock + operands[0]) & 0x3F
                        noise_clock_automation.append((callback, noise_clock))
                        _set_track_noise(track, voice_events, callback, True)
                    elif opcode == 0xB6:
                        _set_track_noise(track, voice_events, callback, True)
                    elif opcode == 0xB7:
                        _set_track_noise(track, voice_events, callback, False)
                    elif opcode == 0xB8:
                        reverb_depth = _signed_byte(operands[0])
                        reverb_depth_automation.append((callback + 1, reverb_depth))
                        if resource.kind == "seds":
                            external_reverb_context_required = True
                        elif resource.metadata["reverb_mode"] != 4:
                            approximated.add(opcode)
                    elif opcode == 0xBA:
                        _set_track_reverb_send(track, voice_events, callback, True)
                        if resource.kind == "seds":
                            external_reverb_context_required = True
                        elif resource.metadata["reverb_mode"] != 4:
                            approximated.add(opcode)
                    elif opcode == 0xBB:
                        _set_track_reverb_send(track, voice_events, callback, False)
                        if (
                            resource.kind == "smds"
                            and resource.metadata["reverb_mode"] != 4
                        ):
                            approximated.add(opcode)
                    elif opcode == 0xAC:
                        track.preset_index = operands[0]
                        _load_track_preset(track, wds_by_id)
                    elif opcode == 0xAD:
                        track.duration_adjust = (
                            0
                            if operands[0] == 0
                            else _signed_byte(
                                (track.duration_adjust + operands[0]) & 0xFF
                            )
                        )
                    elif opcode == 0xAE:
                        track.percussion = True
                    elif opcode == 0xAF:
                        track.percussion = False
                    elif opcode == 0xC0:
                        _load_track_preset(track, wds_by_id)
                    elif opcode == 0xC1:
                        track.modes = operands[0] | operands[1] << 4 | operands[2] << 8
                    elif opcode == 0xC2:
                        track.adsr = track.adsr & ~0x7F | operands[0] & 0x7F
                    elif opcode == 0xC3:
                        track.adsr = track.adsr & ~0xF00 | (operands[0] & 0x0F) << 8
                    elif opcode == 0xC4:
                        track.adsr = track.adsr & ~0x7F0000 | (operands[0] & 0x7F) << 16
                    elif opcode == 0xC5:
                        track.adsr = (
                            track.adsr & ~0x1F000000 | (operands[0] & 0x1F) << 24
                        )
                    elif opcode == 0xC6:
                        track.adsr = track.adsr & ~0xF000 | (operands[0] & 0x0F) << 12
                    elif opcode == 0xC7:
                        track.adsr = (
                            track.adsr & ~0xFF00
                            | (operands[0] & 0x0F) << 8
                            | (operands[1] & 0x0F) << 12
                        )
                    elif opcode == 0xC8:
                        track.modes = track.modes & ~0x7 | operands[0] & 0x7
                    elif opcode == 0xC9:
                        track.modes = track.modes & ~0x70 | (operands[0] & 0x7) << 4
                    elif opcode == 0xCA:
                        track.modes = track.modes & ~0x700 | (operands[0] & 0x7) << 8
                    elif opcode == 0xD0:
                        # Retail leaves the active note unchanged until a later note-on.
                        new_offset = _signed_byte(operands[0]) << 5
                        track.pitch_offset_q8 = new_offset
                    elif opcode == 0xD1:
                        delta = _signed_byte(operands[0]) << 5
                        track.pitch_offset_q8 = _signed_word(
                            (track.pitch_offset_q8 + delta) & 0xFF,
                            (track.pitch_offset_q8 + delta) >> 8 & 0xFF,
                        )
                    elif opcode == 0xD2:
                        delta = _signed_byte(operands[0]) << 3
                        track.pitch_offset_q8 = _signed_word(
                            (track.pitch_offset_q8 + delta) & 0xFF,
                            (track.pitch_offset_q8 + delta) >> 8 & 0xFF,
                        )
                    elif opcode == 0xD3:
                        delta = _signed_word(operands[1], operands[0])
                        track.pitch_offset_q8 = _signed_word(
                            (track.pitch_offset_q8 + delta) & 0xFF,
                            (track.pitch_offset_q8 + delta) >> 8 & 0xFF,
                        )
                    elif opcode == 0xD4:
                        duration = operands[0]
                        delta_q24 = _signed_byte(operands[1]) << 24
                        if duration and delta_q24:
                            track.pitch_slide_ticks = duration
                            track.pitch_slide_step = _trunc_div(delta_q24, duration)
                            track.pitch_slide_active = True
                        else:
                            track.pitch_slide_active = False
                    elif opcode == 0xD5:
                        track.pitch_slide_continuous = not track.pitch_slide_continuous
                    elif opcode == 0xD6:
                        track.glide_duration = operands[0]
                        if not track.glide_duration:
                            track.glide_ticks = 0
                    elif opcode == 0xD7:
                        divisor = (operands[0] + 1) & 0xFF
                        if divisor:
                            ramp_step = 0x400 // (divisor * 4)
                            track.modulators[0].ramp = ramp_step
                            track.modulators[0].ramp_step = ramp_step
                    elif opcode == 0xD8:
                        depth = _signed_byte(operands[1])
                        _configure_modulator(
                            track,
                            0,
                            0,
                            3,
                            operands[0],
                            abs(depth) * depth * 0x4000,
                            operands[2],
                            True,
                        )
                    elif opcode == 0xD9:
                        depth = _signed_byte(operands[1])
                        _configure_modulator(
                            track,
                            0,
                            0,
                            operands[2] & 0x0F,
                            operands[0],
                            abs(depth) * depth * 0x4000,
                            0,
                            not operands[2] & 0x10,
                        )
                    elif opcode == 0xDA:
                        track.modulators[0].flags |= 1
                    elif opcode == 0xDB:
                        track.modulators[0].flags &= ~1
                    elif opcode == 0xDC:
                        track.pitch_slide_ticks = 0
                        track.pitch_slide_active = False
                        track.glide_ticks = 0
                    elif opcode == 0xE0:
                        track.volume_q24 = operands[0] << 24
                        track.volume_slide_ticks = 0
                        track.note_volume_ticks = 0
                    elif opcode == 0xE1:
                        track.volume_q24 = (
                            track.volume_q24 + (_signed_byte(operands[0]) << 24)
                        ) & 0x7FFFFFFF
                        track.volume_slide_ticks = 0
                        track.note_volume_ticks = 0
                    elif opcode == 0xE2:
                        duration, target = operands[0], _signed_byte(operands[1]) << 24
                        track.note_volume_ticks = 0
                        if duration and target != track.volume_q24:
                            track.volume_slide_ticks = duration
                            track.volume_slide_step = _trunc_div(
                                target - track.volume_q24, duration
                            )
                    elif opcode == 0xE3:
                        divisor = (operands[0] + 1) & 0xFF
                        if divisor:
                            ramp_step = 0x400 // (divisor * 4)
                            track.modulators[1].ramp = ramp_step
                            track.modulators[1].ramp_step = ramp_step
                    elif opcode == 0xE4:
                        _configure_modulator(
                            track,
                            1,
                            1,
                            2,
                            operands[0],
                            _signed_byte(operands[1]) << 24,
                            operands[2],
                            True,
                        )
                    elif opcode == 0xE5:
                        _configure_modulator(
                            track,
                            1,
                            1,
                            operands[2] & 0x0F,
                            operands[0],
                            _signed_byte(operands[1]) << 24,
                            0,
                            not operands[2] & 0x10,
                        )
                    elif opcode == 0xE6:
                        track.modulators[1].flags |= 1
                    elif opcode == 0xE7:
                        track.modulators[1].flags &= ~1
                    elif opcode == 0xE8:
                        track.pan_q8 = operands[0] << 8
                        track.pan_slide_ticks = 0
                    elif opcode == 0xE9:
                        track.pan_q8 = (
                            track.pan_q8 + (_signed_byte(operands[0]) << 8)
                        ) & 0x7FFF
                    elif opcode == 0xEA:
                        duration = operands[0]
                        target = _signed_byte(operands[1])
                        current = _signed_byte(track.pan_q8 >> 8 & 0xFF)
                        if duration and target != current:
                            delta_q8 = (target - current) << 8
                            track.pan_slide_ticks = duration
                            # The original handler stores the delta, not the absolute
                            # target, as the terminal value.
                            track.pan_slide_target = _i16(delta_q8 & 0xFFFF)
                            track.pan_slide_step = _trunc_div(delta_q8, duration)
                    elif opcode == 0xEB:
                        divisor = (operands[0] + 1) & 0xFF
                        if divisor:
                            ramp_step = 0x400 // (divisor * 4)
                            track.modulators[2].ramp = ramp_step
                            track.modulators[2].ramp_step = ramp_step
                    elif opcode == 0xEC:
                        _configure_modulator(
                            track,
                            2,
                            2,
                            3,
                            operands[0],
                            _signed_byte(operands[1]) << 24,
                            operands[2],
                            True,
                        )
                    elif opcode == 0xED:
                        _configure_modulator(
                            track,
                            2,
                            2,
                            operands[2] & 0x0F,
                            operands[0],
                            _signed_byte(operands[1]) << 24,
                            0,
                            not operands[2] & 0x10,
                        )
                    elif opcode == 0xEE:
                        track.modulators[2].flags |= 1
                    elif opcode == 0xEF:
                        track.modulators[2].flags &= ~1
                    elif opcode == 0xF0:
                        slot, config, target = operands
                        if not 0 <= slot < len(track.modulators):
                            raise ValueError(f"invalid modulator slot {slot}")
                        track.selected_modulator = slot
                        modulator = track.modulators[slot]
                        modulator.waveform = config & 0x0F
                        modulator.flags = 2 if not config & 0x10 else 0
                        modulator.ramp_step = 0x400
                        modulator.delay_reload = 0
                        modulator.target = target
                    elif opcode == 0xF1:
                        modulator = track.modulators[track.selected_modulator]
                        period = operands[0] + operands[0] * operands[0] // 64
                        amplitude = _i16(operands[1] << 8 | operands[2]) << 16
                        modulator.amplitude = _scale_modulator_amplitude(
                            amplitude, period, modulator.waveform
                        )
                        modulator.phase_reload = period
                    elif opcode == 0xF2:
                        modulator = track.modulators[track.selected_modulator]
                        divisor = (operands[1] + 1) & 0xFF
                        if divisor:
                            modulator.delay_reload = operands[0] << 2
                            modulator.ramp_step = 0x400 // (divisor * 4)
                            modulator.ramp = modulator.ramp_step
                    elif opcode == 0xF6:
                        slot = operands[0]
                        if not 0 <= slot < len(track.modulators):
                            raise ValueError(f"invalid modulator slot {slot}")
                        _initialize_modulator(track.modulators[slot])
                        track.modulators[slot].flags |= 1
                    elif opcode == 0xF7:
                        slot = operands[0]
                        if not 0 <= slot < len(track.modulators):
                            raise ValueError(f"invalid modulator slot {slot}")
                        track.modulators[slot].flags &= ~1
                    elif opcode == 0xFC:
                        track.wds_id, track.preset_index = operands
                        _load_track_preset(track, wds_by_id)
                    elif opcode == 0xFD:
                        if operands[0]:
                            tempo_scale_q8 = operands[0] << 8
                            tick_increment = (tempo_q16 >> 16) * tempo_scale_q8
                    elif opcode == 0xFE:
                        track.wds_id = operands[0]
                    elif opcode == 0xF8:
                        start = operands[0] << 24
                        duration = operands[1]
                        target = operands[2] << 24
                        if duration and target != start:
                            track.note_volume_start_q24 = start
                            track.note_volume_ticks = duration
                            track.note_volume_step = _trunc_div(
                                target - start, duration
                            )
                        else:
                            track.note_volume_ticks = 0
                    elif opcode == 0xF9:
                        # Updates manager bar/beat counters, which do not feed PCM state.
                        pass
                    elif opcode in {
                        0x9C,
                        0x9D,
                        0xA1,
                        0xA7,
                        0xAA,
                    }:
                        approximated.add(opcode)
                    else:
                        raise ValueError(
                            f"unsupported opcode 0x{opcode:02X} in track {track.index}"
                        )
                    if opcode in {
                        0xAC,
                        0xC0,
                        0xC1,
                        0xC2,
                        0xC3,
                        0xC4,
                        0xC5,
                        0xC6,
                        0xC7,
                        0xC8,
                        0xC9,
                        0xCA,
                        0xFC,
                    }:
                        _capture_event_adsr(track, voice_events, callback)
                    track.pc = next_pc
                else:
                    raise ValueError(
                        f"track {track.index} exceeded the command processing limit"
                    )

        if stop_after_cycle:
            for track in tracks:
                if track.active:
                    _set_event_key_off(track, voice_events, callback, 6)
                    track.active = False

        for track in tracks:
            if track.active:
                random_state = _update_track_modulators(track, random_state)
        _capture_voice_automation(tracks, voice_events, manager_volume_q24, callback)

        if not any(track.active for track in tracks):
            callbacks = callback + 1
            break

    if loop_count is not None and any(track.active for track in tracks):
        raise ValueError(
            "sequence did not reach the requested loop count within the "
            "internal safety limit"
        )

    # Notes staged for the callback after the global boundary never reach SPU key-on.
    voice_events = [
        event for event in voice_events if event.start_callback < callbacks
    ]
    active_track_count = sum(track.pc != 0 for track in tracks)
    ended_tracks = sum(track.pc != 0 and not track.active for track in tracks)
    if ended_tracks != active_track_count:
        for track in tracks:
            if track.current_event is not None:
                event = voice_events[track.current_event]
                event.stop_callback = callbacks
                if event.key_off_callback is None:
                    event.key_off_callback = callbacks
    return SequenceSimulationResult(
        tuple(voice_events),
        callbacks,
        logical_ticks,
        ended_tracks,
        tuple(sorted(approximated)),
        tuple(sorted(unresolved_voices)),
        resource.metadata["reverb_depth"] if resource.kind == "smds" else None,
        tuple(reverb_depth_automation),
        tuple(noise_clock_automation),
        external_reverb_context_required,
    )


def _clamp16(value: int) -> int:
    return max(-32768, min(32767, value))


def _reverb_multiply(volume: int, sample: int) -> int:
    return (_i16(volume) * sample) >> 15


def _generate_spu_noise(
    frame_count: int, clock_automation: tuple[tuple[int, int], ...]
) -> array.array:
    output = array.array("h")
    output.extend(array.array("h", [0]) * frame_count)
    changes = tuple(
        (
            callback * WAV_SAMPLE_RATE // SEQUENCER_CALLBACK_RATE,
            clock,
        )
        for callback, clock in clock_automation
    )
    change_index = 0
    clock = 0
    noise_divider = 0
    noise_counter = 0
    noise_level = 0
    for frame in range(frame_count):
        while change_index < len(changes) and changes[change_index][0] <= frame:
            clock = changes[change_index][1]
            change_index += 1
        output[frame] = _i16(noise_level)
        divider_increment = 2 << (clock >> 2)
        counter_increment = 4 + (clock & 3)
        if clock >= 0x3C:
            divider_increment = 0x8000
            counter_increment = 8
        noise_divider += divider_increment
        if noise_divider & 0x8000:
            noise_divider = 0
            noise_counter += counter_increment
            if noise_counter & 8:
                noise_counter &= 7
                parity = (
                    (noise_level >> 15)
                    ^ (noise_level >> 12)
                    ^ (noise_level >> 11)
                    ^ (noise_level >> 10)
                    ^ 1
                ) & 1
                noise_level = ((noise_level << 1) | parity) & 0xFFFF
    return output


def _apply_spu_reverb_mode4_python(
    mix: array.array,
    send: array.array,
    depth: int,
    depth_automation: tuple[tuple[int, int], ...],
    *,
    tail_frames: int = WAV_SAMPLE_RATE * SEQUENCE_REVERB_TAIL_SECONDS,
) -> None:
    if not any(send):
        for index, sample in enumerate(mix):
            mix[index] = _clamp16(sample)
        return
    registers = SPU_REVERB_MODE4_REGISTERS
    (
        dapf1,
        dapf2,
        viir,
        vcomb1,
        vcomb2,
        vcomb3,
        vcomb4,
        vwall,
        vapf1,
        vapf2,
        mlsame,
        mrsame,
        mlcomb1,
        mrcomb1,
        mlcomb2,
        mrcomb2,
        dlsame,
        drsame,
        mldiff,
        mrdiff,
        mlcomb3,
        mrcomb3,
        mlcomb4,
        mrcomb4,
        dldiff,
        drdiff,
        mlapf1,
        mrapf1,
        mlapf2,
        mrapf2,
        vlin,
        vrin,
    ) = registers
    work_size = (0x10000 - SPU_REVERB_MODE4_BASE) * 4
    work = array.array("h", [0]) * work_size
    cursor = 0
    resample_position = 0
    downsample = [[0] * 64 for _ in range(2)]
    upsample = [[0] * 32 for _ in range(2)]
    resample_coefficients = (
        -0x0001,
        0x0002,
        -0x000A,
        0x0023,
        -0x0067,
        0x010A,
        -0x0268,
        0x0534,
        -0x0B90,
        0x2806,
        0x2806,
        -0x0B90,
        0x0534,
        -0x0268,
        0x010A,
        -0x0067,
        0x0023,
        -0x000A,
        0x0002,
        -0x0001,
    )
    source_frames = len(mix) // 2
    max_frames = source_frames + tail_frames
    last_wet_frame = -1
    depth_changes = tuple(
        (
            callback * WAV_SAMPLE_RATE // SEQUENCER_CALLBACK_RATE,
            new_depth,
        )
        for callback, new_depth in depth_automation
    )
    depth_change_index = 0

    def read(register: int, bias: int = 0) -> int:
        return work[(cursor + register * 4 + bias) % work_size]

    def write(register: int, value: int) -> None:
        work[(cursor + register * 4) % work_size] = _clamp16(value)

    def negate(value: int) -> int:
        value = _i16(value)
        return 0x7FFF if value == -0x8000 else -value

    def iir_complement(sample: int) -> int:
        alpha = _i16(viir)
        if alpha == -0x8000:
            return 0 if sample == -0x8000 else sample * -0x10000
        return sample * (0x8000 - alpha)

    for frame in range(max_frames):
        while (
            depth_change_index < len(depth_changes)
            and depth_changes[depth_change_index][0] <= frame
        ):
            depth = depth_changes[depth_change_index][1]
            depth_change_index += 1

        inputs = (
            _clamp16(send[frame * 2] if frame * 2 < len(send) else 0),
            _clamp16(send[frame * 2 + 1] if frame * 2 + 1 < len(send) else 0),
        )
        for channel, sample in enumerate(inputs):
            downsample[channel][resample_position] = sample

        if resample_position & 1:
            downsampled = []
            start = (resample_position - 38) & 0x3F
            for channel in range(2):
                samples = downsample[channel]
                accumulator = sum(
                    coefficient * samples[(start + index * 2) & 0x3F]
                    for index, coefficient in enumerate(resample_coefficients)
                )
                accumulator += 0x4000 * samples[(start + 19) & 0x3F]
                downsampled.append(_clamp16(accumulator >> 15))

            for channel in range(2):
                same_source = (dlsame, drsame)[channel]
                diff_source = (drdiff, dldiff)[channel]
                same_dest = (mlsame, mrsame)[channel]
                diff_dest = (mldiff, mrdiff)[channel]
                input_coefficient = (vlin, vrin)[channel]
                iir_input_same = _clamp16(
                    (
                        (read(same_source) * _i16(vwall) >> 14)
                        + (downsampled[channel] * _i16(input_coefficient) >> 14)
                    )
                    >> 1
                )
                iir_input_diff = _clamp16(
                    (
                        (read(diff_source) * _i16(vwall) >> 14)
                        + (downsampled[channel] * _i16(input_coefficient) >> 14)
                    )
                    >> 1
                )
                previous = read(same_dest, -1)
                iir_same = _clamp16(
                    (
                        (iir_input_same * _i16(viir) >> 14)
                        + (iir_complement(previous) >> 14)
                    )
                    >> 1
                )
                previous = read(diff_dest, -1)
                iir_diff = _clamp16(
                    (
                        (iir_input_diff * _i16(viir) >> 14)
                        + (iir_complement(previous) >> 14)
                    )
                    >> 1
                )
                write(same_dest, iir_same)
                write(diff_dest, iir_diff)

                comb_sources = (
                    (mlcomb1, mlcomb2, mlcomb3, mlcomb4),
                    (mrcomb1, mrcomb2, mrcomb3, mrcomb4),
                )[channel]
                accumulator = sum(
                    read(source) * _i16(coefficient) >> 14
                    for source, coefficient in zip(
                        comb_sources,
                        (vcomb1, vcomb2, vcomb3, vcomb4),
                        strict=True,
                    )
                )
                mix_dest_a = (mlapf1, mrapf1)[channel]
                mix_dest_b = (mlapf2, mrapf2)[channel]
                feedback_a = read(mix_dest_a, -(dapf1 * 4))
                feedback_b = read(mix_dest_b, -(dapf2 * 4))
                mix_a = _clamp16(
                    (accumulator + (feedback_a * negate(vapf1) >> 14)) >> 1
                )
                mix_b = _clamp16(
                    feedback_a
                    + ((mix_a * _i16(vapf1) >> 14) + (feedback_b * negate(vapf2) >> 14))
                    // 2
                )
                reverb_sample = _clamp16(feedback_b + (mix_b * _i16(vapf2) >> 15))
                upsample[channel][resample_position >> 1] = reverb_sample
                write(mix_dest_a, mix_a)
                write(mix_dest_b, mix_b)

            cursor = 0 if cursor + 1 == work_size else cursor + 1

        wet = []
        if resample_position & 1:
            start = ((resample_position >> 1) - 19) & 0x1F
            for channel in range(2):
                accumulator = sum(
                    coefficient * upsample[channel][(start + index) & 0x1F]
                    for index, coefficient in enumerate(resample_coefficients)
                )
                wet.append(_clamp16(accumulator >> 14))
        else:
            index = (((resample_position >> 1) - 19) & 0x1F) + 9
            wet = [upsample[channel][index & 0x1F] for channel in range(2)]

        output_volume = _i16(depth << 8)
        wet_l = _reverb_multiply(output_volume, wet[0])
        wet_r = _reverb_multiply(output_volume, wet[1])
        if frame * 2 + 1 >= len(mix):
            mix.extend(array.array("i", [0, 0]))
        mix[frame * 2] = _clamp16(mix[frame * 2] + wet_l)
        mix[frame * 2 + 1] = _clamp16(mix[frame * 2 + 1] + wet_r)
        if abs(wet_l) > 1 or abs(wet_r) > 1:
            last_wet_frame = frame
        resample_position = (resample_position + 1) & 0x3F

    final_frames = max(source_frames, last_wet_frame + 1)
    del mix[final_frames * 2 :]


@functools.cache
def _load_fast_reverb() -> ctypes._CFuncPtr | None:
    if os.environ.get("XENOGEARS_DISABLE_FAST_REVERB") == "1":
        return None
    try:
        source = SPU_REVERB_FAST_SOURCE.read_bytes()
        digest = hashlib.sha256(source).hexdigest()[:16]
        library_path = Path(tempfile.gettempdir()) / f"xg-spu-reverb-{digest}.so"
        if not library_path.exists():
            temporary_path = library_path.with_name(
                f".{library_path.name}.{os.getpid()}.tmp"
            )
            subprocess.run(
                [
                    "cc",
                    "-O3",
                    "-std=c99",
                    "-shared",
                    "-fPIC",
                    str(SPU_REVERB_FAST_SOURCE),
                    "-o",
                    str(temporary_path),
                ],
                check=True,
                capture_output=True,
            )
            temporary_path.replace(library_path)
        function = ctypes.CDLL(str(library_path)).xg_spu_reverb_mode4
        function.argtypes = [
            ctypes.POINTER(ctypes.c_int32),
            ctypes.c_size_t,
            ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_int32),
            ctypes.c_size_t,
            ctypes.c_int32,
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.POINTER(ctypes.c_int32),
            ctypes.c_size_t,
            ctypes.c_size_t,
        ]
        function.restype = ctypes.c_size_t
        return function
    except (OSError, subprocess.SubprocessError):
        return None


def _apply_spu_reverb_mode4(
    mix: array.array,
    send: array.array,
    depth: int,
    depth_automation: tuple[tuple[int, int], ...],
    *,
    tail_frames: int = WAV_SAMPLE_RATE * SEQUENCE_REVERB_TAIL_SECONDS,
) -> None:
    if not any(send):
        for index, sample in enumerate(mix):
            mix[index] = _clamp16(sample)
        return
    fast_reverb = _load_fast_reverb()
    if fast_reverb is None or mix.itemsize != 4 or send.itemsize != 4:
        _apply_spu_reverb_mode4_python(
            mix,
            send,
            depth,
            depth_automation,
            tail_frames=tail_frames,
        )
        return

    source_frames = len(mix) // 2
    capacity_frames = ((source_frames + 1) // 2 + tail_frames // 2) * 2
    mix.extend(array.array("i", [0]) * ((capacity_frames - source_frames) * 2))
    depth_changes = tuple(
        (
            callback * WAV_SAMPLE_RATE // SEQUENCER_CALLBACK_RATE,
            new_depth,
        )
        for callback, new_depth in depth_automation
    )
    change_frames = (ctypes.c_uint32 * len(depth_changes))(
        *(frame for frame, _ in depth_changes)
    )
    change_depths = (ctypes.c_int32 * len(depth_changes))(
        *(new_depth for _, new_depth in depth_changes)
    )
    mix_buffer = (ctypes.c_int32 * len(mix)).from_buffer(mix)
    send_buffer = (ctypes.c_int32 * len(send)).from_buffer(send)
    final_frames = fast_reverb(
        mix_buffer,
        source_frames,
        capacity_frames,
        send_buffer,
        len(send),
        depth,
        change_frames,
        change_depths,
        len(depth_changes),
        tail_frames,
    )
    if final_frames == ctypes.c_size_t(-1).value:
        raise MemoryError("fast SPU reverb failed to allocate its work area")
    del mix_buffer, send_buffer
    del mix[final_frames * 2 :]


def render_sequence(
    resource: AudioResource,
    wds_by_id: dict[int, AudioResource],
    *,
    entry_index: int = 0,
    max_seconds: float = 120.0,
    ignore_loops: bool = False,
    loop_count: int | None = None,
    seds_reverb_depth: int | None = DEFAULT_SEDS_REVERB_DEPTH,
) -> SequenceRenderResult:
    if seds_reverb_depth is not None and not 0 <= seds_reverb_depth <= 0x7F:
        raise ValueError("SEDS reverb depth must be between 0 and 127")
    simulation = simulate_sequence(
        resource,
        wds_by_id,
        entry_index=entry_index,
        max_seconds=max_seconds,
        ignore_loops=ignore_loops,
        loop_count=loop_count,
    )
    ended = simulation.ended_tracks > 0 and simulation.ended_tracks == (
        len(resource.metadata["track_offsets"])
        if resource.kind == "smds"
        else sum(
            bool(offset)
            for offset in resource.metadata["events"][entry_index][
                "channel_script_offsets"
            ]
        )
    )
    sequence_end_frame = (
        simulation.callbacks * WAV_SAMPLE_RATE // SEQUENCER_CALLBACK_RATE
    )
    release_tail = WAV_SAMPLE_RATE * SEQUENCE_RELEASE_TAIL_SECONDS if ended else 0
    noise_samples = (
        _generate_spu_noise(
            sequence_end_frame + release_tail, simulation.noise_clock_automation
        )
        if any(
            event.noise_enabled or event.noise_automation for event in simulation.events
        )
        else None
    )
    mix = array.array("i")
    maximum_mix_frames = sequence_end_frame + release_tail
    mix.extend(array.array("i", [0]) * (maximum_mix_frames * 2))
    has_reverb_send = any(
        event.reverb_send or any(enabled for _, enabled in event.reverb_automation)
        for event in simulation.events
    )
    if resource.kind == "seds" and has_reverb_send and seds_reverb_depth is None:
        raise ValueError(
            "SEDS reverb depth is inherited from external state; "
            "provide seds_reverb_depth"
        )
    use_reverb = has_reverb_send and (
        resource.kind == "smds"
        and resource.metadata["reverb_mode"] == 4
        or resource.kind == "seds"
        and seds_reverb_depth is not None
    )
    reverb_send = (
        array.array("i", [0]) * (maximum_mix_frames * 2)
        if use_reverb
        else array.array("i")
    )
    fm_source_voices = {
        event.physical_voice_index - 1
        for event in simulation.events
        if event.frequency_modulation
        or any(enabled for _, enabled in event.frequency_modulation_automation)
    }
    voice_outputs: dict[int, array.array] = {}
    last_mix_frame = 0

    for event in sorted(
        simulation.events, key=lambda item: (item.track_index, item.start_callback)
    ):
        wds = wds_by_id[event.wds_id]
        preset = _resolve_wds_preset(wds, event.preset_index)
        if preset is None:
            raise ValueError(
                f"WDS {event.wds_id} preset {event.preset_index} became unavailable"
            )
        preset = replace(preset, adsr=event.adsr, modes=event.modes)
        start_frame = event.start_callback * WAV_SAMPLE_RATE // SEQUENCER_CALLBACK_RATE
        stop_frame = (
            event.stop_callback * WAV_SAMPLE_RATE // SEQUENCER_CALLBACK_RATE
            if event.stop_callback is not None
            else sequence_end_frame + release_tail
        )
        max_frames = max(0, stop_frame - start_frame)
        key_off_frame = (
            max(
                0,
                event.key_off_callback * WAV_SAMPLE_RATE // SEQUENCER_CALLBACK_RATE
                - start_frame,
            )
            if event.key_off_callback is not None
            else None
        )
        captured_voice = (
            array.array("h") if event.physical_voice_index in fm_source_voices else None
        )
        modulation_source = voice_outputs.get(event.physical_voice_index - 1)
        rendered = render_wds_voice(
            wds,
            preset,
            note_q8=event.note_q8,
            gain=event.gain,
            pan_q8=event.pan_q8,
            key_off_frame=key_off_frame,
            max_frames=max_frames,
            parameter_changes=tuple(
                (
                    max(
                        0,
                        change_callback * WAV_SAMPLE_RATE // SEQUENCER_CALLBACK_RATE
                        - start_frame,
                    ),
                    note_q8,
                    gain,
                    pan_q8,
                )
                for change_callback, note_q8, gain, pan_q8 in event.automation
            ),
            noise_samples=noise_samples,
            noise_frame_offset=start_frame,
            noise_enabled=event.noise_enabled,
            noise_changes=tuple(
                (
                    max(
                        0,
                        change_callback * WAV_SAMPLE_RATE // SEQUENCER_CALLBACK_RATE
                        - start_frame,
                    ),
                    enabled,
                )
                for change_callback, enabled in event.noise_automation
            ),
            frequency_modulation_samples=modulation_source,
            frequency_modulation_frame_offset=start_frame,
            frequency_modulation_enabled=event.frequency_modulation,
            frequency_modulation_changes=tuple(
                (
                    max(
                        0,
                        change_callback * WAV_SAMPLE_RATE // SEQUENCER_CALLBACK_RATE
                        - start_frame,
                    ),
                    enabled,
                )
                for change_callback, enabled in event.frequency_modulation_automation
            ),
            adsr_changes=tuple(
                (
                    max(
                        0,
                        change_callback * WAV_SAMPLE_RATE // SEQUENCER_CALLBACK_RATE
                        - start_frame,
                    ),
                    adsr,
                    modes,
                )
                for change_callback, adsr, modes in event.adsr_automation
            ),
            release_changes=tuple(
                (
                    max(
                        0,
                        change_callback * WAV_SAMPLE_RATE // SEQUENCER_CALLBACK_RATE
                        - start_frame,
                    ),
                    release_rate,
                    release_exponential,
                )
                for change_callback, release_rate, release_exponential in event.release_automation
            ),
            voice_output=captured_voice,
        )
        if captured_voice is not None:
            track_output = voice_outputs.setdefault(
                event.physical_voice_index,
                array.array("h", [0]) * maximum_mix_frames,
            )
            for frame, sample in enumerate(captured_voice, start_frame):
                track_output[frame] = sample
        required_samples = start_frame * 2 + len(rendered)
        last_mix_frame = max(last_mix_frame, required_samples // 2)
        send_enabled = event.reverb_send
        send_changes = tuple(
            (
                callback * WAV_SAMPLE_RATE // SEQUENCER_CALLBACK_RATE,
                enabled,
            )
            for callback, enabled in event.reverb_automation
        )
        send_change_index = 0
        for frame in range(len(rendered) // 2):
            absolute_frame = start_frame + frame
            while (
                send_change_index < len(send_changes)
                and send_changes[send_change_index][0] <= absolute_frame
            ):
                send_enabled = send_changes[send_change_index][1]
                send_change_index += 1
            output_index = absolute_frame * 2
            left = rendered[frame * 2]
            right = rendered[frame * 2 + 1]
            mix[output_index] += left
            mix[output_index + 1] += right
            if send_enabled and use_reverb:
                send_index = absolute_frame * 2
                reverb_send[send_index] += left
                reverb_send[send_index + 1] += right

    del mix[last_mix_frame * 2 :]
    if use_reverb:
        _apply_spu_reverb_mode4(
            mix,
            reverb_send,
            simulation.reverb_depth
            if simulation.reverb_depth is not None
            else seds_reverb_depth or 0,
            simulation.reverb_depth_automation,
        )
    else:
        for index, sample in enumerate(mix):
            mix[index] = _clamp16(sample)
    main_volume = SPU_MAIN_VOLUME << 1
    for index, sample in enumerate(mix):
        mix[index] = _clamp16(_reverb_multiply(main_volume, sample))
    return SequenceRenderResult(
        mix,
        simulation.callbacks,
        simulation.logical_ticks,
        len(simulation.events),
        simulation.ended_tracks,
        simulation.approximated_opcodes,
        simulation.unresolved_voices,
        simulation.external_reverb_context_required,
        seds_reverb_depth if resource.kind == "seds" and use_reverb else None,
    )


def parse_audio_resource(data: bytes, offset: int) -> AudioResource | None:
    magic = data[offset : offset + 4]
    if magic == WDS_MAGIC:
        return parse_wds(data, offset)
    if magic == SEDS_MAGIC:
        return parse_seds(data, offset)
    if magic == SMDS_MAGIC:
        return parse_smds(data, offset)
    return None


def scan_audio_resources(data: bytes) -> list[tuple[int, AudioResource]]:
    candidates = set()
    for magic in MAGICS:
        cursor = 0
        while True:
            cursor = data.find(magic, cursor)
            if cursor < 0:
                break
            candidates.add(cursor)
            cursor += 1

    resources = []
    for offset in sorted(candidates):
        resource = parse_audio_resource(data, offset)
        if resource is not None:
            resources.append((offset, resource))
    return resources


def decode_psx_adpcm(adpcm: bytes) -> list[int]:
    if not adpcm or len(adpcm) % 16:
        raise ValueError("PS1 ADPCM data must contain complete 16-byte blocks")
    filter_0 = (0, 60, 115, 98, 122)
    filter_1 = (0, 0, -52, -55, -60)
    history_1 = 0
    history_2 = 0
    pcm = []
    for block_offset in range(0, len(adpcm), 16):
        header = adpcm[block_offset]
        shift = header & 0x0F
        if shift > 12:
            shift = 9
        predictor = (header >> 4) & 0x0F
        if predictor > 4:
            predictor = 0
        for packed in adpcm[block_offset + 2 : block_offset + 16]:
            for nibble in (packed & 0x0F, packed >> 4):
                if nibble & 8:
                    nibble -= 16
                sample = (nibble << 12) >> shift
                sample += (history_1 * filter_0[predictor] >> 6) + (
                    history_2 * filter_1[predictor] >> 6
                )
                sample = max(-32768, min(32767, sample))
                history_2 = history_1
                history_1 = sample
                pcm.append(sample)
    return pcm


def q8_semitone_to_spu_pitch(note_q8: int) -> int:
    masked_note = note_q8 & 0x7FFF
    note_index = masked_note >> 8
    if note_index < 117:
        packed_note = (note_index // 12 << 4) | note_index % 12
    elif note_index < 120:
        packed_note = 0
    else:
        # Retail's 120-byte note map falls through into the adjacent ratio table.
        packed_note = (0x00, 0x20, 0x02, 0x20, 0x04, 0x20, 0x06, 0x20)[
            note_index - 120
        ]
    semitone_q8 = (packed_note & 0x0F) * 256 + (masked_note & 0xFF)
    octave = packed_note >> 4
    ratio = int(0x2000 * math.pow(2, semitone_q8 / (12 * 256)) + 0.5)
    shift = 6 - octave
    pitch = ratio >> shift if shift >= 0 else ratio << -shift
    return pitch & 0x3FFF


def wds_pitch_to_sample_rate(pitch_q8: int) -> int:
    spu_pitch = q8_semitone_to_spu_pitch(60 * 256 + pitch_q8)
    return (WAV_SAMPLE_RATE * spu_pitch + 0x800) // 0x1000


@functools.cache
def _load_spu_gauss_table() -> tuple[int, ...]:
    source = SPU_GAUSS_HEADER.read_text(encoding="ascii")
    initializer = source.split("spu_gauss_table[512] = {", 1)[1].split("};", 1)[0]
    table = tuple(int(value) for value in re.findall(r"-?\d+", initializer))
    if len(table) != 512:
        raise ValueError(f"expected 512 SPU Gaussian coefficients, found {len(table)}")
    return table


def _calculate_adsr_delta(
    zero_speed: int,
    speed: int,
    exponential: bool,
    decreasing: bool,
    invert_increment: bool,
    current: int,
) -> tuple[int, int]:
    increment = 7 - (speed & 3)
    if invert_increment:
        increment = ~increment
    divider_increment = 32768
    if speed < 0x2C:
        increment <<= (0x2F - speed) >> 2
    if speed >= 0x30:
        divider_increment >>= (speed - 0x2C) >> 2
    if exponential:
        if decreasing:
            increment = (current * increment) >> 15
        elif current >= 0x6000:
            if speed < 0x28:
                increment >>= 2
            elif speed >= 0x2C:
                divider_increment >>= 2
            else:
                increment >>= 1
                divider_increment >>= 1
    if divider_increment == 0 and speed < zero_speed:
        divider_increment = 1
    return increment, divider_increment


def render_wds_voice(
    resource: AudioResource,
    preset: WdsPreset,
    *,
    note_q8: int = 60 * 256,
    gain: int = 0x3FFF,
    pan_q8: int = 0x4000,
    key_off_frame: int | None = None,
    max_frames: int = WAV_SAMPLE_RATE * 10,
    parameter_changes: tuple[tuple[int, int, int, int], ...] = (),
    noise_samples: array.array | None = None,
    noise_frame_offset: int = 0,
    noise_enabled: bool = False,
    noise_changes: tuple[tuple[int, bool], ...] = (),
    frequency_modulation_samples: array.array | None = None,
    frequency_modulation_frame_offset: int = 0,
    frequency_modulation_enabled: bool = False,
    frequency_modulation_changes: tuple[tuple[int, bool], ...] = (),
    adsr_changes: tuple[tuple[int, int, int], ...] = (),
    release_changes: tuple[tuple[int, int, bool], ...] = (),
    voice_output: array.array | None = None,
) -> list[int]:
    if resource.kind != "wds":
        raise ValueError("voice rendering requires a WDS resource")
    if not 0 <= gain <= 0x3FFF:
        raise ValueError("gain must be between 0 and 0x3FFF")
    if max_frames < 0 or key_off_frame is not None and key_off_frame < 0:
        raise ValueError("frame limits must not be negative")

    def calculate_voice_parameters(
        current_note_q8: int, current_gain: int, current_pan_q8: int
    ) -> tuple[int, int, int]:
        current_pan_q8 = max(0, min(0x7F00, current_pan_q8))
        if current_pan_q8 < 0x4000:
            left_coefficient = 0x7F00 - ((current_pan_q8 * 0x2500) >> 14)
            right_coefficient = (current_pan_q8 * 0x5A00) >> 14
        else:
            distance = 0x8000 - current_pan_q8
            left_coefficient = (distance * 0x5A00) >> 14
            right_coefficient = 0x7F00 - ((distance * 0x2500) >> 14)
        current_gain = max(0, min(0x3FFF, current_gain))
        volume_left = (left_coefficient * current_gain) >> 15
        volume_right = (right_coefficient * current_gain) >> 15
        return (
            q8_semitone_to_spu_pitch(current_note_q8 + preset.pitch),
            volume_left << 1,
            volume_right << 1,
        )

    pitch, effective_left, effective_right = calculate_voice_parameters(
        note_q8, gain, pan_q8
    )
    change_index = 0
    noise_change_index = 0
    frequency_modulation_change_index = 0
    adsr_change_index = 0
    release_change_index = 0

    adsr = preset.adsr
    modes = preset.modes
    sustain_level = adsr >> 12 & 0x0F
    decay_rate = adsr >> 8 & 0x0F
    attack_rate = adsr & 0x7F
    release_rate = adsr >> 24 & 0x1F
    sustain_rate = adsr >> 16 & 0x7F
    attack_exponential = bool(modes >> 2 & 1)
    sustain_decreasing = bool(modes >> 5 & 1)
    sustain_exponential = bool(modes >> 6 & 1)
    release_exponential = bool(modes >> 10 & 1)

    adpcm_offset = resource.metadata["adpcm_data_offset"]
    adpcm_size = resource.metadata["adpcm_data_size"]
    adpcm = resource.payload[adpcm_offset : adpcm_offset + adpcm_size]
    current_offset = (preset.start_units & 0xFFFF) * 8
    repeat_offset = ((preset.start_units + preset.repeat_units) & 0xFFFF) * 8
    if repeat_offset >= len(adpcm):
        raise ValueError(f"WDS preset {preset.index} has an invalid repeat address")

    gauss = _load_spu_gauss_table()
    decoded = [0] * 28
    previous = [0, 0, 0]
    # Beetle's first Gaussian window is decoded samples 0 through 3 and its
    # cursor remains there for the four-frame key-on delay.
    sample_index = 31
    phase = 0
    startup_delay = 4
    history_1 = 0
    history_2 = 0
    block_flags = 0
    envelope_level = 0
    envelope_divider = 0
    envelope_phase = "attack"
    output = [0] * (max_frames * 2)
    output_index = 0

    for frame in range(max_frames):
        while (
            change_index < len(parameter_changes)
            and parameter_changes[change_index][0] <= frame
        ):
            _, note_q8, gain, pan_q8 = parameter_changes[change_index]
            pitch, effective_left, effective_right = calculate_voice_parameters(
                note_q8, gain, pan_q8
            )
            change_index += 1
        while (
            noise_change_index < len(noise_changes)
            and noise_changes[noise_change_index][0] <= frame
        ):
            noise_enabled = noise_changes[noise_change_index][1]
            noise_change_index += 1
        while (
            frequency_modulation_change_index < len(frequency_modulation_changes)
            and frequency_modulation_changes[frequency_modulation_change_index][0]
            <= frame
        ):
            frequency_modulation_enabled = frequency_modulation_changes[
                frequency_modulation_change_index
            ][1]
            frequency_modulation_change_index += 1
        while (
            adsr_change_index < len(adsr_changes)
            and adsr_changes[adsr_change_index][0] <= frame
        ):
            _, adsr, modes = adsr_changes[adsr_change_index]
            sustain_level = adsr >> 12 & 0x0F
            decay_rate = adsr >> 8 & 0x0F
            attack_rate = adsr & 0x7F
            release_rate = adsr >> 24 & 0x1F
            sustain_rate = adsr >> 16 & 0x7F
            attack_exponential = bool(modes >> 2 & 1)
            sustain_decreasing = bool(modes >> 5 & 1)
            sustain_exponential = bool(modes >> 6 & 1)
            release_exponential = bool(modes >> 10 & 1)
            adsr_change_index += 1
        while (
            release_change_index < len(release_changes)
            and release_changes[release_change_index][0] <= frame
        ):
            _, release_rate, release_exponential = release_changes[release_change_index]
            release_change_index += 1
        if key_off_frame == frame:
            envelope_phase = "release"
            envelope_divider = 0

        voice_ended = False
        while sample_index >= 28:
            sample_index -= 28
            if block_flags & 1:
                current_offset = repeat_offset
                if not block_flags & 2:
                    voice_ended = True
                    break
            if current_offset + 16 > len(adpcm):
                raise ValueError(f"WDS preset {preset.index} reads past its ADPCM bank")
            block = adpcm[current_offset : current_offset + 16]
            previous[:] = decoded[-3:]
            header = block[0]
            block_flags = block[1]
            shift = header & 0x0F
            if shift > 12:
                shift = 9
            predictor = header >> 4 & 0x0F
            if predictor > 4:
                predictor = 0
            filter_0 = (0, 60, 115, 98, 122)[predictor]
            filter_1 = (0, 0, -52, -55, -60)[predictor]
            decoded = []
            for packed in block[2:]:
                for nibble in (packed & 0x0F, packed >> 4):
                    if nibble & 8:
                        nibble -= 16
                    sample = (nibble << 12) >> shift
                    sample += (history_1 * filter_0 >> 6) + (
                        history_2 * filter_1 >> 6
                    )
                    if sample > 32767:
                        sample = 32767
                    elif sample < -32768:
                        sample = -32768
                    history_2 = history_1
                    history_1 = sample
                    decoded.append(sample)
            if block_flags & 4:
                repeat_offset = current_offset
            current_offset += 16
        if voice_ended:
            break

        gaussian_index = phase >> 4 & 0xFF
        if sample_index == 0:
            window_0, window_1, window_2 = previous
        elif sample_index == 1:
            window_0, window_1, window_2 = previous[1], previous[2], decoded[0]
        elif sample_index == 2:
            window_0, window_1, window_2 = previous[2], decoded[0], decoded[1]
        else:
            window_0 = decoded[sample_index - 3]
            window_1 = decoded[sample_index - 2]
            window_2 = decoded[sample_index - 1]
        window_3 = decoded[sample_index]
        raw_sample = (
            gauss[0x0FF - gaussian_index] * window_0
            + gauss[0x1FF - gaussian_index] * window_1
            + gauss[0x100 + gaussian_index] * window_2
            + gauss[gaussian_index] * window_3
        ) >> 15
        if noise_enabled and noise_samples is not None:
            noise_index = noise_frame_offset + frame
            raw_sample = (
                noise_samples[noise_index] if noise_index < len(noise_samples) else 0
            )
        shaped = raw_sample * envelope_level >> 15
        if voice_output is not None:
            voice_output.append(_clamp16(shaped))
        left = shaped * effective_left >> 15
        right = shaped * effective_right >> 15
        output[output_index] = left
        output[output_index + 1] = right
        output_index += 2

        if startup_delay:
            startup_delay -= 1
            continue

        if envelope_phase == "attack" and envelope_level == 0x7FFF:
            envelope_phase = "decay"
        if envelope_phase == "attack":
            increment, divider_increment = _calculate_adsr_delta(
                0x7F, attack_rate, attack_exponential, False, False, envelope_level
            )
            overflow_level = 0x7FFF
        elif envelope_phase == "decay":
            increment, divider_increment = _calculate_adsr_delta(
                0x7C, decay_rate << 2, True, True, True, envelope_level
            )
            overflow_level = 0
        elif envelope_phase == "sustain":
            increment, divider_increment = _calculate_adsr_delta(
                0x7F,
                sustain_rate,
                sustain_exponential,
                sustain_decreasing,
                sustain_decreasing,
                envelope_level,
            )
            overflow_level = 0 if sustain_decreasing else 0x7FFF
        else:
            increment, divider_increment = _calculate_adsr_delta(
                0x7C, release_rate << 2, release_exponential, True, True, envelope_level
            )
            overflow_level = 0

        envelope_divider += divider_increment
        if envelope_divider & 0x8000:
            previous_level = envelope_level
            envelope_divider = 0
            envelope_level = (envelope_level + increment) & 0xFFFF
            if envelope_phase == "attack":
                if (previous_level ^ envelope_level) & envelope_level & 0x8000:
                    envelope_level = overflow_level
            elif envelope_level & 0x8000:
                envelope_level = overflow_level
            if (
                envelope_phase == "decay"
                and envelope_level < (sustain_level + 1) << 11
            ):
                envelope_phase = "sustain"
        if envelope_phase == "release" and envelope_level == 0:
            break

        frame_pitch = pitch
        if frequency_modulation_enabled and frequency_modulation_samples is not None:
            modulation_index = frequency_modulation_frame_offset + frame
            modulation = (
                frequency_modulation_samples[modulation_index]
                if modulation_index < len(frequency_modulation_samples)
                else 0
            )
            frame_pitch = max(0, min(0x3FFF, pitch * (0x8000 + modulation) >> 15))
        phase += frame_pitch
        while phase >= 0x1000:
            phase -= 0x1000
            sample_index += 1

    if output_index != len(output):
        del output[output_index:]
    return output


def extract_wds_preset(
    resource: AudioResource, preset: WdsPreset
) -> tuple[bytes, dict]:
    if resource.kind != "wds":
        raise ValueError("preset extraction requires a WDS resource")
    adpcm_offset = resource.metadata["adpcm_data_offset"]
    adpcm_size = resource.metadata["adpcm_data_size"]
    data = resource.payload[adpcm_offset : adpcm_offset + adpcm_size]
    start = preset.start_units * 8
    blocks = bytearray()
    end_flags = None
    loop_flag_block = None
    for block_offset in range(start, len(data), 16):
        block = data[block_offset : block_offset + 16]
        if len(block) != 16:
            break
        blocks.extend(block)
        flags = block[1]
        if flags & 4:
            loop_flag_block = (block_offset - start) // 16
        if flags & 1:
            end_flags = flags
            break
    if end_flags is None:
        raise ValueError(f"WDS preset {preset.index} has no ADPCM end block")

    repeat_byte = preset.repeat_units * 8
    configured_repeat_sample = repeat_byte // 16 * 28
    sample_count = len(blocks) // 16 * 28
    effective_repeat_sample = (
        loop_flag_block * 28
        if loop_flag_block is not None
        else configured_repeat_sample
    )
    loops = bool(end_flags & 2) and 0 <= effective_repeat_sample < sample_count
    return bytes(blocks), {
        "encoded_offset": start,
        "encoded_size": len(blocks),
        "block_count": len(blocks) // 16,
        "sample_count": sample_count,
        "repeat_offset": repeat_byte,
        "effective_repeat_offset": effective_repeat_sample // 28 * 16,
        "loop_start_sample": effective_repeat_sample if loops else None,
        "loop_flag_sample": loop_flag_block * 28
        if loop_flag_block is not None
        else None,
        "loops": loops,
        "end_flags": end_flags,
    }


def write_pcm_wav(
    path: Path, samples: list[int], sample_rate: int = WAV_SAMPLE_RATE
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    pcm = array.array("h", samples)
    if sys.byteorder != "little":
        pcm.byteswap()
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(sample_rate)
        output.writeframes(pcm.tobytes())


def write_stereo_pcm_wav(
    path: Path, samples: list[int], sample_rate: int = WAV_SAMPLE_RATE
) -> None:
    if len(samples) % 2:
        raise ValueError("stereo PCM must contain complete left/right frames")
    path.parent.mkdir(parents=True, exist_ok=True)
    pcm = array.array("h", samples)
    if sys.byteorder != "little":
        pcm.byteswap()
    with wave.open(str(path), "wb") as output:
        output.setnchannels(2)
        output.setsampwidth(2)
        output.setframerate(sample_rate)
        output.writeframes(pcm.tobytes())


def load_wds_directory(
    path: Path, preferred_sha256: dict[int, str] | None = None
) -> tuple[dict[int, AudioResource], dict[int, list[str]]]:
    preferred_sha256 = preferred_sha256 or {}
    candidates: dict[int, list[tuple[Path, AudioResource, str]]] = {}
    for resource_path in sorted(path.rglob("*.wds")):
        resource = parse_wds(resource_path.read_bytes())
        if resource is None:
            raise ValueError(f"invalid WDS resource: {resource_path}")
        candidates.setdefault(resource.metadata["wds_id"], []).append(
            (resource_path, resource, hashlib.sha256(resource.payload).hexdigest())
        )
    if not candidates:
        raise ValueError(f"no WDS resources found under {path}")

    selected = {}
    duplicates = {}
    for wds_id, versions in candidates.items():
        preferred = preferred_sha256.get(wds_id)
        versions.sort(
            key=lambda item: (
                item[2] != preferred if preferred else False,
                -item[1].metadata["preset_count"],
                str(item[0]),
            )
        )
        selected[wds_id] = versions[0][1]
        if preferred and versions[0][2] != preferred:
            raise ValueError(
                f"preferred WDS {wds_id} SHA-256 {preferred} was not found under {path}"
            )
        if len(versions) > 1:
            duplicates[wds_id] = [str(item[0]) for item in versions]
    return selected, duplicates


def find_contextual_wds_hashes(
    resource_path: Path, resource: AudioResource, wds_dir: Path
) -> tuple[dict[int, str], dict[int, list[str]]]:
    manifest_path = wds_dir.parent.parent / "manifest.json"
    if not manifest_path.is_file():
        return {}, {}
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    resource_sha256 = hashlib.sha256(resource.payload).hexdigest()
    resource_record = next(
        (
            record
            for record in manifest["resources"]
            if record["sha256"] == resource_sha256
        ),
        None,
    )
    if resource_record is None:
        return {}, {}

    used_wds_ids = {resource.metadata["default_wds_id"]}
    if resource.kind == "seds":
        entry_offsets = (
            offset
            for event in resource.metadata["events"]
            for offset in event["channel_script_offsets"]
            if offset
        )
    else:
        entry_offsets = (
            offset for offset in resource.metadata["track_offsets"] if offset
        )
    for entry_offset in entry_offsets:
        for instruction in walk_sequence(resource.payload, entry_offset).instructions:
            if instruction.opcode in (0xFC, 0xFE):
                used_wds_ids.add(instruction.operands[0])

    resource_containers = {
        (occurrence["disc_index"], occurrence["fat_index"])
        for occurrence in resource_record["occurrences"]
    }
    preceding_routes = {
        (
            occurrence["disc_index"],
            route["directory"],
            int(route["file_id"], 16) - 1,
        )
        for occurrence in resource_record["occurrences"]
        if not occurrence["embedded"]
        for route in occurrence["routes"]
        if int(route["file_id"], 16) > 1
    }
    selected = {}
    ambiguous = {}
    for wds_id in used_wds_ids:
        records = [
            record
            for record in manifest["resources"]
            if record["kind"] == "wds" and record["wds_id"] == wds_id
        ]
        if len(records) < 2:
            continue
        same_container = {
            record["sha256"]
            for record in records
            if any(
                (occurrence["disc_index"], occurrence["fat_index"])
                in resource_containers
                for occurrence in record["occurrences"]
            )
        }
        preceding = {
            record["sha256"]
            for record in records
            if any(
                (
                    occurrence["disc_index"],
                    route["directory"],
                    int(route["file_id"], 16),
                )
                in preceding_routes
                for occurrence in record["occurrences"]
                if not occurrence["embedded"]
                for route in occurrence["routes"]
            )
        }
        direct = {
            record["sha256"]
            for record in records
            if any(not occurrence["embedded"] for occurrence in record["occurrences"])
        }
        candidates = same_container or preceding or direct
        if len(candidates) == 1:
            selected[wds_id] = next(iter(candidates))
        elif candidates:
            ambiguous[wds_id] = sorted(candidates)
    return selected, ambiguous


_sequence_render_worker_state: (
    tuple[
        AudioResource,
        dict[int, AudioResource],
        Path,
        str,
        float,
        bool,
        int | None,
        int | None,
    ]
    | None
) = None


def _initialize_sequence_render_worker(
    resource: AudioResource,
    wds_by_id: dict[int, AudioResource],
    output_dir: Path,
    resource_stem: str,
    max_seconds: float,
    ignore_loops: bool,
    loop_count: int | None,
    seds_reverb_depth: int | None,
) -> None:
    global _sequence_render_worker_state
    _sequence_render_worker_state = (
        resource,
        wds_by_id,
        output_dir,
        resource_stem,
        max_seconds,
        ignore_loops,
        loop_count,
        seds_reverb_depth,
    )


def _render_sequence_entry(
    resource: AudioResource,
    wds_by_id: dict[int, AudioResource],
    output_dir: Path,
    resource_stem: str,
    entry_index: int,
    max_seconds: float,
    ignore_loops: bool,
    loop_count: int | None,
    seds_reverb_depth: int | None,
) -> dict:
    result = render_sequence(
        resource,
        wds_by_id,
        entry_index=entry_index,
        max_seconds=max_seconds,
        ignore_loops=ignore_loops,
        loop_count=loop_count,
        seds_reverb_depth=seds_reverb_depth,
    )
    name = (
        f"effect_{entry_index:04d}.wav"
        if resource.kind == "seds"
        else f"{resource_stem}.wav"
    )
    write_stereo_pcm_wav(output_dir / name, result.samples)
    return {
        "entry_index": entry_index,
        "path": name,
        "frames": len(result.samples) // 2,
        "note_count": result.note_count,
        "classification": (
            "silent-placeholder"
            if resource.kind == "smds"
            and result.note_count == 0
            and b"Movie dammy music\0" in resource.payload
            else "silent"
            if result.note_count == 0
            else "audio"
        ),
        "callbacks": result.callbacks,
        "logical_ticks": result.logical_ticks,
        "ended_tracks": result.ended_tracks,
        "approximated_opcodes": [
            f"0x{opcode:02X}" for opcode in result.approximated_opcodes
        ],
        "unresolved_voices": [
            {"wds_id": wds_id, "preset_index": preset_index}
            for wds_id, preset_index in result.unresolved_voices
        ],
        "external_reverb_context_required": result.external_reverb_context_required,
        "assumed_reverb_depth": result.assumed_reverb_depth,
    }


def _render_sequence_entry_worker(entry_index: int) -> dict:
    if _sequence_render_worker_state is None:
        raise RuntimeError("sequence render worker was not initialized")
    (
        resource,
        wds_by_id,
        output_dir,
        stem,
        max_seconds,
        ignore_loops,
        loop_count,
        reverb_depth,
    ) = _sequence_render_worker_state
    return _render_sequence_entry(
        resource,
        wds_by_id,
        output_dir,
        stem,
        entry_index,
        max_seconds,
        ignore_loops,
        loop_count,
        reverb_depth,
    )


def render_sequence_resource(
    resource_path: Path,
    wds_dir: Path,
    output_dir: Path,
    *,
    entry_index: int | None = None,
    max_seconds: float = 120.0,
    ignore_loops: bool = False,
    loop_count: int | None = None,
    seds_reverb_depth: int | None = DEFAULT_SEDS_REVERB_DEPTH,
    jobs: int = 1,
) -> dict:
    resource = parse_audio_resource(resource_path.read_bytes(), 0)
    if resource is None or resource.kind not in {"seds", "smds"}:
        raise ValueError(f"not an SEDS or SMDS resource: {resource_path}")
    contextual_banks, ambiguous_contextual_banks = find_contextual_wds_hashes(
        resource_path, resource, wds_dir
    )
    wds_by_id, duplicate_banks = load_wds_directory(wds_dir, contextual_banks)
    if resource.kind == "seds":
        if entry_index is None:
            entry_indexes = range(resource.metadata["effect_count"])
        else:
            entry_indexes = (entry_index,)
    else:
        if entry_index not in (None, 0):
            raise ValueError("SMDS resources contain only entry zero")
        entry_indexes = (0,)

    output_dir.mkdir(parents=True, exist_ok=True)
    entry_indexes = tuple(entry_indexes)
    jobs = max(1, min(jobs, len(entry_indexes)))
    if jobs > 1:
        context = multiprocessing.get_context("spawn")
        with concurrent.futures.ProcessPoolExecutor(
            max_workers=jobs,
            mp_context=context,
            initializer=_initialize_sequence_render_worker,
            initargs=(
                resource,
                wds_by_id,
                output_dir,
                resource_path.stem,
                max_seconds,
                ignore_loops,
                loop_count,
                seds_reverb_depth,
            ),
        ) as executor:
            records = list(executor.map(_render_sequence_entry_worker, entry_indexes))
    else:
        records = [
            _render_sequence_entry(
                resource,
                wds_by_id,
                output_dir,
                resource_path.stem,
                index,
                max_seconds,
                ignore_loops,
                loop_count,
                seds_reverb_depth,
            )
            for index in entry_indexes
        ]

    manifest = {
        "schema": "xenogears-sequence-render/v1",
        "resource": str(resource_path),
        "kind": resource.kind,
        "sample_rate": WAV_SAMPLE_RATE,
        "callback_rate": SEQUENCER_CALLBACK_RATE,
        "max_seconds": max_seconds if loop_count is None else None,
        "loop_count": loop_count,
        "loop_policy": (
            "exact-song-loops"
            if loop_count is not None
            else "stop-at-song-loop"
            if ignore_loops
            else "execute"
        ),
        "seds_reverb_depth": seds_reverb_depth,
        "duplicate_wds_ids": {
            str(wds_id): paths for wds_id, paths in sorted(duplicate_banks.items())
        },
        "contextual_wds_sha256": {
            str(wds_id): sha256 for wds_id, sha256 in sorted(contextual_banks.items())
        },
        "ambiguous_contextual_wds_sha256": {
            str(wds_id): hashes
            for wds_id, hashes in sorted(ambiguous_contextual_banks.items())
        },
        "renders": records,
    }
    (output_dir / "render-manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    return manifest


def _render_resource_batch_worker(
    arguments: tuple[Path, Path, Path, float, bool, int | None, int | None],
) -> dict:
    (
        resource_path,
        wds_dir,
        output_root,
        max_seconds,
        ignore_loops,
        loop_count,
        reverb_depth,
    ) = arguments
    manifest = render_sequence_resource(
        resource_path,
        wds_dir,
        output_root / resource_path.stem,
        max_seconds=max_seconds,
        ignore_loops=ignore_loops,
        loop_count=loop_count,
        seds_reverb_depth=reverb_depth,
    )
    return {
        "resource": resource_path.name,
        "output": resource_path.stem,
        "renders": len(manifest["renders"]),
    }


def render_sequence_directory(
    resource_dir: Path,
    wds_dir: Path,
    output_dir: Path,
    *,
    max_seconds: float = 120.0,
    ignore_loops: bool = False,
    loop_count: int | None = None,
    seds_reverb_depth: int | None = DEFAULT_SEDS_REVERB_DEPTH,
    jobs: int = 1,
) -> dict:
    resource_paths = sorted(
        (*resource_dir.glob("*.seds"), *resource_dir.glob("*.smds")),
        key=lambda path: path.name,
    )
    if not resource_paths:
        raise ValueError(f"no SEDS or SMDS resources found in {resource_dir}")
    output_dir.mkdir(parents=True, exist_ok=True)
    arguments = [
        (
            path,
            wds_dir,
            output_dir,
            max_seconds,
            ignore_loops,
            loop_count,
            seds_reverb_depth,
        )
        for path in resource_paths
    ]
    jobs = max(1, min(jobs, len(arguments)))
    if jobs > 1:
        context = multiprocessing.get_context("spawn")
        with concurrent.futures.ProcessPoolExecutor(
            max_workers=jobs,
            mp_context=context,
        ) as executor:
            records = list(executor.map(_render_resource_batch_worker, arguments))
    else:
        records = [_render_resource_batch_worker(argument) for argument in arguments]
    manifest = {
        "schema": "xenogears-sequence-render-batch/v1",
        "resource_dir": str(resource_dir),
        "wds_dir": str(wds_dir),
        "max_seconds": max_seconds if loop_count is None else None,
        "loop_count": loop_count,
        "loop_policy": (
            "exact-song-loops"
            if loop_count is not None
            else "stop-at-song-loop"
            if ignore_loops
            else "execute"
        ),
        "seds_reverb_depth": seds_reverb_depth,
        "jobs": jobs,
        "resources": records,
        "render_count": sum(record["renders"] for record in records),
    }
    (output_dir / "batch-manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    return manifest


def _refresh_wds_asset_worker(
    arguments: tuple[Path, str, Path],
) -> tuple[str, dict, list[dict]]:
    resource_path, expected_sha256, output_dir = arguments
    payload = resource_path.read_bytes()
    if hashlib.sha256(payload).hexdigest() != expected_sha256:
        raise ValueError(f"WDS hash does not match manifest: {resource_path}")
    resource = parse_wds(payload)
    if resource is None:
        raise ValueError(f"invalid WDS resource: {resource_path}")
    metadata = dict(resource.metadata)
    presets = metadata.pop("presets")
    preset_records = []
    for preset in presets:
        encoded, sample_metadata = extract_wds_preset(resource, preset)
        sample_rate = wds_pitch_to_sample_rate(preset.pitch)
        wav_relative = (
            Path("samples") / resource_path.stem / f"preset_{preset.index:03d}.wav"
        )
        write_pcm_wav(
            output_dir / wav_relative, decode_psx_adpcm(encoded), sample_rate
        )
        preset_records.append(
            {
                "index": preset.index,
                "start_units": preset.start_units,
                "repeat_units": preset.repeat_units,
                "pitch": preset.pitch,
                "adsr": f"0x{preset.adsr:08X}",
                "modes": f"0x{preset.modes:04X}",
                "reserved": f"0x{preset.reserved:04X}",
                **sample_metadata,
                "wav_path": wav_relative.as_posix(),
                "wav_sample_rate": sample_rate,
            }
        )
    return expected_sha256, metadata, preset_records


def refresh_wds_assets(
    manifest_path: Path,
    output_dir: Path,
    *,
    jobs: int = 1,
) -> dict:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    records_by_hash = {
        record["sha256"]: record
        for record in manifest["resources"]
        if record["kind"] == "wds"
    }
    arguments = [
        (manifest_path.parent / record["path"], sha256, output_dir)
        for sha256, record in sorted(records_by_hash.items())
    ]
    output_dir.mkdir(parents=True, exist_ok=True)
    jobs = max(1, min(jobs, len(arguments)))
    if jobs > 1:
        context = multiprocessing.get_context("spawn")
        with concurrent.futures.ProcessPoolExecutor(
            max_workers=jobs,
            mp_context=context,
        ) as executor:
            updates = list(executor.map(_refresh_wds_asset_worker, arguments))
    else:
        updates = [_refresh_wds_asset_worker(argument) for argument in arguments]
    preset_count = 0
    for sha256, metadata, presets in updates:
        record = records_by_hash[sha256]
        record.update(metadata)
        record["presets"] = presets
        preset_count += len(presets)
    (output_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    return {"wds_resources": len(updates), "presets": preset_count}


def _safe_audio_label(name: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")


def _resource_stem(resource: AudioResource, sha256: str) -> str:
    if resource.kind == "wds":
        identity = f"{resource.metadata['wds_id']:03d}"
    elif resource.kind == "seds":
        identity = f"{resource.metadata['seds_id']:04d}"
    else:
        internal_name = resource.metadata.get("internal_name")
        if internal_name:
            return f"{_safe_audio_label(internal_name)}__smds_{sha256[:12]}"
        identity = "sequence"
    return f"{resource.kind}_{identity}_{sha256[:12]}"


def build_audio_name_map(manifest: dict, source_manifest: str | None = None) -> dict:
    direct_wds_filenames = {
        0x00: "main_se.wd",
        0x01: "bat_se.wd",
        0x02: "gear_se.wd",
        0x26: "battle2.wd",
    }
    records = []
    for resource in manifest["resources"]:
        kind = resource["kind"]
        sha256 = resource["sha256"]
        locations = {
            (
                occurrence["disc_index"],
                occurrence["disc_name"],
                route["directory"],
                route["file_id"],
                occurrence["embedded"],
                occurrence["container_offset"],
            )
            for occurrence in resource["occurrences"]
            for route in occurrence["routes"]
        }
        record = {
            "kind": kind,
            "sha256": sha256,
            "path": resource["path"],
            "filesystem_locations": [
                {
                    "disc_index": disc_index,
                    "disc_name": disc_name,
                    "directory": directory,
                    "file_id": file_id,
                    "embedded": embedded,
                    "container_offset": container_offset,
                }
                for (
                    disc_index,
                    disc_name,
                    directory,
                    file_id,
                    embedded,
                    container_offset,
                ) in sorted(locations)
            ],
        }
        if kind == "wds":
            wds_id = resource["wds_id"]
            internal_name = INTERNAL_WDS_NAMES.get(wds_id)
            record.update(
                {
                    "wds_id": wds_id,
                    "wds_id_hex": f"0x{wds_id:02X}",
                    "recovered_name": internal_name,
                    "name_status": (
                        "documented_wds_name" if internal_name else "unknown"
                    ),
                    "name_evidence": (
                        "historical_q_gears_snd_name_table" if internal_name else None
                    ),
                    "original_filename": direct_wds_filenames.get(wds_id),
                    "original_filename_evidence": (
                        "movie_module_debug_path"
                        if wds_id in direct_wds_filenames
                        else None
                    ),
                    "suggested_label": internal_name
                    or f"wds_{wds_id:03d}_{sha256[:12]}",
                }
            )
        else:
            wds_id = resource["default_wds_id"]
            bank_name = INTERNAL_WDS_NAMES.get(wds_id)
            internal_name = resource.get("internal_name") if kind == "smds" else None
            record.update(
                {
                    "seds_id": resource.get("seds_id"),
                    "recovered_name": internal_name,
                    "name_status": (
                        "embedded_smds_name"
                        if internal_name
                        else "associated_wds_name_only"
                        if bank_name
                        else "unknown"
                    ),
                    "name_evidence": (
                        "smds_internal_name_field" if internal_name else None
                    ),
                    "associated_wave_bank": {
                        "wds_id": wds_id,
                        "wds_id_hex": f"0x{wds_id:02X}",
                        "recovered_name": bank_name,
                        "evidence": (
                            "resource_default_wds_id_and_historical_q_gears_snd_name_table"
                            if bank_name
                            else None
                        ),
                    },
                    "suggested_label": (
                        f"{_safe_audio_label(internal_name)}__smds_{sha256[:12]}"
                        if internal_name
                        else f"bank-{bank_name}__{kind}_{sha256[:12]}"
                        if bank_name
                        else f"{kind}_{sha256[:12]}"
                    ),
                }
            )
        records.append(record)

    battle2_candidates = [
        record
        for record in records
        if record["kind"] == "smds" and record["associated_wave_bank"]["wds_id"] == 0x26
    ]
    battle2_context_matches = [
        record
        for record in battle2_candidates
        if any(
            location["directory"] in {"0x22", "0x24"}
            for location in record["filesystem_locations"]
        )
    ]
    battle2_match = (
        battle2_context_matches[0] if len(battle2_context_matches) == 1 else None
    )
    if battle2_match is not None:
        battle2_match.update(
            {
                "probable_original_filename": "battle2.smd",
                "probable_filename_confidence": "high_contextual",
                "probable_filename_evidence": [
                    "movie_module_debug_path",
                    "associated_wave_bank_0x26",
                    "unique_candidate_in_battle_audio_directories_0x22_and_0x24",
                ],
            }
        )
    return {
        "schema": NAME_MAP_SCHEMA,
        "source_manifest": source_manifest,
        "evidence_sources": {
            "historical_q_gears_snd_name_table": {
                "commit": "e0fabf7",
                "path": "xeno/utilites/cd extractor/cd extractor.cpp",
                "scope": "WD bank IDs only; it does not assign SMD or SED names",
            },
            "movie_module_debug_path": {
                "program": "movie_module.bin",
                "filenames": [
                    "main_se.wd",
                    "bat_se.wd",
                    "gear_se.wd",
                    "battle2.wd",
                    "battle2.smd",
                ],
            },
            "smds_internal_name_field": {
                "header_offset": "0x1E",
                "encoding": "NUL-terminated ASCII",
                "scope": "embedded name in each SMDS resource",
            },
        },
        "documented_source_filenames": [
            {
                "filename": "battle2.smd",
                "kind": "smds",
                "status": (
                    "probable_contextual_match"
                    if battle2_match is not None
                    else "ambiguous_associated_wave_bank"
                ),
                "confidence": "high_contextual" if battle2_match is not None else None,
                "reason": (
                    "The filename is present in movie_module.bin. The selected resource "
                    "is the only candidate using the battle2 WD bank that also occurs in "
                    "the battle audio directories; no source-file hash is available."
                    if battle2_match is not None
                    else "The filename is present in movie_module.bin, but multiple unique "
                    "SMDS resources use the battle2 WD bank."
                ),
                "matched_sha256": battle2_match["sha256"] if battle2_match else None,
                "candidate_sha256": [record["sha256"] for record in battle2_candidates],
            }
        ],
        "summary": {
            "resources": len(records),
            "documented_wds_names": sum(
                record["name_status"] == "documented_wds_name" for record in records
            ),
            "sequences_with_associated_wds_name": sum(
                record["name_status"] == "associated_wds_name_only"
                for record in records
            ),
            "sequences_with_embedded_smds_name": sum(
                record["name_status"] == "embedded_smds_name" for record in records
            ),
            "probable_original_filenames": sum(
                bool(record.get("probable_original_filename")) for record in records
            ),
            "unknown_names": sum(
                record["name_status"] == "unknown" for record in records
            ),
            "battle2_smd_candidates": len(battle2_candidates),
        },
        "resources": records,
    }


def write_audio_name_map(
    manifest: dict, output_path: Path, source_manifest: str | None = None
) -> dict:
    name_map = build_audio_name_map(manifest, source_manifest)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(name_map, indent=2) + "\n", encoding="utf-8")
    return name_map


def apply_audio_names(audio_root: Path) -> dict:
    extracted_dir = audio_root / "extracted"
    manifest_path = extracted_dir / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    for resource in manifest["resources"]:
        if resource["kind"] != "smds" or resource.get("internal_name"):
            continue
        resource_path = extracted_dir / resource["path"]
        parsed = parse_smds(resource_path.read_bytes())
        if parsed is None or not parsed.metadata["internal_name"]:
            raise ValueError(
                f"SMDS resource has no valid internal name: {resource_path}"
            )
        resource["internal_name"] = parsed.metadata["internal_name"]
    name_map = build_audio_name_map(manifest, str(manifest_path))
    names_by_sha256 = {record["sha256"]: record for record in name_map["resources"]}

    plans = []
    for resource in manifest["resources"]:
        sha256 = resource["sha256"]
        naming = names_by_sha256[sha256]
        old_relative = Path(resource["path"])
        old_stem = old_relative.stem
        if resource["kind"] == "wds":
            prefix = naming["recovered_name"]
            identity = f"wds_{resource['wds_id']:03d}"
        elif resource["kind"] == "smds" and naming["recovered_name"]:
            prefix = _safe_audio_label(naming["recovered_name"])
            identity = "smds"
        elif naming["name_status"] == "probable_original_filename":
            prefix = Path(naming["probable_original_filename"]).stem
            identity = resource["kind"]
        else:
            prefix = f"bank-{naming['associated_wave_bank']['recovered_name']}"
            identity = (
                f"seds_{resource['seds_id']:04d}"
                if resource["kind"] == "seds"
                else "smds"
            )
        new_stem = f"{prefix}__{identity}_{sha256[:12]}"
        new_relative = old_relative.with_name(new_stem + old_relative.suffix)
        plans.append((resource, old_relative, new_relative, old_stem, new_stem))

    file_moves = [
        (extracted_dir / old_relative, extracted_dir / new_relative)
        for _, old_relative, new_relative, _, _ in plans
        if old_relative != new_relative
    ]
    sample_moves = [
        (extracted_dir / "samples" / old_stem, extracted_dir / "samples" / new_stem)
        for resource, _, _, old_stem, new_stem in plans
        if resource["kind"] == "wds" and old_stem != new_stem
    ]
    render_moves = [
        (
            audio_root / "rendered" / resource["kind"] / old_stem,
            audio_root / "rendered" / resource["kind"] / new_stem,
        )
        for resource, _, _, old_stem, new_stem in plans
        if resource["kind"] in {"seds", "smds"} and old_stem != new_stem
    ]
    for source, destination in file_moves + sample_moves + render_moves:
        if not source.exists():
            raise FileNotFoundError(f"name application source is missing: {source}")
        if destination.exists():
            raise FileExistsError(f"name application destination exists: {destination}")

    render_manifests = {}
    for resource, _, _, old_stem, _ in plans:
        if resource["kind"] not in {"seds", "smds"}:
            continue
        old_render_dir = audio_root / "rendered" / resource["kind"] / old_stem
        old_manifest_path = old_render_dir / "render-manifest.json"
        render_manifest = json.loads(old_manifest_path.read_text(encoding="utf-8"))
        for render in render_manifest["renders"]:
            render_path = old_render_dir / render["path"]
            if not render_path.exists():
                raise FileNotFoundError(
                    f"sequence render is missing during name validation: {render_path}"
                )
        render_manifests[resource["sha256"]] = render_manifest

    for source, destination in file_moves + sample_moves + render_moves:
        source.rename(destination)

    renamed_scores = 0
    for resource, _, new_relative, _, new_stem in plans:
        resource["path"] = new_relative.as_posix()
        if resource["kind"] == "wds":
            for preset in resource["presets"]:
                if "wav_path" in preset:
                    wav_path = Path(preset["wav_path"])
                    preset["wav_path"] = (
                        Path("samples") / new_stem / wav_path.name
                    ).as_posix()
            continue

        render_dir = audio_root / "rendered" / resource["kind"] / new_stem
        render_manifest_path = render_dir / "render-manifest.json"
        render_manifest = render_manifests[resource["sha256"]]
        render_manifest["resource"] = str(extracted_dir / new_relative)
        if resource["kind"] == "smds":
            for render in render_manifest["renders"]:
                old_wav = render_dir / render["path"]
                new_name = f"{new_stem}.wav"
                new_wav = render_dir / new_name
                if old_wav != new_wav:
                    if not old_wav.exists():
                        raise FileNotFoundError(
                            f"SMDS render is missing during name application: {old_wav}"
                        )
                    if new_wav.exists():
                        raise FileExistsError(
                            f"named SMDS render already exists: {new_wav}"
                        )
                    old_wav.rename(new_wav)
                    renamed_scores += 1
                render["path"] = new_name
        render_manifest_path.write_text(
            json.dumps(render_manifest, indent=2) + "\n", encoding="utf-8"
        )

    moves_by_kind = {
        kind: {
            old_relative.name: (new_relative.name, new_stem)
            for resource, old_relative, new_relative, _, new_stem in plans
            if resource["kind"] == kind
        }
        for kind in ("seds", "smds")
    }
    for kind, moves in moves_by_kind.items():
        batch_manifest_path = audio_root / "rendered" / kind / "batch-manifest.json"
        if not batch_manifest_path.exists():
            continue
        batch_manifest = json.loads(batch_manifest_path.read_text(encoding="utf-8"))
        for record in batch_manifest["resources"]:
            if record["resource"] in moves:
                record["resource"], record["output"] = moves[record["resource"]]
        batch_manifest_path.write_text(
            json.dumps(batch_manifest, indent=2) + "\n", encoding="utf-8"
        )

    manifest["applied_name_scheme"] = {
        "version": 2,
        "smds_prefix": "embedded-internal-name",
        "seds_associated_bank_prefix": "bank-",
        "hash_length": 12,
    }
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    write_audio_name_map(manifest, extracted_dir / "name-map.json", str(manifest_path))
    return {
        "resource_files_renamed": len(file_moves),
        "wds_sample_directories_renamed": len(sample_moves),
        "sequence_directories_renamed": len(render_moves),
        "smds_wavs_renamed": renamed_scores,
    }


def scan_disc(
    path: Path, disc_index: int
) -> list[tuple[AudioResource, ResourceOccurrence]]:
    disc = open_disc(path)
    sector_count = disc.path.stat().st_size // disc.sector_size
    fat_data = disc.read_user_data(FAT_LBA, FAT_SECTORS * SECTOR_SIZE)
    entries, _ = parse_fat_table(fat_data, sector_count)
    directory_data = disc.read_user_data(DIRECTORY_LBA, DIRECTORY_COUNT * 2)
    routes = map_physical_routes(parse_directory_table(directory_data), len(entries))

    found = []
    for fat_index, entry in enumerate(entries):
        if entry.size <= 0:
            continue
        container = disc.read_user_data(entry.lba, entry.size)
        for container_offset, resource in scan_audio_resources(container):
            found.append(
                (
                    resource,
                    ResourceOccurrence(
                        disc_index,
                        path.name,
                        fat_index,
                        entry.lba,
                        entry.size,
                        container_offset,
                        routes.get(fat_index, []),
                    ),
                )
            )
    return found


def extract_audio(
    discs: list[Path], output_dir: Path, export_samples: bool = True
) -> dict:
    unique: dict[str, tuple[AudioResource, list[ResourceOccurrence]]] = {}
    for disc_index, disc_path in enumerate(discs):
        for resource, occurrence in scan_disc(disc_path, disc_index):
            sha256 = hashlib.sha256(resource.payload).hexdigest()
            if sha256 in unique:
                existing, occurrences = unique[sha256]
                if existing.kind != resource.kind:
                    raise ValueError("audio SHA-256 collision across resource kinds")
                occurrences.append(occurrence)
            else:
                unique[sha256] = (resource, [occurrence])

    output_dir.mkdir(parents=True, exist_ok=True)
    records = []
    kind_order = {"wds": 0, "seds": 1, "smds": 2}
    ordered = sorted(
        unique.items(),
        key=lambda item: (
            kind_order[item[1][0].kind],
            _resource_stem(item[1][0], item[0]),
        ),
    )
    for sha256, (resource, occurrences) in ordered:
        stem = _resource_stem(resource, sha256)
        extension = resource.kind
        relative_path = Path("resources") / resource.kind / f"{stem}.{extension}"
        resource_path = output_dir / relative_path
        resource_path.parent.mkdir(parents=True, exist_ok=True)
        resource_path.write_bytes(resource.payload)

        metadata = dict(resource.metadata)
        presets = metadata.pop("presets", None)
        record = {
            "kind": resource.kind,
            "sha256": sha256,
            "size": len(resource.payload),
            "path": relative_path.as_posix(),
            **metadata,
            "occurrences": [
                {
                    "disc_index": occurrence.disc_index,
                    "disc_name": occurrence.disc_name,
                    "fat_index": occurrence.fat_index,
                    "lba": occurrence.lba,
                    "container_size": occurrence.container_size,
                    "container_offset": occurrence.container_offset,
                    "embedded": occurrence.container_offset != 0,
                    "routes": occurrence.routes,
                }
                for occurrence in occurrences
            ],
        }

        if presets is not None:
            preset_records = []
            for preset in presets:
                encoded, sample_metadata = extract_wds_preset(resource, preset)
                preset_record = {
                    "index": preset.index,
                    "start_units": preset.start_units,
                    "repeat_units": preset.repeat_units,
                    "pitch": preset.pitch,
                    "adsr": f"0x{preset.adsr:08X}",
                    "modes": f"0x{preset.modes:04X}",
                    "reserved": f"0x{preset.reserved:04X}",
                    **sample_metadata,
                }
                if export_samples:
                    wav_relative = (
                        Path("samples") / stem / f"preset_{preset.index:03d}.wav"
                    )
                    samples = decode_psx_adpcm(encoded)
                    sample_rate = wds_pitch_to_sample_rate(preset.pitch)
                    write_pcm_wav(output_dir / wav_relative, samples, sample_rate)
                    preset_record["wav_path"] = wav_relative.as_posix()
                    preset_record["wav_sample_rate"] = sample_rate
                preset_records.append(preset_record)
            record["presets"] = preset_records
        records.append(record)

    summary = {
        "occurrences": sum(len(occurrences) for _, occurrences in unique.values()),
        "unique_resources": len(records),
        "by_kind": {
            kind: {
                "occurrences": sum(
                    len(occurrences)
                    for resource, occurrences in unique.values()
                    if resource.kind == kind
                ),
                "unique": sum(record["kind"] == kind for record in records),
            }
            for kind in ("wds", "seds", "smds")
        },
    }
    manifest = {
        "schema": MANIFEST_SCHEMA,
        "discs": [str(path) for path in discs],
        "samples_are_raw_presets": True,
        "summary": summary,
        "resources": records,
    }
    manifest_path = output_dir / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    write_audio_name_map(manifest, output_dir / "name-map.json", str(manifest_path))
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "discs",
        nargs="*",
        type=Path,
        default=[DEFAULT_DISC],
        help="CUE, BIN, or ISO images to scan (default: game/disc1.cue)",
    )
    parser.add_argument("--output", type=Path, help="extraction directory")
    parser.add_argument(
        "--apply-names-root",
        type=Path,
        help="apply recovered names to an existing extracted/rendered audio tree",
    )
    parser.add_argument(
        "--render-resource",
        type=Path,
        help="render one extracted SEDS or SMDS resource instead of scanning discs",
    )
    parser.add_argument(
        "--render-resources-dir",
        type=Path,
        help="render every SEDS or SMDS resource in a directory",
    )
    parser.add_argument(
        "--name-map-manifest",
        type=Path,
        help="build name-map.json from an existing extraction manifest",
    )
    parser.add_argument(
        "--refresh-wds-manifest",
        type=Path,
        help="regenerate WDS preset WAVs and metadata from an extraction manifest",
    )
    parser.add_argument(
        "--wds-dir",
        type=Path,
        help="directory containing extracted WDS banks for --render-resource",
    )
    parser.add_argument(
        "--entry-index",
        type=int,
        help="render only this SEDS effect index (SMDS only accepts zero)",
    )
    parser.add_argument(
        "--max-seconds",
        type=float,
        default=120.0,
        help="maximum sequence duration when rendering without --loops (default: 120)",
    )
    parser.add_argument(
        "--ignore-loops",
        action="store_true",
        help="stop at the song-level backward jump after executing counted loops",
    )
    parser.add_argument(
        "--loops",
        "--loop-count",
        dest="loop_count",
        type=int,
        metavar="N",
        help="render exactly N song cycles; follows 0x91/0x90 or restarts at the end",
    )
    parser.add_argument(
        "--seds-reverb-depth",
        type=int,
        default=DEFAULT_SEDS_REVERB_DEPTH,
        choices=range(0x80),
        metavar="0..127",
        help="inherited global reverb depth required by wet standalone SEDS",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=1,
        help="parallel SEDS effect renders using isolated worker processes",
    )
    parser.add_argument(
        "--resources-only",
        action="store_true",
        help="extract WDS/SEDS/SMDS resources without decoding WDS presets",
    )
    args = parser.parse_args()

    if args.loop_count is not None and args.loop_count < 1:
        parser.error("--loops must be at least 1")
    if args.loop_count is not None and args.ignore_loops:
        parser.error("--loops cannot be combined with --ignore-loops")

    if args.apply_names_root is not None:
        result = apply_audio_names(args.apply_names_root)
        print(json.dumps(result, indent=2))
        return 0

    if args.output is None:
        parser.error("--output is required unless --apply-names-root is used")

    if args.name_map_manifest is not None:
        manifest = json.loads(args.name_map_manifest.read_text(encoding="utf-8"))
        name_map = write_audio_name_map(
            manifest,
            args.output / "name-map.json",
            str(args.name_map_manifest),
        )
        print(json.dumps(name_map["summary"], indent=2))
        print(f"Name map: {args.output / 'name-map.json'}")
        return 0

    if args.refresh_wds_manifest is not None:
        result = refresh_wds_assets(
            args.refresh_wds_manifest,
            args.output,
            jobs=args.jobs,
        )
        print(json.dumps(result, indent=2))
        print(f"Manifest: {args.output / 'manifest.json'}")
        return 0

    if args.render_resource is not None:
        if args.render_resources_dir is not None:
            parser.error("choose either --render-resource or --render-resources-dir")
        if args.wds_dir is None:
            parser.error("--wds-dir is required with --render-resource")
        manifest = render_sequence_resource(
            args.render_resource,
            args.wds_dir,
            args.output,
            entry_index=args.entry_index,
            max_seconds=args.max_seconds,
            ignore_loops=args.ignore_loops,
            loop_count=args.loop_count,
            seds_reverb_depth=args.seds_reverb_depth,
            jobs=args.jobs,
        )
        print(json.dumps({"renders": len(manifest["renders"])}, indent=2))
        print(f"Manifest: {args.output / 'render-manifest.json'}")
        return 0

    if args.render_resources_dir is not None:
        if args.wds_dir is None:
            parser.error("--wds-dir is required with --render-resources-dir")
        if args.entry_index is not None:
            parser.error("--entry-index cannot be used with --render-resources-dir")
        manifest = render_sequence_directory(
            args.render_resources_dir,
            args.wds_dir,
            args.output,
            max_seconds=args.max_seconds,
            ignore_loops=args.ignore_loops,
            loop_count=args.loop_count,
            seds_reverb_depth=args.seds_reverb_depth,
            jobs=args.jobs,
        )
        print(json.dumps({"renders": manifest["render_count"]}, indent=2))
        print(f"Manifest: {args.output / 'batch-manifest.json'}")
        return 0

    manifest = extract_audio(args.discs, args.output, not args.resources_only)
    print(json.dumps(manifest["summary"], indent=2))
    print(f"Manifest: {args.output / 'manifest.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
