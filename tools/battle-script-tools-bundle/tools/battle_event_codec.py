"""Battle Event instruction forms: operand schemas, decoding and encoding.

Every opcode 0x00..0x4B has one fixed-size form. Operand layouts were read
from the Battle Event overlay handlers (0x801E5C1C..0x801E8750) and the shared
decoder BattleEventDecodeOperands (0x801E57F8):

* ``v15``: u16; bit 15 set selects the immediate ``value & 0x7FFF``,
  bit 15 clear selects the variable at byte offset ``value & 0x7FFE``.
* ``typed``: u16 whose kind is chosen by a bit of the trailing mask byte
  (``0x80`` for the first u16 operand, ``0x40`` for the second); set means a
  full 16-bit immediate, clear means a variable byte offset.
* ``dest``/``var``: u16 direct variable byte offset (bit 0 ignored by the VM).
* ``label``: u16 bytecode offset relative to the shared bytecode base.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

FIELD_SIZES = {"u8": 1, "s8": 1, "mask": 1, "entity": 1, "packed": 1,
               "u16": 2, "v15": 2, "typed": 2, "dest": 2, "var": 2, "label": 2}


@dataclass(frozen=True)
class Operand:
    kind: str
    name: str
    bit: int = 0          # mask bit selecting immediate for ``typed`` and typed ``dest``
    hex: bool = False     # render immediates in hexadecimal


@dataclass(frozen=True)
class OpSpec:
    opcode: int
    namespace: str
    name: str
    operands: tuple[Operand, ...]
    terminal: bool = False
    summary: str = ""

    @property
    def size(self) -> int:
        return 1 + sum(FIELD_SIZES[operand.kind] for operand in self.operands)

    @property
    def qualified(self) -> str:
        return f"{self.namespace}.{self.name}"

    @property
    def mask_operand(self) -> Operand | None:
        return next((operand for operand in self.operands if operand.kind == "mask"), None)


def O(kind: str, name: str, *, bit: int = 0, hex: bool = False) -> Operand:
    return Operand(kind, name, bit, hex)


def _v(*names: str, hex_names: tuple[str, ...] = ()) -> tuple[Operand, ...]:
    return tuple(O("v15", name, hex=name in hex_names) for name in names)


_TYPED_BINARY = (O("dest", "destination", bit=0x80), O("typed", "value", bit=0x40), O("mask", "type_mask"))
_ALIAS = ("actor", "target", "sprite", "character", "destination", "source")

SPECS: dict[int, OpSpec] = {spec.opcode: spec for spec in (
    OpSpec(0x00, "flow", "end_script", (), True, "Release the current slot and reinstall entry 1 as the primary script."),
    OpSpec(0x01, "flow", "jump", (O("label", "target"),), True, "Continue at target."),
    OpSpec(0x02, "flow", "jump_unless", (O("typed", "left", bit=0x80), O("typed", "right", bit=0x40),
                                         O("mask", "type_and_comparison"), O("label", "false_target")),
           False, "Evaluate the comparison; continue when true, otherwise jump to false_target."),
    OpSpec(0x03, "flow", "start_script", (O("entity", "entity"), O("packed", "entry")),
           summary="Start an entity entry in a free secondary slot; poll while all seven are busy."),
    OpSpec(0x04, "flow", "start_script_wait_started", (O("entity", "entity"), O("packed", "entry")),
           summary="Start an entity entry, then wait until the target reports it as current."),
    OpSpec(0x05, "flow", "start_script_wait_finished", (O("entity", "entity"), O("packed", "entry")),
           summary="Start an entity entry, then wait until its tag leaves the target."),
    OpSpec(0x06, "state", "assign", _TYPED_BINARY, summary="destination = value"),
    OpSpec(0x07, "state", "set_true", (O("dest", "destination"),), summary="destination = 1"),
    OpSpec(0x08, "state", "set_false", (O("dest", "destination"),), summary="destination = 0"),
    OpSpec(0x09, "state", "add", _TYPED_BINARY, summary="destination += value (16-bit wrap)"),
    OpSpec(0x0A, "state", "subtract", _TYPED_BINARY, summary="destination -= value (16-bit wrap)"),
    OpSpec(0x0B, "state", "or_bits", _TYPED_BINARY, summary="destination |= value"),
    OpSpec(0x0C, "state", "clear_bits", _TYPED_BINARY, summary="destination &= ~value"),
    OpSpec(0x0D, "state", "increment", (O("dest", "destination"),), summary="destination += 1"),
    OpSpec(0x0E, "state", "decrement", (O("dest", "destination"),), summary="destination -= 1"),
    OpSpec(0x0F, "state", "and_bits", _TYPED_BINARY, summary="destination &= value"),
    OpSpec(0x10, "state", "or_bits_alternate", _TYPED_BINARY, summary="destination |= value (second dispatch entry)"),
    OpSpec(0x11, "state", "xor_bits", _TYPED_BINARY, summary="destination ^= value"),
    OpSpec(0x12, "state", "shift_left", (O("dest", "destination"), O("var", "shift_count")),
           summary="destination <<= (shift_count & 31)"),
    OpSpec(0x13, "state", "shift_right", (O("dest", "destination"), O("var", "shift_count")),
           summary="destination >>= (shift_count & 31), logical"),
    OpSpec(0x14, "state", "random", (O("dest", "destination"),), summary="destination = random 0..0x7FFF"),
    OpSpec(0x15, "state", "random_range", (O("u16", "upper_bound"), O("dest", "destination")),
           summary="destination = random value bounded by the fixed upper_bound"),
    OpSpec(0x16, "state", "multiply", (O("typed", "destination", bit=0x80), O("typed", "value", bit=0x40), O("mask", "type_mask")),
           summary="destination = low 16 bits of left * value; left is the destination field decoded by its type bit"),
    OpSpec(0x17, "state", "divide", (O("typed", "destination", bit=0x80), O("typed", "value", bit=0x40), O("mask", "type_mask")),
           summary="destination = left / value; a zero divisor stores 0xFFFF"),
    OpSpec(0x18, "dialogue", "show", (O("u16", "message_id"), O("u8", "flags", hex=True)),
           summary="Show a blocking message with the current entity's portrait."),
    OpSpec(0x19, "dialogue", "show_with_portrait", (O("u8", "portrait"), O("u16", "message_id"), O("u8", "flags", hex=True)),
           summary="Show a blocking message with an explicit portrait."),
    OpSpec(0x1A, "dialogue", "set_window", _v("x", "y", "width", "height", "flags", hex_names=("flags",)),
           summary="Set dialogue X, Y, width, height and flags; zero selects each geometry default."),
    OpSpec(0x1B, "dialogue", "set_portrait", (O("u8", "portrait", hex=True),),
           summary="Set the current entity's portrait; 0xF3..0xFF read the resident party-ID array."),
    OpSpec(0x1C, "battle", "enter_cutscene_mode", (),
           summary="Set central Battle mode 2 (cutscene): no combat input; Event waits count down."),
    OpSpec(0x1D, "battle", "enter_combat_mode", (),
           summary="Set central Battle mode 1 (active-time combat and its UI)."),
    OpSpec(0x1E, "visual", "fade_out", _v("duration"),
           summary="Subtract a fader toward white (255, 255, 255), darkening the screen to black."),
    OpSpec(0x1F, "visual", "fade_in", _v("duration"),
           summary="Return the subtractive fader to zero, restoring the screen; the fader then removes itself."),
    OpSpec(0x20, "event", "stop_for_final_handoff", (), summary="Permit the final Event handoff and stop the Event VM."),
    OpSpec(0x21, "battle", "set_silent_result", (), summary="Select silent Result presentation."),
    OpSpec(0x22, "flow", "yield_to_battle", (),
           summary="Return control to Battle after the current pass; the Event resumes on its next update."),
    OpSpec(0x23, "actor", "play_mecha_animation_wait", _v("actor", "target", "animation", hex_names=_ALIAS),
           summary="Play a mecha animation under Event camera control and wait until the animation ends."),
    OpSpec(0x24, "battle", "queue_next_battle", _v("formation", "transition_effect"),
           summary="After this Battle, start another one with the given formation and transition effect."),
    OpSpec(0x25, "battle", "continue_on_defeat", (),
           summary="If the party is defeated, end as a reward-exempt Event victory (exit mode 3, result 0x01)."),
    OpSpec(0x26, "battle", "set_field_return", _v("field_id", "camera_yaw", "world_position", "world_mode"),
           summary="Commit the post-Battle Field or World Map return."),
    OpSpec(0x27, "battle", "play_movie", _v("movie_type", "movie_number", "fade", "completion_value"),
           summary="Request a movie (type | 0x80) and select return mode 1."),
    OpSpec(0x28, "visual", "create_fader", (O("u8", "mode"), O("u8", "red"), O("u8", "green"), O("u8", "blue"), O("u8", "duration")),
           summary="BattleCreateFader(duration, mode, red, green, blue)."),
    OpSpec(0x29, "camera", "set_target", _v("x", "y", "z", "frames"), summary="Move the Event camera target."),
    OpSpec(0x2A, "sprite", "play_animation", _v("sprite", "animation", hex_names=_ALIAS),
           summary="Apply an animation to an Event-created sprite."),
    OpSpec(0x2B, "flow", "wait", _v("units"), summary="Wait units * 2 Battle input updates."),
    OpSpec(0x2C, "flow", "set_priority", (O("s8", "priority"), O("u8", "reserved")),
           summary="Store the entity priority; -2 moves the entity to the queue front."),
    OpSpec(0x2D, "audio", "load_music", _v("music_id"), summary="Load directory 0x20 file music_id + 4 at volume 0x7F."),
    OpSpec(0x2E, "audio", "load_music_muted", _v("music_id"), summary="Load Event music with volume zero."),
    OpSpec(0x2F, "audio", "set_music_volume", _v("volume", "frames"), summary="Fade the loaded music volume."),
    OpSpec(0x30, "audio", "set_music_muted", _v("muted"), summary="Mute, or restore the saved volume of, the loaded music."),
    OpSpec(0x31, "audio", "play_sound", _v("effect_id", "volume", "pan", "bank"),
           summary="Play a sound effect from the Event (bank 0) or resident bank."),
    OpSpec(0x32, "flow", "nop", (), summary="Intentional no-op."),
    OpSpec(0x33, "audio", "stop_music", (), summary="Stop and free the Event-loaded music."),
    OpSpec(0x34, "flow", "nop_alternate", (), summary="Second intentional no-op dispatch entry."),
    OpSpec(0x35, "sprite", "create", _v("sprite", "resource", hex_names=_ALIAS),
           summary="Decompress file-3 resource and create the Event sprite when it is not alive."),
    OpSpec(0x36, "sprite", "destroy", _v("sprite", hex_names=_ALIAS), summary="Destroy an Event sprite and free its bundle."),
    OpSpec(0x37, "battle", "end_successfully", (), summary="Set result 0x01, cancel combat and stop the Event VM."),
    OpSpec(0x38, "actor", "play_mecha_animation", _v("actor", "target", "animation", hex_names=_ALIAS),
           summary="Start a mecha animation without Event camera control; does not wait."),
    OpSpec(0x39, "battle", "transform_leader_to_gear", (), summary="Put party slot 0 into Gear ID 0."),
    OpSpec(0x3A, "actor", "play_animation", _v("character", "animation", hex_names=_ALIAS),
           summary="Start an animation on a character sprite; negative IDs select special animations."),
    OpSpec(0x3B, "actor", "restart_animation", _v("character", hex_names=_ALIAS),
           summary="Start the sprite's current animation again from its beginning."),
    OpSpec(0x3C, "actor", "reset_frame", _v("character", hex_names=_ALIAS),
           summary="Clear the sprite's frame timer, decoded tile count and sync counter so its frame is decoded again."),
    OpSpec(0x3D, "actor", "clear_animation_sync", _v("character", hex_names=_ALIAS),
           summary="Clear the sprite's animation-script synchronization counter."),
    OpSpec(0x3E, "actor", "move", _v("character", "x", "y", "z", hex_names=_ALIAS), summary="Linear move; polls completion."),
    OpSpec(0x3F, "actor", "move_eased", _v("character", "x", "y", "z", hex_names=_ALIAS), summary="Eased move; polls completion."),
    OpSpec(0x40, "sprite", "destroy_and_reset", _v("sprite", hex_names=_ALIAS), summary="Destroy an Event sprite and reset camera state."),
    OpSpec(0x41, "audio", "set_sound_volume", _v("effect_id", "volume", "bank"),
           summary="Set the volume of playing instances of a sound effect from the Event (bank 0) or resident bank."),
    OpSpec(0x42, "battle", "initialize_gear_turn_ui", (), summary="BattleInitializeGearTurnUi(0)."),
    OpSpec(0x43, "battle", "free_turn_render_workspace", (), summary="BattleFreeTurnRenderWorkspace(0)."),
    OpSpec(0x44, "actor", "clear_mecha_event_flags", (),
           summary="Clear the Event-controlled animation flag of all eleven mecha slots."),
    OpSpec(0x45, "actor", "load_sprite", _v("destination", "source", "animation", "sprite_mode", hex_names=_ALIAS),
           summary="Asynchronously replace a character sprite; polls completion."),
    OpSpec(0x46, "actor", "swap_sprite", _v("destination", "source", "effect", hex_names=_ALIAS),
           summary="Prepare and commit a character sprite swap."),
    OpSpec(0x47, "battle", "clear_animation_data", (), summary="Finish transfers and release temporary animation data."),
    OpSpec(0x48, "audio", "play_clip_blocking", _v("clip_group", "clip_index"), summary="Play an indexed audio clip synchronously."),
    OpSpec(0x49, "battle", "set_return_mode", _v("mode"), summary="Set the Field return mode."),
    OpSpec(0x4A, "battle", "activate_slot0_gear_hyper", (), summary="BattleActivateGearHyperMode(0)."),
    OpSpec(0x4B, "actor", "suppress_critical_pose", _v("character", hex_names=_ALIAS),
           summary="Set combatant flag +0x36 bit 0, which keeps the mecha out of its critical-HP idle pose."),
)}

BY_NAME = {spec.qualified: spec for spec in SPECS.values()}
TABLE_PATH = Path(__file__).with_name("battle_event_opcode_table.json")


def load_opcode_table() -> dict:
    return json.loads(TABLE_PATH.read_text(encoding="utf-8"))


def check_table() -> None:
    """Assert that every schema size matches the documented dispatch size."""
    table = load_opcode_table()["opcodes"]
    for opcode, spec in SPECS.items():
        documented = int(table[f"{opcode:02X}"]["bytes"].split()[0])
        if documented != spec.size:
            raise ValueError(f"opcode {opcode:02X}: schema size {spec.size} != documented {documented}")


@dataclass
class Instruction:
    opcode: int
    values: list[int]      # one raw value per operand, in operand order
    pc: int | None = None

    @property
    def spec(self) -> OpSpec:
        return SPECS[self.opcode]

    @property
    def size(self) -> int:
        return self.spec.size

    def operand(self, name: str) -> int:
        for operand, value in zip(self.spec.operands, self.values):
            if operand.name == name:
                return value
        raise KeyError(name)

    def labels(self) -> list[int]:
        return [value for operand, value in zip(self.spec.operands, self.values) if operand.kind == "label"]


def decode(bytecode: bytes, pc: int) -> Instruction | None:
    """Decode one instruction, or return None for an invalid/truncated form."""
    if pc >= len(bytecode) or bytecode[pc] not in SPECS:
        return None
    spec = SPECS[bytecode[pc]]
    if pc + spec.size > len(bytecode):
        return None
    cursor = pc + 1
    values = []
    for operand in spec.operands:
        size = FIELD_SIZES[operand.kind]
        values.append(int.from_bytes(bytecode[cursor:cursor + size], "little"))
        cursor += size
    return Instruction(spec.opcode, values, pc)


def encode(instruction: Instruction) -> bytes:
    spec = instruction.spec
    if len(instruction.values) != len(spec.operands):
        raise ValueError(f"{spec.qualified}: expected {len(spec.operands)} operands")
    output = bytearray([spec.opcode])
    for operand, value in zip(spec.operands, instruction.values):
        size = FIELD_SIZES[operand.kind]
        if not 0 <= value < 1 << (8 * size):
            raise ValueError(f"{spec.qualified}: {operand.name} value {value} does not fit {size} byte(s)")
        output += value.to_bytes(size, "little")
    return bytes(output)


def successors(instruction: Instruction) -> list[int]:
    """Control-flow successors relative to the shared bytecode base."""
    following = instruction.pc + instruction.size
    if instruction.opcode == 0x00:
        return []
    if instruction.opcode == 0x01:
        return [instruction.values[0]]
    if instruction.opcode == 0x02:
        return [following, instruction.values[3]]
    return [following]


COMPARISONS = {0: "==", 1: "!=", 2: ">", 3: "<", 4: ">=", 5: "<=", 6: "&", 8: "||"}
COMPARISON_FUNCTIONS = {9: "mask_contains", 10: "mask_lacks"}
COMPARISON_DESCRIPTIONS = {
    0: "left == right", 1: "left != right", 2: "(s16)left > (s16)right", 3: "(s16)left < (s16)right",
    4: "(s16)left >= (s16)right", 5: "(s16)left <= (s16)right", 6: "(left & right) != 0",
    7: "left != right (second encoding)", 8: "left != 0 || right != 0",
    9: "Battle entity right is in target mask left", 10: "Battle entity right is absent from target mask left",
    11: "false", 12: "false", 13: "false", 14: "false", 15: "false",
}
