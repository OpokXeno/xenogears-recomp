#ifndef XG_WORLD_TERRAIN_WATER_SOURCE_CAPTURE_H
#define XG_WORLD_TERRAIN_WATER_SOURCE_CAPTURE_H

#include "xg_world_terrain_water.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XG_WORLD_TERRAIN_WATER_MAX_AUTHENTICATED_READS = 8388,
    XG_WORLD_TERRAIN_WATER_NATIVE_MAX_AUTHENTICATED_READS = 8391,
    XG_WORLD_TERRAIN_WATER_NATIVE_MAX_AUTHENTICATED_BYTES = 33116,
    XG_WORLD_TERRAIN_WATER_NATIVE_OT_BUCKET_COUNT = 0xf0,
    XG_WORLD_TERRAIN_WATER_NATIVE_PACKET_STRIDE = 0x20,
    XG_WORLD_TERRAIN_WATER_NATIVE_PACKET_WORD_COUNT = 8,
    XG_WORLD_TERRAIN_WATER_NATIVE_PACKET_TAG_WORD_COUNT = 7,
    XG_WORLD_TERRAIN_WATER_NATIVE_PACKET_FULL_WORD_WRITE_MASK = 0x7d,
    XG_WORLD_TERRAIN_WATER_NATIVE_PACKET_LOW_HALF_WRITE_MASK = 0x80,
    XG_WORLD_TERRAIN_WATER_NATIVE_SCRATCH_BYTES = 0x390,
    XG_WORLD_TERRAIN_WATER_NATIVE_SCRATCH_WORD_COUNT =
        XG_WORLD_TERRAIN_WATER_NATIVE_SCRATCH_BYTES / 4,
};

#define XG_WORLD_TERRAIN_WATER_NATIVE_ENTRY_PC UINT32_C(0x8009932c)
#define XG_WORLD_TERRAIN_WATER_NATIVE_CONTEXT_ADDRESS UINT32_C(0x8009be3c)
#define XG_WORLD_TERRAIN_WATER_NATIVE_POSITION_ADDRESS UINT32_C(0x8009be28)
#define XG_WORLD_TERRAIN_WATER_NATIVE_FINAL_COUNT_ADDRESS \
    UINT32_C(0x8009d7dc)
#define XG_WORLD_TERRAIN_WATER_NATIVE_SCRATCH_ADDRESS UINT32_C(0x1f800000)
#define XG_WORLD_TERRAIN_WATER_NATIVE_PACKET_MATERIAL_WORD \
    UINT32_C(0x24808080)

typedef bool (*XgWorldTerrainWaterReadU16)(void *context, uint32_t address,
                                           uint16_t *out_value);
typedef bool (*XgWorldTerrainWaterReadU32)(void *context, uint32_t address,
                                           uint32_t *out_value);

typedef struct XgWorldTerrainWaterAuthenticatedReader {
    void *context;
    XgWorldTerrainWaterReadU16 read_u16;
    XgWorldTerrainWaterReadU32 read_u32;
    uint64_t authentication_generation;
    bool authenticated;
} XgWorldTerrainWaterAuthenticatedReader;

typedef struct XgWorldTerrainWaterRasterState {
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
    bool dither;
    bool mask_set;
    bool mask_check;
} XgWorldTerrainWaterRasterState;

typedef struct XgWorldTerrainWaterCaptureRequest {
    uint64_t authentication_generation;
    uint32_t caller_return;
    int32_t screen_offset_x;
    int32_t screen_offset_y;
    int32_t screen_x_cull_margin;
    uint16_t projection_distance;
    XgWorldTerrainWaterRasterState raster;
    bool projection_state_authenticated;
} XgWorldTerrainWaterCaptureRequest;

typedef struct XgWorldTerrainWaterCapture {
    XgWorldTerrainWaterSource source;
    uint32_t authenticated_read_count;
    uint32_t authenticated_read_bytes;
    uint32_t captured_resource_count;
    bool authenticated;
    bool sealed;
} XgWorldTerrainWaterCapture;

typedef enum XgWorldTerrainWaterCaptureResult {
    XG_WORLD_TERRAIN_WATER_CAPTURE_OK = 0,
    XG_WORLD_TERRAIN_WATER_CAPTURE_INVALID_ARGUMENT,
    XG_WORLD_TERRAIN_WATER_CAPTURE_UNAUTHENTICATED,
    XG_WORLD_TERRAIN_WATER_CAPTURE_READ_FAILED,
    XG_WORLD_TERRAIN_WATER_CAPTURE_SOURCE_MISMATCH,
    XG_WORLD_TERRAIN_WATER_CAPTURE_FORBIDDEN_RANGE,
} XgWorldTerrainWaterCaptureResult;

