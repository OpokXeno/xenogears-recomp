#ifndef XG_FIELD_CHARACTER_SOURCE_CAPTURE_TYPES_H
#define XG_FIELD_CHARACTER_SOURCE_CAPTURE_TYPES_H

#include "xg_field_character_source_types.h"

#include <stdbool.h>
#include <stdint.h>

typedef bool (*XgFieldCharacterSourceReadU16)(void *context, uint32_t address,
                                              uint16_t *out_value);
typedef bool (*XgFieldCharacterSourceReadU32)(void *context, uint32_t address,
                                              uint32_t *out_value);

typedef struct XgFieldCharacterAuthenticatedReader {
    void *context;
    XgFieldCharacterSourceReadU16 read_u16;
    XgFieldCharacterSourceReadU32 read_u32;
    uint64_t authentication_generation;
    uint8_t authenticated;
} XgFieldCharacterAuthenticatedReader;

typedef struct XgFieldCharacterSourceCaptureRequest {
    uint8_t game_sha256[XG_FIELD_CHARACTER_SOURCE_DIGEST_SIZE];
    uint8_t manifest_sha256[XG_FIELD_CHARACTER_SOURCE_DIGEST_SIZE];
    uint64_t source_generation;
    uint64_t scene_generation;
    XgFieldCharacterSourceVisualState visual_state;
    uint64_t vram_mutation_serial;
    uint32_t producer_record_id;
    uint32_t producer_entry;
    uint32_t actor_index;
    uint32_t actor_record_address;
    uint32_t model_address;
    uint32_t producer_stack_pointer;
    uint32_t producer_ft4_index;
    XgFieldCharacterSourceRasterState raster;
} XgFieldCharacterSourceCaptureRequest;

typedef enum XgFieldCharacterSourceCaptureResult {
    XG_FIELD_CHARACTER_SOURCE_CAPTURE_OK = 0,
    XG_FIELD_CHARACTER_SOURCE_CAPTURE_INVALID_ARGUMENT = 1,
    XG_FIELD_CHARACTER_SOURCE_CAPTURE_UNAUTHENTICATED = 2,
    XG_FIELD_CHARACTER_SOURCE_CAPTURE_READ_FAILED = 3,
    XG_FIELD_CHARACTER_SOURCE_CAPTURE_SOURCE_MISMATCH = 4,
    XG_FIELD_CHARACTER_SOURCE_CAPTURE_FORBIDDEN_RANGE = 5,
    XG_FIELD_CHARACTER_SOURCE_CAPTURE_CULLED = 6,
} XgFieldCharacterSourceCaptureResult;

#endif
