"""Reversible operand forms shared by the Field DSL renderer and compiler.

An encoding selects the opcode, mode and otherwise invisible control bits.
Forms describe only fields actually displayed by the DSL. Unclassified fields
are encoded bytes, not guessed semantic arguments.
"""

from __future__ import annotations

import re
import struct
from dataclasses import dataclass, field
from functools import lru_cache


# Complete input schemas verified against FieldReadEvaluatedOperand (800ACDEC)
# call sites in field-overlay.bin. In these words, bit 15 selects an immediate;
# it is not another argument. Roles are also used for variable discovery.
EVALUATED_OPERATION_INPUTS = {
    (0x0B, None): ((1, "field_graphic"),),                # 800A1624
    (0x21, None): ((1, "actor_movement_speed"),),         # 8009E094
    (0x69, None): ((1, "actor_rotation_sector"),),        # 8009AC7C
    (0x71, None): ((1, "battle_configuration"),),         # 80093568
    (0x72, None): ((1, "music_id"),),                     # 8008F724 -> 8008F7B8
    (0x74, None): ((1, "sound_effect_id"),),              # 8008F668
    (0x75, None): ((1, "music_id"),),                     # 8008F76C -> 8008F7B8
    (0x8C, None): ((1, "inventory_object"),),             # 8009631C
    (0x8D, None): ((1, "inventory_object"),),             # 8009640C
    (0x9A, None): ((1, "camera_reacquisition_duration"),), # 8008FC4C
    (0xA0, None): (                                      # 8009BA7C
        (1, "camera_sector"),
        (3, "camera_dip"),
        (5, "projection_screen_distance"),
    ),
    (0xFE, 0x13): ((2, "positional_sound_id"), (4, "positional_sound_volume")), # 8008CD48
    (0xFE, 0x14): ((2, "positional_sound_id"), (4, "positional_sound_volume")), # 8008CDD4
    (0xFE, 0x0E): ((2, "music_volume_target"), (4, "music_fade_duration")), # 8008C84C
    (0xFE, 0x10): ((2, "music_tempo_target"), (4, "music_fade_duration")), # 8008CA60
    (0xFE, 0x03): ((2, "actor_uniform_scale"),),       # 8008D0F4
    (0xFE, 0x04): ((2, "actor_sprite_geometry_scale"),), # 8008D26C
    (0xFE, 0x08): ((2, "actor_scale_x"), (4, "actor_scale_y"), (6, "actor_scale_z")), # 8008D180
    (0xFE, 0x12): ((2, "music_channel_mask"),),        # 8008CC74
    # FE handler read sites in field-overlay.bin. Offsets include the FE byte;
    # FUN_800acdec(n) reads the word at FE-relative byte n+1 (bit 15 is mode).
    (0xFE, 0x05): ((3, "walkmesh_id"),),                  # 80095CC4
    (0xFE, 0x06): ((3, "walkmesh_material"),),            # 80095D6C
    (0xFE, 0x09): ((2, "mecha_suppression_mode"),),       # 8008D078
    (0xFE, 0x15): ((2, "field_graphic"), (4, "graphic_variant")), # 800A14F0
    (0xFE, 0x21): ((2, "party_character"),),             # 800A06E8
    (0xFE, 0x27): ((3, "distortion_parameter"),),        # 8008B328, mode 0 only
    (0xFE, 0x23): ((15, "leader_facing"), (17, "second_facing"),
                    (19, "third_facing")),              # 8009B398; after mask at +14
    (0xFE, 0x26): tuple((offset, f"distortion_parameter_{index}")
                       for index, offset in enumerate(range(2, 16, 2), 1)), # 800A484C, FE26 wrapper
    (0xFE, 0x39): ((2, "animation_offset_scale"),),     # 8008D230
    (0xFE, 0x3A): ((2, "party_character"),),             # 8008CED0
    (0xFE, 0x3B): ((2, "party_character"),),             # 8008CE64
    (0xFE, 0x3C): ((2, "mecha_slot"), (4, "animation_id")), # 8008B180
    (0xFE, 0x3F): ((2, "background_red"), (4, "background_green"), (6, "background_blue")), # 8008B0E8
    (0xFE, 0x40): ((2, "line_scroll_buffer"), (4, "line_scroll_offset")), # 80092148; +6 is direct word, low byte stored
    (0xFE, 0x41): ((2, "party_character"),),             # 8009FC48
    (0xFE, 0x42): ((2, "party_character"),),             # 8009FCAC
    (0xFE, 0x47): ((2, "default_turn_rate"),),           # 8008B144
    (0xFE, 0x4A): ((2, "special_animation"),),           # 8008ACE8
    (0xFE, 0x56): ((2, "menu_selection"),),              # 80093930
    (0xFE, 0x58): ((2, "name_record"),),                 # 80093824
    (0xFE, 0x59): ((2, "shop_inventory"),),             # 800939A0
    (0xFE, 0x5A): ((2, "gear_shop_inventory"),),        # 80093A04
    (0xFE, 0x5B): ((2, "model_turn_rate"),),             # 8008B210
    (0xFE, 0x5D): ((2, "sound_id"), (4, "pan"), (6, "volume")), # 8008F6AC -> 800855C8
    (0xFE, 0x5E): ((2, "transparency_mode"),),           # 8008F2D8
    (0xFE, 0x5F): ((3, "lighting_red"), (5, "lighting_green"), (7, "lighting_blue")), # 8008F1C8; +2 mode
    (0xFE, 0x60): ((2, "presentation_resource"), (4, "offset"), (6, "limit"), (8, "mode")), # 8008EC30
    (0xFE, 0x62): ((2, "volume"), (4, "sound_channel")), # 8008F444
    (0xFE, 0x63): ((2, "pan"), (4, "sound_channel")),    # 8008F4A0
    (0xFE, 0x64): ((2, "sound_channel_mask"),),          # 8008F5E4
    (0xFE, 0x65): ((2, "sound_id"), (4, "sound_channel")), # 8008F4FC -> 80085634
    (0xFE, 0x66): ((2, "sound_id"), (4, "pan"), (6, "volume"), (8, "sound_channel")), # 8008F558 -> 800855C8
    (0xFE, 0x67): tuple((offset, f"presentation_parameter_{index}") for index, offset in enumerate(range(2, 20, 2), 1)), # 8008EE14
    (0xFE, 0x6A): ((2, "ordering_table_index"),),       # 8008A604
    (0xFE, 0x6B): ((2, "progress_total"), (4, "party_character")), # 8008A640
    (0xFE, 0x70): ((2, "background_render_mode"),),     # 80089F54
    (0xFE, 0x82): tuple((offset, f"panorama_color_{index}")
                       for index, offset in enumerate(range(2, 26, 2), 1)), # 8008A148
    (0xFE, 0x83): ((2, "boot_mode"),),                   # 80092FB4
    (0xFE, 0x84): ((2, "battle_id"), (6, "return_field"), (8, "map_entry_point")), # 800933F8; +4 is raw
    (0xFE, 0x8A): ((2, "spatial_listener_source"),),    # 80089F18
    (0xFE, 0x8C): ((2, "volume"), (4, "sound_channel"), (6, "duration")), # 8008F3D0
    (0xFE, 0x8D): ((2, "sound_preserve_mask"),),         # 8008F394
    (0xFE, 0x8E): ((2, "horizontal_padding"), (4, "vertical_padding")), # 8008F348
    (0xFE, 0x8F): ((3, "particle_modifier_1"), (5, "particle_modifier_2"), (7, "particle_modifier_3")), # 80088790; +2 is raw
    (0xFE, 0x90): ((2, "particle_bank"), (4, "particle_parameter_1"), (6, "particle_parameter_2"), (8, "particle_parameter_3")), # 80089004
    (0xFE, 0x93): tuple((offset, f"particle_parameter_{index}") for index, offset in enumerate(range(2, 12, 2), 1)), # 80089574
    (0xFE, 0x98): ((2, "spatial_audio_falloff"),),       # 800884CC
    (0xFE, 0x9A): ((4, "lighting_red"), (6, "lighting_green"),
                    (8, "lighting_blue")),             # 8008F0B4; +3 actor
    (0xFE, 0x9B): ((2, "transition_parameter"),),       # 8008EF5C
    (0xFE, 0x9C): ((2, "transition_parameter"),),       # 8008EFA0
    (0xFE, 0x9D): ((2, "transition_parameter"),),       # 8008F070
    (0xFE, 0x9E): ((2, "clip_x"), (4, "clip_y"), (6, "clip_width"), (8, "clip_height")), # 8008EFE4
    (0xFE, 0x9F): ((3, "party_character"),),             # 800883D4; +2 is raw
    (0xFE, 0xA1): ((2, "party_character"), (4, "gear_id")), # 80088360
    (0xFE, 0xA5): ((2, "particle_slot"), (4, "particle_flags"), (6, "particle_angle")), # 80088C1C
    (0xFE, 0xA6): ((2, "motion_key_selector"), (4, "motion_key_value")), # 800888A4
    (0xFE, 0xA7): tuple((offset, f"motion_key_parameter_{index}")
                       for index, offset in enumerate(range(2, 10, 2), 1)), # 800889BC
    (0xFE, 0xAE): ((2, "special_movement_enable"), (4, "animation_id"), (6, "countdown")), # 80096AF4
    (0xFE, 0xB0): ((3, "sound_bank_slot"), (5, "sound_bank_id")), # 8008AACC, mode-dependent
    (0xFE, 0xB2): ((3, "party_member_hp"),),             # 80096D28
    (0xFE, 0xB3): ((3, "party_member_mp"),),             # 80096E20
    (0xFE, 0xB7): ((2, "interaction_override"),),       # 80087E5C
    (0xFE, 0xB8): ((3, "transition_music_id"),),        # 80087DE0; +2 selector
    (0xFE, 0xBD): ((2, "particle_attachment"),),        # 80088B68
    (0xFE, 0xBF): tuple((offset, f"battle_parameter_{index}") for index, offset in enumerate(range(2, 14, 2), 1)), # 80087848
    (0xFE, 0xC2): ((2, "particle_actor"), (4, "particle_modifier_1"),
                    (6, "particle_modifier_2"), (8, "particle_modifier_3")), # 80088674; 10 bytes
    (0xFE, 0xC5): ((2, "model_animation"), (4, "animation_mode")), # 80086F7C
    (0xFE, 0xC6): ((2, "party_character"),),             # 8008BC80
    (0xFE, 0xCE): ((2, "max_mecha_count"),),             # 800A0DC0
    (0xFE, 0xCF): ((2, "return_field"), (4, "map_entry_point")), # 80093888
    (0xFE, 0xD0): ((2, "source_gear"), (4, "destination_gear")), # 8008764C
    (0xFE, 0xD4): tuple((offset, f"overlay_parameter_{index}")
                       for index, offset in enumerate(range(3, 11, 2), 1)), # 80086FD0, long modes
    (0xFE, 0xDB): ((2, "party_character"),),             # 80097200
    (0xFE, 0xDC): ((2, "party_slot"), (4, "column_offset")), # 800873C4
    (0xFE, 0xDD): ((3, "vram_parameter_1"), (5, "vram_parameter_2")), # 800871B0, long modes
    (0xFE, 0xDE): ((2, "party_character"), (4, "record_flags")), # 80087148
    (0xFE, 0xDF): ((2, "display_mode"),),                # 80086E1C
    (0xFE, 0xE1): ((2, "source_gear"), (4, "destination_gear")), # 80087580
}