XgWorldTerrainWaterCaptureResult xg_world_terrain_water_source_capture(
    const XgWorldTerrainWaterCaptureRequest *request,
    const XgWorldTerrainWaterAuthenticatedReader *reader,
    XgWorldTerrainWaterCapture *out_capture);

bool xg_world_terrain_water_caller_is_valid(uint32_t caller_return);

typedef struct XgWorldTerrainWaterNativeRequest {
    XgWorldTerrainWaterCaptureRequest capture;
    uint32_t entry_pc;
    uint32_t ot_base;
    uint32_t packet_base;
    uint32_t position_address;
} XgWorldTerrainWaterNativeRequest;

/* Final scratch state is represented as aligned words plus bit write masks.
 * A zero mask preserves the complete existing word; 0x0000ffff preserves its
 * high half. */
typedef struct XgWorldTerrainWaterNativeScratch {
    uint32_t values[XG_WORLD_TERRAIN_WATER_NATIVE_SCRATCH_WORD_COUNT];
    uint32_t write_masks[XG_WORLD_TERRAIN_WATER_NATIVE_SCRATCH_WORD_COUNT];
} XgWorldTerrainWaterNativeScratch;

typedef struct XgWorldTerrainWaterNativePreparation {
    XgWorldTerrainWaterNativeScratch scratch;
    uint64_t authentication_generation;
    uint32_t ot_base;
    uint32_t packet_base;
    uint32_t position_address;
    uint32_t record_count;
    uint32_t final_count;
    uint32_t continuation_pc;
    uint32_t authenticated_read_count;
    uint32_t authenticated_read_bytes;
    uint32_t captured_resource_count;
    bool authenticated;
    bool sealed;
} XgWorldTerrainWaterNativePreparation;

typedef enum XgWorldTerrainWaterNativeResult {
    XG_WORLD_TERRAIN_WATER_NATIVE_OK = 0,
    XG_WORLD_TERRAIN_WATER_NATIVE_INVALID_ARGUMENT,
    XG_WORLD_TERRAIN_WATER_NATIVE_UNAUTHENTICATED,
    XG_WORLD_TERRAIN_WATER_NATIVE_READ_FAILED,
    XG_WORLD_TERRAIN_WATER_NATIVE_SOURCE_MISMATCH,
    XG_WORLD_TERRAIN_WATER_NATIVE_FORBIDDEN_RANGE,
    XG_WORLD_TERRAIN_WATER_NATIVE_BUILD_FAILED,
    XG_WORLD_TERRAIN_WATER_NATIVE_INVALID_OUTPUT,
} XgWorldTerrainWaterNativeResult;

/* Authenticated value-only boundary for replacing the complete body at
 * XG_WORLD_TERRAIN_WATER_NATIVE_ENTRY_PC. On success, records are in Original
 * allocation order and preparation contains every non-transient guest output.
 *
 * Commit contract:
 * - stage every semantic FT3 before publishing guest state;
 * - require packet word 1 to equal NATIVE_PACKET_MATERIAL_WORD and preserve
 *   it, write full words selected by NATIVE_PACKET_FULL_WORD_WRITE_MASK, and
 *   merge the low half selected by NATIVE_PACKET_LOW_HALF_WRITE_MASK;
 * - insert each record into its OT bucket in record order, using a 7-word tag
 *   and replacing the touched OT word with packet_address & 0x00ffffff;
 * - merge each scratch word with its write mask, write final_count as a full
 *   word, leave the packet cursor and GTE/register state unchanged, then resume
 *   at continuation_pc.
 * No output is authoritative after a non-OK result, and no guest write may be
 * published until target preflight and all primitive staging have succeeded. */
XgWorldTerrainWaterNativeResult xg_world_terrain_water_native_prepare(
    const XgWorldTerrainWaterNativeRequest *request,
    const XgWorldTerrainWaterAuthenticatedReader *reader,
    XgWorldTerrainWaterRecord *records, uint32_t record_capacity,
    XgWorldTerrainWaterNativePreparation *out_preparation);

#ifdef __cplusplus
}
#endif

#endif
