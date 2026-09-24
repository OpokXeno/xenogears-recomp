#!/usr/bin/env python3
"""Generate the complete Field event DSL operation catalog."""

from __future__ import annotations

from collections import defaultdict
from pathlib import Path
import re

from decompile_field_scripts import (
    CONDITIONAL_OUTPUT_VARIABLES,
    EXTENDED,
    EXTENDED_CONDITIONAL_BRANCHES,
    IMPLICIT_OUTPUT_VARIABLES,
    OUTPUT_VARIABLES,
    PRIMARY,
    PRIMARY_CONDITIONAL_BRANCHES,
    decode_instruction,
    operation_dsl_name,
    operation_namespace,
)
from field_instruction_codec import (DIRECT_EXTENDED_BYTES, DIRECT_EXTENDED_WORDS, EVALUATED_OPERATION_INPUTS,
                                      MASKED_EXTENDED_WORDS, PRIMARY_INPUT_SCHEMAS,
                                      instruction_form, instruction_seeds)
from field_semantic_ops import semantic_forms


OUTPUT = Path(__file__).resolve().parents[1] / "docs" / "OPERATION_CATALOG.md"
CONTRACTS_OUTPUT = OUTPUT.with_name("OPERATION_CONTRACTS.md")

LIFTED_OPERATIONS = {
    (0x10, None): ("move_begin", "move_finish"),
    (0x11, None): ("bounded_begin", "bounded_finish"),
    (0x57, None): ("ballistic_finish",),
    (0xFE, 0x18): ("party_stage",),
    (0xFE, 0x5C): ("mecha_load", "mecha_reinterpret"),
    (0xFE, 0x6C): ("controller_stop", "controller_keep"),
    (0xFE, 0x77): ("image_load",),
    (0xFE, 0xD7): ("world_marker",),
}
LIFTED_PARAMETERS = {
    "move_begin": ("x", "z", "y"), "move_finish": ("x", "z", "y"),
    "bounded_begin": ("x", "z", "y", "step_limit"),
    "bounded_finish": ("x", "z", "y", "limit_if_uninitialized"),
    "ballistic_finish": ("x", "z", "y"), "party_stage": ("character", "staged", "already_present"),
    "mecha_load": ("resource",), "mecha_reinterpret": ("party_selector",),
    "controller_stop": (), "controller_keep": (), "image_load": ("resource",),
    "world_marker": ("x", "z"),
}


def lifted_parameters(item):
    names = list(LIFTED_PARAMETERS[item.kind])
    for index in range(len(names), len(item.form.operands)):
        match = re.search(r'([a-z_][a-z_0-9]*)=\{' + str(index) + r'\}', item.form.template)
        names.append(match[1] if match else f'format_value_{index}')
    return names

CATEGORY_ORDER = (
    "actor",
    "movement",
    "world",
    "camera",
    "dialogue",
    "audio",
    "visual",
    "battle",
    "inventory",
    "input",
    "state",
    "flow",
    "event",
)

CATEGORY_DESCRIPTIONS = {
    "actor": "Entities, characters, party members, sprites, and actor control.",
    "movement": "Position, rotation, walkmesh, movement, and collision.",
    "world": "Maps, encounters, triggers, transitions, and scene state.",
    "camera": "Camera position, projection, tracking, and view geometry.",
    "dialogue": "Text, portraits, windows, and dialogue choices.",
    "audio": "Music, sound, channels, volume, and tempo.",
    "visual": "Fades, lighting, models, particles, effects, and video.",
    "battle": "Battle handoffs, Battling results, and return destinations.",
    "inventory": "Inventory, items, currency, and menus.",
    "input": "Button state and accumulated input history.",
    "state": "Script-variable reads, writes, arithmetic, and bit operations.",
    "flow": "Jumps, calls, waits, yields, returns, and termination.",
    "event": "Operations that do not belong exclusively to another subsystem.",
}

SPECIAL_FORMS = {
    "P:00": ("flow", "stop"),
    "P:01": ("flow", "goto label"),
    "P:02": ("flow", "if (!(condition)) goto label"),
    "P:05": ("flow", "call label"),
    "P:06": ("flow", "call label"),
    "P:07": ("flow", "start actor.routine priority async"),
    "P:08": ("flow", "start actor.routine priority wait"),
    "P:09": ("flow", "start actor.routine priority wait_extended"),
    "P:0A": ("world", "if (inside_trigger_2d(...)) call label"),
    "P:0C": ("actor", "actor.process_player_control_if_owned_preserve_ip()"),
    "P:0D": ("flow", "return"),
    "P:13": ("flow", "nop"),
    "P:14": ("world", "world.encounters.enabled = false"),
    "P:15": ("world", "world.encounters.enabled = true"),
    "P:16": ("actor", "actor.bind_playable_character(value)"),
    "P:22": ("actor", "actor.self.visible = true"),
    "P:23": ("actor", "actor.self.visible = false"),
    "P:24": ("actor", "actor.visible = true"),
    "P:25": ("actor", "actor.visible = false"),
    "P:26": ("flow", "flow.sleep(duration)"),
    "P:2A": ("actor", "actor.self.dialogue_enabled = false"),
    "P:2B": ("actor", "actor.self.dialogue_enabled = true"),
    "P:31": ("input", "if ((input.held & mask) == 0) goto label"),
    "P:32": ("input", "if ((input.accumulated & mask) == 0) goto label"),
    "P:33": ("input", "input.accumulated = 0"),
    "P:36": ("state", "state = true"),
    "P:37": ("state", "state = false"),
    "P:3C": ("state", "state++"),
    "P:3D": ("state", "state--"),
    "P:41": ("state", "state <<= value"),
    "P:42": ("state", "state >>= value"),
    "P:49": ("state", "state.read_script_u16(base, index) / state.read_script_s16(base, index) -> state"),
    "P:5B": ("movement", "movement.park_actor_movement_update()"),
    "P:5C": ("actor", "actor.bind_party_slot(value)"),
    "P:A6": ("flow", "flow.dispatch_triplet_table(value, [case -> target, ...]) / flow.skip_triplets_to(target)"),
    "P:A7": ("actor", "actor.process_player_control_if_owned()"),
    "P:C9": ("world", "if (!inside_trigger_2d(...)) goto label"),
    "P:CB": ("world", "if (!inside_trigger_3d(...)) goto label"),
    "P:CC": ("world", "if (inside_trigger_3d(...)) call label"),
    "P:DC": ("state", "state.swap(left, right)"),
    "P:D1": ("flow", "stall_forever"),
    "P:E4": ("flow", "stall_forever"),
    "P:FD": ("flow", "nop"),
    "P:FF": ("flow", "nop"),
    "E:0A": ("state", "state |= (1 << bit)"),
    "E:0B": ("state", "state &= ~(1 << bit)"),
    "E:0D": ("dialogue", "dialogue.set_portrait(value)"),
    "E:0C": ("event", "event.set_shared_geometry_parameters(p1, p2, p3, p4, p5, p6)"),
    "E:03": ("actor", "actor.set_current_actor_uniform_scale(scale)"),
    "E:04": ("actor", "actor.set_current_actor_sprite_geometry_scale(scale)"),
    "E:08": ("actor", "actor.set_current_actor_axis_scales(x, y, z)"),
    "E:0E": ("audio", "audio.fade_music_volume(target, duration)"),
    "E:0F": ("audio", "audio.fade_music_pitch(target, duration)"),
    "E:10": ("audio", "audio.fade_music_tempo(target, duration)"),
    "E:11": ("audio", "audio.fade_music_pan(target, duration)"),
    "E:12": ("audio", "audio.set_music_channel_mask(mask)"),
    "E:13": ("audio", "audio.configure_actor_positional_sound(sound_id, volume)"),
    "E:14": ("audio", "audio.configure_actor_positional_sound_mode80(sound_id, volume)"),
    "E:18": ("actor", "actor.add_immediate_party_character(character) staged goto staged already_present goto present"),
    "E:1B": ("visual", "visual.adjust_current_model_red_green(red_delta, green_delta)"),
    "E:1C": ("movement", "movement.set_actor_position3d_immediate(x, z, y)"),
    "E:6C": ("event", "event.clear_controller_enable_flag(script.byte_at(next))"),
    "E:D2": ("event", "event.skip_three_byte_instruction()"),
    "E:D7": ("movement", "movement.set_world_map_marker_position_xz(script.masked_word(x_word, mask, 128), script.masked_word(z_word, mask, 64))"),
    "E:1D": ("visual", "visual.set_global_model_translation_step(x, z, y)"),
    "E:23": ("movement", "movement.move_party_to_formation(x1, z1, x2, z2, x3, z3, facing1, facing2, facing3)"),
    "E:3D": ("visual", "visual.set_primary_mecha_matrix_row(row, x, y, z)"),
    "E:3E": ("visual", "visual.set_secondary_mecha_matrix_row(row, x, y, z)"),
    "E:48": ("camera", "camera.set_projection_angles(yaw, dip, depth)"),
    "E:68": ("movement", "movement.walk_player_to_position_and_wait(x, z)"),
    "E:6E": ("visual", "visual.set_scene_angle_y(angle)"),
    "E:6F": ("visual", "visual.set_global_model_rotation(x, z, y)"),
    "E:72": ("movement", "movement.write_interpolated_angle(from, to, step) -> state"),
    "E:73": ("movement", "movement.write_distance_between_2d_points(x1, z1, x2, z2) -> state"),
    "E:76": ("movement", "movement.write_distance_between_3d_points(x1, z1, y1, x2, z2, y2) -> state"),
    "E:A0": ("visual", "visual.start_signed_2d_presentation(resource, configuration_1, configuration_2, configuration_3, path)"),
}