# FE words read by FUN_800acdb8, not FUN_800acdec: bit 15 is data,
# not an immediate-mode selector. The remaining fields retain their raw bytes.
DIRECT_EXTENDED_WORDS = {
    **{sub: ((2, "actor_flag_mask"),) for sub in range(0x30, 0x38)}, # 8008E148/8008E0DC
    0x40: ((6, "line_scroll_byte"),),                     # 80092148
    0x80: tuple((offset, f"panorama_parameter_{index}") for index, offset in enumerate(range(2, 16, 2), 1)), # 80089FD0
}

# FE direct bytes: actor entries use FUN_8009CDB4, other entries use direct
# byte loads. These fields do not overlap adjacent evaluated words and labels.
DIRECT_EXTENDED_BYTES = {
    0x02: ((2, "actor"),),                      # 80095A7C, called by 80095B3C
    0x07: ((2, "u8"),),                         # 8008D604, flag 0x400 mode
    0x05: ((2, "actor"),), 0x06: ((2, "actor"),), # 80095CC4/80095D6C
    0x17: ((2, "actor"), (3, "actor")),         # 8009AA00
    0x18: ((2, "u8"),),                         # 8008BDD8, party character byte
    0x19: ((2, "u8"),),                         # 8008C334, party character byte
    0x20: ((2, "u8"),),                         # 8009FE4C, party slot byte
    0x1E: ((2, "u8"),),                         # 8009FB98, Gear map mode
    0x25: ((2, "u8"),),                         # 8008D5C8, follow-height mode
    0x45: ((2, "u8"),),                         # 8009A0FC, animation override
    0x46: ((2, "u8"),),                         # 8008AE5C, rotation authority
    0x4C: ((2, "u8"),),                         # 8008A93C wrapper
    0x4D: ((2, "u8"),),                         # 8008A93C, forced animation
    0x86: ((2, "u8"),),                         # 80089F94, video transition
    0x97: ((2, "u8"),),                         # 80089AE4, particle effect ID
    0x99: ((2, "u8"),),                         # 8008848C, menu argument
    0xA3: ((2, "u8"),),                         # 800881E8, VRAM backup/restore mode
    0x89: ((11, "actor"),),                     # 80089DCC, trailing actor association
    0x8F: ((2, "actor"),),                      # 80088790
    0x9A: ((3, "actor"),),                      # 8008F0B4
    0xAA: ((2, "actor"),),                      # 8008DB2C
    0xB6: ((2, "actor"),),                      # 80087E98
    0xC4: ((2, "actor"),),                      # 8009DF78
    0xCA: ((2, "u8"),),                         # 800A0EE8, release mode
    0xD8: ((2, "u8"),),                         # 80087A40, lighting bypass
    0xD9: ((2, "u8"),),                         # 80087A7C, direction table
    0xE0: ((2, "u8"),),                         # 80086DE0, pause state
    **{sub: ((4, "actor"),) for sub in range(0x34, 0x38)}, # 8008E298..8008E394
}

# These handlers pass one trailing control byte to successive masked-word
# readers (8009CF78, 8009CFBC, 8009D000, 8009D044, 8009D088).
MASKED_EXTENDED_WORDS = {
    0x1D: ((2, "translation_x", 8, 0x80), (4, "translation_z", 8, 0x40),
           (6, "translation_y", 8, 0x20)),              # 800984EC
    0x23: tuple((at, f"party_position_{index}", 14, 0x80 >> (index - 1))
                for index, at in enumerate(range(2, 14, 2), 1)), # 8009B398
    0x3D: tuple((at, "matrix_row" if index == 1 else f"matrix_value_{index - 1}", 10,
                  0x80 >> (index - 1)) for index, at in enumerate(range(2, 10, 2), 1)), # 8008AEC8
    0x3E: tuple((at, "matrix_row" if index == 1 else f"matrix_value_{index - 1}", 10,
                  0x80 >> (index - 1)) for index, at in enumerate(range(2, 10, 2), 1)), # 8008AFD8
    0x48: ((2, "projection_yaw", 8, 0x80), (4, "projection_dip", 8, 0x40),
           (6, "projection_depth", 8, 0x20)),           # 8008B518
    0x68: ((2, "destination_x", 6, 0x80), (4, "destination_z", 6, 0x40)), # 80092C20
    0x6E: ((2, "scene_y_angle", 4, 0x80),),              # 8008FABC
    0x6F: ((2, "model_rotation_x", 8, 0x80), (4, "model_rotation_z", 8, 0x40),
           (6, "model_rotation_y", 8, 0x20)),           # 8008B45C
    0x72: ((4, "from_angle", 10, 0x40), (6, "to_angle", 10, 0x20),
           (8, "interpolation_step", 10, 0x10)),        # 800988B8; +2 output
    0x73: ((4, "point_1_x", 12, 0x40), (6, "point_1_z", 12, 0x20),
           (8, "point_2_x", 12, 0x10), (10, "point_2_z", 12, 0x08)), # 8009861C
    0x76: ((4, "point_1_x", 16, 0x40), (6, "point_1_z", 16, 0x20),
           (8, "point_1_y", 16, 0x20), (10, "point_2_x", 16, 0x10),
           (12, "point_2_z", 16, 0x08), (14, "point_2_y", 16, 0x08)), # 80098738; shared bits
    0x81: ((2, "orientation_x", 8, 0x80), (4, "orientation_z", 8, 0x40),
           (6, "orientation_y", 8, 0x20)),              # 8008A08C
    0x88: tuple((at, f"light_parameter_{index}", 18, 0x80 >> (index - 1))
                for index, at in enumerate(range(2, 18, 2), 1)), # 80089BF0
    0x89: tuple((at, "light_index" if index == 1 else f"light_position_{index - 1}", 10,
                  0x80 >> (index - 1)) for index, at in enumerate(range(2, 10, 2), 1)), # 80089DCC
    0x91: tuple((at, f"particle_position_{index}", 14, 0x80 >> (index - 1))
                for index, at in enumerate(range(2, 14, 2), 1)), # 80089174
    0x92: tuple((at, f"particle_physics_{index}", 14, 0x80 >> (index - 1))
                for index, at in enumerate(range(2, 14, 2), 1)), # 80089374
    0x94: tuple((at, f"particle_scale_{index}", 10, 0x80 >> (index - 1))
                for index, at in enumerate(range(2, 10, 2), 1)), # 800896D4
    0x95: tuple((at, f"particle_color_{index}", 14, 0x80 >> (index - 1))
                for index, at in enumerate(range(2, 14, 2), 1)), # 80089880
    0xA0: tuple((offset, f"presentation_parameter_{index}", 12, 0x80 >> (index - 1))
                for index, offset in enumerate(range(2, 12, 2), 1)), # 8008EA58
    0xAB: ((2, "gear_hp_delta", 4, 0x80),),             # 8008DC74
    0xAC: ((2, "gear_hp_delta", 4, 0x80),),             # 8008DD6C
    0xAF: tuple((at, f"joint_input_{index}", 12, 0x80 >> (index - 1))
                for index, at in enumerate(range(2, 12, 2), 1)), # 8008800C, +13/+15/+17 outputs
    0xBA: tuple((at, f"world_position_{index}", 10, 0x80 >> (index - 1))
                for index, at in enumerate(range(2, 10, 2), 1)), # 80087C34
    0xBC: ((2, "world_vehicle_state", 4, 0x80),),      # 80087D80
    0xC8: tuple((at, f"particle_direction_{index}", 18, 0x80 >> (index - 1))
                for index, at in enumerate(range(2, 18, 2), 1)), # 80088D38(0)
    0xC9: tuple((at, f"particle_direction_{index + 4}", 18, 0x80 >> (index - 1))
                for index, at in enumerate(range(2, 18, 2), 1)), # 80088D38(4)
}

# All these handlers read the destination with 800ACDB8(1) and the source
# through 8009CFBC(3, control). Only source-control bit 0x40 is observed.
# Verified: 8009D9A4, 8009D890, 8009D804, 8009D644, 8009D408, 8009D5B8,
# 8009D52C, 8009D4A0, 8009D6D8 and 8009D768.
MASKED_VARIABLE_OPCODES = frozenset({0x35, 0x38, 0x39, 0x3A, 0x3B, 0x3E, 0x3F, 0x40, 0xDE, 0xDF})

