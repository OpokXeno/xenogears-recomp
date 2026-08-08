#ifndef XG_WORLD_MINIMAP_SOURCE_CAPTURE_H
#define XG_WORLD_MINIMAP_SOURCE_CAPTURE_H

#include "xg_world_minimap.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XG_WORLD_MINIMAP_MAX_AUTHENTICATED_READS = 97,
};

typedef bool (*XgWorldMinimapReadU16)(void *context, uint32_t address,
                                      uint16_t *out_value);
typedef bool (*XgWorldMinimapReadU32)(void *context, uint32_t address,
                                      uint32_t *out_value);

typedef struct XgWorldMinimapAuthenticatedReader {
    void *context;
    XgWorldMinimapReadU16 read_u16;
    XgWorldMinimapReadU32 read_u32;
    uint64_t authentication_generation;
    bool authenticated;
} XgWorldMinimapAuthenticatedReader;

typedef struct XgWorldMinimapCaptureRequest {
    uint64_t authentication_generation;
    uint32_t caller_return;
    int32_t screen_offset_x;
    int32_t screen_offset_y;
    uint16_t projection_distance;
    XgWorldMinimapRasterState raster;
    bool projection_state_authenticated;
} XgWorldMinimapCaptureRequest;

typedef struct XgWorldMinimapCapture {
    XgWorldMinimapSource source;
    uint32_t authenticated_read_count;
    uint32_t authenticated_read_bytes;
    uint32_t active_marker_count;
    bool authenticated;
    bool sealed;
} XgWorldMinimapCapture;

typedef enum XgWorldMinimapCaptureResult {
    XG_WORLD_MINIMAP_CAPTURE_OK = 0,
    XG_WORLD_MINIMAP_CAPTURE_INVALID_ARGUMENT,
    XG_WORLD_MINIMAP_CAPTURE_UNAUTHENTICATED,
    XG_WORLD_MINIMAP_CAPTURE_READ_FAILED,
    XG_WORLD_MINIMAP_CAPTURE_SOURCE_MISMATCH,
    XG_WORLD_MINIMAP_CAPTURE_FORBIDDEN_RANGE,
} XgWorldMinimapCaptureResult;

XgWorldMinimapCaptureResult xg_world_minimap_source_capture(
    const XgWorldMinimapCaptureRequest *request,
    const XgWorldMinimapAuthenticatedReader *reader,
    XgWorldMinimapCapture *out_capture);

#ifdef __cplusplus
}
#endif

#endif
