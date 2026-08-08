#ifndef XG_WORLD_EFFECTS_SOURCE_CAPTURE_H
#define XG_WORLD_EFFECTS_SOURCE_CAPTURE_H

#include "xg_world_effects.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XG_WORLD_EFFECTS_MAX_AUTHENTICATED_READS = 2681,
};

typedef bool (*XgWorldEffectsReadU16)(void *context, uint32_t address,
                                      uint16_t *out_value);
typedef bool (*XgWorldEffectsReadU32)(void *context, uint32_t address,
                                      uint32_t *out_value);

typedef struct XgWorldEffectsAuthenticatedReader {
    void *context;
    XgWorldEffectsReadU16 read_u16;
    XgWorldEffectsReadU32 read_u32;
    uint64_t authentication_generation;
    bool authenticated;
} XgWorldEffectsAuthenticatedReader;

typedef struct XgWorldEffectsRasterState {
    uint16_t draw_area_left;
    uint16_t draw_area_top;
    uint16_t draw_area_right;
    uint16_t draw_area_bottom;
    int16_t draw_offset_x;
    int16_t draw_offset_y;
    bool dither;
    bool mask_set;
    bool mask_check;
} XgWorldEffectsRasterState;

typedef struct XgWorldEffectsCaptureRequest {
    uint64_t authentication_generation;
    uint32_t caller_return;
    int32_t screen_offset_x;
    int32_t screen_offset_y;
    int32_t screen_x_cull_margin;
    uint16_t projection_distance;
    XgWorldEffectsRasterState raster;
    bool projection_state_authenticated;
} XgWorldEffectsCaptureRequest;

typedef struct XgWorldEffectsCapture {
    XgWorldEffectsSource source;
    uint32_t authenticated_read_count;
    uint32_t authenticated_read_bytes;
    uint32_t active_source_count;
    uint32_t particle_array_address;
    bool authenticated;
    bool sealed;
} XgWorldEffectsCapture;

typedef enum XgWorldEffectsCaptureResult {
    XG_WORLD_EFFECTS_CAPTURE_OK = 0,
    XG_WORLD_EFFECTS_CAPTURE_INVALID_ARGUMENT,
    XG_WORLD_EFFECTS_CAPTURE_UNAUTHENTICATED,
    XG_WORLD_EFFECTS_CAPTURE_READ_FAILED,
    XG_WORLD_EFFECTS_CAPTURE_SOURCE_MISMATCH,
    XG_WORLD_EFFECTS_CAPTURE_FORBIDDEN_RANGE,
} XgWorldEffectsCaptureResult;

XgWorldEffectsCaptureResult xg_world_effects_source_capture(
    const XgWorldEffectsCaptureRequest *request,
    const XgWorldEffectsAuthenticatedReader *reader,
    XgWorldEffectsCapture *out_capture);

#ifdef __cplusplus
}
#endif

#endif