# Primary-dispatch operand readers in field-overlay.bin. Each row is
# (kind, byte offset, role, control offset, immediate bit). The 800ACDEC
# reader uses bit 15 of the *word*; 8009CF78..8009D154 instead use successive
# bits 7..0 of the separate trailing control byte. Neither marker is an
# additional semantic argument. Direct words/bytes are kept distinct from
# evaluated inputs. Standalone compilation canonicalizes unused control bits.
PRIMARY_INPUT_SCHEMAS: dict[int, tuple[tuple[str, int, str, int, int], ...]] = {}


def _primary_inputs(opcodes: str, kind: str, offsets: tuple[int, ...], roles: tuple[str, ...] = (),
                    control: int = 0, masks: tuple[int, ...] = ()) -> None:
    for token in opcodes.split():
        opcode = int(token, 16)
        PRIMARY_INPUT_SCHEMAS[opcode] = tuple(
            (kind, offset, roles[index] if index < len(roles) else f"value_{index + 1}",
             control, masks[index] if masks else 0)
            for index, offset in enumerate(offsets)
        )


# 800ACDEC call sites, including wrappers shared by 10/11, 72/75 and
# movement handlers 44..55. See docs/xenogears/field/05-primary-opcodes.md
# for dispatch identities and descriptions.
_primary_inputs("0B 16 20 21 26 34 5C 69 6A 6B 6C 71 72 74 75 7F 87 8C 8D 8F 90 93 97 9A A1 A6 B3 B4 C7 C8 D6 D7 D8 D9 EF", "v80", (1,))
_primary_inputs("44", "v80", (1, 3), ("angle", "step_limit"))
_primary_inputs("45", "v80", (1, 5), ("angle", "step_limit"))
_primary_inputs("98", "v80", (1, 3), ("destination_field", "entry_point"))
_primary_inputs("53", "v80", (2,), ("step_limit",))
_primary_inputs("55", "v80", (5,), ("step_limit",))
_primary_inputs("4B 4F", "v80", (6,), ("step_limit",))
_primary_inputs("4D 51", "v80", (8,), ("step_limit",))
_primary_inputs("11", "v80", (11,), ("step_limit",))
_primary_inputs("73", "v80", (2, 4, 6), ("particle_parameter_1", "particle_parameter_2", "particle_parameter_3"))
_primary_inputs("94 9B B5 B6 F7", "v80", (1, 3))
_primary_inputs("A0", "v80", (1, 3, 5),
                ("camera_sector", "camera_dip", "projection_screen_distance"))
_primary_inputs("D0", "v80", (1, 3, 5, 7, 9),
                ("window_x", "window_y", "window_width_three_pixel_units", "window_height", "window_flags"))
_primary_inputs("DB", "v80", (1, 3))
_primary_inputs("F1", "v80", (1, 3, 5, 7, 9),
                ("semi_transparency_abr_mode", "target_red", "target_green", "target_blue", "duration"))
_primary_inputs("E5", "v80", (1, 3, 5, 7, 9, 11, 13, 15),
                ("depth_cue_base_red", "depth_cue_base_green", "depth_cue_base_blue",
                 "fog_far_red", "fog_far_green", "fog_far_blue", "fog_near_distance", "fog_far_distance"))
_primary_inputs("E7", "v80", (1, 3, 5),
                ("background_clear_red", "background_clear_green", "background_clear_blue"))
_primary_inputs("E8 E9", "v80", (1, 3, 5),
                ("movement_step", "duration", "turn_angle_or_axis_mode"))
_primary_inputs("F2", "v80", (1, 3, 5, 7), ("shake_x", "shake_z", "shake_y", "duration"))
_primary_inputs("F7", "v80", (1, 3), ("encounter_timer_range", "active_timer_count"))
_primary_inputs("FA", "v80", (3,), ("rotation_delta",))
_primary_inputs("67 68", "v80", (2,), ("direction",))
_primary_inputs("12", "v80", (1, 3, 5, 7),
                ("destination_field", "entry_point", "transition_mode", "transition_duration"))
_primary_inputs("A6", "v80", (1,), ("triplet_skip_count",))

# Mask readers: 8009CF78 (0x80), 8009CFBC (0x40), 8009D000
# (0x20), 8009D044 (0x10), 8009D088 (0x08), 8009D0CC (0x04),
# 8009D110 (0x02), 8009D154 (0x01).
for _ops, _offsets, _control, _roles in (
    ("10 11", (2, 4, 6), 8, ("x", "z", "y")),
    ("4A 4B 4E 4F", (1, 3), 5, ("x", "z")),
    ("45", (3,), 7, ("height_offset",)),
    ("17", (1, 3, 5, 7, 9, 11, 13, 15), 17, ("vertex_1_x", "vertex_1_z", "vertex_2_x", "vertex_2_z", "vertex_3_x", "vertex_3_z", "vertex_4_x", "vertex_4_z")),
    ("19", (1, 3), 5, ("x", "z")),
    ("1B", (1, 3), 6, ("x", "z")),
    ("1C", (1,), 3, ("actor_y",)),
    ("4C 4D 50 51", (1, 3, 6), 5, ("x", "z", "y")),
    ("56", (1, 3, 5, 7), 9, ("return_field", "start_position", "camera_yaw", "world_map_mode")),
    ("57", (2, 4, 6, 8), 10, ("x", "z", "y", "duration")),
    ("61 63 65 A3", (1, 3, 5), 7, ("x", "z", "y")),
    ("6D 6E CA", (3, 5), 7, ("angle", "magnitude")),
    ("A4 7B 7C 7D 7E", (1,), 3, ("value",)),
    ("DD", (1, 3), 5, ("blend_mode", "blend_value")),
    ("E0", (2, 4), 6, ("blend_mode", "blend_value")),
    ("E1", (1, 3, 5, 7, 9, 11), 13, ("source_x", "source_y", "width", "height", "destination_x", "destination_y")),
    ("EB", (1, 3, 5, 7, 9, 11), 13, ("origin_x", "origin_z", "origin_y", "yaw", "pitch", "radius")),
    ("EC", (2, 4, 6), 8, ("yaw", "pitch", "radius")),
):
    _masks = tuple(0x80 >> index for index in range(len(_offsets)))
    for _token in _ops.split():
        _op = int(_token, 16)
        PRIMARY_INPUT_SCHEMAS[_op] = tuple(
            ("masked", offset, _roles[index], _control, _masks[index])
            for index, offset in enumerate(_offsets)
        ) + PRIMARY_INPUT_SCHEMAS.get(_op, ())

# Mixed direct fields and evaluated values; these positions are proven by
# direct byte/halfword loads in the indicated handler, not by the name alone.
_primary_inputs("1D", "s16", (1, 3, 5), ("x", "z", "y"))
_primary_inputs("E6", "s16", (1, 3, 5, 7), ("limit_x", "limit_z", "extent_x", "extent_z"))
_primary_inputs("31 32 E2 E3", "u16", (1,), ("input_mask",))
_primary_inputs("8E", "u32", (1,), ("gold_threshold",))
_primary_inputs("18", "u8", (1, 2, 3, 4),
                ("collision_half_width_x", "collision_half_width_z", "collision_height", "solid_contact_range"))
_primary_inputs("CF", "u8", (1, 2, 3, 4),
                ("window_x_half_pixels", "window_y", "window_width_three_pixel_units", "window_height"))
_primary_inputs("03 D2 D3 F5", "u16", (1,), ("dialogue_id",))
_primary_inputs("A9", "high4", (1,), ("first_choice",))
PRIMARY_INPUT_SCHEMAS[0xA9] += (("low4", 1, "last_choice", 0, 0),)
_primary_inputs("9D", "v80", (1,), ("scene_scale",))
_primary_inputs("F8", "u8", (1,), ("flag_quarter_and_action",))
PRIMARY_INPUT_SCHEMAS[0xF8] += (("u16", 2, "flag_mask", 0, 0),)
PRIMARY_INPUT_SCHEMAS[0x89] = (("actor", 1, "actor", 0, 0), ("v80", 2, "distance", 0, 0))
for _op in (0x84, 0x85, 0x86, 0x8B):
    PRIMARY_INPUT_SCHEMAS[_op] = (("v80", 1, "scenario_flags" if _op != 0x8B else "inventory_object", 0, 0),)
PRIMARY_INPUT_SCHEMAS[0xDA] = tuple(
    ("u16", offset, role, 0, 0) for offset, role in zip(
        range(1, 15, 2), ("source_x", "source_y", "source_width",
                             "source_total_height", "row_count", "destination_x", "destination_y"))
) + (("u8", 15, "initial_scroll_byte", 0, 0),)
PRIMARY_INPUT_SCHEMAS[0xED] = (("u8", 1, "camera_vector_selector", 0, 0),)
PRIMARY_INPUT_SCHEMAS[0x8E] += (("label", 5, "target", 0, 0),)
for _op in (0x84, 0x85, 0x86, 0x8B):
    PRIMARY_INPUT_SCHEMAS[_op] += (("label", 3, "target", 0, 0),)
PRIMARY_INPUT_SCHEMAS[0x89] += (("label", 4, "target", 0, 0),)
PRIMARY_INPUT_SCHEMAS[0x8A] = (("actor", 1, "actor", 0, 0), ("label", 2, "target", 0, 0))
PRIMARY_INPUT_SCHEMAS[0x91] = (("u8", 1, "party_character_id", 0, 0), ("label", 2, "target", 0, 0))
PRIMARY_INPUT_SCHEMAS[0xB9] = (("u8", 1, "availability_bit_index", 0, 0), ("label", 2, "target", 0, 0))
PRIMARY_INPUT_SCHEMAS[0xFB] = (("packedvar", 1, "indexed_flags", 0, 0),
                                ("bit", 1, "bit_index", 0, 0),
                                ("label", 3, "target", 0, 0))
