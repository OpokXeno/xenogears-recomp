#ifndef XG_FIELD_CHARACTER_RUNTIME_H
#define XG_FIELD_CHARACTER_RUNTIME_H

#include "cpu_state.h"
#include "xg_field_character_adapter.h"
#include "xg_field_character_source.h"
#include "xg_field_character_source_adapter.h"
#include "xg_render_auth_runtime.h"
#include "xg_render_ir.h"

#include <stdint.h>

typedef enum XgFieldCharacterRuntimeResult {
    XG_FIELD_CHARACTER_RUNTIME_OK = 0,
    XG_FIELD_CHARACTER_RUNTIME_INVALID_ARGUMENT = 1,
    XG_FIELD_CHARACTER_RUNTIME_UNMAPPED_TEMPLATE = 2,
    XG_FIELD_CHARACTER_RUNTIME_SEMANTIC_READ_FAILED = 3,
    XG_FIELD_CHARACTER_RUNTIME_INVALID_MATERIAL = 4,
    XG_FIELD_CHARACTER_RUNTIME_ADAPTER_FAILED = 5,
    XG_FIELD_CHARACTER_RUNTIME_COMPARE_FAILED = 6,
    XG_FIELD_CHARACTER_RUNTIME_INCOMPLETE_SOURCE = 7,
} XgFieldCharacterRuntimeResult;

typedef struct XgFieldCharacterRuntimeCandidate {
    XgFieldCharacterCandidate candidate;
    XgFieldCharacterSourceDerived source_derived;
    uint32_t packet_guest_address;
    uint16_t draw_area_left;
    uint16_t draw_area_top;
    uint16_t draw_area_right;
    uint16_t draw_area_bottom;
    int16_t draw_offset_x;
    int16_t draw_offset_y;
    uint8_t texture_window_mask_x;
    uint8_t texture_window_mask_y;
    uint8_t texture_window_offset_x;
    uint8_t texture_window_offset_y;
    uint8_t dither;
    uint8_t mask_set;
    uint8_t mask_check;
    uint8_t valid;
} XgFieldCharacterRuntimeCandidate;

XgFieldCharacterRuntimeResult xg_field_character_runtime_build_candidate(
    const XgFieldCharacterSourceSnapshot *snapshot,
    XgFieldCharacterRuntimeCandidate *out_candidate);
XgFieldCharacterRuntimeResult xg_field_character_runtime_build_shadow_candidate(
    CPUState *cpu, const PsxXgRenderFt4Geometry *geometry,
    XgFieldCharacterRuntimeCandidate *out_candidate);
XgFieldCharacterRuntimeResult xg_field_character_runtime_compare_original(
    CPUState *cpu, const XgFieldCharacterRuntimeCandidate *runtime_candidate,
    uint32_t original_ot_bucket, uint32_t authenticated_ot_bucket,
    XgRenderIrNativePrimitive *out_primitive, uint32_t *out_compare_result,
    uint32_t *out_mismatch_word, uint32_t *out_mismatch_byte);

#endif
