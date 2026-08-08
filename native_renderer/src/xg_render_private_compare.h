#ifndef XG_RENDER_PRIVATE_COMPARE_H
#define XG_RENDER_PRIVATE_COMPARE_H

#include "xg_field_character_adapter.h"
#include "xg_render_ir.h"

#include <stdint.h>

enum {
    XG_RENDER_PRIVATE_FT4_WORD_COUNT = XG_FIELD_CHARACTER_PACKET_WORD_COUNT,
};

typedef enum XgRenderPrivateCompareResult {
    XG_RENDER_PRIVATE_COMPARE_EQUAL = 0,
    XG_RENDER_PRIVATE_COMPARE_INVALID_ARGUMENT = 1,
    XG_RENDER_PRIVATE_COMPARE_CANDIDATE_NOT_FINALIZED = 2,
    XG_RENDER_PRIVATE_COMPARE_CANDIDATE_INVALID = 3,
    XG_RENDER_PRIVATE_COMPARE_ORIGINAL_UNAVAILABLE = 4,
    XG_RENDER_PRIVATE_COMPARE_PACKET_LENGTH_MISMATCH = 5,
    XG_RENDER_PRIVATE_COMPARE_PACKET_MISMATCH = 6,
    XG_RENDER_PRIVATE_COMPARE_OT_BUCKET_MISMATCH = 7,
    XG_RENDER_PRIVATE_COMPARE_DRAW_STATE_MISMATCH = 8,
} XgRenderPrivateCompareResult;

typedef struct XgRenderPrivateDrawState {
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
} XgRenderPrivateDrawState;

typedef struct XgRenderPrivateOriginalPacket {
    const uint32_t *words;
    uint16_t word_count;
    uint32_t ot_bucket;
    XgRenderPrivateDrawState draw_state;
} XgRenderPrivateOriginalPacket;

XgRenderPrivateCompareResult xg_render_private_compare_field_character(
    const XgFieldCharacterCandidate *candidate,
    const XgRenderPrivateOriginalPacket *original,
    uint32_t authenticated_ot_bucket,
    XgRenderIrNativePrimitive *out_primitive);
XgRenderPrivateCompareResult xg_render_private_compare_field_character_detailed(
    const XgFieldCharacterCandidate *candidate,
    const XgRenderPrivateOriginalPacket *original,
    uint32_t authenticated_ot_bucket,
    XgRenderIrNativePrimitive *out_primitive,
    uint32_t *out_mismatch_word,
    uint32_t *out_mismatch_byte);

#endif