for _op in (0xE2, 0xE3):
    PRIMARY_INPUT_SCHEMAS[_op] += (("label", 3, "target", 0, 0),)
PRIMARY_INPUT_SCHEMAS[0xFA] = (("actor", 1, "actor", 0, 0),
                                ("u8", 2, "rotation_axis", 0, 0)) + PRIMARY_INPUT_SCHEMAS[0xFA]
for _op in (0x52, 0x53, 0x54, 0x55, 0x62, 0x66, 0x67, 0x68, 0x6F, 0x70, 0xC4, 0xC5, 0xD4, 0xE0, 0xFC):
    PRIMARY_INPUT_SCHEMAS[_op] = (("actor", 1, "actor", 0, 0),) + PRIMARY_INPUT_SCHEMAS.get(_op, ())
for _op in (0x54, 0x55):
    PRIMARY_INPUT_SCHEMAS[_op] += (("masked", 2, "height_offset", 4, 0x80),)
for _op in (0x47, 0xEA):
    # 80092894 consumes two evaluated words; +1 is skipped and retained only
    # by lossless XGA only, not displayed as an independent source argument.
    PRIMARY_INPUT_SCHEMAS[_op] = (("v80", 2, "destination_field", 0, 0),
                                  ("v80", 4, "entry_point", 0, 0))
for _op in (0x80, 0x81, 0x82, 0x83):
    PRIMARY_INPUT_SCHEMAS[_op] = (("u8", 1, "walkmesh_material", 0, 0),
                                  ("u8", 2, "byte_lane", 0, 0)) + (
                                      () if _op == 0x82 else (("v80", 3, "material_value", 0, 0),))
PRIMARY_INPUT_SCHEMAS[0x78] = (("u8", 1, "archive", 0, 0),
                                ("u16", 2, "file_id", 0, 0))
PRIMARY_INPUT_SCHEMAS[0x9D] += (("u8", 3, "duration", 0, 0),)
PRIMARY_INPUT_SCHEMAS[0xAC] = (("u8", 1, "camera_movement_mode", 0, 0),
                                ("v80", 2, "duration", 0, 0))
PRIMARY_INPUT_SCHEMAS[0x1B] += (("u8", 5, "walkmesh", 0, 0),)
PRIMARY_INPUT_SCHEMAS[0xEC] = (("u8", 1, "camera_orbit_selector", 0, 0),) + PRIMARY_INPUT_SCHEMAS[0xEC]
_primary_inputs("D5", "u16", (1,), ("controller_button_mask",))
_primary_inputs("F4 F6", "u8", (1,), ("mode",))
_primary_inputs("58", "v80", (1,), ("axis_rotation",))
PRIMARY_INPUT_SCHEMAS[0x58] += (("u8", 3, "axis", 0, 0),)
_primary_inputs("BD BE BF C0 C1 C2", "v80", (1,), ("rotation_delta",))
_primary_inputs("1A 1F 27 28 29 2C 5D 5F 95 A2 AA B2 BA BB", "u8", (1,))
_primary_inputs("F9", "actor", (1,), ("parent_actor",))
for _op, _role in ((0x1A, "walkmesh"), (0x1F, "packed_actor_flags"),
                   (0x27, "actor"), (0x28, "actor"), (0x29, "actor"),
                   (0x2C, "animation_id"), (0x5D, "animation_id"),
                   (0x5F, "cardinal_direction"), (0x95, "timer_control"),
                   (0xA2, "camera_animation_mask"), (0xAA, "cardinal_direction"),
                   (0xB2, "camera_animation_mask"), (0xBA, "character_id"),
                   (0xBB, "character_id")):
    PRIMARY_INPUT_SCHEMAS[_op] = (("u8", 1, _role, 0, 0),)
for _op in (0x27, 0x28, 0x29):
    PRIMARY_INPUT_SCHEMAS[_op] = (("actor", 1, "actor", 0, 0),)
for _op, _roles in {
    0x0B: ("field_graphic",), 0x16: ("character_selector",),
    0x20: ("actor_flags",), 0x21: ("movement_speed",),
    0x26: ("delay_duration",), 0x34: ("inventory_object",),
    0x5C: ("party_slot",), 0x69: ("rotation_sector",),
    0x6A: ("camera_relative_direction",), 0x6B: ("turn_sector",),
    0x6C: ("turn_sector",), 0x71: ("battle_configuration",),
    0x72: ("music_id",), 0x74: ("sound_effect_id",),
    0x75: ("music_id",), 0x7F: ("motion_angle_offset",),
    0x87: ("scenario_flags",), 0x8C: ("inventory_object",),
    0x8D: ("inventory_object",), 0x8F: ("gold_delta",),
    0x90: ("gold_delta",), 0x93: ("mecha_graphic",),
    0x94: ("timer_high_byte", "timer_low_byte"),
    0x97: ("camera_sector_mask",), 0x9A: ("camera_reacquisition_duration",),
    0x9B: ("target_smoothing_divisor", "eye_smoothing_divisor"),
    0xA1: ("blocked_camera_sector_mask",),
    0xB3: ("fade_duration",), 0xB4: ("fade_duration",),
    0xB5: ("camera_direction", "duration"),
    0xB6: ("projection_depth", "duration"),
    0xC7: ("dolly_duration",), 0xC8: ("dolly_duration",),
    0xD6: ("dialogue_animation_mode",),
    0xD7: ("rotation_angle",), 0xD8: ("rotation_angle",),
    0xD9: ("rotation_angle",), 0xDB: ("deformation_slot", "strength"),
    0xEF: ("camera_movement_mask",),
}.items():
    PRIMARY_INPUT_SCHEMAS[_op] = tuple((kind, offset, _roles[index], control, mask)
                                       for index, (kind, offset, _, control, mask)
                                       in enumerate(PRIMARY_INPUT_SCHEMAS[_op]))
_primary_inputs("EE", "u8", (1, 2),
                ("source_camera_vector_selector", "destination_camera_vector_selector"))
for _op in (0xD4, 0xFC):
    PRIMARY_INPUT_SCHEMAS[_op] += (("u16", 2, "dialogue_id", 0, 0),
                                    ("u8", 4, "dialogue_control", 0, 0))
for _op in (0x7B, 0x7C, 0x7D, 0x7E):
    PRIMARY_INPUT_SCHEMAS[_op] = (("masked", 1,
                                  "party_hp_amount" if _op in {0x7B, 0x7E} else "party_mp_amount",
                                  3, 0x80), ("low2", 3, "party_member_mask_index", 0, 0))
PRIMARY_INPUT_SCHEMAS[0xA4] = (("masked", 1, "camera_dip", 3, 0x80),
                                ("low7", 3, "duration", 0, 0))
for _op in (0x03, 0xD2, 0xD3, 0xF5):
    PRIMARY_INPUT_SCHEMAS[_op] += (("u8", 3, "dialogue_control", 0, 0),)

# Their first word is an output destination; the two inputs use reader slots
# two and three, not one and two (8009A6AC/8009A768/8009A824).
for _op in (0x6D, 0x6E, 0xCA):
    PRIMARY_INPUT_SCHEMAS[_op] = (("masked", 3, "angle", 7, 0x40),
                                ("masked", 5, "magnitude", 7, 0x20))



def integer(text: str) -> int:
    return int(text, 16 if text.lower().lstrip("-").startswith("0x") else 10)


def checked(value: int, low: int, high: int, description: str) -> int:
    if not low <= value <= high:
        raise ValueError(f"{description} must be in {low}..{high}, got {value}")
    return value


def normalized(text: str) -> str:
    return re.sub(r"\s+", "", text)


def split_instruction_annotations(statement: str) -> tuple[str, bytes | None, dict[int, str]]:
    """Accept annotations from older XGS versions (not generated anymore).

    These are part of the source, never inferred from trace comments. A hint
    seeds invisible bits only: encode_statement still encodes every argument.
    """
    match = re.fullmatch(r'(.*?)\s*@encoding\("([0-9A-Fa-f\s]+)"\)(\s*@layout\([^)]*\))?;', statement.strip())
    if match:
        body, seed, layout = match[1], bytes.fromhex(match[2]), match[3]
    else:
        match = re.fullmatch(r'(.*?)\s*(@layout\([^)]*\));', statement.strip())
        if not match:
            return statement, None, {}
        body, seed, layout = match[1], None, match[2]
    dependencies = {}
    if layout:
        for item in layout.strip()[8:-1].split(","):
            pair = re.fullmatch(r"\s*(-?\d+)\s*=\s*([A-Za-z_][A-Za-z_0-9]*)\s*", item)
            if pair is None or int(pair[1]) in dependencies:
                raise ValueError(f"invalid or duplicate layout dependency: {item}")
            dependencies[int(pair[1])] = pair[2]
    return body.rstrip() + ";", seed, dependencies


def instruction_dependencies(instruction) -> tuple[int, ...]:
    """Bytes whose physical relationship is observed by the VM handler.

    Include the entire connecting span: preserving only the final byte would
    allow a linker to insert an executable jump inside the observed window.
    Offsets are relative to the opcode, including the FE prefix.
    """
    op, sub, raw = instruction.opcode, instruction.subopcode, instruction.raw
    if raw == b"\xfe":
        return (1,)
    if sub == 0x18:
        return (3, 4, 5)  # staging resumes at +3, duplicate path at +5
    if sub == 0x6C:
        return (2,)
    if sub == 0x5C and raw[2] == 1:
        return tuple(range(3, 8))
    if sub == 0x5C and raw[2] > 2:
        # Unsupported subcommands return with IP still pointing at the FE
        # subopcode. Primary 5C then reads a word spanning mode and next byte.
        return (1, 2, 3)
    if sub == 0x77 and raw[2] == 0:
        return tuple(range(3, 15))
    if sub == 0xD7:
        return tuple(range(7, 11))
    if sub is None and op == 0x57 and len(raw) == 2 and raw[1] != 15:
        # On completion the handler changes IP to IP-11 before its readers.
        return tuple(range(-11, 0))
    if sub is None and op in {0x10, 0x11}:
        offsets = list(range(-9, 0)) if raw[1] != 0 else []
        if op == 0x11 and raw[1] == 0:
            offsets.extend(range(len(raw), 13))
        return tuple(offsets)
    return ()