STATE_OPERATORS = {
    0x35: "=",
    0x38: "+=",
    0x39: "-=",
    0x3A: "|= 1 <<",
    0x3B: "&= ~(1 << ...)",
    0x3E: "&=",
    0x3F: "|=",
    0x40: "^=",
    0xDE: "*=",
    0xDF: "/=",
}


def operation_form(opcode: int, subopcode: int | None, record: dict) -> tuple[str, str]:
    key = (opcode, subopcode)
    if key in LIFTED_OPERATIONS:
        forms = [item for item in semantic_forms() if item.kind in LIFTED_OPERATIONS[key]]
        return operation_namespace(record["name"]), " / ".join(dict.fromkeys(
            item.form.template.format(*lifted_parameters(item)) for item in forms))
    table = "E" if subopcode is not None else "P"
    value = subopcode if subopcode is not None else opcode
    special = SPECIAL_FORMS.get(f"{table}:{value:02X}")
    if special is not None:
        return special
    if subopcode is None and opcode in STATE_OPERATORS:
        return "state", f"state {STATE_OPERATORS[opcode]} value"
    dsl_name = operation_dsl_name(record["name"])
    key = (opcode, subopcode)
    if key in OUTPUT_VARIABLES or key in IMPLICIT_OUTPUT_VARIABLES:
        if subopcode is None and opcode in PRIMARY_INPUT_SCHEMAS:
            arguments = ", ".join(role for _, _, role, _, _ in
                                  sorted(PRIMARY_INPUT_SCHEMAS[opcode], key=lambda field: field[1]))
            results = ", ".join(role for _, role in
                                (*OUTPUT_VARIABLES.get(key, ()), *IMPLICIT_OUTPUT_VARIABLES.get(key, ())))
            return operation_namespace(record["name"]), f"{dsl_name}({arguments}) -> ({results})"
        return operation_namespace(record["name"]), f"{dsl_name}(...) -> state"
    if key in CONDITIONAL_OUTPUT_VARIABLES:
        return operation_namespace(record["name"]), f"{dsl_name}(...) -> state in read mode"
    if subopcode is not None and subopcode in EXTENDED_CONDITIONAL_BRANCHES:
        return operation_namespace(record["name"]), f"{dsl_name}(..., label)"
    if subopcode is None and opcode in PRIMARY_CONDITIONAL_BRANCHES:
        if opcode in PRIMARY_INPUT_SCHEMAS:
            arguments = ", ".join(role if kind != "label" else "label"
                                  for kind, _, role, _, _ in PRIMARY_INPUT_SCHEMAS[opcode])
            return operation_namespace(record["name"]), f"{dsl_name}({arguments})"
        return operation_namespace(record["name"]), f"{dsl_name}(..., label)"
    if subopcode is None and opcode in PRIMARY_INPUT_SCHEMAS:
        arguments = ", ".join(role if kind != "label" else "label"
                              for kind, _, role, _, _ in PRIMARY_INPUT_SCHEMAS[opcode])
        return operation_namespace(record["name"]), f"{dsl_name}({arguments})"
    return operation_namespace(record["name"]), f"{dsl_name}(...)"


def _escape(value: str) -> str:
    return value.replace("|", "\\|")


def render_catalog() -> str:
    grouped = defaultdict(list)
    for opcode, record in sorted(PRIMARY.items()):
        namespace, form = operation_form(opcode, None, record)
        grouped[namespace].append((f"{opcode:02X}", record, form))
    for subopcode, record in sorted(EXTENDED.items()):
        namespace, form = operation_form(0xFE, subopcode, record)
        grouped[namespace].append((f"FE {subopcode:02X}", record, form))

    lines = [
        "# Complete Operation Catalog",
        "",
        "This file is generated from `tools/field_opcode_table.json`. It lists all",
        "256 primary opcodes and 227 extended opcodes known to the dispatcher.",
        "The DSL column shows the usual rendering. Instructions with special syntax",
        "may appear as assignments, conditions, or control-flow statements.",
        "",
        "Opcodes and handler addresses remain hexadecimal because they are technical",
        "identities. Arguments emitted in `script.xgs` are displayed in decimal.",
        "See [operation contracts](OPERATION_CONTRACTS.md) for per-instruction",
        "inputs, outputs and verified operand semantics.",
        "",
        "## Summary By Object",
        "",
        "| Object | Operations | Responsibility |",
        "|---|---:|---|",
    ]
    for category in CATEGORY_ORDER:
        records = grouped.get(category, [])
        if records:
            lines.append(
                f"| [`{category}`](#{category}) | {len(records)} | {CATEGORY_DESCRIPTIONS[category]} |"
            )

    for category in CATEGORY_ORDER:
        records = grouped.get(category, [])
        if not records:
            continue
        lines.extend(
            (
                "",
                f"## `{category}`",
                "",
                CATEGORY_DESCRIPTIONS[category],
                "",
                "| Opcode | Bytes | DSL form | Handler | Original function | Behavior |",
                "|---|---:|---|---|---|---|",
            )
        )
        for opcode, record, form in records:
            lines.append(
                f"| `{opcode}` | {_escape(record['bytes'])} | `{_escape(form)}` | "
                f"`{record['handler']}` | `{record['name']}` | {_escape(record['behavior'])} |"
            )

    lines.extend(
        (
            "",
            "## Regeneration",
            "",
            "```bash",
            "python3 tools/build_dsl_documentation.py",
            "```",
            "",
            "When the Field source documentation changes, regenerate the opcode table first:",
            "",
            "```bash",
            "python3 tools/build_opcode_table.py",
            "python3 tools/build_dsl_documentation.py",
            "```",
            "",
        )
    )
    return "\n".join(lines)


