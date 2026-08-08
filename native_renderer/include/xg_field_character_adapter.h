#ifndef XG_FIELD_CHARACTER_ADAPTER_H
#define XG_FIELD_CHARACTER_ADAPTER_H

#include "xg_render_ir.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XG_FIELD_CHARACTER_VERTEX_COUNT = 4,
    XG_FIELD_CHARACTER_PACKET_WORD_COUNT = 9,
    XG_FIELD_CHARACTER_FAMILY_OPCODE = 0x2c,
    XG_FIELD_CHARACTER_FIXED_FRACTION_BITS = 16,
};

typedef enum XgFieldCharacterAdapterResult {
    XG_FIELD_CHARACTER_ADAPTER_OK = 0,
    XG_FIELD_CHARACTER_ADAPTER_INVALID_ARGUMENT = 1,
    XG_FIELD_CHARACTER_ADAPTER_INVALID_CAPTURE = 2,
} XgFieldCharacterAdapterResult;

typedef struct XgFieldCharacterCaptureVertex {
    int16_t x;
    int16_t y;
    uint8_t u;
    uint8_t v;
} XgFieldCharacterCaptureVertex;

typedef struct XgFieldCharacterCapture {
    XgFieldCharacterCaptureVertex vertices[XG_FIELD_CHARACTER_VERTEX_COUNT];
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint16_t tpage;
    uint16_t clut_x;
    uint16_t clut_y;
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
    uint8_t semi_transparent;
} XgFieldCharacterCapture;

typedef struct XgFieldCharacterCandidateVertex {
    int32_t x;
    int32_t y;
    int32_t u;
    int32_t v;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} XgFieldCharacterCandidateVertex;

typedef struct XgFieldCharacterCandidate {
    XgFieldCharacterCandidateVertex vertices[XG_FIELD_CHARACTER_VERTEX_COUNT];
    uint16_t tpage;
    uint16_t texture_page_x;
    uint16_t texture_page_y;
    uint16_t clut_x;
    uint16_t clut_y;
    uint16_t draw_area_left;
    uint16_t draw_area_top;
    uint16_t draw_area_right;
    uint16_t draw_area_bottom;
    int16_t draw_offset_x;
    int16_t draw_offset_y;
    uint8_t texture_depth;
    uint8_t blend_mode;
    uint8_t texture_window_mask_x;
    uint8_t texture_window_mask_y;
    uint8_t texture_window_offset_x;
    uint8_t texture_window_offset_y;
    uint8_t dither;
    uint8_t mask_set;
    uint8_t mask_check;
    uint8_t semi_transparent;
    uint8_t finalized;
} XgFieldCharacterCandidate;

XgFieldCharacterAdapterResult xg_field_character_adapter_build(
    const XgFieldCharacterCapture *capture,
    XgFieldCharacterCandidate *out_candidate);
XgFieldCharacterAdapterResult xg_field_character_adapter_build_primitive(
    const XgFieldCharacterCandidate *candidate,
    XgRenderIrNativePrimitive *out_primitive);

#ifdef __cplusplus
}
#endif

#endif