def equivalent_instruction_encoding(left: bytes | None, right: bytes) -> bool:
    """Allow only independently verified, behavior-neutral encoding differences.

    The masked variable family reads its destination directly and passes its
    control byte to 8009CFBC. That helper tests only bit 0x40; every other bit
    is ignored. FE 1C and FE 0F/11 likewise test only their documented mask
    bits. Opcode, operand values and immediate/variable mode must match.
    """
    if left == right:
        return True
    if left is not None and len(left) == len(right):
        # Verified skips: these bytes advance with the VM instruction but are
        # not read by the corresponding handler. Treat only those positions
        # as equivalent, never an adjacent instruction's bytes.
        unused = {
            (0x06, None): {3, 4},
            (0xDA, None): {16},
            (0x47, None): {1},
            (0xEA, None): {1},
            (0xFE, 0x84): {4, 5},
            (0xFE, 0x77): {3, 4},
            (0xFE, 0xBD): {4, 5, 6, 7},
            (0xFE, 0xD2): {2, 3},
        }
        key = (left[0], left[1] if left[0] == 0xFE and len(left) > 1 else None)
        if (key in unused and left[:2 if key[1] is not None else 1] == right[:2 if key[1] is not None else 1]
                and all(a == b for index, (a, b) in enumerate(zip(left, right))
                        if index not in unused[key])):
            return True
    # FE 1C passes the final byte only to the three operand readers. They
    # inspect bits 7, 6 and 5 respectively; the remaining five are unused.
    if (left is not None and len(left) == len(right) == 9
            and left[:8] == right[:8] and left[:2] == b"\xfe\x1c"
            and (left[8] & 0xE0) == (right[8] & 0xE0)):
        return True
    if (left is not None and len(left) == len(right) == 7
            and left[:6] == right[:6] and left[:2] in {b"\xfe\x0f", b"\xfe\x11"}
            and (left[6] & 0xC0) == (right[6] & 0xC0)):
        return True
    return (
        left is not None
        and len(left) == len(right) == 6
        and left[0] == right[0]
        and left[0] in MASKED_VARIABLE_OPCODES
        and left[:5] == right[:5]
        and (left[5] & 0x40) == (right[5] & 0x40)
    )


@lru_cache(maxsize=2048)
def form_pattern(template: str, count: int) -> re.Pattern:
    pattern = re.escape(normalized(template))
    for index in range(count):
        pattern = pattern.replace(re.escape("{" + str(index) + "}"), f"(?P<f{index}>.+?)")
    return re.compile(pattern)


@dataclass(frozen=True)
class Operand:
    kind: str
    offset: int
    control: int = 0
    mask: int = 0

    def clear_value(self, raw: bytearray) -> None:
        """Discard seed operand bits before encoding; retain only non-operands."""
        if self.kind in {"fixedvar", "relative"}:
            return
        if self.kind == "bits":
            raw[self.offset] &= ~self.mask
            return
        byte_masks = {"routine": 0x1F, "priority": 0xE0, "low2": 3,
                      "low7": 0x7F, "low4": 15, "high4": 0xF0}
        if self.kind in byte_masks:
            raw[self.offset] &= ~byte_masks[self.kind]
        elif self.kind == "packedvar":
            raw[self.offset] &= 15
            raw[self.offset + 1] = 0
        elif self.kind == "bit":
            raw[self.offset] &= 0xF0
        else:
            width = 1 if self.kind in {"u8", "actor"} else 4 if self.kind == "u32" else 2
            raw[self.offset:self.offset + width] = bytes(width)
        if self.kind == "masked":
            raw[self.control] &= ~self.mask

    def read(self, raw: bytes, symbols: dict) -> str:
        from decompile_field_scripts import _actor, _variable

        if self.kind == "fixedvar":
            return _variable(self.offset, symbols)
        if self.kind == "relative":
            return f"L_{self.control + self.offset:04X}"
        if self.kind == "bits":
            return str(raw[self.offset] & self.mask)
        value = (raw[self.offset] if self.kind in {"u8", "actor", "routine", "priority", "low2", "low7", "low4", "high4"}
                 else struct.unpack_from("<I" if self.kind == "u32" else "<H", raw, self.offset)[0])
        if self.kind == "low4":
            return str(value & 15)
        if self.kind == "high4":
            return str(value >> 4)
        if self.kind == "low2":
            return str(value & 3)
        if self.kind == "low7":
            return str(value & 0x7F)
        if self.kind == "label":
            return f"L_{value:04X}"
        if self.kind == "actor":
            return _actor(value)
        if self.kind == "routine":
            return str(value & 31)
        if self.kind == "priority":
            return str(value >> 5)
        if self.kind == "packedvar":
            return _variable(value >> 4, symbols)
        if self.kind == "bit":
            return str(value & 15)
        if self.kind == "var":
            return _variable(value, symbols)
        if self.kind == "v80":
            return str(value & 0x7FFF) if value & 0x8000 else _variable(value, symbols)
        if self.kind == "masked" and not raw[self.control] & self.mask:
            return _variable(value, symbols)
        if self.kind in {"s16", "masked"} and value & 0x8000:
            value -= 0x10000
        return str(value)

    def write(self, raw: bytearray, text: str, variables: dict[str, int], labels: dict[str, int]) -> None:
        kind = self.kind
        if kind == "bits":
            value = checked(integer(text), 0, 255, "reserved flags")
            if value & ~self.mask:
                raise ValueError("reserved flags overlap semantic operand bits")
            raw[self.offset] = (raw[self.offset] & ~self.mask) | value
            return
        if kind == "relative":
            if text not in labels:
                raise ValueError(f"unresolved script-byte dependency: {text}")
            return  # This is a placement constraint, not an encoded address.
        if kind == "fixedvar":
            if variables.get(text) != self.offset:
                raise ValueError(f"implicit output must name the declared VM slot at 0x{self.offset:04X}")
            return
        if kind == "label":
            if text not in labels:
                raise ValueError(f"unresolved label: {text}")
            value = checked(labels[text], 0, 65535, "target offset")
        elif kind == "actor":
            special = {"self": 0xFB, "party[0]": 0xFF, "party[1]": 0xFE, "party[2]": 0xFD}
            if text in special:
                value = special[text]
            else:
                match = re.fullmatch(r"actor\[(\d+)\]", text)
                if not match:
                    raise ValueError(f"invalid actor selector: {text}")
                value = checked(int(match[1]), 0, 255, "actor selector")
        elif kind in {"var", "packedvar"} or (kind in {"v80", "masked"} and text in variables):
            if text not in variables:
                raise ValueError(f"unresolved state symbol: {text}")
            value = checked(variables[text], 0, 0xFFFF, "variable offset")
            if kind == "v80":
                checked(value, 0, 0x7FFF, "v80 variable offset")
            if kind == "masked":
                raw[self.control] &= ~self.mask
            if kind == "packedvar":
                checked(value, 0, 0xFFF, "packed variable offset")
                value = (value << 4) | (raw[self.offset] & 15)
        else:
            try:
                value = integer(text)
            except ValueError as error:
                raise ValueError(f"expected integer or declared state symbol, got {text}") from error
            if kind == "v80":
                value = checked(value, 0, 32767, "v80 immediate") | 0x8000
            elif kind in {"masked", "s16"}:
                value = checked(value, -32768, 32767, "signed immediate") & 0xFFFF
                if kind == "masked":
                    raw[self.control] |= self.mask
            elif kind == "bit":
                value = (struct.unpack_from("<H", raw, self.offset)[0] & 0xFFF0) | checked(value, 0, 15, "bit index")
            elif kind == "routine":
                value = (raw[self.offset] & 0xE0) | checked(value, 0, 31, "routine ID")
            elif kind == "priority":
                value = (raw[self.offset] & 31) | (checked(value, 0, 7, "priority") << 5)
            elif kind in {"low2", "low7"}:
                mask = 3 if kind == "low2" else 0x7F
                value = (raw[self.offset] & ~mask) | checked(value, 0, mask, kind)
            elif kind == "low4":
                value = (raw[self.offset] & 0xF0) | checked(value, 0, 15, kind)
            elif kind == "high4":
                value = (raw[self.offset] & 0x0F) | (checked(value, 0, 15, kind) << 4)
            else:
                checked(value, 0, 255 if kind == "u8" else 0xFFFFFFFF if kind == "u32" else 65535, kind)
        if kind in {"u8", "actor", "routine", "priority", "low2", "low7", "low4", "high4"}:
            raw[self.offset] = value
        else:
            struct.pack_into("<I" if kind == "u32" else "<H", raw, self.offset, value)


@dataclass
class InstructionForm:
    template: str = ""
    operands: list[Operand] = field(default_factory=list)

    def operand(self, kind: str, offset: int, control: int = 0, mask: int = 0) -> str:
        self.operands.append(Operand(kind, offset, control, mask))
        return "{" + str(len(self.operands) - 1) + "}"

    def render(self, raw: bytes, symbols: dict) -> str:
        return self.template.format(*(operand.read(raw, symbols) for operand in self.operands))

    def encode(self, statement: str, seed: bytes, variables: dict, labels: dict) -> bytes | None:
        match = form_pattern(self.template, len(self.operands)).fullmatch(normalized(statement))
        if match is None:
            return None
        raw = bytearray(seed)
        for operand in self.operands:
            operand.clear_value(raw)
        for index, operand in enumerate(self.operands):
            operand.write(raw, match[f"f{index}"], variables, labels)
        return bytes(raw)


