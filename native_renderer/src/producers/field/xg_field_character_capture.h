#ifndef XG_FIELD_CHARACTER_CAPTURE_H
#define XG_FIELD_CHARACTER_CAPTURE_H

#include "xg_field_character_adapter_types.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum XgFieldCharacterCaptureResult {
    XG_FIELD_CHARACTER_CAPTURE_OK = 0,
    XG_FIELD_CHARACTER_CAPTURE_INVALID_ARGUMENT = 1,
    XG_FIELD_CHARACTER_CAPTURE_UNMAPPED_TEMPLATE = 2,
    XG_FIELD_CHARACTER_CAPTURE_SEMANTIC_READ_FAILED = 3,
    XG_FIELD_CHARACTER_CAPTURE_INVALID_MATERIAL = 4,
} XgFieldCharacterCaptureResult;

typedef enum XgFieldCharacterSemanticTemplate {
    XG_FIELD_CHARACTER_SEMANTIC_STATIC_TABLE = 0,
    XG_FIELD_CHARACTER_SEMANTIC_DYNAMIC_ACTOR = 1,
} XgFieldCharacterSemanticTemplate;

typedef struct XgFieldCharacterGeometryInput {
    uint32_t packet_guest_address;
    XgFieldCharacterSemanticTemplate semantic_template;
    int16_t x[XG_FIELD_CHARACTER_VERTEX_COUNT];
    int16_t y[XG_FIELD_CHARACTER_VERTEX_COUNT];
} XgFieldCharacterGeometryInput;

typedef struct XgFieldCharacterDrawStateInput {
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
} XgFieldCharacterDrawStateInput;

typedef bool (*XgFieldCharacterReadU16)(void *context, uint32_t address,
                                        uint16_t *out_value);

typedef struct XgFieldCharacterSemanticReader {
    void *context;
    XgFieldCharacterReadU16 read_u16;
} XgFieldCharacterSemanticReader;

XgFieldCharacterCaptureResult xg_field_character_capture_build(
    const XgFieldCharacterGeometryInput *geometry,
    const XgFieldCharacterDrawStateInput *draw_state,
    const XgFieldCharacterSemanticReader *reader,
    XgFieldCharacterCapture *out_capture);

#endif
