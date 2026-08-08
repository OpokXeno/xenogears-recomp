#ifndef XG_WORLD_CLOUDS_SOURCE_CAPTURE_H
#define XG_WORLD_CLOUDS_SOURCE_CAPTURE_H

#include "xg_world_clouds.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XG_WORLD_CLOUDS_MAX_AUTHENTICATED_READS = 635,
};

typedef bool (*XgWorldCloudsReadU16)(void *context, uint32_t address,
                                     uint16_t *out_value);
typedef bool (*XgWorldCloudsReadU32)(void *context, uint32_t address,
                                     uint32_t *out_value);

typedef struct XgWorldCloudsAuthenticatedReader {
    void *context;
    XgWorldCloudsReadU16 read_u16;
    XgWorldCloudsReadU32 read_u32;
    uint64_t authentication_generation;
    bool authenticated;
} XgWorldCloudsAuthenticatedReader;

typedef struct XgWorldCloudsRasterState {
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
} XgWorldCloudsRasterState;

typedef struct XgWorldCloudsCaptureRequest {
    uint64_t authentication_generation;
    uint32_t caller_return;
    int32_t screen_offset_x;
    int32_t screen_offset_y;
    int32_t screen_x_cull_margin;
    uint16_t projection_distance;
    XgWorldCloudsRasterState raster;
    bool projection_state_authenticated;
} XgWorldCloudsCaptureRequest;

typedef struct XgWorldCloudsCapture {
    XgWorldCloudsSource source;
    uint32_t position_array_address;
    uint32_t velocity_array_address;
    uint32_t authenticated_read_count;
    uint32_t authenticated_read_bytes;
    bool authenticated;
    bool sealed;
} XgWorldCloudsCapture;

typedef enum XgWorldCloudsCaptureResult {
    XG_WORLD_CLOUDS_CAPTURE_OK = 0,
    XG_WORLD_CLOUDS_CAPTURE_INVALID_ARGUMENT,
    XG_WORLD_CLOUDS_CAPTURE_UNAUTHENTICATED,
    XG_WORLD_CLOUDS_CAPTURE_READ_FAILED,
    XG_WORLD_CLOUDS_CAPTURE_SOURCE_MISMATCH,
    XG_WORLD_CLOUDS_CAPTURE_FORBIDDEN_RANGE,
} XgWorldCloudsCaptureResult;

XgWorldCloudsCaptureResult xg_world_clouds_source_capture(
    const XgWorldCloudsCaptureRequest *request,
    const XgWorldCloudsAuthenticatedReader *reader,
    XgWorldCloudsCapture *out_capture);

#ifdef __cplusplus
}
#endif

#endif