def instruction_form(instruction, symbols: dict, *, lossless=False) -> InstructionForm:
    # Imports are local so that decoding, symbol discovery and this codec can
    # share the existing dispatch inventory without an import cycle.
    from decompile_field_scripts import (
        CONDITION_OPERATORS, CONDITIONAL_OUTPUT_VARIABLES, IMPLICIT_OUTPUT_VARIABLES,
        _encoded_outputs, _operation_name, target_operand_offset,
    )

    raw, op, sub = instruction.raw, instruction.opcode, instruction.subopcode
    form = InstructionForm()
    arg = form.operand
    name = _operation_name(instruction)
    start = 2 if sub is not None else 1
    key = (op, sub)
    outputs = _encoded_outputs(instruction)
    implicit = IMPLICIT_OUTPUT_VARIABLES.get(key, ())

    def generic() -> str:
        target = target_operand_offset(op, sub, len(raw))
        arguments = []
        for i in range(start, len(raw)):
            if i == target:
                arguments.append(arg("label", i))
            elif target is None or i != target + 1:
                arguments.append(arg("u8", i))
        return ", ".join(arguments)

    def primary_arguments() -> str:
        fields = PRIMARY_INPUT_SCHEMAS.get(op, ())
        if op == 0x11:
            fields = (("u8", 1, "phase", 0, 0),) + (tuple(field for field in fields if field[1] < 9) if raw[1] == 0 else ())
        if op == 0x10 and len(raw) == 2:
            fields = (("u8", 1, "movement_mode", 0, 0),)
        elif op == 0x73 and len(raw) == 2:
            fields = (("u8", 1, "particle_subcommand", 0, 0),)
        elif op == 0x73:
            fields = (("u8", 1, "particle_subcommand", 0, 0),) + fields
            if len(raw) == 8:
                # 80086C34 evaluates +2 and +6 but discards their results;
                # only the evaluated preset at +4 affects initialization.
                fields = (fields[0], next(field for field in fields if field[1] == 4))
        elif op == 0x57:
            fields = (("u8", 1, "ballistic_mode", 0, 0),) + (fields if len(raw) == 11 else ())
        return ", ".join(arg(kind, offset, control, mask)
                         for kind, offset, _role, control, mask in sorted(fields, key=lambda field: field[1]))

    if sub is not None and len(raw) == 1:
        # These dispatch slots advance over FE only. Their identity comes from
        # the following byte, which belongs to a different instruction/region.
        form.template = "flow.extended_prefix();"
    elif sub == 0x18:
        form.template = (f"{name}({arg('u8', 2)}) staged goto "
                         f"{arg('relative', 3, instruction.pc)} already_present goto "
                         f"{arg('relative', 5, instruction.pc)};")
    elif sub == 0x6C:
        form.template = f"{name}(script.byte_at({arg('relative', 2, instruction.pc)}));"
    elif sub == 0xD7:
        # The source explicitly models how the VM interprets the two encoded
        # words using a control byte that lives in another instruction.
        form.template = (f"{name}(script.masked_word({arg('u16', 2)}, "
                         f"{arg('relative', 10, instruction.pc)}, 128), "
                         f"script.masked_word({arg('u16', 4)}, "
                         f"{arg('relative', 10, instruction.pc)}, 64));")
    elif sub == 0x5C and raw[2] == 1:
        form.template = f"{name}(1, script.evaluated_word_at({arg('relative', 6, instruction.pc)}));"
    elif sub == 0x5C and raw[2] > 2:
        form.template = (f"{name}({arg('u8', 2)}) resume_primary_at "
                         f"{arg('relative', 1, instruction.pc)};")
    elif sub == 0x77 and raw[2] == 0:
        form.template = (f"{name}(0, script.masked_word_at({arg('relative', 6, instruction.pc)}, "
                         f"{arg('relative', 14, instruction.pc)}, 128));")
    elif sub is None and op == 0x57 and len(raw) == 2:
        if raw[1] == 15:
            form.template = "movement.refresh_actor_walkmesh();"
        else:
            form.template = (f"{name}({arg('u8', 1)}, "
                             f"script.ballistic_origin({arg('relative', -11, instruction.pc)}));")
    elif sub is None and op == 0x73 and raw[1] > 1:
        form.template = "stall_forever;"
    elif outputs or implicit:
        destinations = [arg("var", offset) for offset, _ in outputs]
        destinations.extend(arg("fixedvar", offset) for offset, _ in implicit)
        if sub == 0xC1:
            arguments = arg("v80", 6)
        elif sub == 0x38:
            arguments = f"[{arg('actor', 4)}, {arg('actor', 5)}]"
        elif sub == 0x69:
            arguments = arg("v80", 4)
        elif sub == 0xC7:
            arguments = arg("v80", 2)
        elif sub == 0x75:
            arguments = arg("actor", 2)
        elif sub == 0xD3:
            # 80087420 evaluates six input words, then writes two direct VM
            # destinations at +14/+16 (already emitted as outputs above).
            arguments = ", ".join(arg("v80", offset) for offset in range(2, 14, 2))
        elif sub == 0x84:
            # +4..+5 are skipped, not another battle parameter. The third
            # evaluated word is copied into the fixed map-entry VM slot.
            arguments = f"{arg('v80', 2)}, {arg('v80', 6)}, {arg('v80', 8)}"
        elif sub == 0xCF:
            arguments = f"{arg('v80', 2)}, {arg('v80', 4)}"
        elif sub in {0x72, 0x73, 0x76}:
            arguments = ", ".join(arg("masked", offset, control, mask)
                                  for offset, _, control, mask in MASKED_EXTENDED_WORDS[sub])
        elif sub == 0xAF:
            arguments = ", ".join(arg("masked", offset, control, mask)
                                  for offset, _, control, mask in MASKED_EXTENDED_WORDS[sub])
        elif op == 0x34:
            arguments = arg("v80", 1)
        elif op in {0x48, 0x49}:
            if op == 0x49:
                name = "state.read_script_s16" if raw[7] else "state.read_script_u16"
            arguments = f"{arg('label', 1)}, {arg('v80', 5)}"
        elif op == 0x2D:
            arguments = arg("actor", 1)
        elif op == 0xA8:
            arguments = arg("v80", 3)
        elif op == 0x87:
            arguments = arg("v80", 1)
        elif op == 0x94:
            arguments = f"{arg('v80', 1)}, {arg('v80', 3)}"
        elif sub == 0x56:
            arguments = arg("v80", 2)
        elif sub is None and op in PRIMARY_INPUT_SCHEMAS:
            arguments = primary_arguments()
        elif key in CONDITIONAL_OUTPUT_VARIABLES:
            arguments = ""
        else:
            positions = {p for offset, _ in outputs for p in (offset, offset + 1)}
            arguments = ", ".join(arg("u8", i) for i in range(start, len(raw)) if i not in positions)
        form.template = f"{name}({arguments}) -> ({', '.join(destinations)});"
    elif sub == 0x0D:
        form.template = f"dialogue.set_portrait({arg('v80', 2)});"
    elif sub in {0x0A, 0x0B}:
        operator = "|= " if sub == 0x0A else "&= ~"
        form.template = f"{arg('packedvar', 2)} {operator}(1 << {arg('bit', 2)});"
    elif sub == 0x0C:
        # Six direct signed words; the handler stores each as a short and has
        # no per-word variable/immediate control byte (8008CFEC).
        form.template = f"{name}({', '.join(arg('s16', offset) for offset in range(2, 14, 2))});"
    elif sub in {0x0F, 0x11}:
        # Pitch and pan fades use the same bit-controlled signed-word readers
        # as FE 1C, with their mask byte at offset 6 (8008C938/8008CB4C).
        form.template = (f"{name}({arg('masked', 2, 6, 0x80)}, "
                         f"{arg('masked', 4, 6, 0x40)});")
    elif sub == 0x1B:
        form.template = f"{name}({arg('s16', 2)}, {arg('s16', 4)});"
    elif sub == 0x1C:
        # The final byte is a control mask, not a fourth argument. The VM
        # reads XYZ in that order with three distinct mask bits.
        form.template = (f"{name}({arg('masked', 2, 8, 0x80)}, "
                          f"{arg('masked', 4, 8, 0x40)}, "
                          f"{arg('masked', 6, 8, 0x20)});")
    elif sub == 0x74:
        # 800985BC passes the raw word to FUN_800A3018: always a VM slot.
        form.template = f"{name}({arg('var', 2)});"
    elif sub == 0xD2:
        # 8008752C only advances the instruction pointer; its payload is not
        # decoded as parameters. Standalone compilation supplies zero payload.
        form.template = f"{name}();"
    elif sub == 0xBD:
        # 80088B68 reads only the evaluated attachment mode at +2.
        form.template = f"{name}({arg('v80', 2)});"
    elif sub == 0x5C and len(raw) == 5:
        # Mode 2 evaluates +3 but discards it; the only independent input is
        # the mode byte; standalone compilation uses an immediate-zero payload.
        form.template = f"{name}({arg('u8', 2)});"
    elif sub == 0x77 and len(raw) == 12:
        # Mode 1 uses three signed words selected by control bits in +11.
        # +3..+4 are skipped; the short mode-0 form reads *past* its boundary
        # and therefore remains conservative in the generic path below.
        form.template = (f"{name}({arg('u8', 2)}, "
                         f"{arg('masked', 5, 11, 0x40)}, "
                         f"{arg('masked', 7, 11, 0x20)}, "
                         f"{arg('masked', 9, 11, 0x10)});")
    elif sub == 0x23:
        positions = [arg("masked", offset, control, mask)
                     for offset, _, control, mask in MASKED_EXTENDED_WORDS[sub]]
        facings = [arg("v80", offset) for offset, _ in EVALUATED_OPERATION_INPUTS[key]]
        form.template = f"{name}({', '.join(positions + facings)});"
    elif sub in MASKED_EXTENDED_WORDS:
        arguments = [arg("masked", at, control, mask)
                     for at, _, control, mask in MASKED_EXTENDED_WORDS[sub]]
        if sub == 0x89:
            arguments.append(arg("actor", 11))
        if sub in {0xAB, 0xAC}:
            arguments.append(arg("low2", 4))
        form.template = f"{name}({', '.join(arguments)});"
    elif sub is not None and (key in EVALUATED_OPERATION_INPUTS or sub in DIRECT_EXTENDED_WORDS
                              or sub in DIRECT_EXTENDED_BYTES):
        # Only collapse proven complete words. Untyped bytes (including mode,
        # selector, and reserved bytes) stay independently editable as raw u8.
        typed = {offset: "v80" for offset, _ in EVALUATED_OPERATION_INPUTS.get(key, ())
                 if offset + 1 < len(raw)}
        typed.update({offset: "u16" for offset, _ in DIRECT_EXTENDED_WORDS.get(sub, ())
                      if offset + 1 < len(raw)})
        typed.update({offset: kind for offset, kind in DIRECT_EXTENDED_BYTES.get(sub, ())
                      if offset < len(raw)})
        arguments = []
        at = 2
        while at < len(raw):
            kind = typed.get(at)
            if kind is not None:
                arguments.append(arg(kind, at))
                at += 1 if kind in {"actor", "u8"} else 2
            else:
                target = target_operand_offset(op, sub, len(raw))
                if target == at:
                    arguments.append(arg("label", at))
                    at += 2
                else:
                    arguments.append(arg("u8", at))
                    at += 1
        form.template = f"{name}({', '.join(arguments)});"
    elif key in EVALUATED_OPERATION_INPUTS:
        arguments = ", ".join(arg("v80", offset) for offset, _ in EVALUATED_OPERATION_INPUTS[key])
        form.template = f"{name}({arguments});"
    elif sub is not None:
        form.template = f"{name}({generic()});"
    elif op == 0x5B:
        form.template = "movement.park_actor_movement_update();"
    elif op in {0x00, 0x0D, 0x13, 0xFD, 0xFF, 0xD1, 0xE4}:
        form.template = {0x00: "stop;", 0x0D: "return;", 0x13: "nop;", 0xFD: "nop;", 0xFF: "nop;"}.get(op, "stall_forever;")
    elif op in {0x01, 0x05, 0x06}:
        verb = "goto" if op == 0x01 else "call"
        # 800A1730 pushes PC+5 and jumps to +1; +3..+4 are skipped payload,
        # not a call argument. Standalone calls relocate their continuation.
        form.template = f"{verb} {arg('label', 1)};"
    elif op == 0x02:
        if raw[5] & 0x30:
            # The handler switches on the whole high nibble; unsupported
            # modes evaluate neither word and leave both comparison values 0.
            lhs, rhs = "0", "0"
        else:
            lhs, rhs = arg("masked", 1, 5, 0x80), arg("masked", 3, 5, 0x40)
        condition = raw[5] & 15
        if condition in {6, 9}:
            test = f"({lhs} & {rhs}) != 0"
        elif condition == 8:
            test = f"({lhs} | {rhs}) != 0"
        elif condition == 10:
            test = f"((~{lhs}) & {rhs}) != 0"
        else:
            test = f"{lhs} {CONDITION_OPERATORS.get(condition, f'cond_{condition:X}')} {rhs}"
        form.template = f"if (!({test})) goto {arg('label', 6)};"
    elif op in {0x07, 0x08, 0x09}:
        mode = {7: "async", 8: "wait", 9: "wait_extended"}[op]
        form.template = f"start {arg('actor', 1)}.routine[{arg('routine', 2)}] priority {arg('priority', 2)} {mode};"
    elif op in {0x0A, 0xCC, 0xC9, 0xCB}:
        dimension = "3d" if op in {0xCC, 0xCB} else "2d"
        negation, verb = ("!", "goto") if op in {0xC9, 0xCB} else ("", "call")
        form.template = f"if ({negation}inside_trigger_{dimension}({arg('u8', 1)})) {verb} {arg('label', 2)};"
    elif op in {0x0C, 0xA7}:
        suffix = "_preserve_ip" if op == 0x0C else ""
        form.template = f"actor.process_player_control_if_owned{suffix}();"
    elif op in {0x14, 0x15, 0x22, 0x23, 0x2A, 0x2B}:
        prop = "world.encounters.enabled" if op in {0x14, 0x15} else "actor.self.visible" if op in {0x22, 0x23} else "actor.self.dialogue_enabled"
        form.template = f"{prop} = {'true' if op in {0x15, 0x22, 0x2B} else 'false'};"
    elif op in {0x16, 0x5C}:
        form.template = f"actor.bind_{'playable_character' if op == 0x16 else 'party_slot'}({arg('v80', 1)});"
    elif op in {0x24, 0x25}:
        form.template = f"{arg('actor', 1)}.visible = {'true' if op == 0x24 else 'false'};"
    elif op == 0x26:
        form.template = f"flow.sleep({arg('v80', 1)});"
    elif op in {0x31, 0x32}:
        prop = "held" if op == 0x31 else "accumulated"
        form.template = f"if ((input.{prop} & {arg('u16', 1)}) == 0) goto {arg('label', 3)};"
    elif op == 0x33:
        form.template = "input.accumulated = 0;"
    elif op in MASKED_VARIABLE_OPCODES:
        operator = {0x35: "=", 0x38: "+=", 0x39: "-=", 0x3A: "|= 1 <<", 0x3B: "&= ~(1 <<", 0x3E: "&=", 0x3F: "|=", 0x40: "^=", 0xDE: "*=", 0xDF: "/="}[op]
        form.template = f"{arg('var', 1)} {operator} {arg('masked', 3, 5, 0x40)}{')' if op == 0x3B else ''};"
    elif op in {0x36, 0x37}:
        form.template = f"{arg('var', 1)} = {'true' if op == 0x36 else 'false'};"
    elif op in {0x3C, 0x3D}:
        form.template = f"{arg('var', 1)}{'++' if op == 0x3C else '--'};"
    elif op in {0x41, 0x42}:
        form.template = f"{arg('var', 1)} {'<<=' if op == 0x41 else '>>='} {arg('v80', 3)};"
    elif op == 0xDC:
        form.template = f"state.swap({arg('var', 1)}, {arg('var', 3)});"
    elif op in {0xAF, 0xB0, 0xB1}:
        prop = {0xAF: "yaw", 0xB0: "projection_dip", 0xB1: "projection_depth"}[op]
        form.template = f"camera.{prop} = {arg('u16' if op == 0xB1 else 's16', 1)};"
    elif op == 0xA6:
        form.template = f"flow.dispatch_triplet_table({arg('v80', 1)});"
    elif sub is None and op in PRIMARY_INPUT_SCHEMAS:
        form.template = f"{name}({primary_arguments()});"
    else:
        form.template = f"{name}({generic()});"
    return complete_instruction_form(form, instruction) if lossless else form