PRIMARY_ENCODING_NOTES = {
    0x02: "800A1BD0 accepts only high nibbles 00/40/80/C0. Other modes leave both values zero without reading variables; standalone source uses the equivalent constant comparison. Conditions 1/7 and 6/9 are aliases.",
    0x06: "800A1730 reads only the target at +1 and pushes PC+5. flow.call_with_reserved_word(target, reserved=word) preserves its skipped u16 as an explicit format field; the linker relocates target and return continuation.",
    0x10: "80098C00 uses +1 as a mode: zero executes the nine-byte masked X/Z/Y form; nonzero advances two bytes. 80098CAC reads control +8 in the long form.",
    0x11: "80098C3C/80098CAC read X/Z/Y at +2/+4/+6 with control +8 and the evaluated step limit at +11. Phase zero advances nine bytes; nonzero phases reread nine bytes earlier on completion. Compiler-owned operand islands encode each phase independently.",
    0x47: "80092EA0 delegates to 80092894: the words at +2/+4 are evaluated; byte +1 has no verified semantic read.",
    0x57: "80099214 selects a two-byte form when mode byte +1 has low bits 3. Exact mode 15 refreshes the walkmesh. Other short modes continue the jump and, on completion, first set IP to IP-11, then reread the initiating instruction's words at origin+2/+4/+6 and mask at origin+10. script.ballistic_origin names that earlier instruction; the compiler derives the required eleven-byte displacement. Long forms read their own four masked words.",
    0x73: "80086C34 selects the two-byte disabled form for subcommand zero. Mode 1 evaluates +2/+4/+6 but only +4 affects the preset; the other evaluated results are discarded. Modes 2/3 retain their named subcommand and original encoding but do not advance in the handler.",
    0x8E: "80095F24 compares the unsigned 32-bit little-endian amount at +1 against party gold; +5 is a separate branch target.",
    0xEA: "80092DFC delegates to 80092894: the words at +2/+4 are evaluated; byte +1 has no verified semantic read.",
}

