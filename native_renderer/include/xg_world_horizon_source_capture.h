#ifndef XG_WORLD_HORIZON_SOURCE_CAPTURE_H
#define XG_WORLD_HORIZON_SOURCE_CAPTURE_H

#include "xg_world_horizon.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XG_WORLD_HORIZON_MAX_AUTHENTICATED_READS = 29,
};

typedef bool (*XgWorldHorizonReadU16)(void *context, uint32_t address,
                                      uint16_t *out_value);
typedef bool (*XgWorldHorizonReadU32)(void *context, uint32_t address,
                                      uint32_t *out_value);

typedef struct XgWorldHorizonAuthenticatedReader {
    void *context;
    XgWorldHorizonReadU16 read_u16;
    XgWorldHorizonReadU32 read_u32;
    uint64_t authentication_generation;
    bool authenticated;
} XgWorldHorizonAuthenticatedReader;

typedef struct XgWorldHorizonRasterState {
    uint16_t draw_area_left;
    uint16_t draw_area_top;
    uint16_t draw_area_right;
    uint16_t draw_area_bottom;
    int16_t draw_offset_x;
    int16_t draw_offset_y;
    bool dither;
    bool mask_set;
    bool mask_check;
} XgWorldHorizonRasterState;

typedef struct XgWorldHorizonCaptureRequest {
    uint64_t authentication_generation;
    uint32_t caller_return;
    int32_t screen_offset_x;
    int32_t screen_offset_y;
    uint16_t projection_distance;
    XgWorldHorizonRasterState raster;
    bool projection_state_authenticated;
} XgWorldHorizonCaptureRequest;

typedef struct XgWorldHorizonCapture {
    XgWorldHorizonSource source;
    uint32_t authenticated_read_count;
    uint32_t authenticated_read_bytes;
    uint32_t buffer_index;
    bool authenticated;
    bool sealed;
} XgWorldHorizonCapture;

typedef enum XgWorldHorizonCaptureResult {
    XG_WORLD_HORIZON_CAPTURE_OK = 0,
    XG_WORLD_HORIZON_CAPTURE_INVALID_ARGUMENT,
    XG_WORLD_HORIZON_CAPTURE_UNAUTHENTICATED,
    XG_WORLD_HORIZON_CAPTURE_READ_FAILED,
    XG_WORLD_HORIZON_CAPTURE_SOURCE_MISMATCH,
    XG_WORLD_HORIZON_CAPTURE_FORBIDDEN_RANGE,
} XgWorldHorizonCaptureResult;

XgWorldHorizonCaptureResult xg_world_horizon_source_capture(
    const XgWorldHorizonCaptureRequest *request,
    const XgWorldHorizonAuthenticatedReader *reader,
    XgWorldHorizonCapture *out_capture);

#ifdef __cplusplus
}
#endif

#endif