def append_named_operands(form, fields):
    """Add actual format fields as typed arguments, not opaque instruction bytes."""
    if not fields:
        return form
    start = form.template.index('(')
    depth = 1
    end = start + 1
    while depth:
        if form.template[end] == '(':
            depth += 1
        elif form.template[end] == ')':
            depth -= 1
        end += 1
    end -= 1
    args = [f'{name}={form.operand(kind, offset, control, mask)}'
            for name, kind, offset, control, mask in fields]
    prefix = ', ' if form.template[start + 1:end] else ''
    form.template = form.template[:end] + prefix + ', '.join(args) + form.template[end:]
    return form


def complete_instruction_form(form, instruction):
    """Preserve distinctions that the convenient statement spelling omits.

    Reserved/skipped fields are named numeric arguments. Distinct dispatch slots
    have distinct operation spellings. No saved instruction is an encoder input.
    """
    op, sub, raw = instruction.opcode, instruction.subopcode, instruction.raw
    if sub in {0x0A, 0x0B}:
        name = 'set' if sub == 0x0A else 'clear'
        form.template = f'state.{name}_packed_bit({{0}}, {{1}});'
    elif sub is None and op in {0xFD, 0xFF, 0xD1, 0xE4}:
        verb = 'nop' if op in {0xFD, 0xFF} else 'stall'
        form.template = f'flow.{verb}_{op:02x}();'
    elif sub is None and op == 0x73 and raw[1] > 1:
        form.template = f'visual.stall_particle_processing({form.operand("u8", 1)});'
    elif sub is None and op == 0x06:
        form.template = 'flow.call_with_reserved_word({0});'
    elif sub is None and op == 0x02:
        if raw[5] & 15 == 9 and not raw[5] & 0x30:
            form.template = 'if (!any_bits_set({0}, {1})) goto {2};'
        return form

    fields = []
    skipped = {
        (0x06, None): ((3, 'u16', 'reserved'),),
        (0x47, None): ((1, 'u8', 'reserved'),),
        (0xEA, None): ((1, 'u8', 'reserved'),),
        (0xDA, None): ((16, 'u8', 'reserved_tail'),),
        (0xFE, 0x84): ((4, 'u16', 'reserved'),),
        (0xFE, 0xBD): ((4, 'u16', 'reserved_1'), (6, 'u16', 'reserved_2')),
        (0xFE, 0xD2): ((2, 'u16', 'reserved'),),
    }
    extra = skipped.get((op, sub), ())
    if sub == 0x77 and len(raw) == 12:
        extra = ((3, 'u16', 'reserved'),)
    elif sub == 0x5C and len(raw) == 5:
        extra = ((3, 'v80', 'discarded_value'),)
    elif sub is None and op == 0x73 and len(raw) == 8:
        extra = ((2, 'v80', 'discarded_1'), (6, 'v80', 'discarded_2'))
    fields.extend((name, kind, offset, 0, 0) for offset, kind, name in extra)

    # Only the complement of actual operand bits is exposed. The writer rejects
    # a reserved flag value that would overwrite immediate/variable selection.
    coverage = bytearray(b'\xff' * len(raw))
    for operand in form.operands:
        operand.clear_value(coverage)
    controls = sorted({operand.control for operand in form.operands if operand.kind == 'masked'})
    for offset in controls:
        if coverage[offset]:
            fields.append(('reserved_flags', 'bits', offset, 0, coverage[offset]))
    if fields and sub is None and op in MASKED_VARIABLE_OPCODES:
        names = {0x35: 'assign', 0x38: 'add', 0x39: 'subtract', 0x3A: 'set_bit',
                 0x3B: 'clear_bit', 0x3E: 'and', 0x3F: 'or', 0x40: 'xor',
                 0xDE: 'multiply', 0xDF: 'divide'}
        form.template = f'state.{names[op]}({{0}}, {{1}});'
    return append_named_operands(form, fields)