VERIFIED_INPUTS = {
    **{(0xFE, sub): {4: ("party_selection", "0 selects all party members; 1/2/3 select party slot 0/1/2")}
       for sub in (0xAB, 0xAC)},
    (0xFE, 0xD7): {2: ("x_word", "encoded X word: signed literal or VM offset selected by the external control byte"),
                    4: ("z_word", "encoded Z word: signed literal or VM offset selected by the external control byte")},
    (0x73, None): {4: ("particle_default_preset", "0x27 chooses the alternate particle defaults")},
    (0xFE, 0x0C): {offset: (f"geometry_{index}", f"shared geometry parameter {index}")
                    for index, offset in enumerate(range(2, 14, 2), 1)},
    (0xFE, 0x1C): {2: ("x", "X coordinate"), 4: ("z", "Z coordinate"),
                    6: ("y", "Y/elevation coordinate")},
    (0xFE, 0x13): {2: ("sound_id", "current actor's positional sound identifier"),
                    4: ("volume", "positional sound volume (stored as a byte)")},
    (0xFE, 0x14): {2: ("sound_id", "current actor's positional sound identifier"),
                    4: ("volume", "positional sound volume (stored as a byte)")},
    (0xFE, 0x0F): {2: ("target", "signed music pitch target"),
                    4: ("duration", "pitch fade duration")},
    (0xFE, 0x11): {2: ("target", "signed music pan target"),
                    4: ("duration", "pan fade duration")},
    (0xFE, 0x1B): {2: ("red_delta", "signed red-vertex increment (low byte used)"),
                    4: ("green_delta", "signed green-vertex increment (low byte used)")},
    (0xFE, 0x0D): {2: ("character", "portrait character selector")},
    (0xFE, 0x27): {2: ("mode", "distortion subcommand; only mode 0 reads a duration")},
    (0xFE, 0x5C): {2: ("mode", "mecha-load subcommand; modes have different lengths")},
    (0xFE, 0x5F): {2: ("write_mask", "bit 0 writes the first RGB triplet; bit 1 the second")},
    (0xFE, 0x77): {2: ("mode", "image-asset subcommand; mode 0 reads beyond its own instruction"),
                    5: ("screen_x", "mode-1 image upload X coordinate"),
                    7: ("screen_y", "mode-1 image upload Y coordinate"),
                    9: ("placement", "mode-1 image placement selector")},
    (0xFE, 0x84): {2: ("battle_id", "battle configuration identifier"),
                    6: ("return_field", "post-battle field; 0x7FFF disables return setup"),
                    8: ("entry_point", "value stored in fixed VM slot 2 when return is configured")},
    (0xFE, 0x9A): {2: ("write_mask", "bit 0 writes first actor RGB; bit 1 writes second")},
    (0xFE, 0x9F): {2: ("mode", "zero sets party-frame lock; nonzero clears it")},
    (0xFE, 0xB0): {2: ("mode", "sound-bank subcommand; mode 1 finalizes without operands")},
    (0xFE, 0xB8): {2: ("destination", "zero selects field music; nonzero selects battle music")},
    (0xFE, 0xCF): {2: ("field_id", "destination field identifier"),
                    4: ("entry_point", "value stored in fixed VM slot 2")},
    (0xFE, 0x69): {4: ("character", "character selector for progression lookup")},
    (0xFE, 0x74): {2: ("source", "VM slot printed in debug builds")},
    (0xFE, 0xAD): {4: ("character", "party-character mapping index")},
    (0xFE, 0xB4): {4: ("character", "party-character mapping index")},
    (0xFE, 0xB2): {2: ("party_slot", "party mapping index for the write; byte +4 is also checked")},
    (0xFE, 0xB3): {2: ("party_slot", "party mapping index for the write; byte +4 is also checked")},
    (0xFE, 0xC1): {6: ("party_slot", "party-to-actor mapping selector")},
    (0xFE, 0xC7): {2: ("character", "character selector for Gear ID lookup")},
    (0xFE, 0xD3): {2: ("scale_1", "scale of first ratio"),
                    4: ("scale_2", "scale of second ratio"),
                    6: ("divisor_1", "divisor of first ratio"),
                    8: ("divisor_2", "divisor of second ratio"),
                    10: ("numerator_1", "numerator of first ratio"),
                    12: ("numerator_2", "numerator of second ratio")},
    (0xFE, 0x23): {**{offset: (f"party_{index // 2}_position_{'x' if index % 2 == 0 else 'z'}",
                               f"party slot {index // 2} destination {'X' if index % 2 == 0 else 'Z'}")
                         for index, offset in enumerate(range(2, 14, 2))},
                    15: ("party_0_facing", "first party slot facing"),
                    17: ("party_1_facing", "second party slot facing"),
                    19: ("party_2_facing", "third party slot facing")},
    (0xFE, 0x26): {**{offset: (f"distortion_target_{index}", f"distortion target state field {index}")
                          for index, offset in enumerate(range(2, 14, 2), 1)},
                    14: ("duration", "fade duration, zero promoted to one")},
    (0xFE, 0x67): {2: ("resource", "presentation resource"),
                    4: ("configuration_1", "first presentation configuration field"),
                    6: ("configuration_2", "second presentation configuration field"),
                    8: ("configuration_3", "third presentation configuration field"),
                    10: ("mode", "low nibble and bit 0x40 control presentation mode"),
                    12: ("viewport_1", "first viewport/offset field"),
                    14: ("viewport_2", "second viewport/offset field"),
                    16: ("dimension", "presentation dimension field"),
                    18: ("scale", "presentation scale field")},
    (0xFE, 0x82): {**{offset: (f"color_{index // 3 + 1}_{('red', 'green', 'blue')[index % 3]}",
                               f"panorama color {index // 3 + 1} {('red', 'green', 'blue')[index % 3]} (stored as byte)")
                         for index, offset in enumerate(range(2, 20, 2))},
                    **{offset: (f"panorama_scalar_{index}", f"panorama scalar field {index}")
                       for index, offset in enumerate(range(20, 26, 2), 1)}},
    (0xFE, 0x80): {offset: (f"geometry_state_{address:04X}",
                           f"direct panorama geometry field 800B{address:04X}")
                    for offset, address in zip(range(2, 16, 2),
                                               (0x0080, 0x0082, 0x0084, 0x0086,
                                                0x008A, 0x008C, 0x008E))},
    (0xFE, 0x88): {2: ("light_index", "light-gradient index"),
                    **{offset: (f"color_{index // 3 + 1}_{('red', 'green', 'blue')[index % 3]}",
                               f"light gradient color {index // 3 + 1} {('red', 'green', 'blue')[index % 3]}")
                       for index, offset in enumerate(range(4, 16, 2))},
                    16: ("interpolation", "light-gradient distance/interpolation field")},
    (0xFE, 0x90): {2: ("bank", "particle bank index activated by this call"),
                    4: ("count", "per-bank particle instance count at state +0x02D2"),
                    6: ("bank_field_1", "per-bank field at +0x02CE"),
                    8: ("bank_field_2", "per-bank field at +0x02D0")},
    (0xFE, 0x93): {2: ("bank_field_0322", "particle bank state +0x0322"),
                    4: ("bank_field_0324", "particle bank state +0x0324"),
                    6: ("bank_field_0320", "particle bank state +0x0320"),
                    8: ("flag_1", "first value ORed into particle flags"),
                    10: ("flag_2", "second value shifted left one and ORed into particle flags")},
    (0xFE, 0xA0): {2: ("resource", "presentation resource at 800C3A20"),
                    4: ("configuration_1", "presentation field 800C3A2A"),
                    6: ("configuration_2", "presentation field 800C3A2C"),
                    8: ("configuration_3", "presentation field 800C3A2E"),
                    10: ("path", "presentation path field 800C3A38")},
    (0xFE, 0xA6): {2: ("selector_and_key_high", "low nibble selects motion key; upper bits supply key high portion"),
                    4: ("key_low", "low portion of motion key")},
    (0xFE, 0xA7): {2: ("selector_1_and_key_high", "first selector and key high portion"),
                    4: ("key_1_low", "first key low portion"),
                    6: ("selector_2_and_key_high", "second selector and key high portion"),
                    8: ("key_2_low", "second key low portion")},
    (0xFE, 0xAF): {2: ("joint_selector_1", "joint transform selector 1"),
                    4: ("joint_selector_2", "joint transform selector 2"),
                    6: ("vector_x", "input vector X"), 8: ("vector_y", "input vector Y"),
                    10: ("vector_z", "input vector Z")},
    (0xFE, 0xBF): {2: ("match_mode", "battle handoff/match mode"),
                    4: ("setup_subtype", "battle setup subtype"),
                    6: ("selection_1", "first battle selection"),
                    8: ("selection_2", "second battle selection"),
                    10: ("handoff_id", "battle handoff identifier"),
                    12: ("handoff_value", "battle handoff value")},
    (0xFE, 0xC8): {offset: (f"direction_{index // 2}_component_{index % 2 + 1}",
                           f"particle direction {index // 2} component {index % 2 + 1}")
                    for index, offset in enumerate(range(2, 18, 2))},
    (0xFE, 0xC9): {offset: (f"direction_{index // 2 + 4}_component_{index % 2 + 1}",
                           f"particle direction {index // 2 + 4} component {index % 2 + 1}")
                    for index, offset in enumerate(range(2, 18, 2))},
    (0xFE, 0xD4): {2: ("mode", "overlay-list subcommand; see mode-specific forms")},
    (0xFE, 0xDD): {2: ("mode", "VRAM-snapshot subcommand; see mode-specific forms")},
    **{(0xFE, sub): {2: (label, meaning)} for sub, label, meaning in (
        (0x07, "mode", "current actor flag 0x400 mode"),
        (0x18, "character_id", "staged party character byte"),
        (0x19, "character_id", "party character byte"),
        (0x1E, "gear_mode", "requested map Gear mode"),
        (0x20, "party_slot", "party slot to dismount"),
        (0x25, "height_mode", "camera follow-height mode"),
        (0x45, "animation_id", "current actor animation override"),
        (0x46, "rotation_mode", "mecha rotation authority mode"),
        (0x4C, "animation_id", "forced animation identifier"),
        (0x4D, "animation_id", "forced animation identifier"),
        (0x86, "transition_mode", "video transition mode"),
        (0x97, "particle_id", "current actor particle effect identifier"),
        (0x99, "menu_argument", "one-bit menu argument, inverted for storage"),
        (0xA3, "vram_mode", "zero captures VRAM; nonzero restores it"),
        (0xCA, "release_mode", "mecha release mode"),
        (0xD8, "lighting_mode", "sprite lighting bypass mode"),
        (0xD9, "direction_table", "random-turn direction table index"),
        (0xE0, "pause_disabled", "Field pause-disable byte"),
    )},
}