def render_lossless_instruction(instruction, symbols):
    form = instruction_form(instruction, symbols)
    statement = form.render(instruction.raw, symbols)
    labels = {operand.read(instruction.raw, symbols): int.from_bytes(instruction.raw[operand.offset:operand.offset + 2], 'little')
              for operand in form.operands if operand.kind == 'label'}
    try:
        encoded = encode_statement(statement, None, symbols, labels)
    except ValueError:
        encoded = None
    if encoded == instruction.raw:
        return statement
    return instruction_form(instruction, symbols, lossless=True).render(instruction.raw, symbols)


@lru_cache(maxsize=1)
def instruction_seeds() -> tuple[bytes, ...]:
    """Canonical encodings for every distinct opcode/mode form, not name guesses."""
    from decompile_field_scripts import decode_instruction

    result = []
    for opcode in range(256):
        if opcode == 0xFE:
            continue
        modes = (0, 1, 2, 3, 15) if opcode == 0x57 else range(4) if opcode in {0x10, 0x49, 0x73, 0xAF, 0xB0, 0xB1} else (0,)
        for mode in modes:
            raw = bytearray(bytes([opcode]) + bytes(31))
            raw[7 if opcode == 0x49 else 3 if opcode in {0xAF, 0xB0, 0xB1} else 1] = mode
            if opcode == 0x73 and mode == 1:
                raw[3] = raw[7] = 0x80  # Discarded reads use immediate zero.
            # Masked-variable destinations are direct VM offsets, not evaluated
            # operands. Leave their control byte zero; Operand.write sets only
            # the source's actual immediate bit (0x40), never an unused 0x80.
            result.append(bytes(decode_instruction(raw, 0).raw))
    for sub in range(227):
        if sub == 0 or 0x78 <= sub <= 0x7E:
            continue
        for mode in range(4) if sub in {0x27, 0x5C, 0x77, 0xB0, 0xD4, 0xDD} else (0,):
            raw = bytearray(bytes([0xFE, sub, mode]) + bytes(29))
            if sub == 0x5C and mode == 2:
                raw[4] = 0x80  # The evaluated value is discarded by the handler.
            result.append(bytes(decode_instruction(raw, 0).raw))
    return tuple(dict.fromkeys(result))


def _form_key(text: str) -> str:
    match = re.match(r"([a-z_][a-z_0-9]*\.[a-z_][a-z_0-9]*)\(", text.strip())
    if match:
        return match[1]
    match = re.match(r"(if|goto|call|start|stop|return|nop|stall_forever)\b", text.strip())
    return match[1] if match else "*"


@lru_cache(maxsize=64)
def _indexed_seeds(seeds: tuple[bytes, ...]) -> dict[str, tuple[bytes, ...]]:
    from decompile_field_scripts import decode_instruction

    grouped = {}
    for seed in seeds:
        if seed == b"\xfe":
            continue
        instruction = decode_instruction(seed, 0)
        keys = {_form_key(instruction_form(instruction, {}, lossless=complete).template)
                for complete in (False, True)}
        for key in keys:
            grouped.setdefault(key, []).append(seed)
    return {key: tuple(values) for key, values in grouped.items()}


def encode_statement(statement: str, seed: bytes | None, symbols: dict, labels: dict[str, int], *, references: dict[int, str] | None = None, layout_references: dict[int, str] | None = None) -> bytes:
    from decompile_field_scripts import decode_instruction

    statement, hint, dependencies = split_instruction_annotations(statement)
    if hint is not None:
        if seed is not None and hint != seed:
            raise ValueError("conflicting instruction encodings")
        seed = hint
    if layout_references is not None:
        layout_references.update(dependencies)
    if normalized(statement) == "nop;" and seed == b"\xfe":
        return b"\xfe"  # The linker checks the following dispatch byte.
    if normalized(statement) == "flow.extended_prefix();":
        if seed not in {None, b"\xfe"}:
            raise ValueError("extended_prefix encoding must be FE")
        return b"\xfe"  # Selected explicitly in code; the linker checks dispatch.
    match = re.fullmatch(r'raw\("([0-9A-Fa-f\s]+)"(?:,\s*([A-Za-z_0-9,\s]+))?\);', statement.strip())
    if match:
        from decompile_field_scripts import address_operand_offsets

        raw = bytearray(bytes.fromhex(match[1]))
        if match[2] is not None:
            names = [name.strip() for name in match[2].split(",")]
            offsets = address_operand_offsets(decode_instruction(raw, 0))
            if len(names) != len(offsets):
                raise ValueError("raw instruction label count does not match its address fields")
            for offset, name in zip(offsets, names):
                if name not in labels:
                    raise ValueError(f"unresolved label: {name}")
                value = labels[name] if references is None else 0
                struct.pack_into("<H", raw, offset, checked(value, 0, 65535, "target offset"))
                if references is not None:
                    references[offset] = name
        return bytes(raw)
    if seed is not None:
        if seed == b"\xfe":
            # An author may replace a companion's prefix no-op with a different
            # operation. The standalone prefix cannot be decoded as that op.
            seed = None
    if seed is not None:
        instruction = decode_instruction(seed, 0)
        if instruction.size != len(seed):
            raise ValueError("instruction encoding must contain exactly one complete instruction")
    variables = {symbol.reference: offset for offset, symbol in symbols.items()}
    # Prefer the original opcode and its corresponding operator families before
    # canonical forms, retaining invisible control bits wherever possible.
    families = (
        (0x14, 0x15), (0x22, 0x23), (0x24, 0x25), (0x2A, 0x2B),
        (0x36, 0x37), (0x3C, 0x3D), (0x41, 0x42), (7, 8, 9),
        (0x35, 0x38, 0x39, 0x3A, 0x3B, 0x3E, 0x3F, 0x40, 0xDE, 0xDF),
    )
    candidates = [] if seed is None else [seed]
    for family in families:
        if seed is not None and seed[0] in family:
            candidates.extend(bytes([op]) + seed[1:] for op in family if op != seed[0])
    if seed is not None and seed[0] == 0x02:
        candidates.extend(seed[:5] + bytes([(seed[5] & 0xF0) | mode]) + seed[6:] for mode in range(16) if mode != seed[5] & 15)
    # Explicit one-instruction forms allow insertion and replacement with a
    # different-size opcode. Try provenance first to preserve invisible bits.
    key = _form_key(statement)
    candidates.extend(_indexed_seeds(instruction_seeds()).get(key, ()))
    if key == "if" and (seed is None or seed[0] != 0x02):
        candidates.extend(bytes([2, 0, 0, 0, 0, mode, 0, 0]) for mode in range(1, 16))
    errors = []
    choices = [(candidate, complete) for candidate in candidates for complete in (True, False)]
    for candidate, complete in choices:
        instruction = decode_instruction(candidate, 0)
        form = instruction_form(instruction, symbols, lossless=complete)
        matched = form_pattern(form.template, len(form.operands)).fullmatch(normalized(statement))
        if matched is None:
            continue
        try:
            encoding_labels = labels if references is None else dict.fromkeys(labels, 0)
            result = form.encode(statement, candidate, variables, encoding_labels)
        except ValueError as error:
            errors.append(str(error))
            continue
        if result is not None:
            try:
                decoded = decode_instruction(result, 0)
            except ValueError:
                continue
            if decoded.size != len(result):
                continue
            # A second rendering detects overlapping fields and mode-dependent
            # interpretations that cannot represent the requested statement.
            rendered = instruction_form(decoded, symbols, lossless=complete).render(result, symbols)
            values = [matched[f"f{i}"] for i in range(len(form.operands))]
            for i, operand in enumerate(form.operands):
                if operand.kind == "label":
                    values[i] = f"L_{encoding_labels[values[i]]:04X}"
                elif operand.kind == "relative":
                    values[i] = f"L_{operand.offset:04X}"
            expected = form.template.format(*values)
            if normalized(rendered) != normalized(expected):
                errors.append("operands do not round-trip through the selected encoding")
                continue
            if references is not None:
                references.update({operand.offset: matched[f"f{i}"] for i, operand in enumerate(form.operands) if operand.kind == "label"})
            if layout_references is not None:
                for i, operand in enumerate(form.operands):
                    if operand.kind == "relative":
                        name = matched[f"f{i}"]
                        if operand.offset in layout_references and layout_references[operand.offset] != name:
                            raise ValueError("conflicting script-byte dependencies")
                        layout_references[operand.offset] = name
            return result
    raise ValueError(errors[0] if errors else "statement does not match a supported instruction form")