SPECIAL_OPERAND_ROLES = {
    (0x10, None): {1: ("mode", "nonzero mode advances without setting a position")},
    (0x02, None): {1: ("left", "left comparison/bit-test value"),
                   3: ("right", "right comparison/bit-test value")},
    **{(op, None): {1: ("actor", "target actor"),
                     2: ("routine", "routine ID and priority packed in one byte")}
       for op in (0x07, 0x08, 0x09)},
    **{(op, None): {1: ("trigger", "indexed 2D/3D trigger record")}
       for op in (0x0A, 0xC9, 0xCB, 0xCC)},
    **{(op, None): {1: ("actor", "actor selector")}
       for op in (0x24, 0x25, 0x2D)},
    **{(op, None): {1: ("destination", "VM slot being updated"),
                     3: ("source", "value applied to destination")}
       for op in (0x35, 0x38, 0x39, 0x3A, 0x3B, 0x3E, 0x3F, 0x40, 0xDE, 0xDF)},
    **{(op, None): {1: ("destination", "VM slot being updated")}
       for op in (0x36, 0x37, 0x3C, 0x3D, 0x41, 0x42)},
    **{(op, None): {1: ("destination", "VM slot being shifted"),
                     3: ("count", "shift count")}
       for op in (0x41, 0x42)},
    **{(op, None): {1: ("data_base", "script-data base offset; not a branch target"),
                     5: ("index", "evaluated index added to script-data base")}
       for op in (0x48, 0x49)},
    (0x57, None): {1: ("mode", "ballistic mode and walkmesh-height flag")},
    (0x73, None): {1: ("subcommand", "particle initialization subcommand")},
    (0xA8, None): {3: ("scale", "evaluated upper scale for random result")},
    **{(op, None): {1: ("destination", "VM slot written with captured camera state")}
       for op in (0xAF, 0xB0, 0xB1)},
    (0xDC, None): {1: ("left", "first VM slot"), 3: ("right", "second VM slot")},
    **{(0xFE, sub): {2: ("flags", "packed VM slot and bit index")}
       for sub in (0x0A, 0x0B)},
}


def operand_role(key, operand, index):
    if operand.kind == "relative":
        names = {
            ((0x57, None), -11): "jump_initializer",
            ((0xFE, 0x18), 3): "staged_target",
            ((0xFE, 0x18), 5): "already_present_target",
            ((0xFE, 0x5C), 6): "resource_word",
            ((0xFE, 0x6C), 2): "next_opcode",
            ((0xFE, 0x77), 6): "resource_word",
            ((0xFE, 0x77), 14): "external_control",
            ((0xFE, 0xD7), 10): "external_control",
        }
        return names.get((key, operand.offset), "script_byte"), "symbolic script-byte address at a fixed displacement from this instruction"
    if key[1] is None and key[0] in {0xAF, 0xB0, 0xB1} and operand.kind != "var":
        return "value", "direct camera yaw/dip/depth value assigned in write mode"
    if operand.kind == "fixedvar":
        for at, name in IMPLICIT_OUTPUT_VARIABLES.get(key, ()):
            if at == operand.offset:
                return name, f"fixed VM output: {name.replace('_', ' ')}"
    role = VERIFIED_INPUTS.get(key, {}).get(operand.offset)
    if role is None:
        role = SPECIAL_OPERAND_ROLES.get(key, {}).get(operand.offset)
        if role is not None and operand.kind == "priority":
            role = ("priority", "priority in the same packed routine byte")
        if role is not None and operand.kind == "bit":
            role = ("bit", "bit index in the same packed word")
    if role is None and key[1] is None:
        role = next(((name, f"primary handler {PRIMARY[key[0]]['handler']} operand: {name.replace('_', ' ')}")
                     for kind, offset, name, control, mask in PRIMARY_INPUT_SCHEMAS.get(key[0], ())
                     if offset == operand.offset and kind == operand.kind
                     and (kind != "masked" or (control, mask) == (operand.control, operand.mask))), None)
        if role is not None and role[0] == "unclassified_byte":
            role = (role[0], "encoded byte; semantic read not established by the handler")
    if role is None:
        role = next(((name, name.replace("_", " "))
                     for at, name in EVALUATED_OPERATION_INPUTS.get(key, ())
                     if at == operand.offset), None)
    if role is None and key[1] is not None:
        role = next(((name, name.replace("_", " "))
                     for at, name in DIRECT_EXTENDED_WORDS.get(key[1], ())
                     if at == operand.offset), None)
    if role is None and key[1] is not None:
        role = next(((name, name.replace("_", " "))
                     for at, name, _, _ in MASKED_EXTENDED_WORDS.get(key[1], ())
                     if at == operand.offset), None)
    if role is None and key[1] is not None and operand.kind == "actor":
        role = ("actor", "resolved actor selector (including self/party aliases)")
    if role is None:
        role = next(((name, f"VM output: {name.replace('_', ' ')}")
                     for at, name in (*OUTPUT_VARIABLES.get(key, ()), *IMPLICIT_OUTPUT_VARIABLES.get(key, ()))
                     if at == operand.offset and operand.kind in {"var", "fixedvar"}), None)
    if role is None and operand.kind == "label":
        role = ("target", "control-flow destination")
    return role or (f"operand_{index + 1}", "semantic meaning not yet verified")


def render_contracts() -> str:
    """Emit a per-opcode contract; do not invent meanings for unverified bytes."""
    def describe_operand(key, index, operand):
        offset, kind = operand.offset, operand.kind
        label, meaning = operand_role(key, operand, index)
        if kind == "masked":
            encoding = ("signed 16-bit immediate (-32768..32767) or declared VM variable; "
                        f"control byte +{operand.control} bit 0x{operand.mask:02X} selects immediate")
        elif kind == "v80":
            encoding = "0..32767 immediate or declared VM variable; word bit 15 selects immediate"
        elif kind == "s16":
            encoding = "signed 16-bit integer (-32768..32767)"
        elif kind == "u16":
            encoding = "unsigned 16-bit integer (0..65535)"
        elif kind == "u32":
            encoding = "unsigned 32-bit integer (0..4294967295)"
        elif kind == "u8":
            encoding = "encoded byte (0..255); may be a subfield of a larger value"
        elif kind == "label":
            encoding = ("symbolic script-data address, relocated during linking"
                        if key in {(0x48, None), (0x49, None)} else
                        "symbolic bytecode target, relocated during linking")
        elif kind == "relative":
            encoding = f"label constrained to instruction PC {offset:+d}; no address is stored in the bytecode"
        elif kind in {"var", "fixedvar", "packedvar"}:
            encoding = "declared VM slot" + (" (implicit)" if kind == "fixedvar" else "")
        elif kind == "actor":
            encoding = "actor selector (including self/party selectors)"
        elif kind == "routine":
            encoding = "routine ID (0..31) in the low five bits"
        elif kind == "priority":
            encoding = "priority (0..7) in the high three bits"
        elif kind == "bit":
            encoding = "bit index (0..15) in the low nibble"
        elif kind in {"low2", "low4", "high4", "low7"}:
            encoding = {"low2": "low two bits of a control byte (0..3)",
                        "low4": "low nibble of a packed byte (0..15)",
                        "high4": "high nibble of a packed byte (0..15)",
                        "low7": "low seven bits of a control byte (0..127)"}[kind]
        else:
            encoding = f"{kind} operand"
        location = f"fixed VM slot 0x{offset:04X}" if kind == "fixedvar" else f"byte +{offset}"
        return f"- `{label}` ({location}): {meaning}; {encoding}."

    seeds = {(seed[0], seed[1] if seed[0] == 0xFE and len(seed) > 1 else None): seed
             for seed in reversed(instruction_seeds())}
    extended_modes = defaultdict(list)
    primary_modes = defaultdict(list)
    for variant in instruction_seeds():
        if variant[0] == 0xFE and len(variant) > 1:
            extended_modes[variant[1]].append(variant)
        elif variant[0] in {0x10, 0x49, 0x57, 0x73, 0xAF, 0xB0, 0xB1}:
            primary_modes[variant[0]].append(variant)
    lines = [
        "# Field VM operation contracts",
        "",
        "Generated by `tools/build_dsl_documentation.py` from the opcode table and",
        "the executable operand forms. Every dispatcher entry has its own contract.",
        "Descriptions of unclassified operands deliberately state only their encoded",
        "position and range; a byte is not a proven independent semantic parameter.",
        "XGS contains operations and symbolic arguments, without encoding/layout annotations.",
        "The compiler constructs native operand islands from semantic values. No XGA",
        "or original binary is consulted, and no raw instruction is emitted.",
        "The same per-instruction encoder and layout relaxation run on every source.",
        "All comments are ignored; encoding and layout are derived only from code.",
        "Lossless forms retain reserved fields, dispatch variants and movement phases as typed source arguments.",
        "Both concise and lossless spellings are valid when authoring new code.",
        "Use the concise spelling to omit its additional format fields together; the defaults below apply in that case.",
        "The lossless spelling uses the named fields in the displayed order. The decompiler emits it when needed for byte identity.",
        "See SHARED_SCRIPT_BYTES.md for the native lowering of multi-phase operations.",
        "",
        "`-> (state)` means a VM slot is written, not a Python-style return value.",
        "No operation returns a value as an expression; some change control flow.",
    ]
    for table, records in (("primary", PRIMARY), ("extended", EXTENDED)):
        lines.extend(("", f"## {table.capitalize()} instructions"))
        if table == "extended":
            explicit = ({sub for op, sub in EVALUATED_OPERATION_INPUTS if op == 0xFE}
                        | set(DIRECT_EXTENDED_BYTES) | set(DIRECT_EXTENDED_WORDS)
                        | set(MASKED_EXTENDED_WORDS)
                        | {sub for op, sub in OUTPUT_VARIABLES if op == 0xFE}
                        | {sub for op, sub in IMPLICIT_OUTPUT_VARIABLES if op == 0xFE}
                        | {0x0A, 0x0B, 0x0C, 0x0D, 0x0F, 0x11, 0x1B, 0x1C, 0x74})
            fallback = {0, *range(0x78, 0x7F)}
            generic = set(EXTENDED) - explicit - fallback
            no_payload = {sub for sub in generic if EXTENDED[sub]["bytes"] == "2"}
            lines.extend(("", f"FE coverage: {len(explicit)} table-driven typed/output entries, "
                          f"{len(no_payload)} two-byte entries with no encoded payload, "
                          f"{len(fallback)} prefix-only fallbacks, and "
                           f"{len(generic - no_payload)} specially encoded/mode-dependent entries "
                          "(`FE 5C`, `FE 77`, `FE D2`, `FE D7`).",
                          "Runtime-dependent or out-of-form reads also occur at "
                          "`FE 18` and `FE 6C`; see their individual notes."))
        if table == "primary":
            lines.extend(("", f"All {len(PRIMARY)} primary dispatch slots have handler identities and "
                          f"effect summaries; {len(PRIMARY_INPUT_SCHEMAS)} have an explicit "
                          "input schema here. Other slots use dedicated flow/state/output forms, "
                          "take no operand, or retain unclassified encoded bytes. "
                          "Unverified bytes are called out at their handlers below."))
        for value, record in sorted(records.items()):
            op, sub = (value, None) if table == "primary" else (0xFE, value)
            key = (op, sub)
            identity = f"{op:02X}" if sub is None else f"FE {sub:02X}"
            lines.extend(("", f"### `{identity}` — `{record['name']}`", "",
                          f"Handler: `{record['handler']}`; encoded length: {record['bytes']} byte(s).",
                          "", f"**Effect:** {record['behavior']}", ""))
            if key in LIFTED_OPERATIONS:
                for item in semantic_forms():
                    if item.kind not in LIFTED_OPERATIONS[key]:
                        continue
                    names = lifted_parameters(item)
                    lines.extend((f"**XGS:** `{item.form.template.format(*names)}`", "", "**Inputs:**"))
                    if not names:
                        lines.append("- None.")
                    for index, (name, operand) in enumerate(zip(names, item.form.operands)):
                        description = ("symbolic continuation; may move freely" if operand.kind == "label" else
                                       "signed 16-bit value or VM variable" if operand.kind == "masked" else
                                       "0..32767 immediate or VM variable" if operand.kind == "v80" else
                                        f"reserved format flags, subset of 0x{operand.mask:02X}" if operand.kind == "bits" else "byte value")
                        if item.complete and index >= len(LIFTED_PARAMETERS[item.kind]):
                            description += f"; concise default `{operand.read(item.seed, {})}`"
                        lines.append(f"- `{name}`: {description}.")
                    lines.extend(("", "**Result:** " + ("Terminates or branches to the named continuation; no expression value."
                                  if item.terminal else "Native VM state effects; no expression value."), ""))
                if key == (0x57, None):
                    lines.append("Long modes still use `movement.continue_ballistic_actor_move(mode, x, z, y, duration)`; mode 15 uses `movement.refresh_actor_walkmesh()`. Finish takes a final boolean selecting absolute Y or walkmesh height.")
                if key == (0xFE, 0x5C):
                    lines.append("Modes 0 (hide) and 2 (construct/finalize) use `actor.load_current_actor_mecha(mode)`. Mode 2 also accepts `discarded_value=value` to retain its evaluated-but-discarded operand. The reinterpret form requires an encoded party-selector low byte above 2.")
                if key == (0xFE, 0x77):
                    lines.append("Mode 1 uses `event.manage_overlay_image_asset(1, x, y, placement)` and accepts `reserved=word, reserved_flags=flags` for its skipped word and unused mask bits; other nonzero modes release the asset.")
                lines.extend(("", "**Lowering:** Operands and control flow are rebuilt from this source using private native data and jumps. There is no dependency on a neighboring source instruction or original encoding. See [shared-byte lowering](SHARED_SCRIPT_BYTES.md)."))
                continue
            seed = seeds.get(key)
            if seed is None:
                if sub is not None:
                    lines.extend(("**XGS form:** `flow.extended_prefix();`", "",
                                  "**Inputs:** The following opcode byte selects this no-op FE slot;",
                                  "the linker retains a valid prefix or uses the equivalent one-byte NOP.", "",
                                  "**Result:** Consume FE only, then dispatch the following byte as a primary opcode."))
                else:
                    lines.extend(("**Form:** secondary dispatcher prefix; see the FE entries.", "",
                                  "**Inputs:** The following subopcode byte selects its handler.", "",
                                  "**Result:** Dispatch the selected FE handler."))
                continue
            instruction = decode_instruction(seed, 0)
            form = instruction_form(instruction, {})
            args = [f"<{operand_role(key, operand, index)[0]}>"
                    for index, operand in enumerate(form.operands)]
            lines.extend((f"**XGS form:** `{form.template.format(*args)}`", ""))
            complete = instruction_form(instruction, {}, lossless=True)
            if complete.template != form.template:
                values = []
                for index, operand in enumerate(complete.operands):
                    named = re.search(r'([a-z_][a-z_0-9]*)=\{' + str(index) + r'\}', complete.template)
                    role = named[1] if named else operand_role(key, operand, index)[0]
                    values.append(f'<{role}>')
                lines.extend((f"**Lossless form:** `{complete.template.format(*values)}`", ""))
                for operand in complete.operands[len(form.operands):]:
                    domain = f'flag subset 0x{operand.mask:02X}' if operand.kind == 'bits' else {
                        'u8': 'u8 (0..255)', 'u16': 'u16 (0..65535)',
                    }.get(operand.kind, operand.kind)
                    default = operand.read(seed, {})
                    lines.append(f'- Additional format field at byte +{operand.offset}: `{domain}`; encoded from the named argument; concise default `{default}`.')
                lines.append('')
            if sub is None and op in primary_modes:
                for variant in primary_modes[op]:
                    variant_form = instruction_form(decode_instruction(variant, 0), {})
                    values = [f"<{operand_role(key, field, index)[0]}>"
                              for index, field in enumerate(variant_form.operands)]
                    mode = variant[7] if op == 0x49 else variant[3] if op in {0xAF, 0xB0, 0xB1} else variant[1]
                    lines.append(f"**Mode {mode} ({len(variant)} bytes):** "
                                 f"`{variant_form.template.format(*values)}`")
                lines.append("")
            if sub in {0x27, 0x5C, 0x77, 0xB0, 0xD4, 0xDD}:
                for variant in extended_modes[sub]:
                    variant_form = instruction_form(decode_instruction(variant, 0), {})
                    values = [f"<{operand_role(key, field, index)[0]}>"
                              for index, field in enumerate(variant_form.operands)]
                    lines.append(f"**Mode {variant[2]} ({len(variant)} bytes):** "
                                 f"`{variant_form.template.format(*values)}`")
                lines.append("")
            output_offsets = {at for at, _ in OUTPUT_VARIABLES.get(key, ())}
            if key in CONDITIONAL_OUTPUT_VARIABLES and instruction.raw[3] == 0:
                output_offsets.update(at for at, _ in CONDITIONAL_OUTPUT_VARIABLES[key])
            output_operands = [(index, operand) for index, operand in enumerate(form.operands)
                               if operand.kind == "fixedvar" or
                               (operand.kind == "var" and operand.offset in output_offsets)]
            input_operands = [(index, operand) for index, operand in enumerate(form.operands)
                              if (index, operand) not in output_operands]
            lines.append("**Inputs:**")
            if not input_operands:
                if key == (0xFE, 0x18):
                    lines.append("- Encoded character ID: direct byte +2; subsequent bytes are path-dependent.")
                elif key == (0xFE, 0xD7):
                    lines.append("- Encoded X and Z: signed/VM words at +2 and +4, with control in the next instruction (+10).")
                elif key == (0xFE, 0x6C):
                    lines.append("- No byte in this encoding; tests the following opcode byte without consuming it.")
                else:
                    lines.append("- None.")
            lines.extend(describe_operand(key, index, operand) for index, operand in input_operands)
            if output_operands:
                lines.extend(("", "**VM writes:**"))
                lines.extend(describe_operand(key, index, operand) for index, operand in output_operands)
            if sub is None and op in PRIMARY_INPUT_SCHEMAS:
                control_bytes = sorted({control for kind, _, _, control, mask
                                        in PRIMARY_INPUT_SCHEMAS[op] if kind == "masked"})
                if control_bytes:
                    lines.extend(("", "**Encoding:** The primary handler's separate control byte(s) at "
                                  + ", ".join(f"+{at}" for at in control_bytes)
                                  + " select variable vs immediate for the listed words; "
                                   "unused bits are explicit reserved_flags in the lossless form; every field is supplied by the source."))
            if sub is None and op in PRIMARY_ENCODING_NOTES:
                lines.extend(("", "**Handler evidence:** " + PRIMARY_ENCODING_NOTES[op]))
            if key == (0xFE, 0x1C):
                lines.extend(("", "The engine resolves X, Z and Y in that order, shifts each to 16.16",
                              "fixed-point and synchronizes the physical and rendered actor positions.",
                              "The control byte is encoding, never an XGS argument. Example:",
                              "`FE 1C E3 FE 84 01 E0 01 E0` →",
                               "`movement.set_actor_position3d_immediate(-285, 388, 480);`."))
            elif key == (0xFE, 0x76):
                lines.extend(("", "The third coordinate of the first point shares control bit 0x20",
                              "with its second coordinate; the last two coordinates of the second",
                              "point share bit 0x08. Each pair must use the same immediate/variable mode."))
            elif key == (0xFE, 0xC2):
                lines.extend(("", "The handler at 80088674 reads four evaluated words and advances",
                              "nine bytes after the FE prefix: full instruction length is 10 bytes."))
            elif key in {(0xFE, 0x2C), (0xFE, 0x2D), (0xFE, 0x2E), (0xFE, 0x2F)}:
                lines.extend(("", "The output word is also used as an actor selector:",
                              "its low byte is resolved through 8009CDB4(1).",
                              "These are overlapping views of one encoded field."))
            elif key == (0xFE, 0x6C):
                lines.extend(("", "The handler reads the NEXT byte after this instruction",
                              "without consuming it. That byte belongs to the following dispatch."))
            elif key == (0xFE, 0x18):
                lines.extend(("", "Staging and duplicate-character paths advance different PC distances",
                              "after the FE prefix (+2 versus +4). The only argument is the",
                              "immediate character ID at byte +2. Bytes +3/+4 belong to later",
                              "dispatch on the staging path. The named call exposes both",
                              "continuations, and the compiler derives the two-byte skip constraint."))
            elif key == (0xFE, 0x5C):
                lines.extend(("", "Mode 1 evaluates a word beyond its short three-byte form",
                              "(FE-relative +6); preserve the following instruction's bytes.",
                              "Mode 2 evaluates +3/+4 but does not use the result; mode 0",
                              "uses only its subcommand. Modes above 2 return with IP at",
                              "the subopcode; resume_primary_at exposes that reinterpretation",
                              "and constrains the extra byte read by the primary 5C handler."))
            elif key in {(0xFE, 0x77), (0xFE, 0xD7)}:
                lines.extend(("", "A masked reader in the handler addresses a control byte beyond",
                              "the instruction length. XGS makes that script-byte read explicit",
                              "and the compiler constrains its symbolic label after linking."))
                if key == (0xFE, 0xD7):
                    lines.extend(("", "The two input words are saved marker X at bytes +2/+3 and Z",
                              "at +4/+5. Byte +6 is skipped. The immediate/variable mask is",
                                  "read at +10. script.masked_word encodes an unsigned word,",
                                  "interpreted as signed immediate or VM-slot offset according",
                                  "to that external mask; the opcode still consumes seven bytes."))
                else:
                    lines.extend(("", "Mode 0 reads its resource word at +6/+7 using bit 0x80",
                                  "of byte +14. Mode 1 reads its three words within the",
                                  "instruction. Other modes release the resource."))
            elif key == (0xFE, 0x84):
                lines.extend(("", "Bytes +4/+5 are skipped. The value at +8 is written to",
                              "fixed VM slot 2 only when a return field is installed."))
            elif key == (0xFE, 0xBD):
                lines.extend(("", "Only the first evaluated word (+2/+3) is read; bytes +4..+7",
                               "are skipped payload, not separate attachment arguments.", "",
                               "**Authoring/defaults:** `visual.set_particle_attachment_mode(1);` is valid and",
                               "is equivalent to `visual.set_particle_attachment_mode(1, reserved_1=0, reserved_2=0);`.",
                               "Both emit `FE BD 01 80 00 00 00 00`. The reserved fields have no handler effect.",
                               "For an original containing `FE BD 01 80 00 80 00 80`, the decompiler emits",
                               "`visual.set_particle_attachment_mode(1, reserved_1=32768, reserved_2=32768);`.",
                               "Each reserved value is an unsigned 16-bit word, written little-endian.",
                               "All of these forms compile from code alone; comments are ignored."))
            elif key == (0xFE, 0x26):
                lines.extend(("", "Six evaluated targets become 16.16 distortion interpolation",
                              "targets at 800B2080..800B2094. The seventh evaluated word is",
                              "the duration; zero is promoted to one before calculating deltas."))
            elif key in {(0xFE, 0x5F), (0xFE, 0x9A)}:
                lines.extend(("", "Mask bit 0 writes RGB to the first actor lighting triplet;",
                              "bit 1 writes to the second. RGB words are evaluated only when",
                              "at least one selected triplet is written and are stored as bytes."))
            elif key == (0xFE, 0x67):
                lines.extend(("", "The viewport/offset words at +12/+14 each feed two state",
                              "fields; +10 contributes both its low nibble and bit 0x40 to",
                              "presentation mode. The nine words are not interchangeable."))
            elif key == (0xFE, 0x80):
                lines.extend(("", "Seven direct words fill panorama geometry fields at",
                              "800B0080/82/84/86/8A/8C/8E; the handler clears 800B0088",
                              "itself. Bit 15 in these words is data, not an immediate tag."))
            elif key == (0xFE, 0x88):
                lines.extend(("", "The first masked value indexes a light gradient; the next",
                              "six form two RGB triplets. The final value controls the indexed",
                              "distance/interpolation field. All eight use control byte +18."))
            elif key == (0xFE, 0x93):
                lines.extend(("", "The last two evaluated inputs are combined into flags as",
                              "value(+8) | (value(+10) << 1) | existing_flags, rather than",
                              "stored as independent output fields."))
            elif key == (0xFE, 0x91):
                lines.extend(("", "Six masked words populate per-bank fields at offsets",
                              "+0x02D8/+0x02DA/+0x02DC and +0x02E0/+0x02E2/+0x02E4.",
                              "The handler alone does not establish XYZ axis names."))
            elif key == (0xFE, 0x92):
                lines.extend(("", "The first masked word is stored as a 32-bit per-bank value",
                              "at +0x02D4; the other five fill +0x02E8/+0x02EA/+0x02EC",
                              "and +0x02F2/+0x02F4. These are distinct physics fields."))
            elif key == (0xFE, 0x95):
                lines.extend(("", "The first three masked values form one byte-truncated",
                              "particle RGB triplet; the remaining three form a second."))
            elif key == (0xFE, 0xBF):
                lines.extend(("", "All six evaluated words are truncated to bytes in the",
                              "battle handoff record. The battle consumer branches on match",
                              "mode/subtype and consumes both selections and handoff fields."))
            elif key == (0xFE, 0xD3):
                lines.extend(("", "Each VM result computes (((numerator << 16) / divisor)",
                              "* scale) >> 16 from its corresponding three inputs; the two",
                              "output destinations are encoded at +14 and +16."))
            elif key in {(0xFE, 0xA6), (0xFE, 0xA7)}:
                lines.extend(("", "Each motion key uses a pair of evaluated words: the first",
                              "contains a low-nibble selector and upper key portion; the",
                              "second supplies the low key portion. Keep the pair packed."))
            elif key in {(0xFE, 0xC8), (0xFE, 0xC9)}:
                lines.extend(("", "Eight masked signed words make four two-component particle",
                              "direction entries; C8 writes entries 0..3 and C9 writes 4..7."))
            elif key == (0xFE, 0xD4):
                lines.extend(("", "Mode 0 allocates the list and mode 2 frees it, with no more",
                              "inputs. Mode 1 uses the four evaluated words at +3/+5/+7/+9",
                              "as entry index, screen X, screen Y and placement variant.",
                              "Mode 3 uses them as entry index, red, green and blue; the colors",
                              "are byte-truncated. Other modes do not advance normally."))
            elif key == (0xFE, 0xDD):
                lines.extend(("", "Mode 0 uses +3 as snapshot origin and +5 as allocation",
                              "height/count (shifted left 9). Mode 1 uses +3 as a 0x200-byte",
                              "buffer slice index and passes +5 to the conversion routine.",
                              "Modes 2/3 have no additional operands and yield/free resources."))
            elif key in {(0xFE, 0xB2), (0xFE, 0xB3)}:
                lines.extend(("", "The handler checks byte +4 as a party-slot index while also",
                              "using it as the high byte of the evaluated HP/MP word at +3;",
                              "byte +2 supplies a second slot index for the write. These",
                              "fields overlap and must not be treated as independent arguments."))
            elif key == (0xFE, 0xD2):
                lines.extend(("", "The handler only advances the VM PC; the following payload bytes",
                              "are not read as separate semantic parameters."))
            elif key in {(0xFE, 0x13), (0xFE, 0x14)}:
                lines.extend(("", "The sound ID is stored as a 16-bit value; volume is truncated to",
                              "the low byte. Zero ID disables the sound. The mode (0 or 0x80) is",
                              "chosen by the opcode, not by an input argument.",
                              ("`FE 13 15 80 7F 80` → `audio.configure_actor_positional_sound(21, 127);`."
                               if sub == 0x13 else
                               "`FE 14 15 80 7F 80` → `audio.configure_actor_positional_sound_mode80(21, 127);`.")))
            if "->" in form.template:
                lines.extend(("", "**Result:** Writes the VM output slot(s) shown after `->`; no expression return."))
    lines.extend(("", "## Regeneration", "", "```bash", "python3 tools/build_dsl_documentation.py", "```", ""))
    return "\n".join(lines)


def main() -> int:
    OUTPUT.write_text(render_catalog(), encoding="utf-8")
    CONTRACTS_OUTPUT.write_text(render_contracts(), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
